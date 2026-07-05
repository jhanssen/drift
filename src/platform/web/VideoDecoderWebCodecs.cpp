// See VideoDecoderWebCodecs.h for the design. Ownership split: C++ keeps the
// file bytes and the demuxed sample table alive (JS reads both straight out
// of the wasm heap); JS owns the VideoDecoder, the decoded-frame queue, and
// the capture canvas, keyed by decoder id in Module.driftWc.

#include "VideoDecoderWebCodecs.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

#include <emscripten/emscripten.h>

#include "core/Mp4Demux.h"

namespace {

// Table pointers stay valid for the decoder's lifetime; sizes/pts/keys are
// parallel arrays in decode order. lastPts identifies the final displayed
// frame of a non-looping stream (§17.4 'finished').
EM_JS(int, wcOpen, (const char* codec, const uint8_t* desc, int descLen,
                    int width, int height, int loop, const uint8_t* file,
                    const double* offsets, const uint32_t* sizes,
                    const double* pts, const uint8_t* keys, int count,
                    double duration, double lastPts), {
  // ||= not ??=: the latter spells a C trigraph inside EM_JS source.
  const decoders = Module.driftWc ||
      (Module.driftWc = { next: 1, map: new Map() });
  const state = {
    codec: UTF8ToString(codec),
    width, height, loop: !!loop, file, offsets, sizes, pts, keys, count,
    duration, lastPts,
    queue: [], pending: null, next: 0, loopBase: 0, fedMax: -1, picked: 0,
    flushing: false, eosFlushed: false, dead: false,
    canvas: null, ctx2d: null, decoder: null, config: null,
  };
  state.config = { codec: state.codec, codedWidth: width, codedHeight: height };
  if (descLen > 0) {
    state.config.description = HEAPU8.slice(desc, desc + descLen);
  }
  const die = (why) => {
    if (!state.dead) {
      state.dead = true;
      console.error(`drift: video: ${why}`);
    }
  };
  state.decoder = new VideoDecoder({
    output: (frame) => state.queue.push(frame),
    error: (e) => die(`decode error (${state.codec}): ${e.message}`),
  });
  if (typeof VideoDecoder.isConfigSupported === 'function') {
    VideoDecoder.isConfigSupported(state.config)
        .then((res) => {
          if (!res.supported) {
            die(`codec '${state.codec}' is not supported by this browser`);
          }
        })
        .catch((e) => die(`codec probe failed: ${e.message}`));
  }
  try {
    state.decoder.configure(state.config);
  } catch (e) {
    die(`configure failed (${state.codec}): ${e.message}`);
  }
  const id = decoders.next++;
  decoders.map.set(id, state);
  return id;
});

// The decode pump + timestamp picker. Returns 1 when a new frame is pending
// capture (dims filled), 0 when the newest suitable frame was already
// delivered (or decode has not caught up yet), -1 when the decoder is dead.
EM_JS(int, wcPoll, (int id, double seconds, uint32_t* dims), {
  const state = Module.driftWc?.map.get(id);
  if (!state || state.dead) {
    return -1;
  }
  const chunkAt = (i) => new EncodedVideoChunk({
    type: HEAPU8[state.keys + i] ? 'key' : 'delta',
    timestamp: Math.round(
        (state.loopBase + HEAPF64[(state.pts >> 3) + i]) * 1e6),
    data: HEAPU8.subarray(
        state.file + HEAPF64[(state.offsets >> 3) + i],
        state.file + HEAPF64[(state.offsets >> 3) + i] +
            HEAPU32[(state.sizes >> 2) + i]),
  });

  // A large forward jump (editor clock sync) would otherwise decode every
  // frame in between: reset and re-enter at the nearest keyframe instead.
  if (seconds > state.fedMax + 1.5 && state.fedMax >= 0) {
    for (const f of state.queue) {
      f.close();
    }
    state.queue = [];
    state.pending?.close();
    state.pending = null;
    state.decoder.reset(); // aborts the pending flush; state: unconfigured
    state.decoder.configure(state.config);
    const local = state.loop
        ? seconds % state.duration
        : Math.min(seconds, state.lastPts);
    state.loopBase = state.loop ? seconds - local : 0;
    let start = 0;
    for (let i = 0; i < state.count; i++) {
      if (HEAPU8[state.keys + i] &&
          HEAPF64[(state.pts >> 3) + i] <= local + 1e-6) {
        start = i;
      }
    }
    state.next = start;
    state.fedMax = -1;
    state.flushing = false;
    state.eosFlushed = false;
  }

  // Feed ahead until decode covers the requested time (plus margin) or the
  // in-flight window fills; wrap or flush at end of stream.
  const covered = () => state.queue.length &&
      state.queue[state.queue.length - 1].timestamp / 1e6 >= seconds + 0.2;
  while (state.next < state.count && !covered() &&
         state.decoder.decodeQueueSize < 8) {
    const i = state.next++;
    state.fedMax = state.loopBase + HEAPF64[(state.pts >> 3) + i];
    try {
      state.decoder.decode(chunkAt(i));
    } catch (e) {
      state.dead = true;
      console.error(`drift: video: decode() threw: ${e.message}`);
      return -1;
    }
  }
  if (state.next >= state.count && !state.flushing) {
    if (state.loop) {
      // Flush the tail of this lap, then continue seamlessly with the next
      // lap's timestamps; scene time keeps growing, so ours must too.
      state.flushing = true;
      state.decoder.flush().then(() => {
        state.flushing = false;
        state.loopBase += state.duration;
        state.next = 0;
      }).catch(() => {}); // reset() aborted us; the seek path re-arms
    } else if (!state.eosFlushed) {
      state.flushing = true;
      state.decoder.flush().then(() => {
        state.flushing = false;
        state.eosFlushed = true;
      }).catch(() => {});
    }
  }

  // Pick the newest decoded frame at or before the requested time.
  let picked = null;
  while (state.queue.length &&
         state.queue[0].timestamp / 1e6 <= seconds + 1e-6) {
    picked?.close();
    picked = state.queue.shift();
  }
  if (!picked) {
    return 0;
  }
  state.picked++; // observability: frames delivered over the lifetime
  state.pending?.close();
  state.pending = picked;
  HEAPU32[dims >> 2] = picked.displayWidth;
  HEAPU32[(dims >> 2) + 1] = picked.displayHeight;
  return 1;
});

// Converts the pending frame to sRGB RGBA via canvas (drawImage handles the
// YUV matrix/range) and releases it. meta[0] = last frame of a non-looping
// stream.
EM_JS(int, wcCapture, (int id, uint8_t* out, int w, int h, uint32_t* meta), {
  const state = Module.driftWc?.map.get(id);
  const frame = state?.pending;
  if (!frame) {
    return 0;
  }
  state.pending = null;
  if (!state.canvas || state.canvas.width !== w || state.canvas.height !== h) {
    state.canvas = new OffscreenCanvas(w, h);
    state.ctx2d = state.canvas.getContext('2d', { willReadFrequently: true });
  }
  state.ctx2d.drawImage(frame, 0, 0, w, h);
  const last = !state.loop &&
      Math.abs(frame.timestamp / 1e6 - state.lastPts) < 1e-3;
  frame.close();
  HEAPU8.set(state.ctx2d.getImageData(0, 0, w, h).data, out);
  HEAPU32[meta >> 2] = last ? 1 : 0;
  return 1;
});

// §9.2 restart: rewind to the first sample and rearm a finished stream.
EM_JS(void, wcRestart, (int id), {
  const state = Module.driftWc?.map.get(id);
  if (!state || state.dead) {
    return;
  }
  for (const f of state.queue) {
    f.close();
  }
  state.queue = [];
  state.pending?.close();
  state.pending = null;
  state.decoder.reset();
  state.decoder.configure(state.config);
  state.next = 0;
  state.loopBase = 0;
  state.fedMax = -1;
  state.flushing = false;
  state.eosFlushed = false;
});

EM_JS(void, wcClose, (int id), {
  const decoders = Module.driftWc;
  const state = decoders?.map.get(id);
  if (!state) {
    return;
  }
  for (const f of state.queue) {
    f.close();
  }
  state.pending?.close();
  try {
    state.decoder.close();
  } catch (e) {
  }
  decoders.map.delete(id);
});

class VideoDecoderWebCodecs final : public drift::core::VideoDecoder {
public:
    VideoDecoderWebCodecs(std::vector<uint8_t> file,
                          drift::core::Mp4Track track, bool loop)
        : mFile(std::move(file)), mTrack(std::move(track))
    {
        mOffsets.reserve(mTrack.samples.size());
        mSizes.reserve(mTrack.samples.size());
        mPts.reserve(mTrack.samples.size());
        mKeys.reserve(mTrack.samples.size());
        double lastPts = 0.0;
        for (const auto& s : mTrack.samples) {
            mOffsets.push_back((double)s.offset);
            mSizes.push_back(s.size);
            mPts.push_back(s.pts);
            mKeys.push_back(s.keyframe ? 1 : 0);
            lastPts = std::max(lastPts, s.pts);
        }
        mId = wcOpen(mTrack.codec.c_str(), mTrack.description.data(),
                     (int)mTrack.description.size(), (int)mTrack.width,
                     (int)mTrack.height, loop ? 1 : 0, mFile.data(),
                     mOffsets.data(), mSizes.data(), mPts.data(),
                     mKeys.data(), (int)mTrack.samples.size(),
                     mTrack.duration, lastPts);
    }

