#include "core/scene/visibility.h"

#include "core/bsdf/closure.h"

#include <cctype>

namespace nova {

static_assert(kVisibleCamera == 1u << uint8_t(RayClass::Camera) && kVisibleDiffuse == 1u << uint8_t(RayClass::Diffuse) &&
                  kVisibleReflect == 1u << uint8_t(RayClass::Reflect) &&
                  kVisibleRefract == 1u << uint8_t(RayClass::Refract) && kVisibleSss == 1u << uint8_t(RayClass::Sss) &&
                  kVisibleVolume == 1u << uint8_t(RayClass::Volume) && kVisibleShadow == 1u << uint8_t(RayClass::Shadow),
              "visibility bits follow RayClass");

namespace {

// Recursive descent over rendervisibility (visibility.h).
class VisibilityParser {
public:
    explicit VisibilityParser(std::string_view s) : _s(s) {}

    bool parse(uint8_t &mask, std::string *error) {
        uint8_t result = 0;
        bool first = true;
        skip();
        while (_i < _s.size() && _error.empty()) {
            const bool removes = isNegation(_s[_i]);
            const uint8_t term = orExpr();
            if (!_error.empty())
                break;
            if (first)
                result = term; // a leading negated term is a complement: all but it
            else
                result = removes ? uint8_t(result & term) : uint8_t(result | term);
            first = false;
            skip();
        }
        if (!_error.empty()) {
            if (error)
                *error = _error;
            return false;
        }
        mask = result;
        return true;
    }

private:
    static bool isNegation(char c) { return c == '-' || c == '!' || c == '^'; }
    void skip() {
        while (_i < _s.size() && std::isspace(static_cast<unsigned char>(_s[_i])))
            ++_i;
    }
    bool peek(char c) {
        skip();
        return _i < _s.size() && _s[_i] == c;
    }
    void fail(std::string message) {
        if (_error.empty())
            _error = std::move(message);
    }

    uint8_t orExpr() {
        uint8_t v = andExpr();
        while (_error.empty() && peek('|')) {
            ++_i;
            v |= andExpr();
        }
        return v;
    }
    uint8_t andExpr() {
        uint8_t v = unary();
        while (_error.empty() && peek('&')) {
            ++_i;
            v &= unary();
        }
        return v;
    }
    uint8_t unary() {
        skip();
        if (_i < _s.size() && isNegation(_s[_i])) {
            ++_i;
            return uint8_t(~unary() & kVisibleAll);
        }
        return primary();
    }
    uint8_t primary() {
        skip();
        if (_i >= _s.size()) {
            fail("a ray type is missing at the end");
            return 0;
        }
        const char c = _s[_i];
        if (c == '*') {
            ++_i;
            return kVisibleAll;
        }
        if (c == '(') {
            ++_i;
            const uint8_t v = orExpr();
            if (!peek(')'))
                fail("')' is missing");
            else
                ++_i;
            return v;
        }
        size_t end = _i;
        while (end < _s.size() && (std::isalnum(static_cast<unsigned char>(_s[end])) || _s[end] == '_'))
            ++end;
        if (end == _i) {
            fail(std::string("unexpected '") + c + "'");
            return 0;
        }
        std::string name(_s.substr(_i, end - _i));
        for (char &ch : name)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        _i = end;
        static const struct {
            const char *name;
            uint8_t bit;
        } kTypes[] = {{"primary", kVisibleCamera}, {"camera", kVisibleCamera}, {"shadow", kVisibleShadow},
                      {"diffuse", kVisibleDiffuse}, {"reflect", kVisibleReflect}, {"refract", kVisibleRefract},
                      {"volume", kVisibleVolume},   {"sss", kVisibleSss}};
        for (const auto &t : kTypes)
            if (name == t.name)
                return t.bit;
        fail("unknown ray type '" + name + "'");
        return 0;
    }

    std::string_view _s;
    size_t _i = 0;
    std::string _error;
};

} // namespace

bool parseRenderVisibility(std::string_view text, uint8_t &mask, std::string *error) {
    return VisibilityParser(text).parse(mask, error);
}

bool parseHoldoutMode(std::string_view text, HoldoutMode &mode) {
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            t.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (t == "0" || t == "none") {
        mode = HoldoutMode::None;
        return true;
    }
    if (t == "1" || t == "matte") {
        mode = HoldoutMode::Matte;
        return true;
    }
    if (t == "2" || t == "background") {
        mode = HoldoutMode::Background;
        return true;
    }
    return false;
}

} // namespace nova
