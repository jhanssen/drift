// Fuzzy image comparison for golden tests. Compares two images per channel
// against a tolerance, allowing a bounded fraction of outlier pixels —
// goldens are rendered on lavapipe, but local runs on real GPUs legitimately
// differ by a few LSBs (sin() precision, filtering, rgba16float rounding).
//
// usage: imgcmp a.png b.png [--max-diff N] [--max-frac F]
//   --max-diff N   per-channel difference (0-255) a pixel may have before it
//                  counts as differing (default 3)
//   --max-frac F   fraction of pixels allowed to differ (default 0.001)
//
// exit: 0 match, 1 mismatch, 2 usage/io error.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

int main(int argc, char** argv)
{
    const char* pathA = nullptr;
    const char* pathB = nullptr;
    int maxDiff = 3;
    double maxFrac = 0.001;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--max-diff") && i + 1 < argc) {
            maxDiff = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-frac") && i + 1 < argc) {
            maxFrac = atof(argv[++i]);
        } else if (!pathA) {
            pathA = argv[i];
        } else if (!pathB) {
            pathB = argv[i];
        } else {
            fprintf(stderr, "usage: %s a.png b.png [--max-diff N] [--max-frac F]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!pathA || !pathB) {
        fprintf(stderr, "usage: %s a.png b.png [--max-diff N] [--max-frac F]\n",
                argv[0]);
        return 2;
    }

    int wa, ha, wb, hb, comp;
    unsigned char* a = stbi_load(pathA, &wa, &ha, &comp, 4);
    if (!a) {
        fprintf(stderr, "imgcmp: cannot load %s\n", pathA);
        return 2;
    }
    unsigned char* b = stbi_load(pathB, &wb, &hb, &comp, 4);
    if (!b) {
        fprintf(stderr, "imgcmp: cannot load %s\n", pathB);
        stbi_image_free(a);
        return 2;
    }

    if (wa != wb || ha != hb) {
        fprintf(stderr, "imgcmp: size mismatch %dx%d vs %dx%d\n", wa, ha, wb, hb);
        stbi_image_free(a);
        stbi_image_free(b);
        return 1;
    }

    const size_t pixels = (size_t)wa * ha;
    size_t differing = 0;
    int worst = 0;
    for (size_t i = 0; i < pixels; ++i) {
        int pixelWorst = 0;
        for (int c = 0; c < 4; ++c) {
            const int d = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            if (d > pixelWorst) {
                pixelWorst = d;
            }
        }
        if (pixelWorst > maxDiff) {
            ++differing;
        }
        if (pixelWorst > worst) {
            worst = pixelWorst;
        }
    }
    stbi_image_free(a);
    stbi_image_free(b);

    const double frac = (double)differing / (double)pixels;
    if (frac > maxFrac) {
        fprintf(stderr,
                "imgcmp: MISMATCH %s vs %s: %zu/%zu pixels differ beyond %d "
                "(%.4f%% > %.4f%%), worst channel delta %d\n",
                pathA, pathB, differing, pixels, maxDiff, frac * 100.0,
                maxFrac * 100.0, worst);
        return 1;
    }
    printf("imgcmp: ok %s vs %s (%zu/%zu outliers, worst delta %d)\n", pathA,
           pathB, differing, pixels, worst);
    return 0;
}
