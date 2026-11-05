// Tests of the prototype: compiler unit tests + differential testing of the
// compiled bytecode (per-lane and batch executors) against a tree-walking
// evaluator of the unoptimized IR.
#include "compiler.h"

#include <chrono>
#include <cstdio>
#include <random>

using namespace nova::shading;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Lane { float P[3], N[3], UV[2]; };

// ---- reference: recursive evaluation of the original graph, one lane
struct RefEval {
    const IrGraph &g;
    const Lane &lane;
    std::vector<int> done;
    std::vector<Value> memo;
    RefEval(const IrGraph &graph, const Lane &l) : g(graph), lane(l), done(graph.nodes.size(), 0), memo(graph.nodes.size()) {}
    Value eval(Ref r) {
        Value v;
        switch (r.kind) {
        case Ref::Const: return g.consts[r.index];
        case Ref::Param: return g.params[r.index].value;
        case Ref::Global: {
            v.type = globalType(Global(r.index));
            const float *src = r.index == 0 ? lane.P : r.index == 1 ? lane.N : lane.UV;
            for (int k = 0; k < width(v.type); ++k) v.v[k] = src[k];
            return v;
        }
        case Ref::Node: break;
        default: return v;
        }
        if (done[r.index]) return memo[r.index];
        const IrNode &n = g.nodes[r.index];
        Value in[3];
        for (int k = 0; k < arity(n.op); ++k) in[k] = eval(n.in[k]);
        v.type = n.type;
        auto c = [&](int k, int comp) { return width(in[k].type) == 1 ? in[k].v[0] : in[k].v[comp]; };
        switch (n.op) {
#define REF_CW(o) case Op::o: for (int k = 0; k < width(n.type); ++k) v.v[k] = Fn<Op::o>::eval(c(0, k), arity(n.op) > 1 ? c(1, k) : 0.0f, arity(n.op) > 2 ? c(2, k) : 0.0f); break;
        REF_CW(Mov) REF_CW(Add) REF_CW(Sub) REF_CW(Mul) REF_CW(Div) REF_CW(Mad) REF_CW(Mix) REF_CW(Clamp) REF_CW(Min)
        REF_CW(Max) REF_CW(Floor) REF_CW(Fract) REF_CW(Abs) REF_CW(Sqrt) REF_CW(Sin) REF_CW(Gt) REF_CW(Select)
#undef REF_CW
        case Op::Dot: { float s = 0; for (int k = 0; k < width(in[0].type); ++k) s += in[0].v[k] * in[1].v[k]; v.v[0] = s; } break;
        case Op::Normalize: {
            float l2 = 0; for (int k = 0; k < width(n.type); ++k) l2 += in[0].v[k] * in[0].v[k];
            const float s = l2 > 0.0f ? 1.0f / std::sqrt(l2) : 0.0f;
            for (int k = 0; k < width(n.type); ++k) v.v[k] = in[0].v[k] * s;
        } break;
        case Op::Extract: v.v[0] = in[0].v[n.imm]; break;
        case Op::Combine3: v.v[0] = in[0].v[0]; v.v[1] = in[1].v[0]; v.v[2] = in[2].v[0]; break;
        default: break;
        }
        done[r.index] = 1;
        memo[r.index] = v;
        return v;
    }
};

