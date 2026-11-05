#include "core/geometry/displace.h"

#include "core/math/frame.h"
#include "core/scene/attributes.h"
#include "core/shading/vm.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

namespace sh = shading;

constexpr int kLanes = 32;

// The attribute `name` of the mesh, as a primvar read sees it.
AttributeRef attributeOf(const MeshDesc &mesh, const std::string &name) {
    AttributeRef ref;
    const MeshAttribute *a = mesh.findAttribute(name);
    if (!a || a->width < 1 || a->width > 4)
        return ref;
    const size_t need = a->rate == AttributeRate::Constant  ? 1
                        : a->rate == AttributeRate::Uniform ? mesh.triangleCount()
                        : a->rate == AttributeRate::Vertex  ? mesh.pointCount()
                                                            : mesh.indices.size();
    if (a->data.size() < need * a->width)
        return ref;
    ref.data = a->data.data();
    ref.width = a->width;
    ref.source = a->rate == AttributeRate::Constant  ? AttributeSource::Constant
                 : a->rate == AttributeRate::Uniform ? AttributeSource::Uniform
                 : a->rate == AttributeRate::Vertex  ? AttributeSource::Vertex
                                                     : AttributeSource::Corner;
    return ref;
}

// a.gs = 1, a.gt = 0, b.gs = 0, b.gt = 1 in the plane of gs and gt.
bool dualBasis(Vec3f gs, Vec3f gt, Vec3f &a, Vec3f &b) {
    const float ss = dot(gs, gs), tt = dot(gt, gt), st = dot(gs, gt), d = ss * tt - st * st;
    if (!(d > 1e-30f) || !isFinite(d))
        return false;
    a = (gs * tt - gt * st) * (1.0f / d);
    b = (gt * ss - gs * st) * (1.0f / d);
    return isFinite(a) && isFinite(b);
}

// Barycentrics (b1, b2) of corner c.
constexpr float kCorner[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};

// The footprint edges of triangle `tri`: its two shorter edges as (from,
// to) corners, the sides of a diced micropolygon (the longest is its
// diagonal). Half of each is a footprint vector (OIIO's full-width
// derivatives: MIP level log2(texels per edge) - 1, Karma -1.08).
void footprintEdges(const Vec3f *points, const uint32_t *tri, int edge[2][2]) {
    float len[3];
    for (int k = 0; k < 3; ++k)
        len[k] = length(points[tri[(k + 1) % 3]] - points[tri[k]]);
    const int longest = len[0] >= len[1] && len[0] >= len[2] ? 0 : len[1] >= len[2] ? 1 : 2;
    int n = 0;
    for (int k = 0; k < 3; ++k)
        if (k != longest) {
            edge[n][0] = k;
            edge[n][1] = (k + 1) % 3;
            ++n;
        }
}

// One lane: a point and its owner triangle's corner.
struct Lane {
    uint32_t point, triangle;
    uint8_t corner;
};

