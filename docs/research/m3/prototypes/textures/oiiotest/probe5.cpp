// Fifth probe: extract a 3x3 matrix from an OCIO transform and detect
// non-linear transforms (decides post-lookup matrix vs baked conversion).
#include <OpenImageIO/color.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace OIIO;

static bool linearMatrix(const ColorConfig &cc, const char *from, float m[3][3]) {
    ColorProcessorHandle p = cc.createColorProcessor(from, "scene_linear");
    if (!p) return false;
    float basis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int c = 0; c < 3; ++c) {
        p->apply(basis[c]);
        for (int r = 0; r < 3; ++r) m[r][c] = basis[c][r];
    }
    // Linearity: f(a x) == a f(x) and f(x + y) == f(x) + f(y) on test colours.
    const float probes[3][3] = {{0.18f, 0.5f, 0.02f}, {4.0f, 0.25f, 1.5f}, {0.001f, 0.003f, 0.0005f}};
    for (const auto &x : probes) {
        float y[3] = {x[0], x[1], x[2]};
        p->apply(y);
        for (int r = 0; r < 3; ++r) {
            const float lin = m[r][0] * x[0] + m[r][1] * x[1] + m[r][2] * x[2];
            if (std::fabs(lin - y[r]) > 1e-4f * (1.0f + std::fabs(y[r]))) return false;
        }
    }
    return true;
}

int main() {
    _putenv_s("OCIO", "C:/Program Files/Side Effects Software/Houdini 22.0.466/packages/ocio/houdini-config-v3.0.0_aces-v2.0_ocio-v2.5.ocio");
    ColorConfig cc;
    std::printf("config: %s\n", cc.configname().c_str());
    for (const char *cs : {"lin_rec709", "ACEScg", "acescg", "lin_ap0", "lin_p3d65", "srgb_texture", "sRGB", "g22_rec709", "raw", "ACEScct", "Linear Rec.2020"}) {
        float m[3][3];
        const bool lin = linearMatrix(cc, cs, m);
        std::printf("%-16s resolve='%s' isColorSpaceLinear=%d matrix-linear=%d", cs, std::string(cc.resolve(cs)).c_str(),
                    cc.isColorSpaceLinear(cs), lin);
        if (lin)
            std::printf("  M=[%.4f %.4f %.4f; %.4f %.4f %.4f; %.4f %.4f %.4f]", m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
        std::printf("\n");
    }
    for (const char *f : {"a/b/diffuse.png", "a/b/diffuse.tx", "a/b/diffuse_ACEScg.exr", "a/b/foo.rat", "a/b/foo.hdr", "a/b/foo_srgb_texture.tx", "a/b/foo.1001.exr"})
        std::printf("file rule %-26s -> '%s'\n", f, std::string(cc.getColorSpaceFromFilepath(f)).c_str());
    return 0;
}
