#include "core/volume/build.h"

#include "core/shading/interval.h"
#include "core/shading/vm.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace nova {

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

int32_t fieldIndex(const VolumeDesc &d, const std::string &name) {
    const VolumeField *f = d.findField(name);
    return f ? int32_t(f - d.fields.data()) : -1;
}

// The object-to-world maps a volume takes over the shutter: its keys and 8
// points per segment between them (rotations sweep past the keys' boxes).
std::vector<Affine3f> worldMaps(const VolumeRecord &rec) {
    if (rec.motionSteps <= 1)
        return {rec.objectToWorld};
    std::vector<Affine3f> maps;
    const int samples = int(rec.motionSteps - 1) * 8;
    for (int s = 0; s <= samples; ++s)
        maps.push_back(srtAtTime(rec.motionKeys.data(), rec.motionSteps, 1, float(s) / float(samples)));
    return maps;
}

// The box around the images of index-space box [a, b] under each map.
void mappedBox(const std::vector<Affine3f> &maps, Vec3f a, Vec3f b, Vec3f &lower, Vec3f &upper) {
    lower = {kInfinity, kInfinity, kInfinity};
    upper = {-kInfinity, -kInfinity, -kInfinity};
    for (const Affine3f &m : maps)
        for (int c = 0; c < 8; ++c) {
            const Vec3f p = m.point({(c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z});
            lower = {std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)};
            upper = {std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)};
        }
}

// The extinction bound of a cell from the volume shader's outputs:
// max over channels of max(scattering, 0) + max(absorption, 0); and the
// largest emission magnitude.
struct CellBound {
    float extinction = 0.0f;
    float emission = 0.0f;
};

// NOVA_DEBUG_VOLUME_BOUNDS set: report why cells have no interval bound.
bool debugBounds() {
    static const bool on = [] {
#if defined(_MSC_VER)
        char *value = nullptr;
        size_t size = 0;
        const bool set = _dupenv_s(&value, &size, "NOVA_DEBUG_VOLUME_BOUNDS") == 0 && value;
        std::free(value);
        return set;
#else
        return std::getenv("NOVA_DEBUG_VOLUME_BOUNDS") != nullptr;
#endif
    }();
    return on;
}

// Interval runs of a volume shader per majorant cell (docs/design/M8.md D7).
struct ShaderBounds {
    const VolumeRecord &rec;
    const MaterialRecord &material;
    const shading::Program &p;
    const std::vector<int32_t> &read;         // the distinct fields read
    const std::vector<CellRanges> &ranges;    // per read field
    std::vector<std::array<int32_t, 4>> slot; // per binding component: its position in `read`, or -1
    std::vector<Affine3f> objectToWorld;
    // The Time and Frame globals: the render's (VolumeEvaluator's values).
    float time = 0.0f;
    float frame = 1.0f;

    // Ray state rows a shader may read: every ray class (Karma's 0 camera ...
    // 6 shadow) and any depth; the time and frame are the render's.
    shading::Interval global(shading::Global g, Vec3f lo, Vec3f hi, Vec3f olo, Vec3f ohi, int k) const {
        using shading::Global;
        using shading::Interval;
        const float a[3] = {lo.x, lo.y, lo.z}, b[3] = {hi.x, hi.y, hi.z};
        const float oa[3] = {olo.x, olo.y, olo.z}, ob[3] = {ohi.x, ohi.y, ohi.z};
        switch (g) {
        case Global::P:
        case Global::PDx:
        case Global::PDy:
            return {a[k], b[k]};
        case Global::Pobject:
        case Global::PobjectDx:
        case Global::PobjectDy:
            return {oa[k], ob[k]};
        case Global::RayType:
            return {0.0f, 6.0f};
        case Global::RayDepth:
            return {0.0f, 255.0f};
        case Global::HitDist:
            return {0.0f, kInfinity};
        case Global::Backface:
            return {0.0f, 1.0f};
        case Global::PrimId:
        case Global::InstanceId:
            return Interval::point(0.0f);
        case Global::ObjX:
        case Global::ObjY:
        case Global::ObjZ:
        case Global::ObjT: {
            if (objectToWorld.size() != 1)
                return Interval::unbounded();
            const Affine3f &m = objectToWorld[0];
            const Vec3f v = g == Global::ObjX ? m.x : g == Global::ObjY ? m.y : g == Global::ObjZ ? m.z : m.t;
            return Interval::point(k == 0 ? v.x : k == 1 ? v.y : v.z);
        }
        case Global::Time:
            return Interval::point(time);
        case Global::Frame:
            return Interval::point(frame);
        case Global::Pndc: // volumes see no camera (0); no bound claimed
            return Interval::unbounded();
        default: // directions (normals, I, tangents): unit vectors
            return {-1.0f, 1.0f};
        }
    }

