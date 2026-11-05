#include "mx_noise.h"
#include <cstdio>
using namespace mxn;
int main() {
    for (int i = 0; i < 2000; ++i) {
        const float x = float(((i * 7919) % 20011) - 10005) / 128.0f + 0.001953125f;
        const float y = float(((i * 6151) % 20011) - 10005) / 128.0f + 0.001953125f;
        const float z = float(((i * 3571) % 20011) - 10005) / 128.0f + 0.001953125f;
        const V2 p2{x, y};
        const V3 p3{x, y, z};
        const float pf2 = perlin(p2);
        const V3 pv2 = perlinV3(p2);
        const float pf3 = perlin(p3);
        const V3 pv3 = perlinV3(p3);
        const float c2 = cell(p2), c3 = cell(p3);
        const V3 cv3 = cellV3(p3);
        float d2[3], d3[3], d3m[3];
        V2 m2; V3 m3;
        worley2(p2, 1.0f, 0, d2, m2);
        worley3(p3, 0.75f, 0, d3, m3);
        worley3(p3, 1.0f, 2, d3m, m3);
        const float fr = fractal(V3{p3.x * 0.1f, p3.y * 0.1f, p3.z * 0.1f}, 3, 2.0f, 0.5f);
        std::printf("%d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
                    i, pf2, pv2.x, pv2.y, pv2.z, pf3, pv3.x, pv3.y, pv3.z, c2, c3, cv3.x, cv3.y, cv3.z, d2[0], d3[0],
                    d3[1], d3[2], d3m[0], d3m[1], d3m[2], fr, 0.0);
    }
}
