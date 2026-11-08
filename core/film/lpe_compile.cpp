#include "core/film/lpe_compile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace nova::lpe {

namespace {

// --- Parsed expressions -----------------------------------------------------

// One position of an event: anything, or (not) one of `names` (symbols as
// one-character strings, quoted names without their quotes).
struct Slot {
    bool any = true;
    bool negated = false;
    std::vector<std::string> names;
};

enum Position { PosType, PosScatter, PosTag, PosLabel, kPositions };

struct Spec {
    Slot slot[kPositions];
};

struct Node {
    enum Kind { One, AnyOf, NoneOf, Concat, Alt, Repeat } kind = Concat;
    std::vector<Spec> specs;                     // One (1), AnyOf, NoneOf
    std::vector<std::unique_ptr<Node>> children; // Concat, Alt, Repeat (1)
    int min = 0, max = -1;                       // Repeat; -1: unbounded
    int pred = -1;                               // single-event nodes: predicate index
};

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr const char *kTypeSymbols = "CRTVLOBA"; // in Type order
constexpr const char *kScatterSymbols = "DGSsx"; // in Scatter order
constexpr int kMaxRepeat = 32;

bool isType(char c) { return std::strchr(kTypeSymbols, c) != nullptr && c != '\0'; }
bool isScatter(char c) { return std::strchr(kScatterSymbols, c) != nullptr && c != '\0'; }

class Parser {
public:
    explicit Parser(std::string text) : _s(std::move(text)) {}

    std::unique_ptr<Node> parse() {
        auto root = alternation();
        skip();
        if (_i < _s.size())
            fail(std::string("unexpected '") + _s[_i] + "'");
        return root;
    }

private:
    [[noreturn]] void fail(std::string const &what) const {
        throw ParseError(what + " at position " + std::to_string(_i + 1));
    }
    void skip() {
        while (_i < _s.size() && std::isspace(static_cast<unsigned char>(_s[_i])))
            ++_i;
    }
    char peek() {
        skip();
        return _i < _s.size() ? _s[_i] : '\0';
    }
    char take() {
        const char c = peek();
        if (c != '\0')
            ++_i;
        return c;
    }
    void expect(char c) {
        if (take() != c)
            fail(std::string("expected '") + c + "'");
    }

    std::unique_ptr<Node> alternation() {
        auto first = concatenation();
        if (peek() != '|')
            return first;
        auto alt = std::make_unique<Node>();
        alt->kind = Node::Alt;
        alt->children.push_back(std::move(first));
        while (peek() == '|') {
            take();
            alt->children.push_back(concatenation());
        }
        return alt;
    }

    std::unique_ptr<Node> concatenation() {
        auto cat = std::make_unique<Node>();
        cat->kind = Node::Concat;
        for (char c = peek(); c != '\0' && c != '|' && c != ')'; c = peek())
            cat->children.push_back(repetition());
        return cat; // may be empty (matches the empty path)
    }

    std::unique_ptr<Node> repetition() {
        auto atom = this->atom();
        for (;;) {
            const char c = peek();
            int min = 0, max = -1;
            if (c == '*') {
                take();
            } else if (c == '+') {
                take();
                min = 1;
            } else if (c == '?') {
                take();
                max = 1;
            } else if (c == '{') {
                take();
                min = number();
                if (peek() == ',') {
                    take();
                    max = peek() == '}' ? -1 : number();
                } else {
                    max = min;
                }
                expect('}');
                if (max >= 0 && max < min)
                    fail("a repeat count {n,m} with m < n");
                if (min > kMaxRepeat || max > kMaxRepeat)
                    fail("a repeat count above " + std::to_string(kMaxRepeat));
            } else {
                return atom;
            }
            auto rep = std::make_unique<Node>();
            rep->kind = Node::Repeat;
            rep->min = min;
            rep->max = max;
            rep->children.push_back(std::move(atom));
            atom = std::move(rep);
        }
    }

