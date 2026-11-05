# M3 research notes

Reports written before M3 (shading core) by research agents, 2026-10-12. They
record verified facts about Houdini 22.0.466 / USD 26.05 / MaterialX 1.39.5 /
OIIO / OCIO, formulas with sources and design recommendations. They are input
to the M3 design, not a specification: where the design document and these
notes disagree, the design document wins. Paths to scratch files inside them
refer to throwaway probes that are not kept.

| File | Topic |
|---|---|
| `hydra-materials.md` | how materials reach a Hydra delegate in Houdini 22 (render contexts, Hydra 2 schemas, edits, primvars) |
| `surface-models.md` | Autodesk Standard Surface 1.0.1 and OpenPBR 1.1: parameters, layering, MaterialX implementations |
| `microfacet.md` | GGX/VNDF, Fresnel/F82, energy compensation, thin film, sheen, EON diffuse, layering |
| `shader-vm.md` | shader IR and bytecode VM designs (Cycles SVM, OSL, pbrt) and a design for Nova |
| `mtlx-nodes.md` | MaterialX stdlib nodes needed, their exact semantics and Karma's implementations |
| `textures.md` | OIIO TextureSystem, UDIM, tx, filtering footprints, color management, GPU path |
| `test-strategy.md` | verification plan: furnace/chi-square tests and Karma-referenced material scenes |
