#include "textureConvert.h"

#include "diagnostics.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/strutil.h>

#include <pxr/base/tf/getenv.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/packageUtils.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace fs = std::filesystem;

std::string utf8(fs::path const &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// 2: the rendering space in the name; grey files with a color space decoded.
constexpr const char *kFormatVersion = "nova-tx-2";
constexpr const char *kPackagedVersion = "nova-usdz-1";
constexpr const char *kLinkVersion = "nova-udim-link-1";
constexpr const char *kRatVersion = "nova-rat-exr-1"; // a .rat as the EXR imaketx makes of it

struct SourceFile {
    std::string path;
    std::string tile; // UDIM: "1001" or "u1_v1"; empty for a single file
    uintmax_t size = 0;
    int64_t mtime = 0;
};

uint64_t fnv1a(std::string const &s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex(uint64_t key) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(key));
    return text;
}

// A temporary name next to `out`, unique to this process and call: files are
// written there, then renamed into place (commit).
fs::path tempPath(fs::path const &out) {
    static std::atomic<unsigned> counter{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = 0;
#endif
    return out.parent_path() / fs::u8path(utf8(out.stem()) + ".tmp" + std::to_string(pid) + "_" +
                                          std::to_string(counter.fetch_add(1)) + utf8(out.extension()));
}

// Atomic: other processes see no file or the whole one.
bool commit(fs::path const &tmp, fs::path const &out, std::string &error) {
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (!ec)
        return true;
    std::error_code exists;
    if (fs::exists(out, exists))
        return true; // another process won the race
    error = "cannot write '" + utf8(out) + "': " + ec.message();
    return false;
}

bool stat(SourceFile &f) {
    std::error_code ec;
    const fs::path p = fs::u8path(f.path);
    f.size = fs::file_size(p, ec);
    if (ec)
        return false;
    const auto t = fs::last_write_time(p, ec);
    f.mtime = ec ? 0 : int64_t(t.time_since_epoch().count());
    return true;
}

// The UDIM token of a path, or empty.
std::string udimToken(std::string const &path) {
    for (const char *token : {"<UDIM>", "<UVTILE>"})
        if (path.find(token) != std::string::npos)
            return token;
    return {};
}

// The existing tiles of a UDIM pattern (in the token's form), sorted.
std::vector<SourceFile> udimTiles(std::string const &pattern, std::string const &token) {
    std::vector<SourceFile> tiles;
    const fs::path p = fs::u8path(pattern);
    const std::string name = utf8(p.filename());
    const size_t at = name.find(token);
    if (at == std::string::npos)
        return tiles;
    const std::string prefix = name.substr(0, at), suffix = name.substr(at + token.size());
    std::error_code ec;
    const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string file = utf8(it->path().filename());
        if (file.size() <= prefix.size() + suffix.size() || file.compare(0, prefix.size(), prefix) != 0 ||
            file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string mid = file.substr(prefix.size(), file.size() - prefix.size() - suffix.size());
        bool ok;
        if (token == "<UDIM>") {
            ok = mid.size() == 4 && std::all_of(mid.begin(), mid.end(), [](char c) { return c >= '0' && c <= '9'; });
        } else { // u<U>_v<V>
            int u = 0, v = 0;
            char rest = 0;
            ok = std::sscanf(mid.c_str(), "u%d_v%d%c", &u, &v, &rest) == 2;
        }
        if (!ok)
            continue;
        SourceFile f;
        f.path = utf8(it->path());
        f.tile = mid;
        if (stat(f))
            tiles.push_back(f);
    }
    std::sort(tiles.begin(), tiles.end(), [](SourceFile const &a, SourceFile const &b) { return a.tile < b.tile; });
    return tiles;
}

std::string lowerExtension(std::string const &path) {
    return OIIO::Strutil::lower(utf8(fs::u8path(path).extension()));
}

// The directory of Houdini's tools (imaketx): $HFS/bin, else the directory of
// the OpenImageIO library this process uses.
std::string houdiniBin() {
    const std::string hfs = TfGetenv("HFS");
    std::error_code ec;
    if (!hfs.empty() && fs::is_directory(fs::u8path(hfs) / "bin", ec))
        return utf8(fs::u8path(hfs) / "bin");
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"OpenImageIO_sidefx.dll");
    if (module) {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
        if (n > 0 && n < path.size()) {
            path.resize(n);
            return utf8(fs::path(path).parent_path());
        }
    }
#endif
    return {};
}

