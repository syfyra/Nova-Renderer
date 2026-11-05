#include "compiler.h"

#include <cassert>
#include <stdexcept>
#include <unordered_map>

namespace nova::shading {

namespace {

uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool isCommutative(Op o) { return o == Op::Add || o == Op::Mul || o == Op::Min || o == Op::Max; }

// Evaluates one node on constant inputs with the real op code (as Cycles
// folds with svm_math): folding is then bit-identical to the runtime result.
Value foldNode(const IrNode &n, const Value *in[3]) {
    float U[12] = {}, R[4] = {};
    Instr ins{n.op, uint8_t(width(n.type)), uint8_t(kUniformA | kUniformB | kUniformC), 0, 0, 4, 8, n.imm};
    for (int k = 0; k < 3; ++k) {
        if (!in[k]) continue;
        std::memcpy(U + 4 * k, in[k]->v, sizeof(float) * 4);
        if (componentWise(n.op) && width(in[k]->type) == 1 && width(n.type) > 1) ins.flags |= uint8_t(kSplatA << k);
    }
    if (n.op == Op::Dot) ins.imm = uint32_t(width(in[0]->type));
    const Instr code[2] = {ins, Instr{Op::End}};
    runLane(code, R, U);
    Value v{n.type};
    std::memcpy(v.v, R, sizeof(float) * width(n.type));
    return v;
}

bool isConstValue(const Value &v, float x) {
    for (int k = 0; k < width(v.type); ++k)
        if (v.v[k] != x) return false;
    return true;
}

struct Compiler {
    IrGraph g;
    CompileOptions opt;
    CompileStats st;
    std::vector<Ref> alias; // node -> replacement (kind None: itself)

    Ref resolve(Ref r) const {
        while (r.kind == Ref::Node && alias[r.index].kind != Ref::None) r = alias[r.index];
        return r;
    }

    // ---- 1. constant folding and algebraic identities (structure only:
    // parameters are never folded, so parameter edits never recompile)
    void fold() {
        for (uint32_t i = 0; i < g.nodes.size(); ++i) {
            if (alias[i].kind != Ref::None) continue; // already replaced (CSE or an earlier round)
            IrNode &n = g.nodes[i];
            const int ar = arity(n.op);
            for (int k = 0; k < ar; ++k) n.in[k] = resolve(n.in[k]);
            bool allConst = true;
            const Value *cv[3] = {};
            for (int k = 0; k < ar; ++k) {
                if (n.in[k].kind == Ref::Const) cv[k] = &g.consts[n.in[k].index];
                else allConst = false;
            }
            if (allConst) {
                g.consts.push_back(foldNode(n, cv));
                alias[i] = {Ref::Const, uint32_t(g.consts.size() - 1)};
                ++st.folded;
                continue;
            }
            auto same = [&](Ref r) { return g.typeOf(r) == n.type; };
            auto bypass = [&](Ref r) { alias[i] = r; ++st.bypassed; };
            switch (n.op) {
            case Op::Mul:
                if (cv[1] && isConstValue(*cv[1], 1.0f) && same(n.in[0])) bypass(n.in[0]);
                else if (cv[0] && isConstValue(*cv[0], 1.0f) && same(n.in[1])) bypass(n.in[1]);
                break;
            case Op::Add:
                if (cv[1] && isConstValue(*cv[1], 0.0f) && same(n.in[0])) bypass(n.in[0]);
                else if (cv[0] && isConstValue(*cv[0], 0.0f) && same(n.in[1])) bypass(n.in[1]);
                break;
            case Op::Sub:
                if (cv[1] && isConstValue(*cv[1], 0.0f) && same(n.in[0])) bypass(n.in[0]);
                break;
            case Op::Mix: // mix(bg, fg, t) = bg*(1-t) + fg*t: t == 0 -> bg, t == 1 -> fg are exact; mix(x, x, t) -> x is not
                if (cv[2] && isConstValue(*cv[2], 0.0f) && same(n.in[0])) bypass(n.in[0]);
                else if (cv[2] && isConstValue(*cv[2], 1.0f) && same(n.in[1])) bypass(n.in[1]);
                break;
            case Op::Mov:
                if (same(n.in[0])) bypass(n.in[0]);
                break;
            default: break;
            }
        }
        for (IrOutput &o : g.outputs) o.value = resolve(o.value);
    }

