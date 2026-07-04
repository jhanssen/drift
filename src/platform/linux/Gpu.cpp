#include "Gpu.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gbm.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

namespace drift::platform {

namespace {

std::string lowered(const char* s, size_t len)
{
    std::string out(s, len);
    for (char& c : out) {
        c = (char)tolower((unsigned char)c);
    }
    return out;
}

std::string adapterName(const wgpu::Adapter& adapter)
{
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    return std::string(info.device.data, info.device.length);
}

// The DRM render node backing the chosen adapter (matched by PCI ids), so
// GBM allocates on the GPU Dawn renders with. Falls back to the first
// render node (e.g. software rasterizers have no PCI identity).
std::string renderNodeForAdapter(const wgpu::Adapter& adapter)
{
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);

    std::string fallback;
    drmDevicePtr devices[16] = {};
    const int count = drmGetDevices2(0, devices, 16);
    for (int i = 0; i < count; ++i) {
        const drmDevicePtr dev = devices[i];
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER))) {
            continue;
        }
        if (fallback.empty()) {
            fallback = dev->nodes[DRM_NODE_RENDER];
        }
        if (dev->bustype == DRM_BUS_PCI &&
            dev->deviceinfo.pci->vendor_id == info.vendorID &&
            dev->deviceinfo.pci->device_id == info.deviceID) {
            std::string node = dev->nodes[DRM_NODE_RENDER];
            drmFreeDevices(devices, count);
            return node;
        }
    }
    if (count > 0) {
        drmFreeDevices(devices, count);
    }
    return fallback.empty() ? "/dev/dri/renderD128" : fallback;
}

} // namespace

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
    wgpu::RequestAdapterOptions opts{};
    opts.backendType = wgpu::BackendType::Vulkan;
    opts.featureLevel = wgpu::FeatureLevel::Core;
    auto adapters = mInstance.EnumerateAdapters(
        reinterpret_cast<const WGPURequestAdapterOptions*>(&opts));
    if (adapters.empty()) {
        fprintf(stderr, "drift: no Vulkan adapter\n");
        return false;
    }
    if (const char* want = getenv("DRIFT_ADAPTER"); want && *want) {
        const std::string needle = lowered(want, strlen(want));
        for (const auto& a : adapters) {
            wgpu::Adapter candidate(a.Get());
            const std::string name = adapterName(candidate);
            if (lowered(name.data(), name.size()).find(needle) != std::string::npos) {
                mAdapter = candidate;
                break;
            }
        }
        if (!mAdapter) {
            fprintf(stderr, "drift: no adapter matching DRIFT_ADAPTER='%s'; have:\n", want);
            for (const auto& a : adapters) {
                fprintf(stderr, "  %s\n", adapterName(wgpu::Adapter(a.Get())).c_str());
            }
            return false;
        }
    } else {
        mAdapter = wgpu::Adapter(adapters[0].Get());
    }
    printf("drift: adapter: %s\n", adapterName(mAdapter).c_str());

    if (needPresent) {
        const std::string node = renderNodeForAdapter(mAdapter);
        mDrmFd = open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (mDrmFd < 0) {
            fprintf(stderr, "drift: cannot open %s\n", node.c_str());
            return false;
        }
        mGbm = gbm_create_device(mDrmFd);
        if (!mGbm) {
            fprintf(stderr, "drift: gbm_create_device failed\n");
            return false;
        }
        printf("drift: render node: %s\n", node.c_str());
    }

    // BC texture compression carries the KTX2 path; present on all desktop
    // Vulkan drivers including lavapipe.
    std::vector<wgpu::FeatureName> features = {
        wgpu::FeatureName::TextureCompressionBC,
    };
    if (needPresent) {
        features.push_back(wgpu::FeatureName::SharedTextureMemoryDmaBuf);
        features.push_back(wgpu::FeatureName::SharedFenceSyncFD);
        features.push_back(wgpu::FeatureName::DawnDrmFormatCapabilities);
    }
    for (auto f : features) {
        if (!mAdapter.HasFeature(f)) {
            fprintf(stderr, "drift: adapter missing required feature %d\n", (int)f);
            return false;
        }
    }

    wgpu::DeviceDescriptor dd{};
    dd.requiredFeatures = features.data();
    dd.requiredFeatureCount = features.size();
    dd.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg,
           Gpu* self) {
            self->mHasError = true;
            fprintf(stderr, "drift: [dawn error %d] %.*s\n",
                    (int)type, (int)msg.length, msg.data);
        },
        this);
    dd.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason,
           wgpu::StringView msg, Gpu* self) {
            if (reason != wgpu::DeviceLostReason::Destroyed) {
                self->mHasError = true;
                fprintf(stderr, "drift: [dawn device lost %d] %.*s\n",
                        (int)reason, (int)msg.length, msg.data);
            }
        },
        this);
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

void Gpu::endFrame(DmabufTarget& target, bool synchronize)
{
    wgpu::SharedTextureMemoryVkImageLayoutEndState vkEnd{};
    wgpu::SharedTextureMemoryEndAccessState end{};
    end.nextInChain = &vkEnd;
    target.memory.EndAccess(target.texture, &end);
    target.everInitialized = true;

    // The buffer is only handed to the compositor when something was
    // rendered into it; otherwise no synchronization is needed at all
    // (this is also why zero fences below isn't a capability failure).
    if (!synchronize) {
        return;
    }

    // Export EndAccess fences as sync files and attach them to the dmabuf's
    // reservation, so the compositor's implicit sync waits on the GPU
    // instead of us blocking the render thread.
    bool synced = !mSyncFdBroken && end.fenceCount > 0;
    const char* why = "";
    for (size_t i = 0; synced && i < end.fenceCount; ++i) {
        // The exported handle stays owned by the SharedFence (freed with the
        // EndAccessState); the import ioctl only reads the fence from it.
        wgpu::SharedFenceSyncFDExportInfo syncFd{};
        wgpu::SharedFenceExportInfo info{};
        info.nextInChain = &syncFd;
        end.fences[i].ExportInfo(&info);
        if (info.type != wgpu::SharedFenceType::SyncFD || syncFd.handle < 0) {
            synced = false;
            why = "fence is not a sync file";
            break;
        }
        dma_buf_import_sync_file arg{};
        arg.flags = DMA_BUF_SYNC_WRITE;
        arg.fd = syncFd.handle;
        if (ioctl(target.fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) != 0) {
            synced = false;
            why = strerror(errno);
        }
    }
    if (!synced) {
        // Zero fences with work submitted shouldn't happen; wait but don't
        // write the capability off. Export/import failures are permanent.
        if (*why && !mSyncFdBroken) {
            mSyncFdBroken = true;
            fprintf(stderr,
                    "drift: sync-fd export unavailable (%s); falling back to "
                    "CPU sync\n", why);
        }
        waitForQueue();
    }
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
