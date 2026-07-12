#include "Gpu.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

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

} // namespace

bool Gpu::init(bool /*needPresent*/)
{
    wgpu::RequestAdapterOptions opts{};
    opts.backendType = wgpu::BackendType::Metal;
    opts.featureLevel = wgpu::FeatureLevel::Core;
    auto adapters = mInstance.EnumerateAdapters(
        reinterpret_cast<const WGPURequestAdapterOptions*>(&opts));
    if (adapters.empty()) {
        fprintf(stderr, "drift: no Metal adapter\n");
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

    // BC texture compression carries the KTX2 path; present on every Metal
    // desktop GPU (Mac2 family and Apple Silicon alike).
    std::vector<wgpu::FeatureName> features = {
        wgpu::FeatureName::TextureCompressionBC,
    };
    for (auto f : features) {
        if (!mAdapter.HasFeature(f)) {
            fprintf(stderr, "drift: adapter missing required feature %d\n", (int)f);
            return false;
        }
    }
    // IOSurface interop backs zero-copy video decode; opportunistic — the
    // video path falls back to transferred frames without it. EndAccess on
    // an imported texture exports MTLSharedEvent fences, so that feature
    // rides along.
    const wgpu::FeatureName ioSurfaceFeatures[] = {
        wgpu::FeatureName::SharedTextureMemoryIOSurface,
        wgpu::FeatureName::DawnMultiPlanarFormats,
        wgpu::FeatureName::SharedFenceMTLSharedEvent,
    };
    for (auto f : ioSurfaceFeatures) {
        if (mAdapter.HasFeature(f)) {
            features.push_back(f);
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