void runChunk(const MeshDesc &mesh, const DisplacementShader &shader, const std::vector<AttributeRef> &refs,
              int tangentUv, const Lane *lanes, int count, const DisplacementInput &input,
              const DisplacementContext &ctx, const Xform34 &xf, std::vector<float> &R, Vec3f *offsets) {
    const sh::Program &p = *shader.program;
    const float *U = shader.uniforms.data();
    R.assign(std::max<size_t>(p.rowCount, 1) * kLanes, 0.0f);
    auto lane = [&](int l) { return lanes[l < count ? l : count - 1]; };
    const size_t np = mesh.pointCount();
    const Vec3f *points = mesh.points.data(); // step 0
    auto direction = [&](uint32_t i) {
        const Vec3f d = input.directions.empty() ? (mesh.normals.size() >= np ? mesh.normals[i] : Vec3f{0, 0, 1})
                                                 : input.directions[i];
        const float l = length(d);
        return l > 0.0f ? d * (1.0f / l) : Vec3f{0.0f, 0.0f, 1.0f};
    };
    for (const sh::GlobalBinding &g : p.globals) {
        float *row = R.data() + size_t(g.row) * kLanes;
        for (int l = 0; l < kLanes; ++l) {
            const Lane ln = lane(l);
            const uint32_t *tri = &mesh.indices[size_t(ln.triangle) * 3];
            const Vec3f po = points[tri[ln.corner]];
            int edge[2][2];
            footprintEdges(points, tri, edge);
            auto footprint = [&](int axis) { return (points[tri[edge[axis][1]]] - points[tri[edge[axis][0]]]) * 0.5f; };
            const Vec3f no = direction(ln.point);
            Vec3f v{0.0f, 0.0f, 0.0f};
            float s = 0.0f;
            switch (g.global) {
            case sh::Global::P:
                v = xformPoint(xf, po);
                break;
            case sh::Global::Pobject:
                v = po;
                break;
            case sh::Global::N:
            case sh::Global::Ng:
                v = normalize(xformNormal(xf, no));
                break;
            case sh::Global::Nobject:
                v = no;
                break;
            case sh::Global::I: // no ray: towards the surface along the normal
                v = -normalize(xformNormal(xf, no));
                break;
            case sh::Global::Time:
                s = (ctx.frame - 1.0f) / (ctx.fps > 0.0f ? ctx.fps : 24.0f);
                break;
            case sh::Global::Frame:
                s = ctx.frame;
                break;
            case sh::Global::ObjX:
                v = {xf.c[0][0], xf.c[0][1], xf.c[0][2]};
                break;
            case sh::Global::ObjY:
                v = {xf.c[1][0], xf.c[1][1], xf.c[1][2]};
                break;
            case sh::Global::ObjZ:
                v = {xf.c[2][0], xf.c[2][1], xf.c[2][2]};
                break;
            case sh::Global::ObjT:
                v = {xf.c[3][0], xf.c[3][1], xf.c[3][2]};
                break;
            case sh::Global::PDx: // half the micropolygon's sides: the tessellation's footprint
            case sh::Global::PDy:
                v = xformPoint(xf, po + footprint(g.global == sh::Global::PDx ? 0 : 1));
                break;
            case sh::Global::PobjectDx:
            case sh::Global::PobjectDy:
                v = po + footprint(g.global == sh::Global::PobjectDx ? 0 : 1);
                break;
            case sh::Global::DPdu:
            case sh::Global::DPdv: {
                // dP/ds, dP/dt of the tangent uv set over the owner triangle (world).
                const Vec3f p0 = xformPoint(xf, points[tri[0]]), p1 = xformPoint(xf, points[tri[1]]),
                            p2 = xformPoint(xf, points[tri[2]]);
                Vec3f gb1, gb2, du, dv;
                if (!dualBasis(p1 - p0, p2 - p0, gb1, gb2)) {
                    const Frame f = Frame::fromNormal(normalize(xformNormal(xf, no)));
                    v = g.global == sh::Global::DPdu ? f.t : f.b;
                    break;
                }
                Vec3f gs = gb1, gt = gb2;
                if (tangentUv >= 0 && size_t(tangentUv) < refs.size()) {
                    float uv[3][2];
                    bool ok = true;
                    for (int k = 0; k < 3; ++k)
                        ok = ok && fetchAttribute(refs[size_t(tangentUv)], tri, ln.triangle, 0, kCorner[k][0],
                                                  kCorner[k][1], 2, uv[k]);
                    if (ok) {
                        gs = gb1 * (uv[1][0] - uv[0][0]) + gb2 * (uv[2][0] - uv[0][0]);
                        gt = gb1 * (uv[1][1] - uv[0][1]) + gb2 * (uv[2][1] - uv[0][1]);
                    }
                }
                if (!dualBasis(gs, gt, du, dv) && !dualBasis(gb1, gb2, du, dv)) {
                    const Frame f = Frame::fromNormal(normalize(xformNormal(xf, no)));
                    du = f.t;
                    dv = f.b;
                }
                v = g.global == sh::Global::DPdu ? du : dv;
                break;
            }
            case sh::Global::PrimId:
                s = ln.triangle < input.triangleFaces.size() ? float(input.triangleFaces[ln.triangle]) : float(ln.triangle);
                break;
            default: // InstanceId, ray state: 0 (a camera ray at depth 0)
                break;
            }
            if (sh::globalWidth(g.global) == 3) {
                row[l] = v.x;
                row[kLanes + l] = v.y;
                row[2 * kLanes + l] = v.z;
            } else {
                row[l] = s;
            }
        }
    }
    for (const sh::PrimvarBinding &b : p.primvarBindings) {
        float *row = R.data() + size_t(b.row) * kLanes;
        const float *fallback = U + b.fallback;
        for (int l = 0; l < kLanes; ++l) {
            const Lane ln = lane(l);
            const uint32_t *tri = &mesh.indices[size_t(ln.triangle) * 3];
            float b1 = kCorner[ln.corner][0], b2 = kCorner[ln.corner][1];
            if (b.shift) { // moved by the footprint vector (affine in the triangle's plane)
                int edge[2][2];
                footprintEdges(mesh.points.data(), tri, edge);
                const int from = edge[b.shift - 1][0], to = edge[b.shift - 1][1];
                b1 += 0.5f * (kCorner[to][0] - kCorner[from][0]);
                b2 += 0.5f * (kCorner[to][1] - kCorner[from][1]);
            }
            float v[4];
            if (b.name >= refs.size() || !fetchAttribute(refs[b.name], tri, ln.triangle, 0, b1, b2, b.width, v))
                for (int k = 0; k < b.width; ++k)
                    v[k] = fallback[k];
            for (int k = 0; k < b.width; ++k)
                row[size_t(k) * kLanes + size_t(l)] = v[k];
        }
    }
    TextureContext tex{ctx.textures, shader.samplers.data(), uint32_t(shader.samplers.size()), nullptr};
    sh::execute<kLanes>(p.code.data(), p.code.size(), R.data(), U, &tex);
    for (const sh::OutputBinding &o : p.outputs) {
        if (o.field != surface_field::kDisplacement)
            continue;
        for (int l = 0; l < count; ++l) {
            float d[3];
            for (int k = 0; k < 3; ++k)
                d[k] = o.uniform ? U[o.index + k] : R[size_t(o.index + k) * kLanes + size_t(l)];
            const Vec3f off{d[0], d[1], d[2]};
            offsets[l] = isFinite(off) ? off : Vec3f{0.0f, 0.0f, 0.0f};
        }
    }
}

