#include "core/shading/compiler.h"

#include "core/shading/vm.h"
#include "core/volume/shader.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace nova::shading {

namespace {

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <class T> void value(const T &v) { bytes(&v, sizeof(T)); }
};

// The optimized graph: operands refer to its own nodes.
struct Canon {
    std::vector<IrNode> nodes;
    std::vector<float> values; // node values of Const nodes (4 per node)
    std::unordered_map<std::string, Ref> keys;

    // Per field: IrNode has a padding byte after width that nothing
    // initializes, so its raw bytes would make CSE depend on stack contents.
    static std::string key(const IrNode &n, const float *constValues) {
        const uint32_t imm = n.op == Op::Const ? 0u : n.imm; // the values identify a constant, not its pool offset
        std::string k;
        k.reserve(sizeof(n.op) + sizeof(n.width) + 4 * sizeof(uint32_t) + (constValues ? 4 * sizeof(float) : 0));
        auto append = [&k](const void *p, size_t size) { k.append(static_cast<const char *>(p), size); };
        append(&n.op, sizeof(n.op));
        append(&n.width, sizeof(n.width));
        append(&n.a, sizeof(n.a));
        append(&n.b, sizeof(n.b));
        append(&n.c, sizeof(n.c));
        append(&imm, sizeof(imm));
        if (constValues)
            append(constValues, 4 * sizeof(float));
        return k;
    }

    Ref add(const IrNode &n, const float *constValues, bool dedupe) {
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (constValues)
            for (int k = 0; k < n.width; ++k)
                v[k] = constValues[k];
        std::string k;
        if (dedupe) {
            k = key(n, constValues ? v : nullptr);
            const auto it = keys.find(k);
            if (it != keys.end())
                return it->second;
        }
        nodes.push_back(n);
        values.insert(values.end(), v, v + 4);
        const Ref r = Ref(nodes.size() - 1);
        if (dedupe)
            keys.emplace(std::move(k), r);
        return r;
    }
    bool isConst(Ref r) const { return r != kNoRef && nodes[r].op == Op::Const; }
    bool allEqual(Ref r, float x) const {
        if (!isConst(r))
            return false;
        for (int k = 0; k < nodes[r].width; ++k)
            if (values[size_t(r) * 4 + size_t(k)] != x)
                return false;
        return true;
    }
};

bool perComponent(Op op) {
    const OpClass c = opClass(op);
    return c == OpClass::Unary || c == OpClass::Binary || c == OpClass::Ternary;
}

} // namespace

void Program::initUniforms(const float *params, std::vector<float> &U) const {
    U.assign(uniformCount, 0.0f);
    std::copy(constants.begin(), constants.end(), U.begin());
    for (uint32_t i = 0; i < paramCount; ++i)
        U[paramOffset + i] = params ? params[i] : 0.0f;
    if (!prologue.empty())
        executePrologue(*this, U.data());
}

