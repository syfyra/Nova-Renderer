#pragma once

// File names in USD, in render settings and in Houdini's environment are
// UTF-8 (Houdini's processes run with the UTF-8 code page). A string from a
// host with another code page may not be: a std::filesystem::path made from
// UTF-8 throws on such bytes, and an exception that leaves a prim's sync ends
// the host. HdNovaPath() reads such a string in the process's own code page
// instead.

#include <pxr/pxr.h>

#include <filesystem>
#include <string>
#include <string_view>

PXR_NAMESPACE_OPEN_SCOPE

inline std::filesystem::path HdNovaPath(std::string const &utf8) noexcept {
    try {
        const auto *text = reinterpret_cast<char8_t const *>(utf8.data());
        return std::filesystem::path(std::u8string_view(text, utf8.size()));
    } catch (...) {
    }
    try {
        return std::filesystem::path(utf8);
    } catch (...) {
        return {};
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
