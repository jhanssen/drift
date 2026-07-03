#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "core/Renderer.h"
#include "platform/linux/Gpu.h"
#include "platform/linux/WaylandApp.h"

namespace {

void usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  (default)          run as wallpaper (wlr-layer-shell background)\n"
            "  -w, --windowed     run in a regular window (dev mode)\n"
            "      --headless N   render N frames offscreen and write PNGs\n"
            "      --size WxH     initial window size / headless resolution\n"
            "                     (default 1280x720 windowed, 1920x1080 headless)\n"
            "      --out DIR      output directory for --headless (default .)\n"
            "  -h, --help         show this help\n",
            argv0);
}

int runHeadless(int frames, uint32_t width, uint32_t height, const std::string& outDir)
{
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/false)) {
        return 1;
    }

    const wgpu::Device& device = gpu.device();
    const wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;

    wgpu::TextureDescriptor td{};
    td.format = format;
    td.size = { width, height, 1 };
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    wgpu::Texture texture = device.CreateTexture(&td);

    drift::core::Renderer renderer;
    if (!renderer.init(device, format)) {
        fprintf(stderr, "drift: renderer init failed\n");
        return 1;
    }

    const uint32_t bytesPerRow = ((width * 4) + 255) & ~255u; // 256-align
    wgpu::BufferDescriptor bd{};
    bd.size = (uint64_t)bytesPerRow * height;
    bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer readback = device.CreateBuffer(&bd);

    for (int i = 0; i < frames; ++i) {
        renderer.render(texture.CreateView(), (float)i / 60.0f);

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::TexelCopyTextureInfo src{};
        src.texture = texture;
        wgpu::TexelCopyBufferInfo dst{};
        dst.buffer = readback;
        dst.layout.bytesPerRow = bytesPerRow;
        dst.layout.rowsPerImage = height;
        wgpu::Extent3D extent = { width, height, 1 };
        encoder.CopyTextureToBuffer(&src, &dst, &extent);
        wgpu::CommandBuffer commands = encoder.Finish();
        device.GetQueue().Submit(1, &commands);

        bool mapped = false, ok = false;
        readback.MapAsync(wgpu::MapMode::Read, 0, bd.size,
                          wgpu::CallbackMode::AllowProcessEvents,
                          [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                              mapped = true;
                              ok = (status == wgpu::MapAsyncStatus::Success);
                          });
        while (!mapped) {
            gpu.processEvents();
        }
        if (!ok) {
            fprintf(stderr, "drift: readback map failed\n");
            return 1;
        }

        const uint8_t* pixels =
            (const uint8_t*)readback.GetConstMappedRange(0, bd.size);
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%04d.png", outDir.c_str(), i);
        if (!stbi_write_png(path, (int)width, (int)height, 4, pixels,
                            (int)bytesPerRow)) {
            fprintf(stderr, "drift: failed to write %s\n", path);
            return 1;
        }
        readback.Unmap();
        printf("drift: wrote %s\n", path);
    }
    return 0;
}

int runWayland(drift::platform::SurfaceMode mode, uint32_t width, uint32_t height)
{
    drift::platform::Gpu gpu;
    if (!gpu.init(/*needPresent=*/true)) {
        return 1;
    }

    drift::platform::WaylandApp app;
    if (!app.setup(gpu, mode, width, height)) {
        return 1;
    }

    drift::core::Renderer renderer;
    if (!renderer.init(gpu.device(), app.targetFormat())) {
        fprintf(stderr, "drift: renderer init failed\n");
        return 1;
    }

    return app.run([&renderer](const wgpu::TextureView& target, float t) {
        renderer.render(target, t);
    });
}

} // namespace

int main(int argc, char** argv)
{
    bool windowed = false;
    int headlessFrames = -1;
    uint32_t width = 0, height = 0;
    std::string outDir = ".";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!strcmp(arg, "-w") || !strcmp(arg, "--windowed")) {
            windowed = true;
        } else if (!strcmp(arg, "--headless") && i + 1 < argc) {
            headlessFrames = atoi(argv[++i]);
        } else if (!strcmp(arg, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(arg, "--out") && i + 1 < argc) {
            outDir = argv[++i];
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (headlessFrames >= 0) {
        if (width == 0) { width = 1920; height = 1080; }
        return runHeadless(headlessFrames, width, height, outDir);
    }
    if (width == 0) { width = 1280; height = 720; }
    return runWayland(windowed ? drift::platform::SurfaceMode::Windowed
                               : drift::platform::SurfaceMode::Wallpaper,
                      width, height);
}