// The same for a VEX displacement (docs/design/M5.md D8): the bridge gets
// the points and normals in world space and returns the displaced points;
// the offsets go back to object space.
void runExternalChunk(const MeshDesc &mesh, const DisplacementShader &shader, const std::vector<AttributeRef> &refs,
                      const Lane *lanes, int count, const DisplacementInput &input, const DisplacementContext &ctx,
                      const Xform34 &xf, const Xform34 &inverse, Vec3f *offsets) {
    const std::vector<ExternalParam> &params = shader.external->params();
    ExternalLane x[kLanes];
    const size_t np = mesh.pointCount();
    for (int l = 0; l < count; ++l) {
        const Lane ln = lanes[l];
        const uint32_t *tri = &mesh.indices[size_t(ln.triangle) * 3];
        Vec3f no = input.directions.empty() ? (mesh.normals.size() >= np ? mesh.normals[ln.point] : Vec3f{0, 0, 1})
                                            : input.directions[ln.point];
        const Vec3f n = normalize(xformNormal(xf, length(no) > 0.0f ? no : Vec3f{0.0f, 0.0f, 1.0f}));
        const Frame f = Frame::fromNormal(n);
        ExternalLane &e = x[l];
        e.P = xformPoint(xf, mesh.points[tri[ln.corner]]);
        e.N = e.Ng = n;
        e.I = -n; // no ray: towards the surface along the normal
        e.dPds = f.t;
        e.dPdt = f.b;
        e.s = kCorner[ln.corner][0];
        e.t = kCorner[ln.corner][1];
        e.time = (ctx.frame - 1.0f) / (ctx.fps > 0.0f ? ctx.fps : 24.0f);
    }
    // Parameters: the primvar of the same name, else the material's value.
    std::vector<float> scratch;
    size_t total = 0;
    for (size_t i = 0; i < params.size(); ++i)
        if (refs[i].data)
            total += size_t(params[i].width) * size_t(count);
    scratch.resize(total);
    std::vector<ExternalInput> ins(params.size());
    float *cursor = scratch.data();
    for (size_t i = 0; i < params.size(); ++i) {
        const int w = params[i].width;
        if (w == 0)
            continue;
        if (refs[i].data) {
            for (int l = 0; l < count; ++l) {
                const Lane ln = lanes[l];
                const uint32_t *tri = &mesh.indices[size_t(ln.triangle) * 3];
                float v[16] = {};
                if (!fetchAttribute(refs[i], tri, ln.triangle, 0, kCorner[ln.corner][0], kCorner[ln.corner][1],
                                    std::min(w, 4), v))
                    for (int k = 0; k < w && k < int(shader.externalValues[i].size()); ++k)
                        v[k] = shader.externalValues[i][size_t(k)];
                for (int k = 0; k < w; ++k)
                    cursor[l * w + k] = v[k];
            }
            ins[i] = {cursor, true};
            cursor += size_t(w) * size_t(count);
        } else if (i < shader.externalValues.size() && shader.externalValues[i].size() >= size_t(w)) {
            ins[i] = {shader.externalValues[i].data(), false};
        }
    }
    Vec3f P[kLanes];
    // A failed run displaces nothing (the bridge reports the error once).
    if (!shader.external->run(count, x, ins.data(), shader.externalStrings.data(), P, nullptr)) {
        for (int l = 0; l < count; ++l)
            offsets[l] = Vec3f{0.0f, 0.0f, 0.0f};
        return;
    }
    for (int l = 0; l < count; ++l) {
        const Vec3f off = xformVector(inverse, P[l] - x[l].P);
        offsets[l] = isFinite(off) ? off : Vec3f{0.0f, 0.0f, 0.0f};
    }
}

} // namespace

