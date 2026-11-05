// Check of the ray-cone -> texture-derivative math proposed for Nova.
#include <cmath>
#include <cstdio>
#include <initializer_list>

struct V3 { float x, y, z; };
struct V2 { float x, y; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
static V3 nrm(V3 a) { return a * (1.0f / len(a)); }

// World-space gradients of u and v over the triangle plane.
static bool uvGradients(V3 p0, V3 p1, V3 p2, V2 t0, V2 t1, V2 t2, V3 &dudP, V3 &dvdP) {
    const V3 e1 = p1 - p0, e2 = p2 - p0, n = cross(e1, e2);
    const float nn = dot(n, n);
    if (!(nn > 0.0f)) return false;
    const V3 g1 = cross(e2, n) * (1.0f / nn), g2 = cross(n, e1) * (1.0f / nn);
    const V2 d1 = t1 - t0, d2 = t2 - t0;
    dudP = g1 * d1.x + g2 * d2.x;
    dvdP = g1 * d1.y + g2 * d2.y;
    return true;
}

// Footprint ellipse axes of a cone of width w (diameter) along unit dir d on a
// plane with unit normal f (JCGT 2021 eqs. 8-9, diameter instead of radius).
static void coneAxes(V3 d, V3 f, float w, V3 &a1, V3 &a2) {
    V3 h1 = d - f * dot(f, d);
    float l = len(h1);
    if (l < 1e-6f) { // normal incidence: any tangent
        h1 = std::fabs(f.x) < 0.9f ? nrm(cross(f, {1, 0, 0})) : nrm(cross(f, {0, 1, 0}));
    } else h1 = h1 * (1.0f / l);
    const float cosT = std::fmax(std::fabs(dot(f, d)), 1e-4f);
    a1 = h1 * (w / cosT);
    a2 = cross(f, h1) * w;
}

int main() {
    // Plane y=0, 2x2 world units carrying uv [0,1]^2 (texture 1024^2 -> 512 texels per unit).
    V3 p0{-1, 0, 1}, p1{1, 0, 1}, p2{1, 0, -1};
    V2 t0{0, 0}, t1{1, 0}, t2{1, 1};
    V3 du, dv;
    uvGradients(p0, p1, p2, t0, t1, t2, du, dv);
    std::printf("dudP=(%.3f %.3f %.3f) dvdP=(%.3f %.3f %.3f)  (expect (0.5,0,0) and (0,0,-0.5))\n", du.x, du.y, du.z, dv.x, dv.y, dv.z);
    const float spread = std::atan(2.0f * std::tan(0.5f * 0.7f) / 512.0f); // fovY 0.7 rad, 512 px
    for (float deg : {0.f, 60.f, 85.f}) {
        const float th = deg * 3.14159265f / 180.f;
        const V3 d{0, -std::cos(th), -std::sin(th)}; // ray going down and forward
        const float t = 4.0f;
        const float W = spread * t;                   // camera ray: width0 = 0
        V3 a1, a2;
        coneAxes(d, {0, 1, 0}, W, a1, a2);
        const float dsdx = dot(du, a1) * 1024, dtdx = dot(dv, a1) * 1024, dsdy = dot(du, a2) * 1024, dtdy = dot(dv, a2) * 1024;
        const float major = std::hypot(dsdx, dtdx), minor = std::hypot(dsdy, dtdy);
        std::printf("theta=%2.0f deg: W=%.5f  texel derivs x=(%.3f,%.3f) y=(%.3f,%.3f) major=%.3f minor=%.3f expected major=%.3f minor=%.3f\n",
                    deg, W, dsdx, dtdx, dsdy, dtdy, major, minor, W * 512 / std::cos(th), W * 512);
    }
    return 0;
}
