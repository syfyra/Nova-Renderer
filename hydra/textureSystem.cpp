#include "textureSystem.h"

#include "diagnostics.h"

#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/ustring.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
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

} // namespace

HdNovaTextureSystem::HdNovaTextureSystem() {
    _cache = OIIO::ImageCache::create(true); // husk's --oiio-* options apply
    _ts = OIIO::TextureSystem::create(false, _cache);
    _ts->attribute("flip_t", 1); // v up, as MaterialX and USD; also for UDIM rows
    _ts->attribute("gray_to_rgb", 1);
    _ts->attribute("stochastic", 3); // MIP level and anisotropic probe picked by TextureOpt::rnd (M3.9)
}

HdNovaTextureSystem::~HdNovaTextureSystem() {
    OIIO::TextureSystem::destroy(_ts, false); // the shared cache stays
}

void HdNovaTextureSystem::SetOptions(HdNovaTextureOptions const &options) {
    std::lock_guard<std::mutex> lock(_mutex);
    _options = options;
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

bool HdNovaTextureSystem::textureData(nova::TextureId id, nova::TexData &out) const {
    const Entry *e = id < _entries.size() ? &_entries[id] : nullptr;
    if (!e || !e->good)
        return false;
    out = nova::TexData{};
    out.udim = e->prepared.udim;
    std::vector<std::pair<uint32_t, std::string>> files; // UDIM number (0: none), file
    if (out.udim) {
        std::vector<OIIO::ustring> names;
        int nu = 0, nv = 0;
        _ts->inventory_udim(e->handle, nullptr, names, nu, nv);
        for (int v = 0; v < nv; ++v)
            for (int u = 0; u < nu; ++u) {
                const size_t i = size_t(v) * size_t(nu) + size_t(u);
                if (i < names.size() && !names[i].empty())
                    files.emplace_back(uint32_t(1001 + u + 10 * v), names[i].string());
            }
    } else {
        files.emplace_back(0u, e->prepared.lookupPath);
    }
    const bool matrix = e->prepared.transform.kind == HdNovaColorTransform::Kind::Matrix;
    const float *M = e->prepared.transform.matrix;
    // Reads every tile from MIP level `skip` on.
    const auto readFrom = [&](int skip) {
        for (const auto &[udim, file] : files) {
            std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(file);
            if (!in)
                continue;
            int subimage = 0;
            if (!e->request.layer.empty())
                for (int s = 0; in->seek_subimage(s, 0); ++s)
                    if (in->spec().get_string_attribute("oiio:subimagename") == e->request.layer) {
                        subimage = s;
                        break;
                    }
            nova::TexTile tile;
            tile.udim = udim;
            for (int level = 0; in->seek_subimage(subimage, level); ++level) {
                const OIIO::ImageSpec &spec = in->spec();
                if (spec.format.size() > 2)
                    out.half = false;
                if (level < skip)
                    continue;
                const int nch = spec.nchannels;
                if (nch < 1)
                    break;
                const size_t n = size_t(spec.width) * size_t(spec.height);
                nova::TexLevel l;
                l.width = spec.width;
                l.height = spec.height;
                l.texels.resize(n * 4);
                // Straight into the RGBA texels (no staging copy of the level).
                if (!in->read_image(subimage, level, 0, std::min(nch, 4), OIIO::TypeDesc::FLOAT, l.texels.data(),
                                    OIIO::stride_t(4 * sizeof(float))))
                    break;
                for (size_t i = 0; i < n; ++i) {
                    float *q = &l.texels[i * 4];
                    if (nch == 3) {
                        q[3] = 1.0f; // as fill = 1
                    } else if (nch < 3) { // gray_to_rgb, with alpha for two channels
                        q[3] = nch == 2 ? q[1] : 1.0f;
                        q[1] = q[2] = q[0];
                    }
                    if (matrix) {
                        const float x = q[0], y = q[1], z = q[2];
                        q[0] = M[0] * x + M[1] * y + M[2] * z;
                        q[1] = M[3] * x + M[4] * y + M[5] * z;
                        q[2] = M[6] * x + M[7] * y + M[8] * z;
                    }
                }
                tile.levels.push_back(std::move(l));
            }
            if (!tile.levels.empty())
                out.tiles.push_back(std::move(tile));
        }
    };
    // A set that does not fit in host memory drops its finest levels, as the
    // GPU texture budget does (D10), instead of ending the process.
    constexpr int kMaxLevels = 32;
    for (int skip = 0; skip < kMaxLevels; ++skip) {
        try {
            readFrom(skip);
            if (skip > 0)
                HD_NOVA_WARN("texture '%s' does not fit in memory for the GPU; its %d finest MIP levels are dropped",
                             e->request.path.c_str(), skip);
            return !out.tiles.empty();
        } catch (std::bad_alloc const &) {
            const bool udim = out.udim;
            out = nova::TexData{}; // frees what was read
            out.udim = udim;
        }
    }
    HD_NOVA_WARN("texture '%s' does not fit in memory for the GPU; using the fallback", e->request.path.c_str());
    return false;
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
