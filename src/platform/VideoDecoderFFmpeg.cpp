#include "VideoDecoderFFmpeg.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef __linux__
#include <libavutil/hwcontext_vaapi.h>
#endif
}

#ifdef __linux__
#include <va/va.h>
#include <va/va_drmcommon.h>
#endif

#ifdef __APPLE__
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace drift::platform {

namespace {

using drift::core::VideoFrame;

std::string ffmpegError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// Decodes on its own thread into a small bounded queue of RGBA frames.
// frameAt(t) promotes queued frames due at or before t, blocking while the
// decoder is behind — frame content therefore depends only on the requested
// time, which keeps headless rendering deterministic.
class FFmpegVideoDecoder final : public drift::core::VideoDecoder {
public:
    ~FFmpegVideoDecoder() override
    {
        {
            std::lock_guard lock(mMutex);
            mStop = true;
        }
        mConsumed.notify_all();
        if (mThread.joinable()) {
            mThread.join();
        }
        for (auto& item : mQueue) {
            releaseItem(item);
        }
        releaseItem(mCurrent);
        if (mSws) {
            sws_freeContext(mSws);
        }
        if (mCodec) {
            avcodec_free_context(&mCodec);
        }
        if (mHwDevice) {
            av_buffer_unref(&mHwDevice);
        }
        if (mFormat) {
            avformat_close_input(&mFormat);
        }
    }

    bool open(const std::string& path, bool loop, std::string& error)
    {
        mLoop = loop;

        int ret = avformat_open_input(&mFormat, path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            error = ffmpegError(ret);
            return false;
        }
        ret = avformat_find_stream_info(mFormat, nullptr);
        if (ret < 0) {
            error = ffmpegError(ret);
            return false;
        }
        const AVCodec* codec = nullptr;
        mStream = av_find_best_stream(mFormat, AVMEDIA_TYPE_VIDEO, -1, -1,
                                      &codec, 0);
        if (mStream < 0 || !codec) {
            error = "no decodable video stream";
            return false;
        }
        mCodec = avcodec_alloc_context3(codec);
        if (!mCodec ||
            avcodec_parameters_to_context(
                mCodec, mFormat->streams[mStream]->codecpar) < 0) {
            error = "cannot configure decoder";
            return false;
        }
        // The stream's default decoder can be a pure software externa-lib
        // decoder (AV1 defaults to libdav1d, which has no hwaccel configs);
        // hardware decode may need a different decoder for the same codec.
        if (const AVCodec* hwCodec = setupHwDecode(codec)) {
            if (hwCodec != codec) {
                codec = hwCodec;
                avcodec_free_context(&mCodec);
                mCodec = avcodec_alloc_context3(codec);
                if (!mCodec ||
                    avcodec_parameters_to_context(
                        mCodec, mFormat->streams[mStream]->codecpar) < 0) {
                    error = "cannot configure decoder";
                    return false;
                }
            }
            mCodec->hw_device_ctx = av_buffer_ref(mHwDevice);
            mCodec->opaque = this;
            mCodec->get_format = [](AVCodecContext* ctx,
                                    const AVPixelFormat* formats) {
                auto* self = (FFmpegVideoDecoder*)ctx->opaque;
                for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE;
                     ++f) {
                    if (*f == self->mHwFormat) {
                        return *f;
                    }
                }
                // Hardware path not offered (e.g. unsupported profile):
                // decode in software.
                return formats[0];
            };
        }
        ret = avcodec_open2(mCodec, codec, nullptr);
        if (ret < 0) {
            error = ffmpegError(ret);
            return false;
        }

        const AVStream* stream = mFormat->streams[mStream];
        mTimeBase = av_q2d(stream->time_base);
        const AVRational rate = stream->avg_frame_rate;
        mFrameSeconds = rate.num > 0 ? (double)rate.den / rate.num : 1.0 / 30.0;
        if (mFormat->duration != AV_NOPTS_VALUE && mFormat->duration > 0) {
            mDuration = (double)mFormat->duration / AV_TIME_BASE;
        }

        mThread = std::thread([this] { decodeLoop(); });
        return true;
    }

    // Picks a hardware decode backend per DRIFT_HWDEC (Linux:
    // auto|vaapi|cuda|off, default auto = VAAPI then CUDA; macOS:
    // auto|videotoolbox|off) and the decoder implementation that can drive
    // it. Returns null for software decode (and logs the choice). Decoded
    // frames either export zero-copy (VAAPI dmabuf / VideoToolbox
    // IOSurface) or transfer back to system memory once and feed the same
    // swscale path as software.
    const AVCodec* setupHwDecode(const AVCodec* streamCodec)
    {
        const char* pref = getenv("DRIFT_HWDEC");
        if (pref && !strcmp(pref, "off")) {
            printf("drift: video decode: software\n");
            return nullptr;
        }
        struct Backend {
            AVHWDeviceType type;
            const char* name;
        };
        const Backend backends[] = {
#ifdef __APPLE__
            { AV_HWDEVICE_TYPE_VIDEOTOOLBOX, "videotoolbox" },
#else
            { AV_HWDEVICE_TYPE_VAAPI, "vaapi" },
            { AV_HWDEVICE_TYPE_CUDA, "cuda" },
#endif
        };
        for (const Backend& backend : backends) {
            if (pref && *pref && strcmp(pref, "auto") != 0 &&
                strcmp(pref, backend.name) != 0) {
                continue;
            }
            // Any decoder for this codec id will do, not just the stream
            // default — hwaccels live on the native decoders.
            const AVCodec* found = nullptr;
            AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
            void* iter = nullptr;
            for (const AVCodec* c; (c = av_codec_iterate(&iter));) {
                if (!av_codec_is_decoder(c) || c->id != streamCodec->id) {
                    continue;
                }
                for (int i = 0;; ++i) {
                    const AVCodecHWConfig* config = avcodec_get_hw_config(c, i);
                    if (!config) {
                        break;
                    }
                    if ((config->methods &
                         AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                        config->device_type == backend.type) {
                        found = c;
                        hwFormat = config->pix_fmt;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            if (!found) {
                continue;
            }
            AVBufferRef* device = nullptr;
            if (av_hwdevice_ctx_create(&device, backend.type, nullptr, nullptr,
                                       0) < 0
#ifdef __linux__
                && av_hwdevice_ctx_create(&device, backend.type,
                                          "/dev/dri/renderD128", nullptr, 0) < 0
#endif
            ) {
                continue;
            }
            mHwDevice = device;
            mHwFormat = hwFormat;
            // Zero-copy needs a surface-export route: VAAPI dmabufs on
            // Linux, VideoToolbox IOSurfaces on macOS. CUDA frames have
            // neither and always transfer.
            const char* zc = getenv("DRIFT_ZEROCOPY");
#ifdef __APPLE__
            mZeroCopy = backend.type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX &&
                        !(zc && !strcmp(zc, "off"));
#else
            mZeroCopy = backend.type == AV_HWDEVICE_TYPE_VAAPI &&
                        !(zc && !strcmp(zc, "off"));
#endif
            printf("drift: video decode: %s (%s)%s\n", backend.name,
                   found->name, mZeroCopy ? " zero-copy" : "");
            return found;
        }
        printf("drift: video decode: software\n");
        return nullptr;
    }

    const VideoFrame* frameAt(double seconds) override
    {
        std::unique_lock lock(mMutex);
        for (;;) {
            while (!mQueue.empty() && mQueue.front().pts <= seconds) {
                releaseItem(mCurrent);
                mCurrent = std::move(mQueue.front());
                mHaveCurrent = true;
                mQueue.pop_front();
                mConsumed.notify_all();
            }
            if (mHaveCurrent && !mQueue.empty()) {
                return &mCurrent.frame; // next queued frame is in the future
            }
            if (mEof || mError) {
                if (mHaveCurrent) {
                    // The stream is drained: what we hold is the final
                    // frame (§17.4). Set here on the render thread —
                    // mCurrent is only touched under the lock.
                    if (mEof && !mLoop) {
                        mCurrent.frame.last = true;
                    }
                    return &mCurrent.frame;
                }
                return nullptr;
            }
            mProduced.wait(lock); // decoder is behind; wait for it
        }
    }

    void restart() override
    {
        std::lock_guard lock(mMutex);
        mRestart = true;
        // Un-finish immediately so a frameAt right after us blocks for the
        // re-decoded first frame (the poster) instead of returning the old
        // tail; the stale queue would otherwise serve pre-restart frames.
        mEof = false;
        for (auto& item : mQueue) {
            releaseItem(item);
        }
        mQueue.clear();
        mConsumed.notify_all();
    }

    void disableZeroCopy() override
    {
        if (mZeroCopy.exchange(false)) {
            printf("drift: video decode: zero-copy import failed; "
                   "transferring frames instead\n");
        }
    }

private:
    static constexpr size_t kMaxQueued = 4;

    struct Item {
        VideoFrame frame;
        double pts = 0.0;
        AVFrame* hwFrame = nullptr; // keeps the decode surface alive
    };

    static void releaseItem(Item& item)
    {
        for (auto& plane : item.frame.planes) {
            if (plane.fd >= 0) {
                close(plane.fd);
            }
        }
        item.frame.planes.clear();
        if (item.hwFrame) {
            av_frame_free(&item.hwFrame);
        }
    }

    void decodeLoop()
    {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        mTransferFrame = av_frame_alloc();

        while (true) {
            {
                std::unique_lock lock(mMutex);
                // At non-looping EOS the thread idles here (mEof blocks the
                // predicate) until a restart rearms it or teardown.
                mConsumed.wait(lock, [this] {
                    return mStop || mRestart ||
                           (!mEof && mQueue.size() < kMaxQueued);
                });
                if (mStop) {
                    break;
                }
                if (mRestart) {
                    mRestart = false;
                    lock.unlock();
                    if (av_seek_frame(mFormat, mStream, 0,
                                      AVSEEK_FLAG_BACKWARD) < 0) {
                        finish(true);
                        break;
                    }
                    avcodec_flush_buffers(mCodec);
                    // Timestamps restart from zero, and so does the
                    // playback clock upstream (§9.2 restart).
                    mLoopOffset = 0.0;
                    mMaxPts = 0.0;
                    continue;
                }
            }

            const int ret = av_read_frame(mFormat, packet);
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(mCodec, nullptr); // flush
                receiveFrames(frame);
                if (mLoop &&
                    av_seek_frame(mFormat, mStream, 0,
                                  AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(mCodec);
                    // Timestamps restart; keep queue pts monotonic.
                    if (mDuration <= 0.0) {
                        mDuration = mMaxPts - mLoopOffset + mFrameSeconds;
                    }
                    mLoopOffset += mDuration;
                    continue;
                }
                finish(false);
                continue; // idle awaiting restart (§17.4 rearm) or stop
            }
            if (ret < 0) {
                finish(true);
                break;
            }
            if (packet->stream_index != mStream) {
                av_packet_unref(packet);
                continue;
            }
            if (avcodec_send_packet(mCodec, packet) < 0) {
                av_packet_unref(packet);
                finish(true);
                break;
            }
            av_packet_unref(packet);
            if (!receiveFrames(frame)) {
                finish(true);
                break;
            }
        }

        av_frame_free(&frame);
        av_frame_free(&mTransferFrame);
        av_packet_free(&packet);
    }

#ifdef __APPLE__
    // Exports a decoded VideoToolbox frame's IOSurface and queues it. The
    // AVFrame is cloned so the CVPixelBuffer (and with it the IOSurface)
    // isn't recycled while the consumer may still sample it.
    bool exportFrame(AVFrame* frame)
    {
        const auto pixelBuffer = (CVPixelBufferRef)frame->data[3];
        IOSurfaceRef surface =
            pixelBuffer ? CVPixelBufferGetIOSurface(pixelBuffer) : nullptr;
        if (!surface) {
            return false;
        }
        // NV12 only (like the dmabuf path); 10-bit and exotic formats fall
        // back to the transfer path.
        const OSType format = CVPixelBufferGetPixelFormatType(pixelBuffer);
        if (format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
            return false;
        }

        Item item;
        item.frame.ioSurface = surface;
        item.frame.surfaceId = IOSurfaceGetID(surface);
        item.frame.width = (uint32_t)frame->width;
        item.frame.height = (uint32_t)frame->height;
        item.frame.index = mNextIndex++;
        item.frame.bt709 = frame->colorspace == AVCOL_SPC_BT709 ||
                           (frame->colorspace == AVCOL_SPC_UNSPECIFIED &&
                            frame->height >= 720);
        item.frame.fullRange =
            format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
            frame->color_range == AVCOL_RANGE_JPEG;
        item.hwFrame = av_frame_clone(frame);

        const int64_t ts = frame->best_effort_timestamp;
        const double pts = ts != AV_NOPTS_VALUE ? ts * mTimeBase + mLoopOffset
                                                : mMaxPts + mFrameSeconds;
        item.pts = pts;
        mMaxPts = std::max(mMaxPts, pts);
        av_frame_unref(frame);

        std::unique_lock lock(mMutex);
        mQueue.push_back(std::move(item));
        mProduced.notify_all();
        return true;
    }
#endif

#ifdef __linux__
    // Exports a decoded VAAPI frame as dmabuf planes and queues it. The
    // AVFrame is cloned so the surface isn't recycled while the consumer may
    // still sample it.
    bool exportFrame(AVFrame* frame)
    {
        auto* deviceCtx = (AVHWDeviceContext*)mHwDevice->data;
        auto* vaCtx = (AVVAAPIDeviceContext*)deviceCtx->hwctx;
        const VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];

        if (vaSyncSurface(vaCtx->display, surface) != VA_STATUS_SUCCESS) {
            return false;
        }
        VADRMPRIMESurfaceDescriptor desc{};
        if (vaExportSurfaceHandle(vaCtx->display, surface,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY |
                                      VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                  &desc) != VA_STATUS_SUCCESS) {
            return false;
        }
        if (desc.fourcc != VA_FOURCC_NV12 || desc.num_layers != 2) {
            for (unsigned i = 0; i < desc.num_objects; ++i) {
                close(desc.objects[i].fd);
            }
            return false;
        }

        // Every exported object fd is ours to close. A layer's plane takes
        // ownership of its object's fd the first time; further layers on the
        // same object get a dup, and unreferenced objects close immediately,
        // so releaseItem can close every plane fd unconditionally.
        Item item;
        int claims[4] = {};
        for (unsigned i = 0; i < 2; ++i) {
            const auto& layer = desc.layers[i];
            const uint32_t objIdx = layer.object_index[0];
            const auto& object = desc.objects[objIdx];
            drift::core::VideoPlane plane;
            plane.fd = claims[objIdx]++ ? dup(object.fd) : object.fd;
            plane.offset = layer.offset[0];
            plane.stride = layer.pitch[0];
            plane.drmFormat = layer.drm_format;
            plane.modifier = object.drm_format_modifier;
            plane.width = (uint32_t)frame->width >> (i ? 1 : 0);
            plane.height = (uint32_t)frame->height >> (i ? 1 : 0);
            item.frame.planes.push_back(plane);
        }
        for (unsigned o = 0; o < desc.num_objects && o < 4; ++o) {
            if (!claims[o]) {
                close(desc.objects[o].fd);
            }
        }

        item.frame.surfaceId = surface;
        item.frame.width = (uint32_t)frame->width;
        item.frame.height = (uint32_t)frame->height;
        item.frame.index = mNextIndex++;
        item.frame.bt709 = frame->colorspace == AVCOL_SPC_BT709 ||
                           (frame->colorspace == AVCOL_SPC_UNSPECIFIED &&
                            frame->height >= 720);
        item.frame.fullRange = frame->color_range == AVCOL_RANGE_JPEG;
        item.hwFrame = av_frame_clone(frame);

        const int64_t ts = frame->best_effort_timestamp;
        const double pts = ts != AV_NOPTS_VALUE ? ts * mTimeBase + mLoopOffset
                                                : mMaxPts + mFrameSeconds;
        item.pts = pts;
        mMaxPts = std::max(mMaxPts, pts);
        av_frame_unref(frame);

        std::unique_lock lock(mMutex);
        mQueue.push_back(std::move(item));
        mProduced.notify_all();
        return true;
    }
#endif // __linux__

    bool receiveFrames(AVFrame* frame)
    {
        for (;;) {
            const int ret = avcodec_receive_frame(mCodec, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }

            // Zero-copy: export the decode surface — VAAPI as per-plane
            // dmabufs (the portable route: single-plane R8+RG88 imports
            // work even where tiled multiplanar NV12 does not, e.g.
            // NVIDIA), VideoToolbox as the frame's IOSurface. The consumer
            // imports it; a failed import flips mZeroCopy off and we fall
            // back to transferring.
#ifdef __APPLE__
            constexpr AVPixelFormat kZeroCopyFormat = AV_PIX_FMT_VIDEOTOOLBOX;
#else
            constexpr AVPixelFormat kZeroCopyFormat = AV_PIX_FMT_VAAPI;
#endif
            if (mZeroCopy && frame->format == kZeroCopyFormat) {
                if (exportFrame(frame)) {
                    continue;
                }
                disableZeroCopy();
            }

            // Hardware frames come back to system memory once (usually as
            // NV12) and then share the software conversion path.
            AVFrame* src = frame;
            if (mHwDevice && frame->format == mHwFormat) {
                av_frame_unref(mTransferFrame);
                if (av_hwframe_transfer_data(mTransferFrame, frame, 0) < 0) {
                    av_frame_unref(frame);
                    return false;
                }
                src = mTransferFrame;
            }

            mSws = sws_getCachedContext(
                mSws, src->width, src->height, (AVPixelFormat)src->format,
                src->width, src->height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (!mSws) {
                av_frame_unref(frame);
                return false;
            }

            Item item;
            item.frame.width = (uint32_t)src->width;
            item.frame.height = (uint32_t)src->height;
            item.frame.index = mNextIndex++;
            item.frame.rgba.resize((size_t)src->width * src->height * 4);
            uint8_t* dst[4] = { item.frame.rgba.data() };
            int dstStride[4] = { src->width * 4 };
            sws_scale(mSws, src->data, src->linesize, 0, src->height, dst,
                      dstStride);
            if (src == mTransferFrame) {
                av_frame_unref(mTransferFrame);
            }

            const int64_t ts = frame->best_effort_timestamp;
            const double pts = ts != AV_NOPTS_VALUE
                                   ? ts * mTimeBase + mLoopOffset
                                   : mMaxPts + mFrameSeconds;
            item.pts = pts;
            mMaxPts = std::max(mMaxPts, pts);
            av_frame_unref(frame);

            std::unique_lock lock(mMutex);
            mQueue.push_back(std::move(item));
            mProduced.notify_all();
            // Over-fill within one packet is fine; the top of decodeLoop
            // waits for room before reading the next one.
        }
    }

    void finish(bool error)
    {
        std::lock_guard lock(mMutex);
        (error ? mError : mEof) = true;
        mProduced.notify_all();
    }

    AVFormatContext* mFormat = nullptr;
    AVCodecContext* mCodec = nullptr;
    SwsContext* mSws = nullptr;
    AVBufferRef* mHwDevice = nullptr;
    AVPixelFormat mHwFormat = AV_PIX_FMT_NONE;
    AVFrame* mTransferFrame = nullptr; // decode thread only
    int mStream = -1;
    bool mLoop = true;
    double mTimeBase = 0.0;
    double mFrameSeconds = 1.0 / 30.0;
    double mDuration = 0.0;   // container duration; measured at first EOF if absent
    double mLoopOffset = 0.0; // decode thread only
    double mMaxPts = 0.0;     // decode thread only
    int64_t mNextIndex = 0;   // decode thread only

    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mProduced, mConsumed;
    std::deque<Item> mQueue;
    bool mEof = false, mError = false, mStop = false, mRestart = false;

    Item mCurrent; // render thread only (released under the lock)
    bool mHaveCurrent = false;
    std::atomic<bool> mZeroCopy{ false };
};

} // namespace

std::unique_ptr<drift::core::VideoDecoder> createFFmpegVideoDecoder(
    const std::string& path, bool loop, std::string& error)
{
    auto decoder = std::make_unique<FFmpegVideoDecoder>();
    if (!decoder->open(path, loop, error)) {
        return nullptr;
    }
    return decoder;
}

} // namespace drift::platform