    // ---- 2. common subexpressions (Cycles: ShaderGraph::deduplicate_nodes)
    void cse() {
        std::map<std::tuple<Op, Type, uint32_t, Ref, Ref, Ref>, uint32_t> seen;
        for (uint32_t i = 0; i < g.nodes.size(); ++i) {
            if (alias[i].kind != Ref::None) continue;
            IrNode &n = g.nodes[i];
            for (int k = 0; k < arity(n.op); ++k) n.in[k] = resolve(n.in[k]);
            Ref a = n.in[0], b = n.in[1];
            if (isCommutative(n.op) && b < a) std::swap(a, b);
            auto key = std::make_tuple(n.op, n.type, n.imm, a, b, n.in[2]);
            auto [it, inserted] = seen.emplace(key, i);
            if (!inserted) { alias[i] = {Ref::Node, it->second}; ++st.merged; }
        }
        for (IrOutput &o : g.outputs) o.value = resolve(o.value);
    }
};

} // namespace

Program compile(const IrGraph &graph, const CompileOptions &opt, CompileStats *stats) {
    Compiler c{graph, opt};
    c.alias.assign(c.g.nodes.size(), Ref{});
    // fold -> CSE -> fold: merging can expose identities such as mix(x, x, t)
    if (opt.fold) c.fold();
    if (opt.cse) c.cse();
    if (opt.fold && opt.cse) c.fold();
    IrGraph &g = c.g;
    for (IrNode &n : g.nodes)
        for (int k = 0; k < arity(n.op); ++k) n.in[k] = c.resolve(n.in[k]);
    for (IrOutput &o : g.outputs) o.value = c.resolve(o.value);
    const uint32_t nn = uint32_t(g.nodes.size());

    // ---- 3. dead code: live = reachable from the outputs
    std::vector<uint8_t> live(nn, 0);
    std::function<void(Ref)> mark = [&](Ref r) {
        if (r.kind != Ref::Node || live[r.index]) return;
        live[r.index] = 1;
        for (int k = 0; k < arity(g.nodes[r.index].op); ++k) mark(g.nodes[r.index].in[k]);
    };
    for (const IrOutput &o : g.outputs) mark(o.value);
    for (uint32_t i = 0; i < nn; ++i) c.st.removed += (c.alias[i].kind == Ref::None && !live[i]);

    // ---- 4. uniform analysis: no varying input -> evaluated once per
    // parameter change in the prologue, not per hit
    std::vector<uint8_t> uniform(nn, 0);
    for (uint32_t i = 0; i < nn; ++i) {
        if (!live[i]) continue;
        bool u = opt.hoistUniform;
        for (int k = 0; k < arity(g.nodes[i].op); ++k) {
            const Ref r = g.nodes[i].in[k];
            if (r.kind == Ref::Global || (r.kind == Ref::Node && !uniform[r.index])) u = false;
        }
        uniform[i] = u;
        c.st.hoisted += u;
    }
    auto isVarying = [&](Ref r) { return r.kind == Ref::Global || (r.kind == Ref::Node && !uniform[r.index]); };

    Program p;
    // ---- 5. uniform slots: slot 0..3 = zeros (dummy operand), then consts,
    // params and uniform nodes in use
    std::map<Ref, uint16_t> slot;
    uint16_t nextSlot = 4;
    auto slotOf = [&](Ref r) -> uint16_t {
        auto it = slot.find(r);
        if (it != slot.end()) return it->second;
        const uint16_t s = nextSlot;
        nextSlot = uint16_t(nextSlot + width(g.typeOf(r)));
        slot[r] = s;
        if (r.kind == Ref::Const) {
            p.uniformInit.push_back({UniformInit::Const, uint32_t(p.constValues.size()), s, uint8_t(width(g.typeOf(r)))});
            p.constValues.insert(p.constValues.end(), g.consts[r.index].v, g.consts[r.index].v + width(g.typeOf(r)));
        }
        if (r.kind == Ref::Param) p.uniformInit.push_back({UniformInit::Param, r.index, s, uint8_t(width(g.typeOf(r)))});
        return s;
    };

    auto makeInstr = [&](const IrNode &n, uint16_t dst, const std::function<uint16_t(Ref)> &rowOf) {
        Instr ins{n.op, uint8_t(width(n.type)), 0, dst, 0, 0, 0, n.imm};
        uint16_t *ops[3] = {&ins.a, &ins.b, &ins.c};
        for (int k = 0; k < 3; ++k) {
            const Ref r = k < arity(n.op) ? n.in[k] : Ref{};
            if (r.kind == Ref::None) { ins.flags |= uint8_t(kUniformA << k); *ops[k] = 0; continue; }
            if (isVarying(r)) *ops[k] = rowOf(r);
            else { ins.flags |= uint8_t(kUniformA << k); *ops[k] = slotOf(r); }
            if (componentWise(n.op) && width(g.typeOf(r)) == 1 && width(n.type) > 1) ins.flags |= uint8_t(kSplatA << k);
        }
        if (n.op == Op::Dot) ins.imm = uint32_t(width(g.typeOf(n.in[0])));
        return ins;
    };

    // prologue: uniform nodes in topological (index) order
    for (uint32_t i = 0; i < nn; ++i) {
        if (!live[i] || !uniform[i]) continue;
        const uint16_t dst = slotOf({Ref::Node, i});
        p.prologue.push_back(makeInstr(g.nodes[i], dst, [](Ref) -> uint16_t { throw std::logic_error("varying in prologue"); }));
    }
    p.prologue.push_back(Instr{Op::End});

    // ---- 6. schedule varying nodes, Sethi-Ullman order (Cycles:
    // SVMCompiler::generate_svm_nodes): producers needing the most rows first
    std::vector<int> need(nn, -1);
    std::function<int(uint32_t)> sethiUllman = [&](uint32_t i) -> int {
        if (need[i] >= 0) return need[i];
        std::vector<uint32_t> prod;
        for (int k = 0; k < arity(g.nodes[i].op); ++k) {
            const Ref r = g.nodes[i].in[k];
            if (r.kind == Ref::Node && !uniform[r.index] && std::find(prod.begin(), prod.end(), r.index) == prod.end())
                prod.push_back(r.index);
        }
        for (uint32_t q : prod) sethiUllman(q);
        std::sort(prod.begin(), prod.end(), [&](uint32_t a, uint32_t b) {
            return need[a] - width(g.nodes[a].type) > need[b] - width(g.nodes[b].type);
        });
        int held = 0, peak = 0;
        for (uint32_t q : prod) { peak = std::max(peak, held + need[q]); held += width(g.nodes[q].type); }
        need[i] = std::max(peak, held + width(g.nodes[i].type));
        return need[i];
    };
    std::vector<uint32_t> order;
    std::vector<uint8_t> scheduled(nn, 0);
    std::function<void(uint32_t)> schedule = [&](uint32_t i) {
        if (scheduled[i]) return;
        scheduled[i] = 1;
        std::vector<uint32_t> prod;
        for (int k = 0; k < arity(g.nodes[i].op); ++k) {
            const Ref r = g.nodes[i].in[k];
            if (r.kind == Ref::Node && !uniform[r.index]) prod.push_back(r.index);
        }
        std::sort(prod.begin(), prod.end(), [&](uint32_t a, uint32_t b) {
            return need[a] - width(g.nodes[a].type) > need[b] - width(g.nodes[b].type);
        });
        for (uint32_t q : prod) schedule(q);
        order.push_back(i);
    };
    std::vector<uint32_t> sinks;
    for (const IrOutput &o : g.outputs)
        if (o.value.kind == Ref::Node && !uniform[o.value.index]) sinks.push_back(o.value.index);
    for (uint32_t s : sinks) sethiUllman(s);
    std::sort(sinks.begin(), sinks.end(), [&](uint32_t a, uint32_t b) { return need[a] > need[b]; });
    for (uint32_t s : sinks) schedule(s);

    // ---- 7. liveness (last use) and first-fit register allocation over
    // contiguous rows (Cycles: SVMCompiler::stack_find_offset)
    constexpr int kInf = 1 << 30;
    std::map<Ref, int> lastUse;
    for (int pos = 0; pos < int(order.size()); ++pos) {
        const IrNode &n = g.nodes[order[pos]];
        for (int k = 0; k < arity(n.op); ++k)
            if (isVarying(n.in[k])) lastUse[n.in[k]] = pos;
    }
    for (const IrOutput &o : g.outputs)
        if (isVarying(o.value)) lastUse[o.value] = kInf;

    std::vector<uint8_t> used(1024, 0);
    auto alloc = [&](int w) -> uint16_t {
        for (int r = 0, run = 0; r < int(used.size()); ++r) {
            run = used[r] ? 0 : run + 1;
            if (run == w) {
                for (int k = r - w + 1; k <= r; ++k) used[k] = 1;
                p.numRegs = std::max<uint16_t>(p.numRegs, uint16_t(r + 1));
                return uint16_t(r - w + 1);
            }
        }
        throw std::runtime_error("shader too large: out of register rows");
    };
    std::map<Ref, uint16_t> row;
    for (int gi = 0; gi < int(Global::Count); ++gi) {
        const Ref r{Ref::Global, uint32_t(gi)};
        if (lastUse.count(r)) { // the shade stage writes these rows before running
            row[r] = alloc(width(globalType(Global(gi))));
            p.globalRow[gi] = row[r];
            p.globalMask |= 1u << gi;
        }
    }
    for (int pos = 0; pos < int(order.size()); ++pos) {
        const uint32_t i = order[pos];
        const IrNode &n = g.nodes[i];
        // The destination never aliases an input: ops may read inputs after
        // writing the first output component (Cycles keeps the same rule).
        const uint16_t dst = alloc(width(n.type));
        row[{Ref::Node, i}] = dst;
        p.code.push_back(makeInstr(n, dst, [&](Ref r) { return row.at(r); }));
        Ref freed[3] = {};
        for (int k = 0; k < arity(n.op); ++k) {
            const Ref r = n.in[k];
            if (!isVarying(r) || lastUse[r] != pos || std::find(freed, freed + 3, r) != freed + 3) continue;
            freed[k] = r;
            for (int q = 0; q < width(g.typeOf(r)); ++q) used[row[r] + q] = 0;
        }
    }
    p.code.push_back(Instr{Op::End});

    for (const IrOutput &o : g.outputs) {
        if (isVarying(o.value)) p.outputs.push_back({o.id, false, row.at(o.value), uint8_t(width(g.typeOf(o.value)))});
        else p.outputs.push_back({o.id, true, slotOf(o.value), uint8_t(width(g.typeOf(o.value)))});
    }
    p.numUniforms = nextSlot;

    // ---- 8. structure hash: code, layout and bindings, not values
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, p.prologue.data(), p.prologue.size() * sizeof(Instr));
    h = fnv(h, p.code.data(), p.code.size() * sizeof(Instr));
    for (const UniformInit &u : p.uniformInit) {
        const uint32_t key[3] = {u.kind, u.slot, u.width}; // Const values may differ only if folded differently -> code differs
        h = fnv(h, key, sizeof key);
        if (u.kind == UniformInit::Const) h = fnv(h, &p.constValues[u.source], sizeof(float) * u.width);
    }
    p.structureHash = h;
    c.st.peakRows = p.numRegs;
    if (stats) *stats = c.st;
    return p;
}

void evalUniforms(const Program &p, const IrGraph &g, std::vector<float> &u) {
    u.assign(p.numUniforms, 0.0f);
    for (const UniformInit &i : p.uniformInit) {
        const float *src = i.kind == UniformInit::Param ? g.params[i.source].value.v : &p.constValues[i.source];
        std::memcpy(&u[i.slot], src, sizeof(float) * i.width);
    }
    runLane(p.prologue.data(), u.data(), u.data());
}

} // namespace nova::shading
