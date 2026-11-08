#include "core/film/film.h"

#include <algorithm>

namespace nova {

uint8_t utilityWidth(UtilityKind kind) {
    switch (kind) {
    case UtilityKind::FacingN:
    case UtilityKind::FacingNg:
    case UtilityKind::HitDist:
    case UtilityKind::Face:
    case UtilityKind::Ao: // the unoccluded sum, written to every channel
    case UtilityKind::BakeAlpha:
        return 1;
    case UtilityKind::Primvar:
        return 4;
    default:
        return 3;
    }
}

namespace {

void clearPlane(UtilityPlane &p) {
    std::fill(p.data.begin(), p.data.end(), p.request.kind == UtilityKind::Face ? -1.0f : 0.0f);
}

} // namespace

void Film::setUtility(const std::vector<UtilityRequest> &requests) {
    utility.clear();
    for (const UtilityRequest &r : requests) {
        UtilityPlane p;
        p.request = r;
        p.width = utilityWidth(r.kind);
        p.data.resize(pixelCount() * p.width);
        clearPlane(p);
        utility.push_back(std::move(p));
    }
}

void Film::setCrypto(const std::vector<CryptoRequest> &requests) {
    crypto.clear();
    for (const CryptoRequest &r : requests) {
        CryptoPlane p;
        p.request = r;
        p.resize(pixelCount());
        crypto.push_back(std::move(p));
    }
}

UtilityPlane *Film::findUtility(UtilityKind kind, const std::string &name) {
    for (UtilityPlane &p : utility)
        if (p.request.kind == kind && p.request.name == name)
            return &p;
    return nullptr;
}

void Film::resize(int w, int h) {
    width = std::max(w, 0);
    height = std::max(h, 0);
    const size_t n = pixelCount();
    colorSum.resize(n);
    alphaSum.resize(n);
    count.resize(n);
    depth.resize(n);
    normal.resize(n);
    albedo.resize(n);
    primId.resize(n);
    instanceId.resize(n);
    lpeSum.resize(n * lpeOutputs);
    lpeLit.resize(n * lpeShadowOutputs);
    for (UtilityPlane &p : utility)
        p.data.resize(n * p.width);
    for (CryptoPlane &p : crypto)
        p.resize(n);
    clear();
}

void Film::setLpeOutputs(uint32_t outputs, uint32_t shadowOutputs) {
    lpeOutputs = outputs;
    lpeShadowOutputs = shadowOutputs;
    lpeSum.assign(pixelCount() * outputs, Spectrum{0.0f, 0.0f, 0.0f});
    lpeLit.assign(pixelCount() * shadowOutputs, Spectrum{0.0f, 0.0f, 0.0f});
}

void Film::clear() {
    samples = 0;
    std::fill(colorSum.begin(), colorSum.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(alphaSum.begin(), alphaSum.end(), 0.0f);
    std::fill(count.begin(), count.end(), 0u);
    std::fill(depth.begin(), depth.end(), 0.0f);
    std::fill(normal.begin(), normal.end(), Vec3f{0.0f, 0.0f, 0.0f});
    std::fill(albedo.begin(), albedo.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(primId.begin(), primId.end(), -1);
    std::fill(instanceId.begin(), instanceId.end(), -1);
    std::fill(lpeSum.begin(), lpeSum.end(), Spectrum{0.0f, 0.0f, 0.0f});
    std::fill(lpeLit.begin(), lpeLit.end(), Spectrum{0.0f, 0.0f, 0.0f});
    for (UtilityPlane &p : utility)
        clearPlane(p);
    for (CryptoPlane &p : crypto)
        p.resize(pixelCount());
}

} // namespace nova
