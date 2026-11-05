#pragma once
// Prototype of core/shading/{ir.h, compiler.cpp}: typed SSA DAG, optimizer
// (constant folding, identities, CSE, DCE, uniform hoisting), Sethi-Ullman
// scheduling, first-fit register allocation, emission of a POD Program.

#include "vm.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace nova::shading {

enum class Type : uint8_t { Float = 1, Vec2 = 2, Vec3 = 3, Vec4 = 4 }; // value = component count
inline int width(Type t) { return int(t); }

// Per-hit inputs the shade stage provides in reserved register rows.
enum class Global : uint8_t { P, N, UV, Count };
inline Type globalType(Global g) { return g == Global::UV ? Type::Vec2 : Type::Vec3; }

struct Ref {
    enum Kind : uint8_t { None, Node, Const, Param, Global } kind = None;
    uint32_t index = 0;
    bool operator==(const Ref &o) const { return kind == o.kind && index == o.index; }
    bool operator<(const Ref &o) const { return std::tie(kind, index) < std::tie(o.kind, o.index); }
};

struct Value { Type type = Type::Float; float v[4] = {0, 0, 0, 0}; };

struct IrNode {
    Op op = Op::Mov;
    Type type = Type::Float;
    Ref in[3];
    uint32_t imm = 0;
};

struct IrParam { std::string name; Value value; };

struct IrOutput { uint32_t id; Ref value; }; // closure parameter id <- value

struct IrGraph {
    std::vector<IrNode> nodes;
    std::vector<Value> consts;
    std::vector<IrParam> params;
    std::vector<IrOutput> outputs;

    Ref constant(Type t, std::initializer_list<float> v) {
        Value val{t};
        int i = 0;
        for (float x : v) val.v[i++] = x;
        consts.push_back(val);
        return {Ref::Const, uint32_t(consts.size() - 1)};
    }
    Ref param(const std::string &name, Value v) { params.push_back({name, v}); return {Ref::Param, uint32_t(params.size() - 1)}; }
    Ref global(Global g) { return {Ref::Global, uint32_t(g)}; }
    Ref node(Op op, Type t, Ref a = {}, Ref b = {}, Ref c = {}, uint32_t imm = 0) {
        nodes.push_back({op, t, {a, b, c}, imm});
        return {Ref::Node, uint32_t(nodes.size() - 1)};
    }
    Type typeOf(Ref r) const {
        switch (r.kind) {
        case Ref::Node: return nodes[r.index].type;
        case Ref::Const: return consts[r.index].type;
        case Ref::Param: return params[r.index].value.type;
        case Ref::Global: return globalType(shading::Global(r.index));
        default: return Type::Float;
        }
    }
};

// Where a closure parameter comes from after compilation.
struct OutputBinding { uint32_t id; bool uniform; uint16_t index; uint8_t width; };

struct UniformInit { enum Kind : uint8_t { Const, Param } kind; uint32_t source; uint16_t slot; uint8_t width; };

// The POD result. Code and layout are fixed for a material structure;
// parameter edits only rewrite `uniforms` and rerun `prologue` (no recompile).
struct Program {
    std::vector<Instr> prologue;  // uniform part: runs once per parameter change, writes uniform slots
    std::vector<Instr> code;      // varying part: runs per batch of hits
    std::vector<UniformInit> uniformInit; // Const: source = offset into constValues; Param: param index
    std::vector<float> constValues;
    uint16_t numUniforms = 0;
    uint16_t numRegs = 0;         // varying register rows
    uint16_t globalRow[size_t(Global::Count)] = {};
    uint32_t globalMask = 0;      // globals the shade stage must provide
    std::vector<OutputBinding> outputs;
    uint64_t structureHash = 0;   // equal hash -> same code, params may differ
};

struct CompileOptions { bool fold = true; bool cse = true; bool hoistUniform = true; };
struct CompileStats { int folded = 0, bypassed = 0, merged = 0, removed = 0, hoisted = 0, peakRows = 0; };

Program compile(const IrGraph &graph, const CompileOptions &opt = {}, CompileStats *stats = nullptr);
// Fills the uniform block from constants/params and runs the prologue.
void evalUniforms(const Program &p, const IrGraph &g, std::vector<float> &uniforms);

} // namespace nova::shading