    ~VideoDecoderWebCodecs() override { wcClose(mId); }

    const drift::core::VideoFrame* frameAt(double seconds) override
    {
        uint32_t dims[2] = { 0, 0 };
        const int status = wcPoll(mId, seconds, dims);
        if (status < 0) {
            // Dead decoder (unsupported codec, hard decode error): deliver
            // one black frame so downstream has a valid texture — a black
            // layer plus the console error beats an invisible failure.
            if (mFrame.index < 0) {
                mFrame.width = mTrack.width;
                mFrame.height = mTrack.height;
                mFrame.rgba.assign(size_t{mTrack.width} * mTrack.height * 4, 0);
                for (size_t px = 3; px < mFrame.rgba.size(); px += 4) {
                    mFrame.rgba[px] = 255;
                }
                mFrame.index = ++mIndex;
            }
            return &mFrame;
        }
        if (status == 0 || dims[0] == 0 || dims[1] == 0) {
            return mFrame.index >= 0 ? &mFrame : nullptr;
        }
        mFrame.rgba.resize(size_t{dims[0]} * dims[1] * 4);
        uint32_t meta[1] = { 0 };
        if (wcCapture(mId, mFrame.rgba.data(), (int)dims[0], (int)dims[1],
                      meta) <= 0) {
            return mFrame.index >= 0 ? &mFrame : nullptr;
        }
        mFrame.width = dims[0];
        mFrame.height = dims[1];
        mFrame.index = ++mIndex;
        mFrame.last = meta[0] != 0;
        return &mFrame;
    }

    void restart() override
    {
        wcRestart(mId);
        // A held frame from before the restart must not re-fire 'finished'.
        mFrame.last = false;
    }

private:
    std::vector<uint8_t> mFile; // sample data referenced by JS via offsets
    drift::core::Mp4Track mTrack;
    std::vector<double> mOffsets;
    std::vector<uint32_t> mSizes;
    std::vector<double> mPts;
    std::vector<uint8_t> mKeys;
    drift::core::VideoFrame mFrame;
    int64_t mIndex = 0;
    int mId = 0;
};

} // namespace

namespace drift::web {

std::unique_ptr<core::VideoDecoder> createVideoDecoder(
    const std::string& absPath, bool loop, std::string& error)
{
    std::ifstream in(absPath, std::ios::binary);
    if (!in) {
        error = "cannot read video file";
        return nullptr;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string& bytes = ss.str();
    std::vector<uint8_t> file(bytes.begin(), bytes.end());

    core::Mp4Track track;
    if (!core::parseMp4(file.data(), file.size(), track, error)) {
        return nullptr;
    }
    return std::make_unique<VideoDecoderWebCodecs>(std::move(file),
                                                   std::move(track), loop);
}

} // namespace drift::web
