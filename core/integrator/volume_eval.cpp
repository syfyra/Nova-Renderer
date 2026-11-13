#include "core/integrator/volume_eval.h"

#include "core/shading/vm.h"

#include <algorithm>
#include <cmath>

namespace nova {

VolumeEvaluator::VolumeEvaluator(const Scene &scene, VolumeId id, const ShadeGlobals &globals)
    : m_scene(scene), m_id(id), m_rec(scene.volumeRecord(id)), m_globals(globals) {
    if (m_rec.program)
        m_material = &scene.material(m_rec.material);
    for (const VolumeField &f : m_rec.desc.fields)
        m_samplers.push_back(f.grid ? std::make_unique<VolumeSampler>(*f.grid->grid()) : nullptr);
    m_rows.resize(m_rec.program ? std::max<size_t>(m_rec.program->rowCount, 1) : 1);
}

float VolumeEvaluator::field(int32_t index, Vec3f p) {
    if (index < 0 || size_t(index) >= m_samplers.size() || !m_samplers[size_t(index)])
        return 0.0f;
    return (*m_samplers[size_t(index)])(m_rec.fieldObjectToIndex[size_t(index)].point(p));
}

bool VolumeEvaluator::reaches(Vec3f p) {
    for (const int32_t f : m_rec.maskFields)
        if (size_t(f) < m_samplers.size() && m_samplers[size_t(f)] &&
            m_samplers[size_t(f)]->reaches(m_rec.fieldObjectToIndex[size_t(f)].point(p)))
            return true;
    return false;
}

VolumePoint VolumeEvaluator::operator()(Vec3f p, Vec3f w, Vec3f dir, float time, uint8_t rayType, uint32_t depth) {
    VolumePoint out;
    // Velocity blur by lookup offset (D17): the fields are read at
    // p - v(p) dt, dt the ray's time from the fields'.
    Vec3f lookup = p;
    if (m_rec.velocity[0] >= 0) {
        const float *t = m_rec.desc.velocityTime;
        const float dt = t[0] + time * (t[1] - t[0]);
        lookup = p - Vec3f{field(m_rec.velocity[0], p), field(m_rec.velocity[1], p), field(m_rec.velocity[2], p)} * dt;
    }
    if (!m_rec.program) { // the default volume (D6); a shell's: density 1 (Karma)
        const float d = m_rec.desc.shell ? 1.0f : std::max(field(m_rec.reference, lookup), 0.0f);
        out.scattering = {d, d, d};
        return out;
    }
    // Only where a lookup reaches the fields' voxels (Karma; VolumeRecord::maskFields).
    if (!m_rec.maskFields.empty() && !reaches(lookup))
        return out;
    using namespace shading;
    const Program &prog = *m_rec.program;
    const float *U = m_material->volumeUniforms.data();
    float *R = m_rows.data();
    for (const GlobalBinding &g : prog.globals) {
        Vec3f v{0.0f, 0.0f, 0.0f};
        float s = 0.0f;
        switch (g.global) {
        case Global::P:
        case Global::PDx:
        case Global::PDy:
            v = w;
            break;
        case Global::Pobject:
        case Global::PobjectDx:
        case Global::PobjectDy:
            v = p;
            break;
        case Global::I:
            v = dir;
            break;
        case Global::Time:
            s = (m_globals.frame - 1.0f) / (m_globals.fps > 0.0f ? m_globals.fps : 24.0f);
            break;
        case Global::Frame:
            s = m_globals.frame;
            break;
        case Global::ObjX:
        case Global::ObjY:
        case Global::ObjZ:
        case Global::ObjT: {
            const Affine3f xf = m_scene.volumeToWorld(m_id, time);
            v = g.global == Global::ObjX ? xf.x : g.global == Global::ObjY ? xf.y : g.global == Global::ObjZ ? xf.z : xf.t;
            break;
        }
        case Global::RayType:
            s = float(rayType);
            break;
        case Global::RayDepth:
            s = float(depth);
            break;
        default: // normals and surface parameters mean nothing inside a volume
            break;
        }
        if (globalWidth(g.global) == 3) {
            R[g.row] = v.x;
            R[g.row + 1] = v.y;
            R[g.row + 2] = v.z;
        } else {
            R[g.row] = s;
        }
    }
    for (size_t i = 0; i < prog.primvarBindings.size(); ++i) {
        const PrimvarBinding &b = prog.primvarBindings[i];
        for (int k = 0; k < b.width; ++k) {
            const int32_t f = m_rec.bindings[i][size_t(k)];
            R[b.row + k] = f >= 0 ? field(f, lookup) : (f <= -2 ? m_rec.constantValues[size_t(-2 - f)] : U[b.fallback + k]);
        }
    }
    const TextureContext tex{m_scene.textureSampler(), m_material->samplers.data(),
                             uint32_t(m_material->samplers.size()), nullptr};
    execute<1>(prog.code.data(), prog.code.size(), R, U, &tex);
    VolumeInputs in = m_material->volume;
    for (const OutputBinding &o : prog.outputs)
        if (!o.uniform)
            for (int k = 0; k < o.width; ++k)
                in.v[o.field + k] = R[o.index + k];
    return volumePointOf(in, depth);
}

} // namespace nova