    int number() {
        skip();
        if (_i >= _s.size() || !std::isdigit(static_cast<unsigned char>(_s[_i])))
            fail("expected a number");
        int n = 0;
        while (_i < _s.size() && std::isdigit(static_cast<unsigned char>(_s[_i])) && n < 100000)
            n = n * 10 + (_s[_i++] - '0');
        return n;
    }

    std::string quoted() {
        expect('\'');
        const size_t end = _s.find('\'', _i);
        if (end == std::string::npos)
            fail("an unterminated quoted name");
        std::string name = _s.substr(_i, end - _i);
        _i = end + 1;
        if (name.empty())
            fail("an empty quoted name");
        return name;
    }

    static std::unique_ptr<Node> single(Node::Kind kind, std::vector<Spec> specs) {
        auto n = std::make_unique<Node>();
        n->kind = kind;
        n->specs = std::move(specs);
        return n;
    }

    // A bare symbol, '.' or quoted name at the top level or in a set.
    Spec shorthand() {
        Spec spec;
        const char c = peek();
        if (c == '\'') {
            spec.slot[PosTag] = Slot{false, false, {quoted()}};
        } else if (c == '.') {
            take();
        } else if (isType(c)) {
            take();
            spec.slot[PosType] = Slot{false, false, {std::string(1, c)}};
        } else if (isScatter(c) && c != 'x') {
            take();
            spec.slot[PosScatter] = Slot{false, false, {std::string(1, c)}};
        } else {
            fail(c == '\0' ? std::string("an unexpected end") : std::string("unknown symbol '") + c + "'");
        }
        return spec;
    }

    Spec event() {
        expect('<');
        Spec spec;
        int pos = 0;
        for (char c = peek(); c != '>'; c = peek()) {
            if (c == '\0')
                fail("an unterminated <...>");
            if (pos >= kPositions)
                fail("more than four positions in <...>");
            Slot &slot = spec.slot[pos];
            if (c == '.') {
                take();
            } else if (c == '[') {
                take();
                slot.any = false;
                if (peek() == '^') {
                    take();
                    slot.negated = true;
                }
                while (peek() != ']') {
                    if (peek() == '\0')
                        fail("an unterminated [...]");
                    slot.names.push_back(name(pos));
                }
                take();
            } else {
                slot.any = false;
                slot.names.push_back(name(pos));
            }
            ++pos;
        }
        take();
        return spec;
    }

    // A name valid at `pos` of an event.
    std::string name(int pos) {
        const char c = peek();
        if (pos >= PosTag) {
            if (c != '\'')
                fail("a tag or label must be quoted");
            return quoted();
        }
        if (c == '\'')
            fail(pos == PosType ? "a quoted name in the event type position" : "a quoted name in the scatter position");
        take();
        if (pos == PosType ? !isType(c) : !isScatter(c))
            fail(std::string("'") + c + (pos == PosType ? "' is no event type" : "' is no scatter type"));
        return std::string(1, c);
    }

    std::unique_ptr<Node> atom() {
        const char c = peek();
        if (c == '(') {
            take();
            auto inner = alternation();
            expect(')');
            return inner;
        }
        if (c == '<')
            return single(Node::One, {event()});
        if (c == '[') {
            take();
            bool negated = false;
            if (peek() == '^') {
                take();
                negated = true;
            }
            std::vector<Spec> specs;
            while (peek() != ']') {
                if (peek() == '\0')
                    fail("an unterminated [...]");
                specs.push_back(peek() == '<' ? event() : shorthand());
            }
            take();
            if (specs.empty())
                fail("an empty set []");
            return single(negated ? Node::NoneOf : Node::AnyOf, std::move(specs));
        }
        return single(Node::One, {shorthand()});
    }