// Runs imaketx <source> <target>, with `stored` the stored values unchanged
// (-m builtin -l 0: no OCIO, no sRGB decoding), otherwise decoded by the
// stored color space, named in the target's metadata (--disableparse: not
// by the file name) (true on success).
bool runImaketx(std::string const &source, std::string const &target, bool stored, std::string &error) {
    const std::string bin = houdiniBin();
    const fs::path tool = fs::u8path(bin) / "imaketx.exe";
    std::error_code ec;
    if (bin.empty() || !fs::exists(tool, ec)) {
        error = "Houdini's imaketx was not found (needed for .rat files)";
        return false;
    }
#ifdef _WIN32
    const std::wstring exe = tool.wstring(), src = fs::u8path(source).wstring(), dst = fs::u8path(target).wstring();
    // _wspawnv passes the arguments unquoted: quote them.
    const std::wstring a0 = L"\"" + exe + L"\"", a1 = L"\"" + src + L"\"", a2 = L"\"" + dst + L"\"";
    std::vector<const wchar_t *> quoted{a0.c_str(), a1.c_str(), a2.c_str()};
    for (const wchar_t *a : stored ? std::vector<const wchar_t *>{L"-m", L"builtin", L"-l", L"0"}
                                   : std::vector<const wchar_t *>{L"--disableparse"})
        quoted.push_back(a);
    quoted.push_back(nullptr);
    const intptr_t code = _wspawnv(_P_WAIT, exe.c_str(), quoted.data());
    if (code != 0) {
        error = "imaketx failed (exit code " + std::to_string(code) + ")";
        return false;
    }
    return true;
#else
    error = "imaketx is only run on Windows";
    return false;
#endif
}

// Converts one file OIIO reads (for a .rat file: HdNovaReadableTexture's)
// into `target` (half, scene-linear, tiled 64, MIP-mapped).
bool convertOne(SourceFile const &file, std::string const &target, std::string const &colorSpace,
                std::string const &layer, std::string &error) {
    const fs::path out = fs::u8path(target);
    const fs::path tmp = tempPath(out);
    bool ok;
    {
        int subimage = 0;
        if (!layer.empty()) {
            OIIO::ImageBuf probe(file.path);
            for (int i = 0; i < probe.nsubimages(); ++i) {
                OIIO::ImageBuf sub(file.path, i, 0);
                if (sub.spec().get_string_attribute("oiio:subimagename") == layer) {
                    subimage = i;
                    break;
                }
            }
        }
        OIIO::ImageBuf src(file.path, subimage, 0);
        if (!src.read(subimage, 0, true, OIIO::TypeDesc::FLOAT)) {
            error = src.geterror();
            return false;
        }
        if (!colorSpace.empty() && src.spec().nchannels < 3) {
            // A grey image (with alpha) with an explicit color space: decoded
            // and kept as RGB(A), since an OCIO transform may mix channels.
            static const int order[4] = {0, 0, 0, 1};
            const std::string names[4] = {"R", "G", "B", "A"};
            const int n = src.spec().nchannels == 2 ? 4 : 3;
            src = OIIO::ImageBufAlgo::channels(src, n, OIIO::cspan<int>(order, n), {},
                                               OIIO::cspan<std::string>(names, n));
            if (src.has_error() || src.spec().nchannels != n) {
                error = src.has_error() ? src.geterror() : OIIO::geterror();
                return false;
            }
        }
        const OIIO::ImageSpec &spec = src.spec();
        if (!colorSpace.empty())
            HdNovaConvertToSceneLinear(file.path, colorSpace, static_cast<float *>(src.localpixels()),
                                       size_t(spec.width) * size_t(spec.height) * size_t(std::max(spec.depth, 1)),
                                       size_t(spec.nchannels));
        OIIO::ImageSpec config;
        config.tile_width = config.tile_height = 64;
        config.set_format(OIIO::TypeDesc::HALF);
        config.attribute("compression", "zip");
        config.attribute("maketx:fixnan", "box3");
        config.attribute("oiio:ColorSpace", "scene_linear"); // the pixels are linear now (make_texture copies the source's tag)
        ok = OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, src, utf8(tmp), config);
        if (!ok)
            error = OIIO::geterror();
    }
    ok = ok && commit(tmp, out, error);
    std::error_code ec;
    fs::remove(tmp, ec);
    return ok;
}

