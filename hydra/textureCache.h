#pragma once

#include "core/scene/image.h"

#include <pxr/pxr.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

// Loads light textures through Houdini's OpenImageIO (.rat files through
// imaketx) as scene-linear RGB, with Houdini's color-space rules. Lights that
// use the same file share one image, and an unchanged file is not read again
// when a light is edited. Entries live as long as a light uses them.
class HdNovaTextureCache {
public:
    // Returns null (with one warning per file version) if the file cannot be
    // read.
    std::shared_ptr<const nova::Image> Load(std::string const &path);

private:
    struct Entry {
        std::weak_ptr<const nova::Image> image;
        int64_t writeTime = 0;
        bool failed = false;
    };
    std::mutex _mutex; // lights sync in parallel
    std::unordered_map<std::string, Entry> _entries;
};

PXR_NAMESPACE_CLOSE_SCOPE