CompileResult compile(const IrGraph &graph, const CompileOptions &options) {
    // The volume outputs go into their own program, at the volume block's
    // offsets (docs/design/M8.md D4); the rest never computes them.
    bool hasVolume = false;
    if (options.volumeProgram)
        for (const IrOutput &o : graph.outputs)
            hasVolume = hasVolume || (volume_field::isVolumeOutput(o.field) && o.value != kNoRef);
    if (hasVolume) {
        IrGraph rest = graph, volume = graph;
        rest.outputs.clear();
        volume.outputs.clear();
        for (const IrOutput &o : graph.outputs) {
            if (!volume_field::isVolumeOutput(o.field)) {
                rest.outputs.push_back(o);
                continue;
            }
            IrOutput v = o;
            v.field = uint16_t(o.field - volume_field::kBase);
            volume.outputs.push_back(v);
        }
        volume.closure = nullptr;
        volume.aovs.clear();
        volume.model = SurfaceModel::Lambert;
        if (options.volumeParams) {
            const std::vector<float> &values = *options.volumeParams;
            for (IrNode &n : volume.nodes) {
                if (n.op != Op::Param)
                    continue;
                const uint32_t offset = uint32_t(volume.constants.size());
                for (int k = 0; k < n.width; ++k)
                    volume.constants.push_back(n.imm + k < values.size() ? values[n.imm + k] : 0.0f);
                n.op = Op::Const;
                n.imm = offset;
            }
        }
        CompileOptions vopts = options;
        vopts.opacityProgram = vopts.displacementProgram = vopts.volumeProgram = false;
        vopts.derivatives = false; // no texture footprints inside volumes
        CompileResult v = compile(volume, vopts);
        if (!v.program) {
            v.error = "volume: " + v.error;
            return v;
        }
        CompileOptions ropts = options;
        ropts.volumeProgram = false;
        CompileResult r = compile(rest, ropts);
        if (!r.program)
            return r;
        auto p = std::make_shared<Program>(*r.program);
        p->volume = v.program;
        p->structureHash = (p->structureHash ^ (v.program->structureHash + 0x7f4a7c159e3779b9ull)) * 1099511628211ull;
        r.program = std::move(p);
        if (r.warning.empty())
            r.warning = v.warning;
        return r;
    }
    // The displacement output goes into its own program; the surface program
    // never computes it.
    bool displaces = false;
    if (options.displacementProgram)
        for (const IrOutput &o : graph.outputs)
            displaces = displaces || (o.field == surface_field::kDisplacement && o.value != kNoRef);
    if (displaces) {
        IrGraph surface = graph, displacement = graph;
        surface.outputs.clear();
        displacement.outputs.clear();
        for (const IrOutput &o : graph.outputs)
            (o.field == surface_field::kDisplacement ? displacement : surface).outputs.push_back(o);
        displacement.closure = nullptr;
        CompileOptions dopts = options;
        dopts.opacityProgram = false;
        dopts.displacementProgram = false;
        dopts.gpuRowWarning = false; // displacement runs on the host only
        CompileResult d = compile(displacement, dopts);
        if (!d.program) {
            d.error = "displacement: " + d.error;
            return d;
        }
        CompileOptions sopts = options;
        sopts.displacementProgram = false;
        CompileResult r = compile(surface, sopts);
        if (!r.program)
            return r;
        auto p = std::make_shared<Program>(*r.program);
        p->displacement = d.program;
        p->structureHash = (p->structureHash ^ (d.program->structureHash + 0x9e3779b97f4a7c15ull)) * 1099511628211ull;
        r.program = std::move(p);
        if (r.warning.empty())
            r.warning = d.warning;
        return r;
    }
    const IrGraph &g = graph;
    CompileResult result;
    const size_t n = g.nodes.size();

    // ---- Reachability from the outputs ------------------------------------
    std::vector<uint8_t> reachable(n, 0);
    {
        std::vector<Ref> stack;
        for (const IrOutput &o : g.outputs)
            if (o.value != kNoRef)
                stack.push_back(o.value);
        while (!stack.empty()) {
            const Ref r = stack.back();
            stack.pop_back();
            if (r >= n || reachable[r])
                continue;
            reachable[r] = 1;
            for (Ref x : {g.nodes[r].a, g.nodes[r].b, g.nodes[r].c})
                if (x != kNoRef)
                    stack.push_back(x);
        }
    }

    std::vector<std::pair<uint32_t, uint8_t>> paramReads;
    for (size_t i = 0; i < n; ++i)
        if (reachable[i] && g.nodes[i].op == Op::Param)
            paramReads.emplace_back(g.nodes[i].imm, g.nodes[i].width);

    // ---- Canonicalize: fold, identities, CSE ------------------------------
    Canon cn;
    std::vector<Ref> remap(n, kNoRef);
    // Uniformity of the canonical nodes, as they are added: constants and
    // parameters are uniform, hit data and texture lookups vary, operations
    // are uniform when all their operands are.
    std::vector<uint8_t> cu;
    auto addCanon = [&](const IrNode &node, const float *constValues, bool dedupe) {
        const Ref r = cn.add(node, constValues, dedupe);
        if (r == cu.size()) {
            bool u;
            if (node.op == Op::Const || node.op == Op::Param)
                u = true;
            else if (node.op == Op::Global || node.op == Op::Primvar || node.op == Op::Tex2D || node.op == Op::Query)
                u = false;
            else {
                u = true;
                for (Ref x : {node.a, node.b, node.c})
                    u = u && (x == kNoRef || cu[x]);
            }
            cu.push_back(uint8_t(u));
        }
        return r;
    };
    // The varying cone of r with P, Pobject and primvars moved by footprint
    // vector `axis` (0, 1); uniform nodes and other globals are shared.
    std::unordered_map<uint64_t, Ref> shiftMemo;
    std::function<Ref(Ref, int)> shifted = [&](Ref r, int axis) -> Ref {
        if (r == kNoRef || cu[r])
            return r;
        const uint64_t key = (uint64_t(r) << 1) | uint64_t(axis);
        if (const auto it = shiftMemo.find(key); it != shiftMemo.end())
            return it->second;
        IrNode x = cn.nodes[r];
        Ref out = r;
        if (x.op == Op::Query) {
            // a query's result is the base value's (no derivatives)
        } else if (x.op == Op::Global) {
            const Global gl = Global(x.imm);
            if (gl == Global::P || gl == Global::Pobject) {
                x.imm = uint32_t(gl == Global::P ? (axis ? Global::PDy : Global::PDx)
                                                 : (axis ? Global::PobjectDy : Global::PobjectDx));
                out = addCanon(x, nullptr, true);
            }
        } else if (x.op == Op::Primvar) {
            x.imm = (x.imm & kPrimvarIndexMask) | (uint32_t(axis + 1) << kPrimvarShiftBit);
            out = addCanon(x, nullptr, true);
        } else {
            x.a = shifted(x.a, axis);
            if (x.op != Op::Tex2D) // a lookup's own derivatives stay those of its base
                x.b = shifted(x.b, axis);
            x.c = shifted(x.c, axis);
            out = addCanon(x, nullptr, options.optimize);
        }
        shiftMemo.emplace(key, out);
        return out;
    };
    for (size_t i = 0; i < n; ++i) {
        if (!reachable[i])
            continue;
        IrNode node = g.nodes[i];
        node.a = node.a != kNoRef ? remap[node.a] : kNoRef;
        node.b = node.b != kNoRef ? remap[node.b] : kNoRef;
        node.c = node.c != kNoRef ? remap[node.c] : kNoRef;
        switch (node.op) {
        case Op::Const:
            remap[i] = addCanon(node, &g.constants[node.imm], true);
            continue;
        case Op::Param:
        case Op::Global:
        case Op::Primvar:
            remap[i] = addCanon(node, nullptr, true);
            continue;
        case Op::Deriv: {
            IrNode d;
            d.op = Op::Sub;
            d.width = 1;
            if (options.derivatives && !cu[node.a]) {
                d.a = shifted(node.a, 0);
                d.b = node.a;
                const Ref dx = addCanon(d, nullptr, options.optimize);
                d.a = shifted(node.a, 1);
                const Ref dy = addCanon(d, nullptr, options.optimize);
                IrNode cat;
                cat.op = Op::Concat;
                cat.width = 2;
                cat.a = dx;
                cat.b = dy;
                cat.imm = 1;
                remap[i] = addCanon(cat, nullptr, options.optimize);
            } else {
                IrNode z;
                z.op = Op::Const;
                z.width = 2;
                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                remap[i] = addCanon(z, zero, true);
            }
            continue;
        }
        case Op::Tex2D:
            if (options.derivatives && node.b == kNoRef && !cu[node.a]) {
                const Ref ux = shifted(node.a, 0), uy = shifted(node.a, 1);
                if (ux != node.a || uy != node.a) {
                    IrNode d;
                    d.op = Op::Sub;
                    d.width = 2;
                    d.a = ux;
                    d.b = node.a;
                    const Ref dx = addCanon(d, nullptr, options.optimize);
                    d.a = uy;
                    const Ref dy = addCanon(d, nullptr, options.optimize);
                    IrNode cat;
                    cat.op = Op::Concat;
                    cat.width = 4;
                    cat.a = dx;
                    cat.b = dy;
                    cat.imm = 2;
                    node.b = addCanon(cat, nullptr, options.optimize); // (dsdx, dtdx, dsdy, dtdy)
                }
            }
            break;
        default:
            break;
        }
        if (options.optimize && node.op != Op::Tex2D && node.op != Op::Query) {
            const int count = operandCount(node.op);
            bool allConst = true;
            for (int k = 0; k < count; ++k)
                allConst = allConst && cn.isConst(k == 0 ? node.a : k == 1 ? node.b : node.c);
            if (allConst) {
                float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                auto val = [&](Ref r) { return r != kNoRef ? &cn.values[size_t(r) * 4] : nullptr; };
                auto wid = [&](Ref r) { return r != kNoRef ? int(cn.nodes[r].width) : 0; };
                evaluateOp(node, val(node.a), wid(node.a), val(node.b), wid(node.b), val(node.c), wid(node.c), out);
                IrNode c;
                c.op = Op::Const;
                c.width = node.width;
                remap[i] = addCanon(c, out, true);
                continue;
            }
            // Exact identities only.
            auto sameWidth = [&](Ref r) { return int(cn.nodes[r].width) == node.width; };
            if (node.op == Op::Mul && cn.allEqual(node.b, 1.0f) && sameWidth(node.a)) {
                remap[i] = node.a;
                continue;
            }
            if (node.op == Op::Mul && cn.allEqual(node.a, 1.0f) && sameWidth(node.b)) {
                remap[i] = node.b;
                continue;
            }
            if (node.op == Op::Div && cn.allEqual(node.b, 1.0f) && sameWidth(node.a)) {
                remap[i] = node.a;
                continue;
            }
            if (node.op == Op::Copy && sameWidth(node.a)) {
                remap[i] = node.a;
                continue;
            }
            // A switch with a known selector (folded parameters, M8.1): its branch.
            if (node.op == Op::Select && cn.isConst(node.a)) {
                const float *c = &cn.values[size_t(node.a) * 4];
                bool allTrue = true, allFalse = true;
                for (int k = 0; k < int(cn.nodes[node.a].width); ++k) {
                    allTrue = allTrue && c[k] != 0.0f;
                    allFalse = allFalse && c[k] == 0.0f;
                }
                const Ref pick = allTrue ? node.b : allFalse ? node.c : kNoRef;
                if (pick != kNoRef && sameWidth(pick)) {
                    remap[i] = pick;
                    continue;
                }
            }
            if (node.op == Op::Swizzle && sameWidth(node.a)) {
                bool identity = true;
                for (int k = 0; k < node.width; ++k)
                    identity = identity && swizzleComponent(node.imm, k) == k;
                if (identity) {
                    remap[i] = node.a;
                    continue;
                }
            }
        }
        remap[i] = addCanon(node, nullptr, options.optimize);
    }

    const size_t m = cn.nodes.size();
    const std::vector<uint8_t> &uniform = cu;

    // ---- Liveness of the canonical graph ----------------------------------
    std::vector<uint8_t> live(m, 0);
    {
        std::vector<Ref> stack;
        for (const IrOutput &o : g.outputs)
            if (o.value != kNoRef)
                stack.push_back(remap[o.value]);
        while (!stack.empty()) {
            const Ref r = stack.back();
            stack.pop_back();
            if (live[r])
                continue;
            live[r] = 1;
            for (Ref x : {cn.nodes[r].a, cn.nodes[r].b, cn.nodes[r].c})
                if (x != kNoRef)
                    stack.push_back(x);
        }
    }

    auto program = std::make_shared<Program>();
    program->paramReads = std::move(paramReads);
    Program &p = *program;
    p.model = g.model;
    p.primvars = g.primvars;
    p.textures = g.textures;

    // ---- Uniform block: constants | params | hoisted ----------------------
    std::vector<uint32_t> uslot(m, ~0u);
    for (size_t i = 0; i < m; ++i) {
        if (live[i] && cn.nodes[i].op == Op::Const) {
            uslot[i] = uint32_t(p.constants.size());
            for (int k = 0; k < cn.nodes[i].width; ++k)
                p.constants.push_back(cn.values[i * 4 + size_t(k)]);
        }
    }
    p.paramOffset = uint32_t(p.constants.size());
    p.paramCount = g.paramCount;
    uint32_t nextUniform = p.paramOffset + p.paramCount;
    for (size_t i = 0; i < m; ++i) {
        if (!live[i])
            continue;
        if (cn.nodes[i].op == Op::Param)
            uslot[i] = p.paramOffset + cn.nodes[i].imm;
    }
    auto emitOperands = [&](const IrNode &x, Instr &in, bool prologue, const std::vector<uint32_t> &row) {
        const Ref refs[3] = {x.a, x.b, x.c};
        uint16_t *slots[3] = {&in.a, &in.b, &in.c};
        const uint8_t uflag[3] = {kUniformA, kUniformB, kUniformC}, sflag[3] = {kSplatA, kSplatB, kSplatC};
        for (int k = 0; k < 3; ++k) {
            const Ref r = refs[k];
            if (r == kNoRef) {
                *slots[k] = 0;
                continue;
            }
            if (uniform[r]) {
                *slots[k] = uint16_t(uslot[r]);
                if (!prologue)
                    in.flags |= uflag[k];
            } else {
                *slots[k] = uint16_t(row[r]);
            }
            if (perComponent(x.op) && cn.nodes[r].width == 1 && x.width > 1)
                in.flags |= sflag[k];
        }
    };
    const std::vector<uint32_t> noRows;
    for (size_t i = 0; i < m; ++i) {
        const IrNode &x = cn.nodes[i];
        if (!live[i] || !uniform[i] || x.op == Op::Const || x.op == Op::Param)
            continue;
        uslot[i] = nextUniform;
        nextUniform += x.width;
        Instr in{};
        in.op = x.op;
        in.width = x.width;
        in.imm = x.imm;
        in.dst = uint16_t(uslot[i]);
        emitOperands(x, in, true, noRows);
        p.prologue.push_back(in);
    }
    p.uniformCount = nextUniform;
    if (p.uniformCount > 0xFFFFu) {
        result.error = "too many uniform values";
        return result;
    }

    // ---- Schedule the varying code (Sethi-Ullman order) -------------------
    auto isVaryingOp = [&](Ref r) {
        return r != kNoRef && !uniform[r] && cn.nodes[r].op != Op::Global && cn.nodes[r].op != Op::Primvar;
    };
    std::vector<int> need(m, 0);
    for (size_t i = 0; i < m; ++i) {
        if (!live[i] || !isVaryingOp(Ref(i)))
            continue;
        int children[3], count = 0;
        for (Ref r : {cn.nodes[i].a, cn.nodes[i].b, cn.nodes[i].c})
            if (isVaryingOp(r))
                children[count++] = need[r];
        std::sort(children, children + count, [](int a, int b) { return a > b; });
        int nd = cn.nodes[i].width;
        for (int k = 0; k < count; ++k)
            nd = std::max(nd, children[k] + k);
        need[i] = nd;
    }
    std::vector<Ref> schedule;
    std::vector<uint8_t> visited(m, 0);
    std::vector<std::pair<Ref, int>> stack; // node, state (0: enter, 1: exit)
    auto visit = [&](Ref root) {
        stack.push_back({root, 0});
        while (!stack.empty()) {
            const auto [r, state] = stack.back();
            stack.pop_back();
            if (state == 1) {
                schedule.push_back(r);
                continue;
            }
            if (!isVaryingOp(r) || visited[r])
                continue;
            visited[r] = 1;
            stack.push_back({r, 1});
            Ref kids[3];
            int count = 0;
            for (Ref x : {cn.nodes[r].a, cn.nodes[r].b, cn.nodes[r].c})
                if (isVaryingOp(x) && !visited[x])
                    kids[count++] = x;
            // Pushed in reverse: the most demanding child runs first.
            std::sort(kids, kids + count, [&](Ref a, Ref b) { return need[a] < need[b]; });
            for (int k = 0; k < count; ++k)
                stack.push_back({kids[k], 0});
        }
    };
    for (const IrOutput &o : g.outputs)
        if (o.value != kNoRef)
            visit(remap[o.value]);

    // ---- Rows: globals and primvars first, then first fit -----------------
    constexpr size_t kForever = ~size_t(0);
    std::vector<size_t> lastUse(m, 0);
    std::vector<uint8_t> usedByCode(m, 0);
    for (size_t s = 0; s < schedule.size(); ++s)
        for (Ref x : {cn.nodes[schedule[s]].a, cn.nodes[schedule[s]].b, cn.nodes[schedule[s]].c})
            if (x != kNoRef) {
                lastUse[x] = s;
                usedByCode[x] = 1;
            }
    std::vector<uint8_t> isOutput(m, 0);
    for (const IrOutput &o : g.outputs)
        if (o.value != kNoRef) {
            lastUse[remap[o.value]] = kForever;
            isOutput[remap[o.value]] = 1;
        }

    std::vector<uint8_t> used;
    auto allocate = [&](int width) {
        size_t start = 0;
        for (;; ++start) {
            bool fits = true;
            for (int k = 0; k < width; ++k)
                fits = fits && (start + size_t(k) >= used.size() || !used[start + size_t(k)]);
            if (fits)
                break;
        }
        if (used.size() < start + size_t(width))
            used.resize(start + size_t(width), 0);
        for (int k = 0; k < width; ++k)
            used[start + size_t(k)] = 1;
        return uint32_t(start);
    };
    auto release = [&](uint32_t row, int width) {
        for (int k = 0; k < width; ++k)
            used[row + uint32_t(k)] = 0;
    };

    std::vector<uint32_t> row(m, ~0u);
    for (size_t i = 0; i < m; ++i) {
        const IrNode &x = cn.nodes[i];
        if (!live[i] || (x.op != Op::Global && x.op != Op::Primvar))
            continue;
        row[i] = allocate(x.width);
        if (x.op == Op::Global) {
            const Global gl = Global(x.imm);
            p.globals.push_back({gl, uint16_t(row[i])});
            p.needsFootprints |= gl == Global::PDx || gl == Global::PDy || gl == Global::PobjectDx ||
                                 gl == Global::PobjectDy;
            if ((gl == Global::DPdu || gl == Global::DPdv) && p.tangentUv < 0) {
                const std::string uvName = g.tangentUv.empty() ? std::string("st") : g.tangentUv;
                const auto it = std::find(p.primvars.begin(), p.primvars.end(), uvName);
                p.tangentUv = int32_t(it - p.primvars.begin());
                if (it == p.primvars.end())
                    p.primvars.push_back(uvName); // bound like a primvar, read at the corners
            }
        } else {
            const uint32_t index = x.imm & kPrimvarIndexMask;
            if (x.a == kNoRef || !uniform[x.a]) {
                result.error = "the fallback of primvar '" + g.primvars[index] + "' is not uniform";
                return result;
            }
            p.primvarBindings.push_back(
                {index, x.width, uint8_t(x.imm >> kPrimvarShiftBit), uint16_t(row[i]), uint16_t(uslot[x.a])});
            p.needsFootprints |= (x.imm >> kPrimvarShiftBit) != 0;
        }
    }

    for (size_t s = 0; s < schedule.size(); ++s) {
        const Ref r = schedule[s];
        const IrNode &x = cn.nodes[r];
        row[r] = allocate(x.width);
        Instr in{};
        in.op = x.op;
        in.width = x.width;
        in.imm = x.imm;
        in.dst = uint16_t(row[r]);
        emitOperands(x, in, false, row);
        if (x.op == Op::Tex2D && x.b != kNoRef)
            in.flags |= kHasB;
        p.needsQueries |= x.op == Op::Query;
        p.code.push_back(in);
        Ref done[3];
        int count = 0;
        for (Ref o : {x.a, x.b, x.c})
            if (o != kNoRef && !uniform[o] && lastUse[o] == s && std::find(done, done + count, o) == done + count)
                done[count++] = o;
        for (int k = 0; k < count; ++k)
            release(row[done[k]], cn.nodes[done[k]].width);
    }
    p.rowCount = uint32_t(used.size());
    if (p.rowCount > options.maxRows) {
        result.error = "the program needs " + std::to_string(p.rowCount) + " rows (limit " +
                       std::to_string(options.maxRows) + ")";
        return result;
    }
    if (options.gpuRowWarning && p.rowCount > kGpuRowLimit)
        result.warning = "the program needs " + std::to_string(p.rowCount) + " rows, more than the GPU limit of " +
                         std::to_string(kGpuRowLimit) + "; XPU uses its uniform inputs";

    for (const IrOutput &o : g.outputs) {
        if (o.value == kNoRef)
            continue;
        const Ref r = remap[o.value];
        OutputBinding b;
        b.field = o.field;
        b.width = o.width;
        b.uniform = uniform[r] != 0;
        b.index = uint16_t(b.uniform ? uslot[r] : row[r]);
        p.outputs.push_back(b);
    }

    // ---- Structure hash ---------------------------------------------------
    Hasher h;
    h.value(p.model);
    for (const Instr &in : p.code)
        h.value(in);
    h.value(uint32_t(0xC0DEu));
    for (const Instr &in : p.prologue)
        h.value(in);
    for (float c : p.constants)
        h.value(c);
    h.value(p.paramCount);
    h.value(p.rowCount);
    for (const GlobalBinding &b : p.globals) { // per field: the structs have padding
        h.value(b.global);
        h.value(b.row);
    }
    for (const PrimvarBinding &b : p.primvarBindings) {
        h.value(b.width);
        h.value(b.shift);
        h.value(b.row);
        h.value(b.fallback);
        h.bytes(p.primvars[b.name].data(), p.primvars[b.name].size());
    }
    for (const OutputBinding &b : p.outputs) {
        h.value(b.field);
        h.value(b.width);
        h.value(b.uniform);
        h.value(b.index);
    }
    h.value(p.tangentUv);
    if (p.tangentUv >= 0)
        h.bytes(p.primvars[size_t(p.tangentUv)].data(), p.primvars[size_t(p.tangentUv)].size());
    for (const TexSlot &t : p.textures) {
        h.value(t.wrapS);
        h.value(t.wrapT);
        h.value(t.filter);
        h.value(t.channels);
    }
    p.closure = g.closure;
    p.aovs = g.aovs;
    for (const auto &[name, field] : p.aovs) {
        h.bytes(name.data(), name.size());
        h.value(field);
    }
    if (p.closure)
        h.bytes(p.closure.get(), sizeof(ClosureGraph)); // POD, value-initialized (no indeterminate padding)
    p.structureHash = h.h;

    // The opacity-only program: the same graph with only the opacity outputs
    // (dead code removed), no footprints (the filter has none).
    if (options.opacityProgram) {
        IrGraph og;
        bool any = false;
        for (const IrOutput &o : g.outputs)
            any |= o.value != kNoRef && (o.field == surface_field::opacity || o.field == surface_field::opacityThreshold);
        if (any) {
            og = g;
            og.outputs.clear();
            for (const IrOutput &o : g.outputs)
                if (o.field == surface_field::opacity || o.field == surface_field::opacityThreshold)
                    og.outputs.push_back(o);
            CompileOptions oo = options;
            oo.derivatives = false;
            oo.opacityProgram = false;
            const CompileResult r = compile(og, oo);
            if (r.program && !r.program->varyingFree())
                p.opacity = r.program;
        }
    }

    result.program = std::move(program);
    return result;
}

} // namespace nova::shading
