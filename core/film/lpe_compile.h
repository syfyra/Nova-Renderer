#pragma once

// Compiles light path expressions into one automaton (docs/design/M6.md D2,
// D3; docs/research/m6/lpe-design.md 2.1). Host code, no Houdini.
//
// The dialect is OSL's with Karma's changes:
//   events     C R T V L O B A (camera, reflection, transmission, volume,
//              light, emissive object, background, albedo), scattering
//              D G S s (Karma emits no S/s), custom labels in quotes;
//   <...>      one event by position: <type scatter 'tag' 'label'>, each
//              position a symbol, '.', a quoted name or a set [..]/[^..];
//   shorthands L = <L...>, D = <.D..>, 'name' = <..'name'>;
//   operators  . (any event; never A or B), * + ? {n} {n,m} {n,}, |, ( ),
//              [..] (any of the listed events), [^..] (none of them);
//   prefixes   unoccluded; shadow; holdout; (holdouts;) and ! before the
//              expression; holdout; outputs accept the paths whose camera
//              ray hit a holdout (an internal event, TypeHoldout, before the
//              camera), the others only the remaining paths.
//
// Construction: an NFA per expression (Thompson), the concrete events
// grouped into classes that no expression tells apart, subset construction
// over those classes, Moore minimization, then classes with equal columns
// merged.

#include "core/film/lpe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nova::lpe {

// One concrete event, as the renderer produces it (tags and labels by name;
// the empty name is "untagged"/"unlabelled").
struct Event {
    Type type = TypeCamera;
    Scatter scatter = ScatterNone;
    std::string tag, label;
};

struct Compiled {
    // Per input expression: its output id (the input index), or an error.
    std::vector<std::string> errors;     // empty: compiled
    std::vector<uint8_t> outputFlags;    // OutputFlag per expression
    bool ok() const { return stateCount > 0; }

    uint32_t stateCount = 0, classCount = 0;
    State start = 0;
    State holdoutStart = 0; // after the holdout and camera events (Program::holdoutStart)
    std::vector<std::string> tags{""}, labels{""}; // id -> name; 0 = untagged/unlabelled
    std::vector<State> next;
    std::vector<uint16_t> symbolClass;
    std::vector<uint32_t> outBegin;
    std::vector<uint16_t> outs;

    Program program() const;
    uint32_t tagId(std::string const &name) const;   // 0 if no expression names it
    uint32_t labelId(std::string const &name) const; // 0 if no expression names it
    // The state after `events` (which start with the camera, or for a path
    // that hit a holdout with the holdout event and the camera), from scratch.
    State run(std::vector<Event> const &events) const;
    // shadow; outputs keep a second sum (Film::lpeLit): the output's slot
    // there, -1 for other outputs; and the number of slots.
    int32_t shadowSlot(size_t output) const;
    uint32_t shadowCount() const;
};

struct Limits {
    uint32_t maxStates = 65535;
    size_t maxTableBytes = size_t(1) << 20;
};

// Compiles `expressions` together. Expressions that do not parse get an
// error and no output; an automaton beyond `limits` fails every expression.
Compiled compile(std::vector<std::string> const &expressions, Limits const &limits = {});

// Reference matcher for tests: whether expression `text` accepts exactly
// `events`, by backtracking over the parsed expression (not through the
// automaton); holdout; expressions accept the paths that start with the
// holdout event (then the camera), the others only paths without it. False
// with `error` if it does not parse.
bool matches(std::string const &text, std::vector<Event> const &events, std::string *error = nullptr);

} // namespace nova::lpe