// The name OIIO reads a direct (unconverted) UDIM set under: the pattern,
// unless OIIO may have inventoried it in this process with other tiles (it
// scans a pattern only once, invalidate does not rescan it); then hard links
// (copies across volumes) of the tiles in the cache under a name that hashes
// them.
std::string directUdimName(std::string const &pattern, std::string const &token, std::vector<SourceFile> const &files,
                           HdNovaTextureOptions const &options) {
    std::string tiles;
    for (SourceFile const &f : files)
        tiles += f.tile + ";";
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::string> inventoried; // pattern -> its tiles when first read
        std::lock_guard<std::mutex> lock(mutex);
        const auto [it, first] = inventoried.try_emplace(pattern, tiles);
        if (first || it->second == tiles)
            return pattern;
    }
    uint64_t key = fnv1a(kLinkVersion);
    key = fnv1a(utf8(fs::u8path(pattern).lexically_normal()), key);
    for (SourceFile const &f : files)
        key = fnv1a(f.tile + "|" + std::to_string(f.size) + "|" + std::to_string(f.mtime), key);
    const std::string name = utf8(fs::u8path(pattern).filename());
    const size_t at = name.find(token);
    if (at == std::string::npos)
        return pattern;
    const std::string prefix = utf8(fs::u8path(HdNovaTextureCacheDir(options)) / fs::u8path(name.substr(0, at))) +
                               "_" + hex(key) + ".";
    const std::string suffix = name.substr(at + token.size());
    std::error_code ec;
    fs::create_directories(fs::u8path(prefix).parent_path(), ec);
    for (SourceFile const &f : files) {
        const fs::path out = fs::u8path(prefix + f.tile + suffix);
        if (fs::exists(out, ec))
            continue; // linked before
        const fs::path tmp = tempPath(out);
        fs::create_hard_link(fs::u8path(f.path), tmp, ec);
        if (ec) { // another volume
            fs::remove(tmp, ec);
            fs::copy_file(fs::u8path(f.path), tmp, ec);
        }
        std::string error = ec ? ec.message() : std::string();
        const bool ok = !ec && commit(tmp, out, error);
        fs::remove(tmp, ec);
        if (!ok) {
            HD_NOVA_WARN("cannot link the tiles of '%s' into the texture cache (%s); tiles added or removed since "
                         "it was first read are seen after a restart",
                         pattern.c_str(), error.c_str());
            return pattern;
        }
    }
    return prefix + token + suffix;
}

} // namespace

std::string HdNovaTextureSignature(std::string const &path) {
    const std::string token = udimToken(path);
    std::vector<SourceFile> files;
    if (!token.empty()) {
        files = udimTiles(path, token);
    } else {
        SourceFile f;
        // A file in a package changes with the package.
        f.path = ArIsPackageRelativePath(path) ? ArSplitPackageRelativePathOuter(path).first : path;
        if (stat(f))
            files.push_back(f);
    }
    std::string sig;
    for (SourceFile const &f : files)
        sig += f.tile + ":" + std::to_string(f.size) + ":" + std::to_string(f.mtime) + ";";
    return sig;
}

std::string HdNovaTextureCacheDir(HdNovaTextureOptions const &options) {
    if (!options.cacheDir.empty())
        return options.cacheDir;
    const std::string houdiniTemp = TfGetenv("HOUDINI_TEMP_DIR");
    std::error_code ec;
    const fs::path base = !houdiniTemp.empty() ? fs::u8path(houdiniTemp) : fs::temp_directory_path(ec);
    return utf8(base / "nova_tx");
}

