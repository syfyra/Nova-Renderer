#include "textureSystem.h"

#include "diagnostics.h"

#include "core/texture/texel_stream.h"

#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/ustring.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

OIIO::TextureOpt::Wrap toOiio(nova::TexWrap wrap) {
    switch (wrap) {
    case nova::TexWrap::Periodic:
        return OIIO::TextureOpt::WrapPeriodic;
    case nova::TexWrap::Mirror:
        return OIIO::TextureOpt::WrapMirror;
    case nova::TexWrap::Black:
        return OIIO::TextureOpt::WrapBlack;
    default: // Clamp; Constant clamps and tests the range per lane
        return OIIO::TextureOpt::WrapClamp;
    }
}

OIIO::TextureOpt::InterpMode toOiio(nova::TexFilter filter) {
    switch (filter) {
    case nova::TexFilter::Closest:
        return OIIO::TextureOpt::InterpClosest;
    case nova::TexFilter::Cubic:
        return OIIO::TextureOpt::InterpBicubic;
    default:
        return OIIO::TextureOpt::InterpBilinear;
    }
}

bool outside(float x) { return !(x >= 0.0f && x <= 1.0f); }

// A texture file opened for the GPU at the image of the requested layer (the
// first if none is named or found).
struct DeviceFile {
    std::unique_ptr<OIIO::ImageInput> in;
    int subimage = 0;
};

DeviceFile openDeviceFile(std::string const &file, std::string const &layer) {
    DeviceFile f;
    f.in = OIIO::ImageInput::open(file);
    if (f.in && !layer.empty())
        for (int s = 0; f.in->seek_subimage(s, 0); ++s)
            if (f.in->spec().get_string_attribute("oiio:subimagename") == layer) {
                f.subimage = s;
                break;
            }
    return f;
}

// An image's MIP levels from its headers: level 0's size, the levels stored
// from level 0 on with the sizes of a chain (nova::levelSize), and whether
// any level has more than 16 bits per channel.
struct StoredLevels {
    int width = 0, height = 0;
    uint32_t stored = 0;
    bool wide = false;
};

StoredLevels storedLevels(OIIO::ImageInput &in, int subimage) {
    StoredLevels s;
    bool chain = true;
    for (int level = 0; in.seek_subimage(subimage, level); ++level) {
        OIIO::ImageSpec const &spec = in.spec();
        if (spec.format.size() > 2)
            s.wide = true;
        if (spec.nchannels < 1)
            break;
        if (level == 0) {
            s.width = spec.width;
            s.height = spec.height;
        }
        chain = chain && spec.width > 0 && spec.height > 0 && spec.width == nova::levelSize(s.width, uint32_t(level)) &&
                spec.height == nova::levelSize(s.height, uint32_t(level));
        if (chain)
            s.stored = uint32_t(level) + 1;
    }
    return s;
}