bool displacementIsZero(const DisplacementShader &shader) {
    if (shader.external)
        return false;
    if (!shader.program)
        return true;
    const sh::Program &p = *shader.program;
    if (!p.varyingFree())
        return false;
    for (const sh::OutputBinding &o : p.outputs)
        if (o.field == surface_field::kDisplacement)
            for (int k = 0; k < o.width; ++k)
                if (o.uniform && size_t(o.index) + size_t(k) < shader.uniforms.size() &&
                    shader.uniforms[o.index + size_t(k)] != 0.0f)
                    return false;
    return true;
}

size_t displaceMesh(MeshDesc &mesh, const DisplacementInput &input, const DisplacementContext &ctx) {
    const size_t np = mesh.pointCount(), nt = mesh.triangleCount();
    const uint32_t steps = std::max(mesh.pointSteps, 1u);
    if (np == 0 || nt == 0 || input.triangleShader.size() < nt)
        return 0;
    // Owner triangle and corner of every point.
    std::vector<Lane> owner(np, Lane{~0u, ~0u, 0});
    for (size_t t = 0; t < nt; ++t)
        for (int c = 0; c < 3; ++c) {
            const uint32_t i = mesh.indices[t * 3 + size_t(c)];
            if (i < np && owner[i].triangle == ~0u)
                owner[i] = Lane{i, uint32_t(t), uint8_t(c)};
        }
    // Copies take their source's offset, so they are not evaluated.
    std::vector<uint8_t> isCopy(np, 0);
    for (const auto &[copy, source] : input.copies)
        if (copy >= 0 && size_t(copy) < np)
            isCopy[size_t(copy)] = 1;
    // Lanes grouped by shader.
    std::vector<std::vector<Lane>> groups(input.shaders.size());
    for (size_t i = 0; i < np; ++i) {
        if (owner[i].triangle == ~0u || isCopy[i])
            continue;
        const int32_t s = input.triangleShader[owner[i].triangle];
        const DisplacementShader *shader = s >= 0 && size_t(s) < groups.size() ? input.shaders[size_t(s)] : nullptr;
        if (shader && (shader->program || shader->external))
            groups[size_t(s)].push_back(owner[i]);
    }

    const Xform34 xf = toXform34(ctx.objectToWorld);
    const Xform34 inverseXf = toXform34(inverse(ctx.objectToWorld));
    std::vector<Vec3f> offsets(np, Vec3f{0.0f, 0.0f, 0.0f});
    size_t displaced = 0;
    for (size_t s = 0; s < groups.size(); ++s) {
        const std::vector<Lane> &lanes = groups[s];
        if (lanes.empty())
            continue;
        const DisplacementShader &shader = *input.shaders[s];
        std::vector<AttributeRef> refs;
        if (shader.external) {
            const std::vector<ExternalParam> &params = shader.external->params();
            for (size_t i = 0; i < params.size(); ++i) {
                const std::string &name = i < shader.externalPrimvars.size() && !shader.externalPrimvars[i].empty()
                                              ? shader.externalPrimvars[i]
                                              : params[i].name;
                refs.push_back(params[i].width > 0 && params[i].width <= 4 ? attributeOf(mesh, name) : AttributeRef{});
            }
        }
        else
            for (const std::string &name : shader.program->primvars)
                refs.push_back(attributeOf(mesh, name));
        const size_t chunks = (lanes.size() + kLanes - 1) / kLanes;
        tbb::parallel_for(tbb::blocked_range<size_t>(0, chunks, 16), [&](const tbb::blocked_range<size_t> &r) {
            std::vector<float> R;
            Vec3f out[kLanes];
            for (size_t c = r.begin(); c != r.end(); ++c) {
                const size_t first = c * kLanes;
                const int count = int(std::min<size_t>(kLanes, lanes.size() - first));
                if (shader.external)
                    runExternalChunk(mesh, shader, refs, lanes.data() + first, count, input, ctx, xf, inverseXf, out);
                else
                    runChunk(mesh, shader, refs, shader.program->tangentUv, lanes.data() + first, count, input, ctx,
                             xf, R, out);
                for (int l = 0; l < count; ++l)
                    offsets[lanes[first + size_t(l)].point] = out[l];
            }
        });
        displaced += lanes.size();
    }
    for (const auto &[copy, source] : input.copies)
        if (copy >= 0 && size_t(copy) < np && source >= 0 && size_t(source) < np)
            offsets[size_t(copy)] = offsets[size_t(source)];

    // The same object-space offset on every motion step, then smooth normals
    // of the displaced triangles per step (area-weighted).
    mesh.normals.assign(np * steps, Vec3f{0.0f, 0.0f, 0.0f});
    for (uint32_t st = 0; st < steps; ++st) {
        Vec3f *P = mesh.points.data() + size_t(st) * np;
        for (size_t i = 0; i < np; ++i)
            P[i] += offsets[i];
        Vec3f *N = mesh.normals.data() + size_t(st) * np;
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t *ix = &mesh.indices[t * 3];
            const Vec3f n = cross(P[ix[1]] - P[ix[0]], P[ix[2]] - P[ix[0]]);
            for (int k = 0; k < 3; ++k)
                N[ix[k]] += n;
        }
        for (size_t i = 0; i < np; ++i) {
            const float l = length(N[i]);
            N[i] = l > 0.0f && isFinite(N[i]) ? N[i] * (1.0f / l) : Vec3f{0.0f, 0.0f, 1.0f};
        }
    }
    return displaced;
}

} // namespace nova