// ---- random typed DAGs
IrGraph randomGraph(std::mt19937 &rng, int numNodes, int numOutputs) {
    IrGraph g;
    auto U = [&](int n) { return int(rng() % uint32_t(n)); };
    auto F = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    std::vector<Ref> pool;
    for (int i = 0; i < 6; ++i) {
        const Type t = Type(1 + U(4));
        Value v{t};
        for (int k = 0; k < 4; ++k) v.v[k] = F(-2, 2);
        Ref r = U(2) ? g.constant(t, {v.v[0], v.v[1], v.v[2], v.v[3]}) : g.param("p" + std::to_string(i), v);
        pool.push_back(r);
    }
    // special constants that trigger identities
    pool.push_back(g.constant(Type::Float, {1.0f}));
    pool.push_back(g.constant(Type::Float, {0.0f}));
    pool.push_back(g.global(Global::P));
    pool.push_back(g.global(Global::N));
    pool.push_back(g.global(Global::UV));
    auto pick = [&](Type t, bool allowSplat) -> Ref {
        for (int tries = 0; tries < 64; ++tries) {
            Ref r = pool[pool.size() - 1 - U(int(std::min<size_t>(pool.size(), 24)))]; // prefer recent values: deeper graphs
            if (U(3) == 0) r = pool[U(int(pool.size()))];
            const Type rt = g.typeOf(r);
            if (rt == t || (allowSplat && rt == Type::Float)) return r;
        }
        // fallback: mostly varying values, so graphs keep per-hit work after hoisting
        switch (U(4) ? t : Type::Vec4) {
        case Type::Vec3: return g.global(U(2) ? Global::P : Global::N);
        case Type::Vec2: return g.global(Global::UV);
        case Type::Float: return g.node(Op::Extract, Type::Float, g.global(Global::P), {}, {}, uint32_t(U(3)));
        default: return g.constant(t, {F(-1, 1), F(-1, 1), F(-1, 1), F(-1, 1)});
        }
    };
    const Op cw[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mad, Op::Mix, Op::Clamp, Op::Min, Op::Max, Op::Floor,
                     Op::Fract, Op::Abs, Op::Sqrt, Op::Sin, Op::Gt, Op::Select, Op::Mov};
    for (int i = 0; i < numNodes; ++i) {
        if (i % 3 == 0) { const int gsel = U(3); pool.push_back(g.global(Global(gsel))); }
        const int kind = U(10);
        Ref r;
        if (kind < 7) {
            const Op o = cw[U(int(sizeof cw / sizeof cw[0]))];
            const Type t = Type(1 + U(4));
            Ref in[3];
            for (int k = 0; k < arity(o); ++k) in[k] = pick(t, k > 0 || U(4) == 0);
            if (width(g.typeOf(in[0])) == 1 && width(t) > 1 && arity(o) == 1) in[0] = pick(t, false);
            r = g.node(o, t, in[0], in[1], in[2]);
        } else if (kind == 7) {
            const Type t = Type(2 + U(3));
            r = g.node(U(2) ? Op::Dot : Op::Normalize, U(2) ? Type::Float : t, pick(t, false), pick(t, false));
            if (g.nodes.back().op == Op::Normalize) g.nodes.back().type = t, g.nodes.back().in[1] = {};
            else g.nodes.back().type = Type::Float;
        } else if (kind == 8) {
            const Type t = Type(2 + U(3));
            r = g.node(Op::Extract, Type::Float, pick(t, false), {}, {}, uint32_t(U(width(t))));
        } else {
            r = g.node(Op::Combine3, Type::Vec3, pick(Type::Float, true), pick(Type::Float, true), pick(Type::Float, true));
        }
        pool.push_back(r);
    }
    for (int o = 0; o < numOutputs; ++o) g.outputs.push_back({uint32_t(o), pool[pool.size() - 1 - U(std::min<int>(int(pool.size()), 12))]});
    return g;
}

