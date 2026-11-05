#include "core/shading/ir.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nova::shading {

Ref IrBuilder::node(Op op, int width, Ref a, Ref b, Ref c, uint32_t imm) {
    assert(width >= 1 && width <= 4);
    IrNode n;
    n.op = op;
    n.width = uint8_t(width);
    n.a = a;
    n.b = b;
    n.c = c;
    n.imm = imm;
    m_g.nodes.push_back(n);
    return Ref(m_g.nodes.size() - 1);
}

uint32_t IrBuilder::textureSlot(const TexSlot &slot) {
    m_g.textures.push_back(slot);
    return uint32_t(m_g.textures.size() - 1);
}

Ref IrBuilder::constant(const float *values, int width) {
    const uint32_t offset = uint32_t(m_g.constants.size());
    m_g.constants.insert(m_g.constants.end(), values, values + width);
    return node(Op::Const, width, kNoRef, kNoRef, kNoRef, offset);
}

Ref IrBuilder::param(uint32_t offset, int width) {
    m_g.paramCount = std::max(m_g.paramCount, offset + uint32_t(width));
    return node(Op::Param, width, kNoRef, kNoRef, kNoRef, offset);
}

Ref IrBuilder::newParam(int width, uint32_t *offset) {
    const uint32_t o = m_g.paramCount;
    if (offset)
        *offset = o;
    return param(o, width);
}

Ref IrBuilder::global(Global g) { return node(Op::Global, globalWidth(g), kNoRef, kNoRef, kNoRef, uint32_t(g)); }

Ref IrBuilder::primvar(const std::string &name, int width, Ref fallback) {
    uint32_t index = 0;
    while (index < m_g.primvars.size() && m_g.primvars[index] != name)
        ++index;
    if (index == m_g.primvars.size())
        m_g.primvars.push_back(name);
    return node(Op::Primvar, width, convert(fallback, width), kNoRef, kNoRef, index);
}

Ref IrBuilder::unary(Op op, Ref a) { return node(op, width(a), a); }

Ref IrBuilder::binary(Op op, Ref a, Ref b) {
    const int w = std::max(width(a), width(b));
    assert((width(a) == 1 || width(a) == w) && (width(b) == 1 || width(b) == w));
    return node(op, w, a, b);
}

Ref IrBuilder::ternary(Op op, Ref a, Ref b, Ref c) {
    const int w = std::max({width(a), width(b), width(c)});
    assert((width(a) == 1 || width(a) == w) && (width(b) == 1 || width(b) == w) && (width(c) == 1 || width(c) == w));
    return node(op, w, a, b, c);
}

Ref IrBuilder::dot(Ref a, Ref b) {
    assert(width(a) == width(b));
    return node(Op::Dot, 1, a, b, kNoRef, uint32_t(width(a)));
}

Ref IrBuilder::length(Ref a) { return node(Op::Length, 1, a, kNoRef, kNoRef, uint32_t(width(a))); }

Ref IrBuilder::distance(Ref a, Ref b) {
    assert(width(a) == width(b));
    return node(Op::Distance, 1, a, b, kNoRef, uint32_t(width(a)));
}

Ref IrBuilder::cross(Ref a, Ref b) {
    assert(width(a) == 3 && width(b) == 3);
    return node(Op::Cross, 3, a, b);
}

Ref IrBuilder::swizzle(Ref a, int c0, int c1, int c2, int c3) {
    const int w = c1 < 0 ? 1 : c2 < 0 ? 2 : c3 < 0 ? 3 : 4;
    const int wa = width(a);
    auto clampC = [wa](int c) { return c < 0 ? 0 : std::min(c, wa - 1); };
    return node(Op::Swizzle, w, a, kNoRef, kNoRef, swizzleImm(wa, clampC(c0), clampC(c1), clampC(c2), clampC(c3)));
}

Ref IrBuilder::concat(Ref a, Ref b) {
    assert(width(a) + width(b) <= 4);
    return node(Op::Concat, width(a) + width(b), a, b, kNoRef, uint32_t(width(a)));
}

Ref IrBuilder::convert(Ref a, int w) {
    const int wa = width(a);
    if (wa == w)
        return a;
    if (wa == 1) // splat
        return node(Op::Copy, w, a);
    if (wa > w) // truncate
        return w == 1 ? swizzle(a, 0) : w == 2 ? swizzle(a, 0, 1) : swizzle(a, 0, 1, 2);
    // pad with zeros
    const float zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    return concat(a, constant(zeros, w - wa));
}