// Reads one level a band of rows at a time (whole rows of tiles, or
// scanlines) as RGBA floats with the texture's conversions (grey to RGB, a
// missing alpha 1 as fill = 1, the color matrix M if not null) and calls
// row(rgba) per row, from the top down.
template <class Row>
bool readLevel(OIIO::ImageInput &in, int subimage, int level, float const *M, std::atomic<uint64_t> &texelsRead,
               Row &&row) {
    if (!in.seek_subimage(subimage, level))
        return false;
    OIIO::ImageSpec const &spec = in.spec();
    const int width = spec.width, height = spec.height, x0 = spec.x, y0 = spec.y, z0 = spec.z;
    const int nch = spec.nchannels;
    const int tileRows = spec.tile_width > 0 ? std::max(1, spec.tile_height) : 0;
    if (width <= 0 || height <= 0 || nch < 1)
        return false;
    const int band = tileRows > 0 ? tileRows * std::max(1, 64 / tileRows) : 64;
    std::vector<float> texels(size_t(width) * size_t(std::min(band, height)) * 4);
    const OIIO::stride_t stride = OIIO::stride_t(4 * sizeof(float));
    for (int y = 0; y < height; y += band) {
        const int rows = std::min(band, height - y);
        const bool ok =
            tileRows > 0 ? in.read_tiles(subimage, level, x0, x0 + width, y0 + y, y0 + y + rows, z0, z0 + 1, 0,
                                         std::min(nch, 4), OIIO::TypeDesc::FLOAT, texels.data(), stride)
                         : in.read_scanlines(subimage, level, y0 + y, y0 + y + rows, z0, 0, std::min(nch, 4),
                                             OIIO::TypeDesc::FLOAT, texels.data(), stride);
        if (!ok)
            return false;
        texelsRead += uint64_t(width) * uint64_t(rows);
        const size_t n = size_t(width) * size_t(rows);
        for (size_t i = 0; i < n; ++i) {
            float *q = &texels[i * 4];
            if (nch == 3) {
                q[3] = 1.0f; // as fill = 1
            } else if (nch < 3) { // gray_to_rgb, with alpha for two channels
                q[3] = nch == 2 ? q[1] : 1.0f;
                q[1] = q[2] = q[0];
            }
            if (M) {
                const float x = q[0], y1 = q[1], z = q[2];
                q[0] = M[0] * x + M[1] * y1 + M[2] * z;
                q[1] = M[3] * x + M[4] * y1 + M[5] * z;
                q[2] = M[6] * x + M[7] * y1 + M[8] * z;
            }
        }
        for (int r = 0; r < rows; ++r)
            if (!row(&texels[size_t(r) * size_t(width) * 4]))
                return false;
    }
    return true;
}

} // namespace

HdNovaTextureSystem::HdNovaTextureSystem() {
    _cache = OIIO::ImageCache::create(true); // husk's --oiio-* options apply
    _ts = OIIO::TextureSystem::create(false, _cache);
    _ts->attribute("flip_t", 1); // v up, as MaterialX and USD; also for UDIM rows
    _ts->attribute("gray_to_rgb", 1);
    _ts->attribute("stochastic", 3); // MIP level and anisotropic probe picked by TextureOpt::rnd (M3.9)
    _cache->getattribute("max_memory_MB", _cacheMemoryMB);
}

HdNovaTextureSystem::~HdNovaTextureSystem() {
    OIIO::TextureSystem::destroy(_ts, false); // the shared cache stays
}

void HdNovaTextureSystem::SetOptions(HdNovaTextureOptions const &options) {
    std::lock_guard<std::mutex> lock(_mutex);
    // nova:textureCacheMemory: the shared cache's limit, or the one it had.
    if (options.cacheMemoryMB != _options.cacheMemoryMB && _cache) {
        const float limit = options.cacheMemoryMB > 0 ? float(options.cacheMemoryMB) : _cacheMemoryMB;
        if (limit > 0.0f)
            _cache->attribute("max_memory_MB", limit);
    }
    _options = options;
}

float HdNovaTextureSystem::CacheMemoryMB() const {
    float limit = 0.0f;
    if (_cache)
        _cache->getattribute("max_memory_MB", limit);
    return limit;
}

HdNovaTextureOptions HdNovaTextureSystem::GetOptions() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _options;
}

std::string HdNovaTextureSystem::Key(HdNovaTextureRequest const &r) const {
    return r.path + '\x1f' + r.colorSpace + '\x1f' + r.layer;
}