bool sameBits(float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

template <int N>
void runProgramBatch(const Program &p, const std::vector<float> &U, const std::vector<Lane> &lanes, std::vector<float> &out) {
    // out: per lane, concatenated outputs
    int outW = 0;
    for (auto &b : p.outputs) outW += b.width;
    out.assign(lanes.size() * outW, 0.0f);
    std::vector<float> R(size_t(std::max<int>(p.numRegs, 1)) * N);
    for (size_t base = 0; base < lanes.size(); base += N) {
        const int count = int(std::min<size_t>(N, lanes.size() - base));
        for (int l = 0; l < N; ++l) {
            const Lane &ln = lanes[base + std::min(l, count - 1)]; // pad the partial chunk with a valid lane
            for (int k = 0; k < 3; ++k) {
                if (p.globalMask & 1) R[(p.globalRow[0] + k) * N + l] = ln.P[k];
                if (p.globalMask & 2) R[(p.globalRow[1] + k) * N + l] = ln.N[k];
            }
            for (int k = 0; k < 2; ++k)
                if (p.globalMask & 4) R[(p.globalRow[2] + k) * N + l] = ln.UV[k];
        }
        runBatch<N>(p.code.data(), R.data(), U.data());
        for (int l = 0; l < count; ++l) {
            int o = 0;
            for (auto &b : p.outputs)
                for (int k = 0; k < b.width; ++k)
                    out[(base + l) * outW + o++] = b.uniform ? U[b.index + k] : R[(b.index + k) * N + l];
        }
    }
}

void runProgramLane(const Program &p, const std::vector<float> &U, const std::vector<Lane> &lanes, std::vector<float> &out) {
    int outW = 0;
    for (auto &b : p.outputs) outW += b.width;
    out.assign(lanes.size() * outW, 0.0f);
    std::vector<float> R(std::max<int>(p.numRegs, 1));
    for (size_t l = 0; l < lanes.size(); ++l) {
        for (int k = 0; k < 3; ++k) {
            if (p.globalMask & 1) R[p.globalRow[0] + k] = lanes[l].P[k];
            if (p.globalMask & 2) R[p.globalRow[1] + k] = lanes[l].N[k];
        }
        for (int k = 0; k < 2; ++k)
            if (p.globalMask & 4) R[p.globalRow[2] + k] = lanes[l].UV[k];
        runLane(p.code.data(), R.data(), U.data());
        int o = 0;
        for (auto &b : p.outputs)
            for (int k = 0; k < b.width; ++k) out[l * outW + o++] = b.uniform ? U[b.index + k] : R[b.index + k];
    }
}

void refOutputs(const IrGraph &g, const std::vector<Lane> &lanes, std::vector<float> &out) {
    out.clear();
    for (const Lane &l : lanes) {
        RefEval e(g, l);
        for (const IrOutput &o : g.outputs) {
            const Value v = e.eval(o.value);
            for (int k = 0; k < width(v.type); ++k) out.push_back(v.v[k]);
        }
    }
}

int main() {
    std::mt19937 rng(12345);
    auto F = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    // ---------------- 1. differential testing on random graphs
    {
        int graphs = 0, mismatches = 0, totalInstr = 0, totalRegs = 0, totalHoisted = 0;
        for (int gi = 0; gi < 3000; ++gi) {
            IrGraph g = randomGraph(rng, gi % 3 == 0 ? 100 + int(rng() % 300) : 5 + int(rng() % 60), 1 + int(rng() % 6));
            std::vector<Lane> lanes(45); // not a multiple of the batch width
            for (Lane &l : lanes) {
                for (float &x : l.P) x = F(-3, 3);
                for (float &x : l.N) x = F(-1, 1);
                for (float &x : l.UV) x = F(0, 1);
            }
            std::vector<float> ref, outL, out8, out32, outNoOpt;
            refOutputs(g, lanes, ref);
            CompileStats st;
            const Program p = compile(g, {}, &st);
            const Program q = compile(g, {false, false, false});
            std::vector<float> Up, Uq;
            evalUniforms(p, g, Up);
            evalUniforms(q, g, Uq);
            runProgramLane(p, Up, lanes, outL);
            runProgramBatch<8>(p, Up, lanes, out8);
            runProgramBatch<32>(p, Up, lanes, out32);
            runProgramBatch<32>(q, Uq, lanes, outNoOpt);
            bool ok = ref.size() == outL.size();
            for (size_t i = 0; ok && i < ref.size(); ++i)
                ok = sameBits(ref[i], outL[i]) && sameBits(ref[i], out8[i]) && sameBits(ref[i], out32[i]) && sameBits(ref[i], outNoOpt[i]);
            if (!ok && mismatches++ < 5) {
                for (size_t i = 0; i < ref.size(); ++i)
                    if (!sameBits(ref[i], outL[i]) || !sameBits(ref[i], out32[i]) || !sameBits(ref[i], outNoOpt[i])) {
                        std::printf("graph %d value %zu: ref %.9g lane %.9g b8 %.9g b32 %.9g noopt %.9g\n", gi, i, ref[i], outL[i], out8[i], out32[i], outNoOpt[i]);
                        break;
                    }
            }
            ++graphs;
            totalInstr += int(p.code.size()) - 1;
            totalRegs += p.numRegs;
            totalHoisted += st.hoisted;
            static double sumNodes = 0, sumRemoved = 0, sumFolded = 0, sumMerged = 0;
            sumNodes += double(g.nodes.size()); sumRemoved += st.removed; sumFolded += st.folded + st.bypassed; sumMerged += st.merged;
            if (gi == 2999) std::printf("mean nodes %.1f removed %.1f folded+bypassed %.1f merged %.1f\n", sumNodes / 3000, sumRemoved / 3000, sumFolded / 3000, sumMerged / 3000);
        }
        CHECK(mismatches == 0, "%d of %d random graphs differ", mismatches, graphs);
        std::printf("differential: %d graphs x 45 lanes, mismatches %d; mean varying instr %.1f, mean rows %.1f, mean hoisted nodes %.1f\n",
                    graphs, mismatches, double(totalInstr) / graphs, double(totalRegs) / graphs, double(totalHoisted) / graphs);
    }

    // ---------------- 2. register reuse
    {
        IrGraph g; // chain: x = ((P.x + 1) * 0.5 + ...) 200 times
        Ref x = g.node(Op::Extract, Type::Float, g.global(Global::P), {}, {}, 0);
        for (int i = 0; i < 200; ++i) x = g.node(i % 2 ? Op::Mul : Op::Add, Type::Float, x, g.global(Global::UV).kind == Ref::Global ? g.node(Op::Extract, Type::Float, g.global(Global::UV), {}, {}, i % 2) : x);
        g.outputs.push_back({0, x});
        const Program p = compile(g);
        std::printf("chain of 400 ops: %zu instr, %d rows (globals P,UV = 5 rows)\n", p.code.size() - 1, p.numRegs);
        CHECK(p.numRegs <= 5 + 3, "rows %d", p.numRegs);

        IrGraph t; // balanced binary tree of adds over sin(P.x * leafconst), depth 7 (128 leaves)
        std::function<Ref(int)> build = [&](int d) -> Ref {
            if (d == 0) return t.node(Op::Sin, Type::Float, t.node(Op::Mul, Type::Float, t.node(Op::Extract, Type::Float, t.global(Global::P), {}, {}, 0), t.constant(Type::Float, {F(0.5f, 2.0f)})));
            return t.node(Op::Add, Type::Float, build(d - 1), build(d - 1));
        };
        t.outputs.push_back({0, build(7)});
        CompileStats st;
        const Program q = compile(t, {}, &st);
        std::printf("balanced tree depth 7: %zu instr, %d rows (P = 3 rows; Sethi-Ullman bound depth+1 = 8 scalar rows), merged %d\n", q.code.size() - 1, q.numRegs, st.merged);
        CHECK(q.numRegs <= 3 + 8 + 1, "rows %d", q.numRegs);
    }

    // ---------------- 3. folding, identities, CSE, DCE
    {
        IrGraph g;
        Ref six = g.node(Op::Mul, Type::Float, g.constant(Type::Float, {2}), g.constant(Type::Float, {3}));
        Ref px = g.node(Op::Extract, Type::Float, g.global(Global::P), {}, {}, 0);
        Ref a = g.node(Op::Add, Type::Float, six, px);
        Ref b = g.node(Op::Mul, Type::Float, a, g.constant(Type::Float, {1}));               // x * 1 -> x
        Ref s1 = g.node(Op::Sin, Type::Float, b), s2 = g.node(Op::Sin, Type::Float, b);      // CSE
        Ref m = g.node(Op::Mix, Type::Float, s1, s2, g.constant(Type::Float, {0.0f}));       // mix(x, x, 0) -> x
        g.node(Op::Sqrt, Type::Float, m);                                                    // dead
        g.outputs.push_back({0, m});
        CompileStats st;
        const Program p = compile(g, {}, &st);
        std::printf("fold test: folded %d bypassed %d merged %d removed %d -> %zu varying instr\n", st.folded, st.bypassed, st.merged, st.removed, p.code.size() - 1);
        CHECK(p.code.size() - 1 == 3, "expected Extract, Add(6), Sin; got %zu", p.code.size() - 1);
    }

    // ---------------- 4. uniform hoisting and parameter edits without recompiling
    {
        IrGraph g;
        Ref k = g.param("gain", Value{Type::Float, {2.0f}});
        Ref tint = g.param("tint", Value{Type::Vec3, {0.8f, 0.5f, 0.2f}});
        Ref u = g.node(Op::Mul, Type::Vec3, tint, g.node(Op::Sin, Type::Float, k)); // uniform: tint * sin(gain)
        Ref col = g.node(Op::Mul, Type::Vec3, u, g.global(Global::N));            // varying
        g.outputs.push_back({0, col});
        g.outputs.push_back({1, u});
        const Program p = compile(g);
        std::vector<float> U;
        evalUniforms(p, g, U);
        std::printf("hoist test: prologue %zu instr, varying %zu instr\n", p.prologue.size() - 1, p.code.size() - 1);
        CHECK(p.prologue.size() - 1 == 2 && p.code.size() - 1 == 1, "prologue %zu code %zu", p.prologue.size() - 1, p.code.size() - 1);
        IrGraph g2 = g;
        g2.params[0].value.v[0] = 0.5f;
        const Program p2 = compile(g2);
        CHECK(p2.structureHash == p.structureHash, "param edit must keep the structure hash");
        evalUniforms(p, g2, U); // same program, new parameter values
        const float expect = 0.8f * std::sin(0.5f);
        CHECK(U[p.outputs[1].index] == expect, "%g vs %g", U[p.outputs[1].index], expect);
        IrGraph g3 = g;
        g3.nodes[1].op = Op::Abs;
        CHECK(compile(g3).structureHash != p.structureHash, "op change must change the structure hash");
    }

    // ---------------- 5. throughput of the batch executor on a random graph
    {
        std::mt19937 r2(7);
        IrGraph g = randomGraph(r2, 400, 4);
        const Program p = compile(g);
        std::vector<float> U;
        evalUniforms(p, g, U);
        std::vector<Lane> lanes(1 << 14);
        for (Lane &l : lanes) { for (float &x : l.P) x = F(-3, 3); for (float &x : l.N) x = F(-1, 1); for (float &x : l.UV) x = F(0, 1); }
        std::vector<float> out;
        using C = std::chrono::high_resolution_clock;
        auto t0 = C::now();
        for (int it = 0; it < 10; ++it) runProgramBatch<32>(p, U, lanes, out);
        auto t1 = C::now();
        for (int it = 0; it < 2; ++it) runProgramLane(p, U, lanes, out);
        auto t2 = C::now();
        const double nb = std::chrono::duration<double, std::nano>(t1 - t0).count() / (10.0 * lanes.size() * (p.code.size() - 1));
        const double nl = std::chrono::duration<double, std::nano>(t2 - t1).count() / (2.0 * lanes.size() * (p.code.size() - 1));
        std::printf("random 400-node graph: %zu varying instr, %d rows; batch32 %.3f ns/(instr*lane), per-lane %.3f ns/(instr*lane) (incl. global load + output gather)\n",
                    p.code.size() - 1, p.numRegs, nb, nl);
    }
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
