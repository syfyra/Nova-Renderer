// The Hydra tests' delegates render geometry without a material as the matte
// surface of its display colour (nova:defaultMaterial "lambert"), whose
// values the tests can state: a display colour of 0.5 under a white dome is
// 0.5. The shipped default is Karma's plastic (0.4 of the colour and a
// highlight), which test_hydra_default_material.cpp sets and measures.

#include "hydra/renderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const bool kLambertDefault = (HdNovaRenderDelegate::SetLambertDefaultForTests(true), true);
}