void IrBuilder::output(uint16_t field, int w, Ref value) {
    IrOutput o;
    o.field = field;
    o.width = uint8_t(w);
    o.value = convert(value, w);
    for (IrOutput &existing : m_g.outputs)
        if (existing.field == field) {
            existing = o;
            return;
        }
    m_g.outputs.push_back(o);
}

// ---------------------------------------------------------------------------
// Evaluation of single operations (constant folding, reference evaluator)
// ---------------------------------------------------------------------------

void evaluateOp(const IrNode &n, const float *a, int wa, const float *b, int wb, const float *c, int wc,
                float *out) {
    const int w = n.width;
    auto at = [](const float *v, int wv, int k) { return v[wv == 1 ? 0 : k]; };
    switch (opClass(n.op)) {
    case OpClass::Unary:
        for (int k = 0; k < w; ++k)
            out[k] = evalUnary(n.op, at(a, wa, k));
        return;
    case OpClass::Binary:
        for (int k = 0; k < w; ++k)
            out[k] = evalBinary(n.op, at(a, wa, k), at(b, wb, k));
        return;
    case OpClass::Ternary:
        for (int k = 0; k < w; ++k)
            out[k] = evalTernary(n.op, at(a, wa, k), at(b, wb, k), at(c, wc, k));
        return;
    case OpClass::Reduce: {
        const int wr = int(n.imm);
        float s = 0.0f;
        for (int k = 0; k < wr; ++k) {
            if (n.op == Op::Dot)
                s += a[k] * b[k];
            else if (n.op == Op::Length)
                s += a[k] * a[k];
            else
                s += (a[k] - b[k]) * (a[k] - b[k]);
        }
        out[0] = n.op == Op::Dot ? s : std::sqrt(s);
        return;
    }
    default:
        break;
    }
    if (isProcedural(n.op)) {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        evalProcedural(n.op, n.imm, a ? a : zero, b ? b : zero, c ? c : zero, out);
        return;
    }
    if (n.op == Op::Deriv) { // no footprints here
        out[0] = out[1] = 0.0f;
        return;
    }
    if (n.op == Op::Query) { // no scene here: the defaults
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        queryDefault(QueryKind(n.imm), a ? a : zero, out);
        return;
    }
    if (n.op == Op::Tex2D) { // no sampler here: the fallback
        for (int k = 0; k < w; ++k)
            out[k] = c ? at(c, wc, k) : 0.0f;
        return;
    }
    switch (n.op) {
    case Op::Normalize: {
        float s = 0.0f;
        for (int k = 0; k < w; ++k)
            s += a[k] * a[k];
        const float inv = s > 0.0f ? 1.0f / std::sqrt(s) : 0.0f;
        for (int k = 0; k < w; ++k)
            out[k] = a[k] * inv;
        return;
    }
    case Op::Cross:
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
        return;
    case Op::Swizzle:
        for (int k = 0; k < w; ++k)
            out[k] = a[swizzleComponent(n.imm, k)];
        return;
    case Op::Concat: {
        const int wcat = int(n.imm);
        for (int k = 0; k < wcat; ++k)
            out[k] = a[k];
        for (int k = wcat; k < w; ++k)
            out[k] = b[k - wcat];
        return;
    }
    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// Reference evaluator
// ---------------------------------------------------------------------------

void evaluateGraph(const IrGraph &g, const float *params, const float *const *globals,
                   const float *const *primvars, std::vector<float> &v) {
    v.assign(g.nodes.size() * 4, 0.0f);
    auto value = [&](Ref r) { return r == kNoRef ? nullptr : &v[size_t(r) * 4]; };
    auto width = [&](Ref r) { return r == kNoRef ? 0 : int(g.nodes[r].width); };
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const IrNode &n = g.nodes[i];
        float *out = &v[i * 4];
        switch (n.op) {
        case Op::Const:
            for (int k = 0; k < n.width; ++k)
                out[k] = g.constants[n.imm + uint32_t(k)];
            break;
        case Op::Param:
            for (int k = 0; k < n.width; ++k)
                out[k] = params[n.imm + uint32_t(k)];
            break;
        case Op::Global:
            for (int k = 0; k < n.width; ++k)
                out[k] = globals[n.imm][k];
            break;
        case Op::Primvar:
            for (int k = 0; k < n.width; ++k)
                out[k] = primvars && primvars[n.imm] ? primvars[n.imm][k] : value(n.a)[k];
            break;
        default:
            evaluateOp(n, value(n.a), width(n.a), value(n.b), width(n.b), value(n.c), width(n.c), out);
            break;
        }
    }
}

} // namespace nova::shading