    CellBound interval(size_t cell, Vec3f lo, Vec3f hi, Vec3f olo, Vec3f ohi, std::vector<shading::Interval> &R) const {
        using shading::Interval;
        const float *U = material.volumeUniforms.data();
        for (const shading::GlobalBinding &g : p.globals)
            for (int k = 0; k < shading::globalWidth(g.global); ++k)
                R[g.row + k] = global(g.global, lo, hi, olo, ohi, k);
        for (size_t i = 0; i < p.primvarBindings.size(); ++i) {
            const shading::PrimvarBinding &b = p.primvarBindings[i];
            for (int k = 0; k < b.width; ++k) {
                const int32_t r = slot[i][k];
                R[b.row + k] = r >= 0 ? Interval{ranges[r].lo[cell], ranges[r].hi[cell]} : Interval::point(U[b.fallback + k]);
            }
        }
        shading::executeInterval(p.code.data(), p.code.size(), R.data(), U);
        Interval out[volume_field::kCount];
        for (uint16_t f = 0; f < volume_field::kCount; ++f)
            out[f] = Interval::point(material.volume.v[f]);
        for (const shading::OutputBinding &o : p.outputs)
            for (int k = 0; k < o.width; ++k)
                out[o.field + k] = o.uniform ? Interval::point(U[o.index + k]) : R[o.index + k];
        CellBound bound;
        for (int c = 0; c < 3; ++c) {
            const Interval s = out[volume_field::scattering + c], a = out[volume_field::absorption + c];
            const Interval e = out[volume_field::emission + c];
            bound.extinction = std::max(bound.extinction, std::max(s.hi, 0.0f) + std::max(a.hi, 0.0f));
            bound.emission = std::max({bound.emission, std::fabs(e.lo), std::fabs(e.hi)});
        }
        return bound;
    }

    // Diagnosis (NOVA_DEBUG_VOLUME_BOUNDS): the first instruction whose
    // result is unbounded although its operands are bounded.
    void explain(size_t cell, Vec3f lo, Vec3f hi, Vec3f olo, Vec3f ohi, std::vector<shading::Interval> &R) const {
        interval(cell, lo, hi, olo, ohi, R); // fills the leaves
        const float *U = material.volumeUniforms.data();
        std::vector<shading::Interval> rows = R;
        for (const shading::GlobalBinding &g : p.globals)
            for (int k = 0; k < shading::globalWidth(g.global); ++k)
                rows[g.row + k] = global(g.global, lo, hi, olo, ohi, k);
        for (size_t i = 0; i < p.primvarBindings.size(); ++i)
            for (int k = 0; k < p.primvarBindings[i].width; ++k) {
                const int32_t r = slot[i][k];
                rows[p.primvarBindings[i].row + k] = r >= 0 ? shading::Interval{ranges[r].lo[cell], ranges[r].hi[cell]}
                                                            : shading::Interval::point(U[p.primvarBindings[i].fallback + k]);
            }
        int reported = 0;
        for (size_t i = 0; i < p.code.size() && reported < 8; ++i) {
            const shading::Instr &in = p.code[i];
            const auto operand = [&](uint16_t index, bool uniform) {
                return uniform ? shading::Interval::point(U[index]) : rows[index];
            };
            const shading::Interval a = operand(in.a, in.flags & shading::kUniformA);
            const shading::Interval b = operand(in.b, in.flags & shading::kUniformB);
            const bool bounded = a.bounded() && (shading::operandCount(in.op) < 2 || b.bounded());
            shading::executeInterval(&in, 1, rows.data(), U);
            bool unbounded = false;
            for (int k = 0; k < in.width; ++k)
                unbounded = unbounded || !rows[in.dst + k].bounded();
            if (unbounded && bounded) {
                std::fprintf(stderr, "[Nova] volume bounds: instruction %zu (op %d, width %d) is unbounded: a [%g, %g] b [%g, %g]\n",
                             i, int(in.op), int(in.width), a.lo, a.hi, b.lo, b.hi);
                ++reported;
            }
        }
    }