    std::string _s;
    size_t _i = 0;
};

// Prefixes and negation in front of the expression.
uint8_t stripPrefixes(std::string &text) {
    uint8_t flags = 0;
    for (bool again = true; again;) {
        again = false;
        size_t i = 0;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        text.erase(0, i);
        if (!text.empty() && text[0] == '!') {
            flags |= OutputInvert;
            text.erase(0, 1);
            again = true;
            continue;
        }
        for (auto [word, flag] : {std::pair{"unoccluded;", OutputUnoccluded}, std::pair{"shadow;", OutputShadow},
                                  std::pair{"holdouts;", OutputHoldout}, std::pair{"holdout;", OutputHoldout}}) {
            const size_t n = std::char_traits<char>::length(word);
            if (text.compare(0, n, word) == 0) {
                flags |= flag;
                text.erase(0, n);
                again = true;
                break;
            }
        }
    }
    return flags;
}

// --- Event predicates ----------------------------------------------------------

bool slotAccepts(Slot const &slot, std::string const &value) {
    if (slot.any)
        return true;
    const bool in = std::find(slot.names.begin(), slot.names.end(), value) != slot.names.end();
    return slot.negated ? !in : in;
}

// `.` and negations at the type position never match A or B (D4), nor the
// internal holdout event (no expression names it).
bool specAccepts(Spec const &spec, Type type, Scatter scatter, std::string const &tag, std::string const &label) {
    if (type == TypeHoldout)
        return false;
    const std::string t(1, kTypeSymbols[type]);
    const Slot &ts = spec.slot[PosType];
    if ((ts.any || ts.negated) && (type == TypeAlbedo || type == TypeBackground))
        return false;
    return slotAccepts(ts, t) && slotAccepts(spec.slot[PosScatter], std::string(1, kScatterSymbols[scatter])) &&
           slotAccepts(spec.slot[PosTag], tag) && slotAccepts(spec.slot[PosLabel], label);
}

bool nodeAccepts(Node const &n, Type type, Scatter scatter, std::string const &tag, std::string const &label) {
    switch (n.kind) {
    case Node::One:
        return specAccepts(n.specs[0], type, scatter, tag, label);
    case Node::AnyOf:
        for (Spec const &s : n.specs)
            if (specAccepts(s, type, scatter, tag, label))
                return true;
        return false;
    case Node::NoneOf:
        if (type == TypeAlbedo || type == TypeBackground || type == TypeHoldout)
            return false;
        for (Spec const &s : n.specs)
            if (specAccepts(s, type, scatter, tag, label))
                return false;
        return true;
    default:
        return false;
    }
}

bool isSingle(Node const &n) { return n.kind == Node::One || n.kind == Node::AnyOf || n.kind == Node::NoneOf; }

void collect(Node &n, std::vector<Node *> &singles, std::set<std::string> &tags, std::set<std::string> &labels) {
    if (isSingle(n)) {
        n.pred = int(singles.size());
        singles.push_back(&n);
        for (Spec const &s : n.specs) {
            for (std::string const &name : s.slot[PosTag].names)
                tags.insert(name);
            for (std::string const &name : s.slot[PosLabel].names)
                labels.insert(name);
        }
        return;
    }
    for (auto &c : n.children)
        collect(*c, singles, tags, labels);
}

// --- Thompson NFA -----------------------------------------------------------------

struct Nfa {
    std::vector<std::vector<int>> eps;
    std::vector<std::vector<std::pair<int, int>>> moves; // (predicate, target)
    std::vector<int> accept;                             // output id, -1

    int add() {
        eps.emplace_back();
        moves.emplace_back();
        accept.push_back(-1);
        return int(eps.size()) - 1;
    }

