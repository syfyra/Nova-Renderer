#pragma once

// User-facing problems (unreadable textures, unknown settings, invalid data).
// husk does not print Tf diagnostics at all, so besides the Tf warning (the
// Houdini console and the Solaris message log) every message also goes to
// stderr, as Karma's own warnings do.

#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/pxr.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

// TF_DEBUG codes (enable with the TF_DEBUG environment variable).
TF_DEBUG_CODES(HDNOVA_SUBDIV, HDNOVA_MOTION, HDNOVA_MATERIAL, HDNOVA_AOV, HDNOVA_VOLUME, HDNOVA_SSS, HDNOVA_SETTINGS);

inline void HdNovaWarn(std::string const &message) {
    TF_WARN("Nova: %s", message.c_str());
    std::fprintf(stderr, "[Nova] Warning: %s\n", message.c_str());
    std::fflush(stderr);
}

#define HD_NOVA_WARN(...) HdNovaWarn(TfStringPrintf(__VA_ARGS__))

PXR_NAMESPACE_CLOSE_SCOPE