    // The shader's extinction bound terms at one point: a value per read
    // field, a ray class, the cell centre.
    CellBound run(const float *values, float rayType, Vec3f centre, Vec3f objectCentre, std::vector<float> &rows) const {
        const float *U = material.volumeUniforms.data();
        for (const shading::GlobalBinding &g : p.globals) {
            Vec3f v{0.0f, 0.0f, 0.0f};
            float s = 0.0f;
            if (g.global == shading::Global::P)
                v = centre;
            else if (g.global == shading::Global::Pobject)
                v = objectCentre;
            else if (g.global == shading::Global::RayType)
                s = rayType;
            else if (g.global == shading::Global::Time)
                s = time;
            else if (g.global == shading::Global::Frame)
                s = frame;
            if (shading::globalWidth(g.global) == 3) {
                rows[g.row] = v.x;
                rows[g.row + 1] = v.y;
                rows[g.row + 2] = v.z;
            } else {
                rows[g.row] = s;
            }
        }
        for (size_t i = 0; i < p.primvarBindings.size(); ++i) {
            const shading::PrimvarBinding &b = p.primvarBindings[i];
            for (int k = 0; k < b.width; ++k)
                rows[b.row + k] = slot[i][k] >= 0 ? values[size_t(slot[i][k])] : U[b.fallback + k];
        }
        shading::execute<1>(p.code.data(), p.code.size(), rows.data(), U);
        VolumeInputs in = material.volume;
        for (const shading::OutputBinding &o : p.outputs)
            if (!o.uniform)
                for (int k = 0; k < o.width; ++k)
                    in.v[o.field + k] = rows[o.index + k];
        CellBound bound;
        for (int c = 0; c < 3; ++c) {
            const float e = std::max(in.v[volume_field::scattering + c], 0.0f) +
                            std::max(in.v[volume_field::absorption + c], 0.0f);
            if (std::isfinite(e))
                bound.extinction = std::max(bound.extinction, e);
            const float m = std::fabs(in.v[volume_field::emission + c]);
            if (std::isfinite(m))
                bound.emission = std::max(bound.emission, m);
        }
        return bound;
    }

    // Whether the shader depends on its fields (and the ray class, the time
    // and the frame, which the table covers) alone, not on where it runs.
    // The ray depth is not: the table runs at depth 0, deeper rays may see
    // more extinction.
    bool fieldsOnly() const {
        using shading::Global;
        for (const shading::GlobalBinding &g : p.globals)
            if (g.global != Global::RayType && g.global != Global::Time && g.global != Global::Frame)
                return false;
        return true;
    }