std::string HdNovaLocalizeTexture(std::string const &path, HdNovaTextureOptions const &options, std::string &error) {
    if (!ArIsPackageRelativePath(path))
        return path;
    if (!udimToken(path).empty()) {
        error = "UDIM sets inside packages are not supported";
        return {};
    }
    SourceFile package;
    package.path = ArSplitPackageRelativePathOuter(path).first;
    if (!stat(package)) {
        error = "the package '" + package.path + "' does not exist";
        return {};
    }
    // Named by the packaged file and the package's size and time.
    const std::string inner = ArSplitPackageRelativePathInner(path).second;
    const fs::path innerName = fs::u8path(inner).filename();
    uint64_t key = fnv1a(kPackagedVersion);
    key = fnv1a(path + "|" + std::to_string(package.size) + "|" + std::to_string(package.mtime), key);
    const fs::path dir = fs::u8path(HdNovaTextureCacheDir(options));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / fs::u8path(utf8(innerName.stem()) + "_" + hex(key) + utf8(innerName.extension()));
    if (fs::exists(out, ec))
        return utf8(out); // extracted before
    const std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(ArResolvedPath(path));
    if (!asset) {
        error = "cannot open '" + inner + "' in the package";
        return {};
    }
    const fs::path tmp = tempPath(out);
    bool ok;
    {
        std::ofstream file(tmp, std::ios::binary);
        ok = bool(file);
        std::vector<char> chunk(size_t(1) << 20);
        const size_t size = asset->GetSize();
        for (size_t at = 0; ok && at < size;) {
            const size_t n = asset->Read(chunk.data(), std::min(chunk.size(), size - at), at);
            ok = n > 0 && bool(file.write(chunk.data(), std::streamsize(n)));
            at += n;
        }
        file.close();
        ok = ok && !file.fail();
    }
    if (!ok)
        error = "cannot extract '" + inner + "' to '" + utf8(out) + "'";
    ok = ok && commit(tmp, out, error);
    fs::remove(tmp, ec);
    return ok ? utf8(out) : std::string();
}

std::string HdNovaReadableTexture(std::string const &path, HdNovaTextureOptions const &options, bool stored,
                                  std::string &error) {
    const std::string local = HdNovaLocalizeTexture(path, options, error);
    if (local.empty() || lowerExtension(local) != ".rat")
        return local;
    SourceFile f;
    f.path = local;
    if (!stat(f)) {
        error = "the file does not exist";
        return {};
    }
    // Named by the file and the mode; decoding goes through the OCIO config.
    uint64_t key = fnv1a(kRatVersion);
    key = fnv1a(utf8(fs::u8path(local).lexically_normal()) + "|" + std::to_string(f.size) + "|" +
                    std::to_string(f.mtime) + "|" + (stored ? std::string("stored") : "decoded|" + TfGetenv("OCIO")),
                key);
    const fs::path dir = fs::u8path(HdNovaTextureCacheDir(options));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / fs::u8path(utf8(fs::u8path(local).stem()) + "_" + hex(key) + ".exr");
    if (fs::exists(out, ec))
        return utf8(out); // converted before
    const fs::path tmp = tempPath(out);
    const bool ok = runImaketx(local, utf8(tmp), stored, error) && commit(tmp, out, error);
    fs::remove(tmp, ec);
    return ok ? utf8(out) : std::string();
}