    // Returns (entry, exit) of a fragment for `n`.
    std::pair<int, int> build(Node const &n) {
        if (isSingle(n)) {
            const int a = add(), b = add();
            moves[a].push_back({n.pred, b});
            return {a, b};
        }
        if (n.kind == Node::Concat) {
            const int a = add();
            int cur = a;
            for (auto const &c : n.children) {
                auto [s, e] = build(*c);
                eps[cur].push_back(s);
                cur = e;
            }
            return {a, cur};
        }
        if (n.kind == Node::Alt) {
            const int a = add(), b = add();
            for (auto const &c : n.children) {
                auto [s, e] = build(*c);
                eps[a].push_back(s);
                eps[e].push_back(b);
            }
            return {a, b};
        }
        // Repeat: min copies, then (max - min) optional copies or a loop.
        Node const &inner = *n.children[0];
        const int a = add();
        int cur = a;
        for (int k = 0; k < n.min; ++k) {
            auto [s, e] = build(inner);
            eps[cur].push_back(s);
            cur = e;
        }
        if (n.max < 0) {
            auto [s, e] = build(inner);
            const int out = add();
            eps[cur].push_back(s);
            eps[cur].push_back(out);
            eps[e].push_back(s);
            eps[e].push_back(out);
            return {a, out};
        }
        const int out = add();
        for (int k = n.min; k < n.max; ++k) {
            auto [s, e] = build(inner);
            eps[cur].push_back(out);
            eps[cur].push_back(s);
            cur = e;
        }
        eps[cur].push_back(out);
        return {a, out};
    }

    std::vector<int> closure(std::vector<int> set) const {
        std::vector<char> seen(eps.size(), 0);
        std::vector<int> stack = set;
        for (int s : set)
            seen[size_t(s)] = 1;
        while (!stack.empty()) {
            const int s = stack.back();
            stack.pop_back();
            for (int t : eps[size_t(s)])
                if (!seen[size_t(t)]) {
                    seen[size_t(t)] = 1;
                    set.push_back(t);
                    stack.push_back(t);
                }
        }
        std::sort(set.begin(), set.end());
        return set;
    }
};

// --- Reference matcher (tests) -------------------------------------------------

std::set<size_t> ends(Node const &n, std::vector<Event> const &ev, size_t i) {
    if (isSingle(n)) {
        if (i < ev.size() && nodeAccepts(n, ev[i].type, ev[i].scatter, ev[i].tag, ev[i].label))
            return {i + 1};
        return {};
    }
    if (n.kind == Node::Concat) {
        std::set<size_t> cur{i};
        for (auto const &c : n.children) {
            std::set<size_t> next;
            for (size_t p : cur)
                for (size_t q : ends(*c, ev, p))
                    next.insert(q);
            cur = std::move(next);
        }
        return cur;
    }
    if (n.kind == Node::Alt) {
        std::set<size_t> out;
        for (auto const &c : n.children)
            for (size_t q : ends(*c, ev, i))
                out.insert(q);
        return out;
    }
    std::set<size_t> cur{i}, result;
    if (n.min == 0)
        result.insert(i);
    for (int k = 1; n.max < 0 ? k <= int(ev.size()) + 1 : k <= n.max; ++k) {
        std::set<size_t> next;
        for (size_t p : cur)
            for (size_t q : ends(*n.children[0], ev, p))
                next.insert(q);
        if (next.empty())
            break;
        cur = std::move(next);
        if (k >= n.min)
            result.insert(cur.begin(), cur.end());
    }
    return result;
}

} // namespace