nova::TextureId HdNovaTextureSystem::Acquire(HdNovaTextureRequest const &request) {
    if (request.path.empty())
        return nova::kNoTexture;
    std::lock_guard<std::mutex> lock(_mutex);
    const std::string signature = HdNovaTextureSignature(request.path);
    const std::string key = Key(request);
    nova::TextureId id;
    bool changed = false;
    if (const auto it = _byKey.find(key); it != _byKey.end()) {
        id = it->second;
        if (_entries[id].signature == signature)
            return id;
        // Changed on disk: prepared again under the same id.
        changed = true;
    } else {
        id = nova::TextureId(_entries.size());
        _entries.emplace_back();
        _entries.back().request = request;
        _byKey.emplace(key, id);
    }
    Entry &e = _entries[id];
    e.signature = signature;
    e.prepared = HdNovaPrepareTexture(request.path, request.colorSpace, request.layer, _options);
    e.handle = nullptr;
    ++e.generation;
    e.good = false;
    e.reported = false;
    if (e.prepared.found) {
        const OIIO::ustring name(e.prepared.lookupPath);
        // Re-read a source that changed: forced when its signature did, since
        // OIIO compares modification times in whole seconds.
        if (!e.prepared.converted)
            _ts->invalidate(name, changed && !e.prepared.udim);
        e.handle = _ts->get_texture_handle(name);
        if (e.handle && e.prepared.udim && !e.prepared.converted) {
            // OIIO caches a UDIM set's tiles under their own names, not the
            // pattern's: re-read those that changed (a pattern with added or
            // removed tiles has a fresh name, see HdNovaPrepareTexture).
            std::vector<OIIO::ustring> tiles;
            int nu = 0, nv = 0;
            _ts->inventory_udim(e.handle, nullptr, tiles, nu, nv);
            for (OIIO::ustring const &tile : tiles)
                if (!tile.empty())
                    _ts->invalidate(tile, changed);
        }
        e.good = e.handle && (e.prepared.udim || _ts->good(e.handle));
        if (!e.good)
            e.prepared.error = _ts->geterror();
    }
    if (!e.good)
        HD_NOVA_WARN("cannot read texture '%s' (%s); using the fallback", request.path.c_str(),
                     e.prepared.error.c_str());
    if (!e.good != e.countedMissing) {
        e.countedMissing = !e.good;
        e.countedMissing ? ++_missing : --_missing;
    }
    return id;
}

void HdNovaTextureSystem::sample2D(nova::TexSampler const &sampler, nova::TexQuery const &q, float *out,
                                   uint8_t *status) const {
    const Entry *e = sampler.texture < _entries.size() ? &_entries[sampler.texture] : nullptr;
    if (!e || !e->good) {
        for (uint32_t i = 0; i < q.count; ++i)
            status[i] = 1;
        return;
    }
    const nova::TexSlot &slot = sampler.slot;
    const bool udim = e->prepared.udim;
    const bool matrix = slot.channels >= 3 && e->prepared.transform.kind == HdNovaColorTransform::Kind::Matrix;
    const float *M = e->prepared.transform.matrix;
    OIIO::TextureSystem::Perthread *pt = _ts->get_perthread_info();
    OIIO::TextureOpt opt; // per call: OIIO writes into it
    opt.swrap = udim ? OIIO::TextureOpt::WrapClamp : toOiio(slot.wrapS); // tiles never wrap into each other
    opt.twrap = udim ? OIIO::TextureOpt::WrapClamp : toOiio(slot.wrapT);
    opt.interpmode = toOiio(slot.filter);
    opt.anisotropic = 8;
    opt.sblur = opt.tblur = _options.blur;
    opt.fill = 1.0f; // a missing alpha
    if (!e->request.layer.empty())
        opt.subimagename = OIIO::ustring(e->request.layer);
    const int nch = slot.channels;
    for (uint32_t i = 0; i < q.count; ++i) {
        const float s = q.s[i], t = q.t[i];
        if ((udim && !(s >= 0.0f && s < 10.0f && t >= 0.0f && t < 10.0f)) ||
            (slot.wrapS == nova::TexWrap::Constant && outside(s)) ||
            (slot.wrapT == nova::TexWrap::Constant && outside(t))) {
            status[i] = 1;
            continue;
        }
        const float dsdx = q.dsdx ? q.dsdx[i] : 0.0f, dtdx = q.dtdx ? q.dtdx[i] : 0.0f;
        const float dsdy = q.dsdy ? q.dsdy[i] : 0.0f, dtdy = q.dtdy ? q.dtdy[i] : 0.0f;
        if (q.rnd)
            opt.rnd = q.rnd[i];
        float r[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!_ts->texture(e->handle, pt, opt, s, t, dsdx, dtdx, dsdy, dtdy, nch, r)) {
            status[i] = 1;
            const std::string error = _ts->geterror(); // also clears it
            // A missing UDIM tile fails silently, as in Karma.
            if (!udim && !e->reported.exchange(true))
                HD_NOVA_WARN("cannot read texture '%s' (%s); using the fallback", e->request.path.c_str(),
                             error.c_str());
            continue;
        }
        if (matrix) {
            const float x = r[0], y = r[1], z = r[2];
            r[0] = M[0] * x + M[1] * y + M[2] * z;
            r[1] = M[3] * x + M[4] * y + M[5] * z;
            r[2] = M[6] * x + M[7] * y + M[8] * z;
        }
        for (int c = 0; c < nch; ++c)
            out[size_t(c) * q.count + i] = r[c];
        status[i] = 0;
    }
}