HdNovaPreparedTexture HdNovaPrepareTexture(std::string const &path, std::string const &colorSpaceHint,
                                           std::string const &layer, HdNovaTextureOptions const &options) {
    HdNovaPreparedTexture r;
    const std::string token = udimToken(path);
    r.udim = !token.empty();
    // A file in a package is read from its extracted copy (OIIO reads plain
    // files only); the color space rules see the packaged path, as Karma's
    // (Houdini's OCIO config has __usdz_* file rules).
    const bool packaged = ArIsPackageRelativePath(path);
    const std::string local = HdNovaLocalizeTexture(path, options, r.error);
    if (local.empty())
        return r;
    std::vector<SourceFile> files;
    if (r.udim) {
        files = udimTiles(path, token);
    } else {
        SourceFile f;
        f.path = local;
        if (stat(f))
            files.push_back(f);
    }
    if (files.empty()) {
        r.error = r.udim ? "no tile of the UDIM set exists" : "the file does not exist";
        return r;
    }

    // The first file decides the color space and the layout for the set.
    // .rat files (no OIIO reader) are read through the EXRs imaketx makes of
    // them, and their color space is decided like any file's, by the .rat's
    // name and with its stored color space as the metadata (as Karma).
    const bool rat = lowerExtension(files.front().path) == ".rat";
    const bool stored = rat && HdNovaTextureColorSpaceIsNamed(path, colorSpaceHint);
    const std::string first =
        rat ? HdNovaReadableTexture(files.front().path, options, stored, r.error) : files.front().path;
    if (first.empty())
        return r;
    auto in = OIIO::ImageInput::open(first);
    if (!in) {
        r.error = OIIO::geterror();
        return r;
    }
    const OIIO::ImageSpec spec = in->spec();
    const bool tiled = spec.tile_width > 0;
    const bool mipped = in->seek_subimage(0, 1) || (spec.width <= 1 && spec.height <= 1);
    in->close();
    r.found = true;
    r.colorSpace = HdNovaTextureColorSpace(packaged ? path : files.front().path, spec, colorSpaceHint);
    r.transform = HdNovaGetColorTransform(r.colorSpace);
    const bool nonlinear = r.transform.kind == HdNovaColorTransform::Kind::Nonlinear;
    // The tiles of a .rat UDIM set are converted under one name.
    const bool convert = (rat && r.udim) || nonlinear || !tiled || !mipped;
    // What OIIO reads when the file is used as it is.
    const auto direct = [&] { return r.udim ? directUdimName(path, token, files, options) : first; };
    if (!convert || (!options.autoTx && !rat)) {
        if (nonlinear) {
            HD_NOVA_WARN("texture '%s' is in color space '%s' and nova:textureAutoTx is off; it is used undecoded",
                         path.c_str(), r.colorSpace.c_str());
            r.transform = HdNovaColorTransform{};
        }
        r.lookupPath = direct();
        return r;
    }

    // Cache name: the source(s), the color space and its transform to the
    // rendering space (the pixels are baked in it), the layer and the version.
    uint64_t key = fnv1a(kFormatVersion);
    key = fnv1a(r.colorSpace + "|" + HdNovaSceneLinearTransformId(r.colorSpace) + "|" + layer + "|" +
                    utf8(fs::u8path(path).lexically_normal()) + (stored ? "|stored" : ""),
                key);
    for (SourceFile const &f : files)
        key = fnv1a(f.tile + "|" + std::to_string(f.size) + "|" + std::to_string(f.mtime), key);
    std::string stem = utf8(fs::u8path(local).stem());
    if (r.udim)
        stem = stem.substr(0, stem.find(token));
    const fs::path dir = fs::u8path(HdNovaTextureCacheDir(options));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = utf8(dir / fs::u8path(stem + "_" + hex(key)));
    r.lookupPath = r.udim ? base + "." + token + ".exr" : base + ".exr";
    for (SourceFile const &f : files) {
        const std::string target = r.udim ? base + "." + f.tile + ".exr" : base + ".exr";
        if (fs::exists(fs::u8path(target), ec))
            continue; // converted before
        std::string error;
        SourceFile source = f;
        if (rat) // a .rat file has one image ("C")
            source.path = HdNovaReadableTexture(f.path, options, stored, error);
        if (source.path.empty() || !convertOne(source, target, r.colorSpace, rat ? std::string() : layer, error)) {
            HD_NOVA_WARN("cannot convert texture '%s' (%s); using it unconverted", f.path.c_str(), error.c_str());
            r.lookupPath = direct();
            r.converted = false;
            if (nonlinear)
                r.transform = HdNovaColorTransform{};
            return r;
        }
    }
    r.converted = true;
    r.transform = HdNovaColorTransform{}; // baked into the converted file
    return r;
}

PXR_NAMESPACE_CLOSE_SCOPE