    // Where intervals give no bound because a value depends on a field twice
    // (Houdini's complement colour divides XYZ by X+Y+Z, both from density):
    // the shader tabulated over its fields' whole ranges (one or two fields),
    // every ray class per sample; a cell takes the largest sample over its
    // ranges and one sample beyond, 5 % more.
    struct Transfer {
        int n[2] = {1, 1};
        float lo[2] = {0.0f, 0.0f}, step[2] = {0.0f, 0.0f};
        std::vector<CellBound> samples;
    };
    bool buildTransfer(Transfer &t) const {
        const size_t m = read.size();
        if (m == 0 || m > 2 || !fieldsOnly())
            return false;
        const int n = m == 1 ? 1024 : 96;
        for (size_t r = 0; r < m; ++r) {
            const float lo = *std::min_element(ranges[r].lo.begin(), ranges[r].lo.end());
            const float hi = *std::max_element(ranges[r].hi.begin(), ranges[r].hi.end());
            t.n[r] = hi > lo ? n : 1;
            t.lo[r] = lo;
            t.step[r] = hi > lo ? (hi - lo) / float(n - 1) : 0.0f;
        }
        t.samples.assign(size_t(t.n[0]) * size_t(t.n[1]), CellBound{});
        tbb::parallel_for(0, t.n[1], [&](int j) {
            std::vector<float> rows(std::max<size_t>(p.rowCount, 1));
            for (int i = 0; i < t.n[0]; ++i) {
                const float values[2] = {t.lo[0] + t.step[0] * float(i), t.lo[1] + t.step[1] * float(j)};
                CellBound &s = t.samples[size_t(j) * size_t(t.n[0]) + size_t(i)];
                for (int rayType = 0; rayType <= 6; ++rayType) {
                    const CellBound b = run(values, float(rayType), Vec3f{}, Vec3f{}, rows);
                    s.extinction = std::max(s.extinction, b.extinction);
                    s.emission = std::max(s.emission, b.emission);
                }
            }
        });
        return true;
    }
    CellBound fromTransfer(const Transfer &t, size_t cell) const {
        int i0[2] = {0, 0}, i1[2] = {0, 0};
        for (size_t r = 0; r < read.size(); ++r) {
            if (t.n[r] <= 1)
                continue;
            i0[r] = std::clamp(int(std::floor((ranges[r].lo[cell] - t.lo[r]) / t.step[r])) - 1, 0, t.n[r] - 1);
            i1[r] = std::clamp(int(std::ceil((ranges[r].hi[cell] - t.lo[r]) / t.step[r])) + 1, 0, t.n[r] - 1);
        }
        CellBound bound;
        for (int j = i0[1]; j <= i1[1]; ++j)
            for (int i = i0[0]; i <= i1[0]; ++i) {
                const CellBound &s = t.samples[size_t(j) * size_t(t.n[0]) + size_t(i)];
                bound.extinction = std::max(bound.extinction, s.extinction);
                bound.emission = std::max(bound.emission, s.emission);
            }
        bound.extinction *= 1.05f;
        bound.emission *= 1.05f;
        return bound;
    }

    // A sampled estimate where neither gives a bound: the shader at the
    // corners of the read fields' ranges (all of them up to 6 fields, else
    // all low, all high and the middle), at the cell centre, for camera and
    // shadow rays; twice the largest extinction.
    CellBound estimate(size_t cell, Vec3f centre, Vec3f objectCentre, std::vector<float> &rows) const {
        const size_t n = read.size();
        std::vector<std::vector<float>> combos;
        if (n <= 6) {
            for (uint32_t m = 0; m < (1u << n); ++m) {
                std::vector<float> c(n);
                for (size_t r = 0; r < n; ++r)
                    c[r] = (m >> r) & 1u ? ranges[r].hi[cell] : ranges[r].lo[cell];
                combos.push_back(std::move(c));
            }
        } else {
            for (float t : {0.0f, 0.5f, 1.0f}) {
                std::vector<float> c(n);
                for (size_t r = 0; r < n; ++r)
                    c[r] = ranges[r].lo[cell] + (ranges[r].hi[cell] - ranges[r].lo[cell]) * t;
                combos.push_back(std::move(c));
            }
        }
        CellBound bound;
        for (const std::vector<float> &values : combos)
            for (float rayType : {0.0f, 6.0f}) {
                const CellBound b = run(values.data(), rayType, centre, objectCentre, rows);
                bound.extinction = std::max(bound.extinction, 2.0f * b.extinction);
                bound.emission = std::max(bound.emission, b.emission);
            }
        return bound;
    }
};
} // namespace

int32_t volumeFieldComponent(const VolumeDesc &desc, const std::string &name, int width, int k) {
    static const char kAxes[] = "xyzw";
    if (width > 1)
        for (const char *sep : {".", "_"}) {
            const int32_t f = fieldIndex(desc, name + sep + kAxes[k]);
            if (f >= 0)
                return f;
        }
    return fieldIndex(desc, name);
}

