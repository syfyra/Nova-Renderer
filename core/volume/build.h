#pragma once

// What Scene::commit() derives from a volume (docs/design/M8.md D5-D7): the
// volume shader's field bindings, the reference field and its majorant
// cells, each cell's extinction bound (the default volume: density's
// maximum; a volume material: an interval run of its program over the
// cell's field ranges), and the world bounds over the shutter.

#include "core/scene/scene.h"

namespace nova {

// `material`: the record of the volume's material if it has a volume
// shader, else null (the default volume). `frame`, `fps`: the frame being
// rendered (RenderSettings'), for shaders that read Time or Frame.
void buildVolumeRecord(VolumeRecord &rec, const MaterialRecord *material, float frame = 1.0f, float fps = 24.0f);

// The field of `name` a volume shader reads with component k of a `width`
// wide read (D5): "name.x" / "name_x" style components, else a scalar field
// of that name for every component; -1 if none.
int32_t volumeFieldComponent(const VolumeDesc &desc, const std::string &name, int width, int k);

} // namespace nova