bool matches(std::string const &text, std::vector<Event> const &events, std::string *error) {
    try {
        std::string t = text;
        const uint8_t flags = stripPrefixes(t);
        auto root = Parser(t).parse();
        // holdout; expressions take the paths that start with the holdout
        // event, the others never (compile()).
        const bool holdoutPath = !events.empty() && events[0].type == TypeHoldout;
        if (holdoutPath != ((flags & OutputHoldout) != 0))
            return false;
        const size_t first = holdoutPath ? 1 : 0;
        const std::vector<Event> rest(events.begin() + std::ptrdiff_t(first), events.end());
        return ends(*root, rest, 0).count(rest.size()) != 0;
    } catch (ParseError const &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

Program Compiled::program() const {
    Program p;
    p.stateCount = stateCount;
    p.classCount = classCount;
    p.tagCount = uint32_t(tags.size());
    p.labelCount = uint32_t(labels.size());
    p.next = next.data();
    p.symbolClass = symbolClass.data();
    p.outBegin = outBegin.data();
    p.outs = outs.data();
    p.outputFlags = outputFlags.data();
    p.start = start;
    p.holdoutStart = holdoutStart;
    return p;
}

uint32_t Compiled::tagId(std::string const &name) const {
    for (size_t i = 1; i < tags.size(); ++i)
        if (tags[i] == name)
            return uint32_t(i);
    return 0;
}

uint32_t Compiled::labelId(std::string const &name) const {
    for (size_t i = 1; i < labels.size(); ++i)
        if (labels[i] == name)
            return uint32_t(i);
    return 0;
}

int32_t Compiled::shadowSlot(size_t output) const {
    if (output >= outputFlags.size() || !(outputFlags[output] & OutputShadow) || !errors[output].empty())
        return -1;
    int32_t slot = 0;
    for (size_t i = 0; i < output; ++i)
        slot += (outputFlags[i] & OutputShadow) && errors[i].empty() ? 1 : 0;
    return slot;
}

uint32_t Compiled::shadowCount() const {
    uint32_t n = 0;
    for (size_t i = 0; i < outputFlags.size(); ++i)
        n += (outputFlags[i] & OutputShadow) && errors[i].empty() ? 1u : 0u;
    return n;
}

State Compiled::run(std::vector<Event> const &events) const {
    if (!ok() || events.empty())
        return 0;
    const Program p = program();
    // After the camera event, or for a holdout path (the holdout event first)
    // after both.
    const bool holdout = events[0].type == TypeHoldout;
    if (holdout && events.size() < 2)
        return 0;
    State s = holdout ? holdoutStart : start;
    for (size_t i = holdout ? 2 : 1; i < events.size() && s != 0; ++i)
        s = p.step(s, p.symbol(events[i].type, events[i].scatter, tagId(events[i].tag), labelId(events[i].label)));
    return s;
}

Compiled compile(std::vector<std::string> const &expressions, Limits const &limits) {
    Compiled out;
    out.errors.assign(expressions.size(), std::string());
    out.outputFlags.assign(expressions.size(), 0);

    // Parse; collect the single-event predicates, tags and labels.
    std::vector<std::unique_ptr<Node>> roots(expressions.size());
    std::vector<Node *> singles;
    std::set<std::string> tagSet, labelSet;
    for (size_t e = 0; e < expressions.size(); ++e) {
        try {
            std::string text = expressions[e];
            out.outputFlags[e] = stripPrefixes(text);
            roots[e] = Parser(text).parse();
            collect(*roots[e], singles, tagSet, labelSet);
        } catch (ParseError const &err) {
            out.errors[e] = err.what();
            roots[e].reset();
        }
    }
    out.tags.assign(1, std::string());
    out.tags.insert(out.tags.end(), tagSet.begin(), tagSet.end());
    out.labels.assign(1, std::string());
    out.labels.insert(out.labels.end(), labelSet.begin(), labelSet.end());
    const uint32_t T = uint32_t(out.tags.size()), Lb = uint32_t(out.labels.size());
    const uint32_t symbols = uint32_t(kTypeCount) * kScatterCount * T * Lb;

    // The NFA of all expressions, accept states tagged with their output.
    // holdout; outputs take the internal holdout event first (a predicate of
    // its own past the parsed ones); paths of holdouts start with it.
    Nfa nfa;
    const int start = nfa.add();
    bool any = false;
    int holdoutPred = -1;
    for (size_t e = 0; e < roots.size(); ++e) {
        if (!roots[e])
            continue;
        auto [s, end] = nfa.build(*roots[e]);
        if (out.outputFlags[e] & OutputHoldout) {
            if (holdoutPred < 0)
                holdoutPred = int(singles.size());
            const int a = nfa.add(), b = nfa.add();
            nfa.moves[size_t(a)].push_back({holdoutPred, b});
            nfa.eps[size_t(b)].push_back(s);
            nfa.eps[size_t(start)].push_back(a);
        } else {
            nfa.eps[size_t(start)].push_back(s);
        }
        nfa.accept[size_t(end)] = int(e);
        any = true;
    }
    const size_t predicates = singles.size() + (holdoutPred >= 0 ? 1 : 0);
    if (!any)
        return out; // nothing compiled (ok() is false)

    // Concrete events grouped into classes by which predicates accept them.
    std::vector<uint32_t> eventClass(symbols);
    std::vector<std::vector<char>> classAccepts; // [class][pred]
    {
        std::map<std::vector<char>, uint32_t> bySignature;
        for (uint32_t sym = 0; sym < symbols; ++sym) {
            const uint32_t label = sym % Lb, tag = (sym / Lb) % T, ts = sym / (Lb * T);
            const Type type = Type(ts / kScatterCount);
            const Scatter scatter = Scatter(ts % kScatterCount);
            std::vector<char> sig(predicates);
            for (size_t p = 0; p < singles.size(); ++p)
                sig[p] = nodeAccepts(*singles[p], type, scatter, out.tags[tag], out.labels[label]) ? 1 : 0;
            if (holdoutPred >= 0)
                sig[size_t(holdoutPred)] = type == TypeHoldout ? 1 : 0;
            auto [it, inserted] = bySignature.emplace(sig, uint32_t(classAccepts.size()));
            if (inserted)
                classAccepts.push_back(sig);
            eventClass[sym] = it->second;
        }
    }
    const uint32_t classes0 = uint32_t(classAccepts.size());

    // Subset construction; DFA state 0 is the empty set (dead).
    std::map<std::vector<int>, uint32_t> index;
    std::vector<std::vector<int>> sets;
    std::vector<uint32_t> dnext; // [state * classes0 + class]
    auto intern = [&](std::vector<int> set) {
        auto [it, inserted] = index.emplace(set, uint32_t(sets.size()));
        if (inserted) {
            sets.push_back(std::move(set));
            dnext.resize(sets.size() * classes0, 0);
        }
        return it->second;
    };
    intern({});
    const uint32_t initial = intern(nfa.closure({start}));
    for (uint32_t s = 1; s < sets.size(); ++s) {
        for (uint32_t c = 0; c < classes0; ++c) {
            std::vector<int> targets;
            for (int n : sets[s])
                for (auto [pred, to] : nfa.moves[size_t(n)])
                    if (classAccepts[c][size_t(pred)])
                        targets.push_back(to);
            const uint32_t t = targets.empty() ? 0u : intern(nfa.closure(std::move(targets)));
            dnext[size_t(s) * classes0 + c] = t;
            if (sets.size() > size_t(limits.maxStates) * 4) {
                for (std::string &e : out.errors)
                    if (e.empty())
                        e = "the light path expressions together exceed the automaton limit";
                return out;
            }
        }
    }
    const uint32_t dstates = uint32_t(sets.size());
    std::vector<std::vector<uint16_t>> dout(dstates);
    for (uint32_t s = 0; s < dstates; ++s) {
        for (int n : sets[s])
            if (nfa.accept[size_t(n)] >= 0)
                dout[s].push_back(uint16_t(nfa.accept[size_t(n)]));
        std::sort(dout[s].begin(), dout[s].end());
        dout[s].erase(std::unique(dout[s].begin(), dout[s].end()), dout[s].end());
    }

    // Moore minimization: start from the output sets, refine by successors.
    std::vector<uint32_t> part(dstates);
    uint32_t parts = 0;
    {
        std::map<std::vector<uint16_t>, uint32_t> byOut;
        for (uint32_t s = 0; s < dstates; ++s)
            part[s] = byOut.emplace(dout[s], uint32_t(byOut.size())).first->second;
        parts = uint32_t(byOut.size());
    }
    for (;;) {
        std::map<std::vector<uint32_t>, uint32_t> bySig;
        std::vector<uint32_t> np(dstates);
        for (uint32_t s = 0; s < dstates; ++s) {
            std::vector<uint32_t> sig(classes0 + 1);
            sig[0] = part[s];
            for (uint32_t c = 0; c < classes0; ++c)
                sig[c + 1] = part[dnext[size_t(s) * classes0 + c]];
            np[s] = bySig.emplace(sig, uint32_t(bySig.size())).first->second;
        }
        const uint32_t n = uint32_t(bySig.size());
        part = std::move(np);
        if (n == parts)
            break;
        parts = n;
    }
    // Renumber: the dead state's block is 0.
    std::vector<uint32_t> renum(parts, ~0u);
    uint32_t count = 0;
    renum[part[0]] = count++;
    for (uint32_t s = 0; s < dstates; ++s)
        if (renum[part[s]] == ~0u)
            renum[part[s]] = count++;
    if (count > limits.maxStates) {
        for (std::string &e : out.errors)
            if (e.empty())
                e = "the light path expressions together exceed the automaton limit (" + std::to_string(count) +
                    " states)";
        return out;
    }
    std::vector<uint32_t> rep(count, ~0u); // a DFA state per block
    for (uint32_t s = 0; s < dstates; ++s)
        if (rep[renum[part[s]]] == ~0u)
            rep[renum[part[s]]] = s;

    // Merge event classes with equal columns.
    std::vector<uint32_t> classOf(classes0);
    uint32_t classes = 0;
    {
        std::map<std::vector<uint32_t>, uint32_t> byColumn;
        for (uint32_t c = 0; c < classes0; ++c) {
            std::vector<uint32_t> col(count);
            for (uint32_t b = 0; b < count; ++b)
                col[b] = renum[part[dnext[size_t(rep[b]) * classes0 + c]]];
            classOf[c] = byColumn.emplace(col, uint32_t(byColumn.size())).first->second;
        }
        classes = uint32_t(byColumn.size());
    }
    const size_t tableBytes = size_t(count) * classes * sizeof(State) + size_t(symbols) * sizeof(uint16_t);
    if (tableBytes > limits.maxTableBytes) {
        for (std::string &e : out.errors)
            if (e.empty())
                e = "the light path expressions together exceed the automaton's table limit";
        return out;
    }

    out.stateCount = count;
    out.classCount = classes;
    out.next.assign(size_t(count) * classes, 0);
    for (uint32_t b = 0; b < count; ++b)
        for (uint32_t c = 0; c < classes0; ++c)
            out.next[size_t(b) * classes + classOf[c]] = State(renum[part[dnext[size_t(rep[b]) * classes0 + c]]]);
    out.symbolClass.resize(symbols);
    for (uint32_t sym = 0; sym < symbols; ++sym)
        out.symbolClass[sym] = uint16_t(classOf[eventClass[sym]]);
    out.outBegin.assign(size_t(count) + 1, 0);
    for (uint32_t b = 0; b < count; ++b) {
        out.outBegin[b] = uint32_t(out.outs.size());
        out.outs.insert(out.outs.end(), dout[rep[b]].begin(), dout[rep[b]].end());
    }
    out.outBegin[count] = uint32_t(out.outs.size());
    const Program p = out.program();
    const uint32_t camera = p.symbol(TypeCamera, ScatterNone, 0, 0);
    out.start = p.step(State(renum[part[initial]]), camera);
    out.holdoutStart = p.step(p.step(State(renum[part[initial]]), p.symbol(TypeHoldout, ScatterNone, 0, 0)), camera);
    return out;
}

} // namespace nova::lpe