void buildVolumeRecord(VolumeRecord &rec, const MaterialRecord *material, float frame, float fps) {
    const VolumeDesc &d = rec.desc;
    rec.reference = -1;
    rec.layout = MajorantLayout{};
    rec.majorant.clear();
    rec.extinctionMajorant.clear();
    rec.bindings.clear();
    rec.estimatedCells = 0;
    rec.frameDependent = false;
    rec.lightCells = Distribution1D{};
    rec.lightPower = 0.0f;
    rec.cellVolume = 0.0f;
    rec.lower = {1.0f, 1.0f, 1.0f};
    rec.upper = {-1.0f, -1.0f, -1.0f};
    rec.program = material ? material->desc.program->volume.get() : nullptr;
    rec.material = material ? d.material : kDefaultMaterialId;
    // Whether the coefficients depend on the ray level: the shader reads it,
    // or Karma's modifiers are on (varying ones count as on).
    rec.levelDependent = false;
    if (rec.program) {
        using namespace shading;
        for (const GlobalBinding &g : rec.program->globals) {
            rec.levelDependent |= g.global == Global::RayDepth;
            rec.frameDependent |= g.global == Global::Time || g.global == Global::Frame;
        }
        const VolumeInputs &in = material->volume;
        rec.levelDependent |= in.f(volume_field::extinctionModifier) > 0.0f ||
                              in.f(volume_field::contributionModifier) < 1.0f;
        for (const OutputBinding &o : rec.program->outputs)
            for (int k = 0; k < o.width; ++k) {
                const int f = o.field + k;
                rec.levelDependent |= !o.uniform && (f == volume_field::extinctionModifier ||
                                                     f == volume_field::contributionModifier);
            }
    }
    rec.fieldObjectToIndex.clear();
    for (const VolumeField &f : d.fields)
        rec.fieldObjectToIndex.push_back(f.grid ? inverseAffine(f.indexToObject()) : Affine3f{});

    // The fields the shader reads (D5); the default volume reads density.
    std::vector<int32_t> read;
    const auto note = [&read](int32_t f) {
        if (f >= 0 && std::find(read.begin(), read.end(), f) == read.end())
            read.push_back(f);
    };
    if (rec.program) {
        for (const shading::PrimvarBinding &b : rec.program->primvarBindings) {
            std::array<int32_t, 4> c{-1, -1, -1, -1};
            for (int k = 0; k < b.width && k < 4; ++k) {
                c[size_t(k)] = volumeFieldComponent(d, rec.program->primvars[b.name], b.width, k);
                note(c[size_t(k)]);
            }
            rec.bindings.push_back(c);
        }
    } else {
        note(fieldIndex(d, "density"));
    }
    if (read.empty())
        return; // nothing in space to render
    const int32_t density = fieldIndex(d, "density");
    rec.reference = std::find(read.begin(), read.end(), density) != read.end() ? density : read.front();
    rec.indexToObject = d.fields[size_t(rec.reference)].indexToObject();
    const Affine3f objectToRef = inverseAffine(rec.indexToObject);

    // Velocity blur (D17): the components and the largest offset, |v| at
    // most times the longest time from the fields'.
    rec.velocity = {-1, -1, -1};
    rec.blurReach = 0.0f;
    const float blurTime = std::max(std::fabs(d.velocityTime[0]), std::fabs(d.velocityTime[1]));
    if (!d.velocity.empty() && blurTime > 0.0f) {
        float speed2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            rec.velocity[size_t(k)] = volumeFieldComponent(d, d.velocity, 3, k);
            const VolumeField *f = rec.velocity[size_t(k)] >= 0 ? &d.fields[size_t(rec.velocity[size_t(k)])] : nullptr;
            if (!f || !f->grid) {
                rec.velocity = {-1, -1, -1};
                break;
            }
            const nanovdb::FloatGrid &g = *f->grid->grid();
            float m = std::fabs(g.tree().background());
            if (g.activeVoxelCount() > 0) // the root's statistics of its active values
                m = std::max({m, std::fabs(g.tree().root().minimum()), std::fabs(g.tree().root().maximum())});
            speed2 += m * m;
        }
        if (rec.velocity[0] >= 0)
            rec.blurReach = std::sqrt(speed2) * blurTime;
    }
    // The reach in the reference's index units (the linear map's Frobenius
    // norm bounds its stretch).
    const float reach = rec.blurReach * std::sqrt(dot(objectToRef.x, objectToRef.x) + dot(objectToRef.y, objectToRef.y) +
                                                  dot(objectToRef.z, objectToRef.z));

    // The domain: the read fields' supports in the reference's index space
    // (widened by the blur's reach).
    Vec3f lower{kInfinity, kInfinity, kInfinity}, upper{-kInfinity, -kInfinity, -kInfinity};
    std::vector<Affine3f> toRef(read.size());
    for (size_t r = 0; r < read.size(); ++r) {
        const VolumeField &f = d.fields[size_t(read[r])];
        toRef[r] = compose(f.indexToObject(), objectToRef);
        Vec3f a, b;
        if (fieldSupport(*f.grid->grid(), toRef[r], a, b)) {
            lower = {std::min(lower.x, a.x), std::min(lower.y, a.y), std::min(lower.z, a.z)};
            upper = {std::max(upper.x, b.x), std::max(upper.y, b.y), std::max(upper.z, b.z)};
        }
    }
    if (reach > 0.0f && lower.x <= upper.x) {
        lower -= Vec3f{reach, reach, reach};
        upper += Vec3f{reach, reach, reach};
    }
    rec.layout = majorantLayout(lower, upper);
    if (rec.layout.empty())
        return;
    std::vector<CellRanges> ranges(read.size());
    for (size_t r = 0; r < read.size(); ++r) {
        fieldCellRanges(*d.fields[size_t(read[r])].grid->grid(), toRef[r], rec.layout, ranges[r]);
        // A lookup moved by up to `reach` reads the values of the cells
        // that far around.
        if (reach > 0.0f)
            dilateCellRanges(rec.layout, int32_t(std::ceil(reach / float(rec.layout.k))), ranges[r]);
    }

    const std::vector<Affine3f> objectToWorld = worldMaps(rec);
    const size_t cells = rec.layout.cellCount();
    rec.majorant.assign(cells, 0.0f);
    if (!rec.program) {
        for (size_t i = 0; i < cells; ++i)
            rec.majorant[i] = std::max(ranges[0].hi[i], 0.0f);
    } else {
        // Time as VolumeEvaluator and the GPU's evaluateVolume take it.
        const float time = (frame - 1.0f) / (fps > 0.0f ? fps : 24.0f);
        ShaderBounds sb{rec, *material, *rec.program, read, ranges, {}, objectToWorld, time, frame};
        for (size_t i = 0; i < rec.bindings.size(); ++i) {
            std::array<int32_t, 4> s{-1, -1, -1, -1};
            for (size_t k = 0; k < 4; ++k)
                if (rec.bindings[i][k] >= 0)
                    s[k] = int32_t(std::find(read.begin(), read.end(), rec.bindings[i][k]) - read.begin());
            sb.slot.push_back(s);
        }
        // Emission is collected at tentative collisions, so cells that emit
        // get at least one per voxel (research A 4.5).
        float voxel = kInfinity;
        for (const Affine3f &m : objectToWorld) {
            const Affine3f iw = compose(rec.indexToObject, m);
            voxel = std::min({voxel, length(iw.x), length(iw.y), length(iw.z)});
        }
        const float floorRate = voxel > 0.0f && std::isfinite(voxel) ? 1.0f / voxel : 0.0f;
        const MajorantLayout &l = rec.layout;
        const std::vector<Affine3f> indexToObject{rec.indexToObject};
        std::vector<Affine3f> indexToWorld;
        for (const Affine3f &m : objectToWorld)
            indexToWorld.push_back(compose(rec.indexToObject, m));
        // Intervals first; where they give no bound, the transfer table if the
        // shader allows one, else the sampled estimate.
        const auto cellBoxes = [&](size_t i, Vec3f &olo, Vec3f &ohi, Vec3f &wlo, Vec3f &whi) {
            const int32_t x = int32_t(i % size_t(l.res[0])), y = int32_t((i / size_t(l.res[0])) % size_t(l.res[1])),
                          z = int32_t(i / (size_t(l.res[0]) * size_t(l.res[1])));
            const Vec3f a{float(l.lo[0] + x * l.k), float(l.lo[1] + y * l.k), float(l.lo[2] + z * l.k)};
            const Vec3f b = a + Vec3f{float(l.k), float(l.k), float(l.k)};
            mappedBox(indexToObject, a, b, olo, ohi);
            mappedBox(indexToWorld, a, b, wlo, whi);
        };
        std::vector<CellBound> bounds(cells);
        std::atomic<uint32_t> unbounded{0};
        tbb::parallel_for(size_t(0), cells, [&](size_t i) {
            thread_local std::vector<shading::Interval> R;
            R.resize(std::max<size_t>(rec.program->rowCount, 1));
            Vec3f olo, ohi, wlo, whi;
            cellBoxes(i, olo, ohi, wlo, whi);
            bounds[i] = sb.interval(i, wlo, whi, olo, ohi, R);
            if (!std::isfinite(bounds[i].extinction) || !std::isfinite(bounds[i].emission)) {
                if (unbounded.fetch_add(1) == 0 && debugBounds())
                    sb.explain(i, wlo, whi, olo, ohi, R);
                bounds[i].extinction = -1.0f; // to fill below
            }
        });
        if (unbounded.load() > 0) {
            ShaderBounds::Transfer transfer;
            const bool tabulated = sb.buildTransfer(transfer);
            tbb::parallel_for(size_t(0), cells, [&](size_t i) {
                if (bounds[i].extinction >= 0.0f)
                    return;
                if (tabulated) {
                    bounds[i] = sb.fromTransfer(transfer, i);
                    return;
                }
                thread_local std::vector<float> rows;
                rows.resize(std::max<size_t>(rec.program->rowCount, 1));
                Vec3f olo, ohi, wlo, whi;
                cellBoxes(i, olo, ohi, wlo, whi);
                bounds[i] = sb.estimate(i, (wlo + whi) * 0.5f, (olo + ohi) * 0.5f, rows);
            });
            rec.estimatedCells = tabulated ? 0 : unbounded.load();
        }
        bool raised = false;
        for (size_t i = 0; i < cells; ++i) {
            rec.majorant[i] = bounds[i].emission > 0.0f ? std::max(bounds[i].extinction, floorRate) : bounds[i].extinction;
            raised |= rec.majorant[i] != bounds[i].extinction;
        }
        // Rays that collect no emission (shadow rays, light-only rays) track
        // on the extinction bound alone: the floor would only add collisions.
        if (raised) {
            rec.extinctionMajorant.resize(cells);
            for (size_t i = 0; i < cells; ++i)
                rec.extinctionMajorant[i] = bounds[i].extinction;
        }
        // A light source (D13): its cells in proportion to their emission
        // bound, which covers every emitting point.
        // The light-sample pdf divides by one cell's world volume, a constant:
        // a volume whose motion changes its size over the shutter is no
        // light source (as moving emissive meshes, M9 D14), its emission is
        // still collected where paths cross it.
        float detLo = kInfinity, detHi = 0.0f;
        for (const Affine3f &m : objectToWorld) {
            const float det = std::fabs(m.determinant());
            detLo = std::min(detLo, det);
            detHi = std::max(detHi, det);
        }
        const bool resized = detHi > detLo * (1.0f + 1e-4f);
        if (d.light && resized)
            std::fprintf(stderr, "[Nova] volume %s: its size changes over the shutter; it is not a light source\n",
                         d.name.c_str());
        if (d.light && !resized) {
            const float k = float(rec.layout.k);
            rec.cellVolume = std::fabs(rec.indexToObject.determinant() * objectToWorld.front().determinant()) * k * k * k;
            std::vector<float> weight(cells);
            double sum = 0.0;
            for (size_t i = 0; i < cells; ++i) {
                weight[i] = std::isfinite(bounds[i].emission) ? std::max(bounds[i].emission, 0.0f) : 0.0f;
                sum += weight[i];
            }
            if (sum > 0.0 && rec.cellVolume > 0.0f) {
                rec.lightCells.build(weight.data(), uint32_t(cells));
                rec.lightPower = float(4.0 * kPi * sum * double(rec.cellVolume));
            }
        }
    }

    // World bounds over the shutter.
    std::vector<Affine3f> iw;
    for (const Affine3f &m : objectToWorld)
        iw.push_back(compose(rec.indexToObject, m));
    mappedBox(iw, rec.layout.lower(), rec.layout.upper(), rec.lower, rec.upper);
}

} // namespace nova