std::vector<std::pair<uint32_t, std::string>> HdNovaTextureSystem::DeviceFiles(Entry const &e) const {
    std::vector<std::pair<uint32_t, std::string>> files;
    if (e.prepared.udim) {
        std::vector<OIIO::ustring> names;
        int nu = 0, nv = 0;
        _ts->inventory_udim(e.handle, nullptr, names, nu, nv);
        for (int v = 0; v < nv; ++v)
            for (int u = 0; u < nu; ++u) {
                const size_t i = size_t(v) * size_t(nu) + size_t(u);
                if (i < names.size() && !names[i].empty())
                    files.emplace_back(uint32_t(1001 + u + 10 * v), names[i].string());
            }
    } else {
        files.emplace_back(0u, e.prepared.lookupPath);
    }
    return files;
}

bool HdNovaTextureSystem::textureInfo(nova::TextureId id, nova::TexInfo &out) const {
    const Entry *e = id < _entries.size() ? &_entries[id] : nullptr;
    if (!e || !e->good)
        return false;
    out = nova::TexInfo{};
    out.udim = e->prepared.udim;
    for (auto const &[udim, file] : DeviceFiles(*e)) {
        DeviceFile f = openDeviceFile(file, e->request.layer);
        if (!f.in)
            continue;
        const StoredLevels s = storedLevels(*f.in, f.subimage);
        if (s.wide)
            out.half = false;
        if (s.stored == 0)
            continue;
        nova::TexTileInfo tile;
        tile.udim = udim;
        tile.width = s.width;
        tile.height = s.height;
        tile.stored = s.stored;
        out.tiles.push_back(tile);
    }
    return !out.tiles.empty();
}

bool HdNovaTextureSystem::textureLevels(nova::TextureId id, nova::TexLevelRequest const &request,
                                        nova::TexRowSink &sink) const {
    const Entry *e = id < _entries.size() ? &_entries[id] : nullptr;
    if (!e || !e->good || request.count == 0)
        return false;
    std::string file;
    for (auto const &[udim, path] : DeviceFiles(*e))
        if (udim == request.udim) {
            file = path;
            break;
        }
    if (file.empty())
        return false;
    DeviceFile f = openDeviceFile(file, e->request.layer);
    if (!f.in)
        return false;
    const StoredLevels s = storedLevels(*f.in, f.subimage);
    if (s.stored == 0)
        return false;
    const float *M =
        e->prepared.transform.kind == HdNovaColorTransform::Kind::Matrix ? e->prepared.transform.matrix : nullptr;
    for (uint32_t level = request.first; level < request.first + request.count; ++level) {
        nova::LevelWriter out(sink, level, nova::levelSize(s.width, level), nova::levelSize(s.height, level),
                              request.half);
        // A level the file does not store: made from its last stored one.
        const uint32_t from = std::min(level, s.stored - 1);
        std::optional<nova::LevelReducer> reduce;
        if (from != level)
            reduce.emplace(out, nova::levelSize(s.width, from), nova::levelSize(s.height, from));
        const bool read = readLevel(*f.in, f.subimage, int(from), M, _deviceTexelsRead,
                                    [&](float const *rgba) { return reduce ? reduce->row(rgba) : out.row(rgba); });
        if (!read || !out.finish())
            return false;
    }
    return true;
}

uint32_t HdNovaTextureSystem::textureGeneration(nova::TextureId id) const {
    return id < _entries.size() ? _entries[id].generation : 0u;
}

void HdNovaTextureSystem::PrintStats(int level) const {
    const std::string stats = _ts->getstats(level, true);
    std::fprintf(stderr, "[Nova] Texture statistics:\n%s\n", stats.c_str());
    std::fflush(stderr);
}

PXR_NAMESPACE_CLOSE_SCOPE
