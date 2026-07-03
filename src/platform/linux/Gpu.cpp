#include "Gpu.h"

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>

namespace drift::platform {

Gpu::~Gpu()
{
    mDevice = nullptr;
    mAdapter = nullptr;
    if (mGbm) {
        gbm_device_destroy(mGbm);
    }
    if (mDrmFd >= 0) {
        close(mDrmFd);
    }
}

bool Gpu::init(bool needPresent)
{
    if (needPresent) {
        // TODO: enumerate render nodes / pick the node matching the adapter.
        const char* node = "/dev/dri/renderD128";
        mDrmFd = open(node, O_RDWR | O_CLOEXEC);
        if (mDrmFd < 0) {
            fprintf(stderr, "drift: cannot open %s\n", node);
            return false;
        }
        mGbm = gbm_create_device(mDrmFd);
        if (!mGbm) {
            fprintf(stderr, "drift: gbm_create_device failed\n");
            return false;
        }
    }

    wgpu::RequestAdapterOptions opts{};
    opts.backendType = wgpu::BackendType::Vulkan;
    opts.featureLevel = wgpu::FeatureLevel::Core;
    auto adapters = mInstance.EnumerateAdapters(
        reinterpret_cast<const WGPURequestAdapterOptions*>(&opts));
    if (adapters.empty()) {
        fprintf(stderr, "drift: no Vulkan adapter\n");
        return false;
    }
    mAdapter = wgpu::Adapter(adapters[0].Get());

    std::vector<wgpu::FeatureName> features;
    if (needPresent) {
        features = {
            wgpu::FeatureName::SharedTextureMemoryDmaBuf,
            wgpu::FeatureName::SharedFenceSyncFD,
            wgpu::FeatureName::DawnDrmFormatCapabilities,
        };
        for (auto f : features) {
            if (!mAdapter.HasFeature(f)) {
                fprintf(stderr, "drift: adapter missing required feature %d\n", (int)f);
                return false;
            }
        }
    }

    wgpu::DeviceDescriptor dd{};
    dd.requiredFeatures = features.data();
    dd.requiredFeatureCount = features.size();
    dd.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            fprintf(stderr, "drift: [dawn error %d] %.*s\n",
                    (int)type, (int)msg.length, msg.data);
        });
    dd.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            if (reason != wgpu::DeviceLostReason::Destroyed) {
                fprintf(stderr, "drift: [dawn device lost %d] %.*s\n",
                        (int)reason, (int)msg.length, msg.data);
            }
        });
    mDevice = mAdapter.CreateDevice(&dd);
    if (!mDevice) {
        fprintf(stderr, "drift: CreateDevice failed\n");
        return false;
    }
    return true;
}

std::vector<uint64_t> Gpu::importableModifiers(wgpu::TextureFormat format)
{
    wgpu::DawnDrmFormatCapabilities drm{};
    wgpu::DawnFormatCapabilities caps{};
    caps.nextInChain = &drm;
    std::vector<uint64_t> result;
    if (mAdapter.GetFormatCapabilities(format, &caps) != wgpu::Status::Success) {
        return result;
    }
    for (size_t i = 0; i < drm.propertiesCount; ++i) {
        result.push_back(drm.properties[i].modifier);
    }
    return result;
}

bool Gpu::createTarget(uint32_t width, uint32_t height, uint32_t fourcc,
                       const std::vector<uint64_t>& modifiers, DmabufTarget& out)
{
    if (modifiers.empty()) {
        return false;
    }
    gbm_bo* bo = gbm_bo_create_with_modifiers(mGbm, width, height, fourcc,
                                              modifiers.data(),
                                              (unsigned)modifiers.size());
    if (!bo) {
        return false;
    }
    int fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        gbm_bo_destroy(bo);
        return false;
    }

    out.bo = bo;
    out.fd = fd;
    out.width = width;
    out.height = height;
    out.fourcc = fourcc;
    out.modifier = gbm_bo_get_modifier(bo);
    out.everInitialized = false;

    const int planeCount = gbm_bo_get_plane_count(bo);
    out.planes.clear();
    for (int i = 0; i < planeCount; ++i) {
        out.planes.push_back({ gbm_bo_get_offset(bo, i),
                               gbm_bo_get_stride_for_plane(bo, i) });
    }

    std::vector<wgpu::SharedTextureMemoryDmaBufPlane> planes;
    for (const auto& p : out.planes) {
        wgpu::SharedTextureMemoryDmaBufPlane plane{};
        plane.fd = fd;
        plane.offset = p.offset;
        plane.stride = p.stride;
        planes.push_back(plane);
    }

    wgpu::SharedTextureMemoryDmaBufDescriptor dmaDesc{};
    dmaDesc.size = { width, height, 1 };
    dmaDesc.drmFormat = fourcc;
    dmaDesc.drmModifier = out.modifier;
    dmaDesc.planeCount = planes.size();
    dmaDesc.planes = planes.data();

    wgpu::SharedTextureMemoryDescriptor stmDesc{};
    stmDesc.nextInChain = &dmaDesc;
    out.memory = mDevice.ImportSharedTextureMemory(&stmDesc);
    if (!out.memory) {
        destroyTarget(out);
        return false;
    }

    wgpu::SharedTextureMemoryProperties props{};
    out.memory.GetProperties(&props);

    wgpu::TextureDescriptor td{};
    td.format = props.format;
    td.size = props.size;
    td.usage = wgpu::TextureUsage::RenderAttachment;
    out.texture = out.memory.CreateTexture(&td);
    if (!out.texture) {
        destroyTarget(out);
        return false;
    }
    return true;
}

void Gpu::destroyTarget(DmabufTarget& target)
{
    target.texture = nullptr;
    target.memory = nullptr;
    if (target.fd >= 0) {
        close(target.fd);
        target.fd = -1;
    }
    if (target.bo) {
        gbm_bo_destroy(target.bo);
        target.bo = nullptr;
    }
    target.planes.clear();
}

void Gpu::beginFrame(DmabufTarget& target)
{
    // Vulkan backend requires image-layout tracking chained in.
    // VK_IMAGE_LAYOUT_UNDEFINED=0, VK_IMAGE_LAYOUT_GENERAL=1.
    wgpu::SharedTextureMemoryVkImageLayoutBeginState vkBegin{};
    vkBegin.oldLayout = target.everInitialized ? 1 : 0;
    vkBegin.newLayout = 1;
    wgpu::SharedTextureMemoryBeginAccessDescriptor ba{};
    ba.nextInChain = &vkBegin;
    ba.initialized = target.everInitialized;
    ba.concurrentRead = false;
    ba.fenceCount = 0;
    target.memory.BeginAccess(target.texture, &ba);
}

void Gpu::endFrame(DmabufTarget& target)
{
    wgpu::SharedTextureMemoryVkImageLayoutEndState vkEnd{};
    wgpu::SharedTextureMemoryEndAccessState end{};
    end.nextInChain = &vkEnd;
    target.memory.EndAccess(target.texture, &end);
    target.everInitialized = true;

    // Scaffold sync: block until the GPU is done before the compositor sees
    // the buffer. TODO sync-fd export instead.
    waitForQueue();
}

void Gpu::waitForQueue()
{
    bool done = false;
    mDevice.GetQueue().OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; });
    while (!done) {
        dawn::native::InstanceProcessEvents(mInstance.Get());
        if (!done) {
            usleep(200);
        }
    }
}

void Gpu::processEvents()
{
    dawn::native::InstanceProcessEvents(mInstance.Get());
}

} // namespace drift::platform
