"""Builds Nova's LOP HDAs into houdini/package/otls/nova_lops.hda
(docs/design/M4.md D18, 3.7; laid out like Karma's own LOPs since M6.7):

  Nova Render Settings (nova_rendersettings): one node that authors a
  complete render setup for Nova: the RenderSettings prim with every
  nova:* setting of houdini/tools/nova_settings.json written explicitly
  (friendly parameter names, the table's defaults), the camera and
  resolution, and a render product with the AOVs of the Image Output tab.

  Nova (nova): the all-in-one node, as Karma's "Karma" LOP: the same
  settings plus render buttons (Render to Disk, Render to MPlay, in the
  background, Controls) and the frame range, through a USD Render ROP
  inside set to the renderer of the Rendering Engine menu (Nova or Nova
  XPU, which the settings also author as husk's default delegate).

Both share the Karma Render Settings LOP's layout: the render settings
prim, output picture, camera, resolution, samples and Simplified Shading on
top; the tabs Rendering (Sampling, Limits, Camera Effects, Lighting Effects,
Geometry and Shading, Outlines), Image Output (AOVs in the groups of Karma's
Standard Render Vars LOP, the cryptomattes; Filters, Aspect Ratio, Metadata,
Deep Output) and Advanced (Sampling, Performance, Checkpointing, Textures,
Debug).

Aspect Ratio, Metadata, Deep Output and Checkpointing are not Nova settings
but what the Karma LOP authors there, which Nova renders the same: UsdRender's
conform policy, data window and pixel aspect ratio, husk's EXR options on the
product, and two more render products of the types karma:deep and
karma:checkpoint (OUTPUT below).

Inside Nova Render Settings is a plain chain of stock LOPs whose parameters
reference the HDA's, so the stage it authors is exactly what those LOPs
author: Karma's Standard Render Vars LOP (the AOV toggles, docs/design/M6.md
D12: the vars Karma's own LOP authors), render vars for the cryptomattes,
the render product (every var under the vars scope, also those authored
upstream) and the render settings.

Both show Nova's mark on the node and in the TAB menu: the package's icon
houdini/package/config/Icons/NOVA_logo.svg (the shelf tools use it by name),
embedded in each HDA as its IconSVG section.

Run with hython in an isolated environment, with a staged package on
HOUDINI_PATH (the Nova tab of the Render Settings LOP comes from its soho/):

  powershell -ExecutionPolicy Bypass -File tests\\houdini\\run_hython.ps1 -Script houdini\\tools\\build_nova_hdas.py -Stage build\\vs2022\\stage\\Release\\nova [hda]
"""

import json
import os
import re
import shutil
import sys

import hou

HERE = os.path.dirname(os.path.abspath(__file__))
HDA = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'package', 'otls', 'nova_lops.hda')
ICON_FILE = os.path.join(HERE, '..', 'package', 'config', 'Icons', 'NOVA_logo.svg')
enc = hou.text.encodeParm

# Where each render setting goes: (tab, sub-tab[, collapsible group]);
# ('header',) puts it above the tabs, next to the camera and resolution,
# ('header', group) into a collapsible group there (as the Karma LOP's
# Simplified Shading with its occlusion and fog).
PLACE = {
    'nova:samples': ('header',),
    'nova:sampleScale': ('header',),
    'nova:simplifiedShading': ('header',),
    'nova:ambientOcclusion': ('header', 'Simplified Shading'),
    'nova:aoDistance': ('header', 'Simplified Shading'),
    'nova:fogNear': ('header', 'Simplified Shading'),
    'nova:fogFar': ('header', 'Simplified Shading'),
    'nova:fogColor': ('header', 'Simplified Shading'),
    'nova:fogAlpha': ('header', 'Simplified Shading'),
    'nova:animateNoise': ('Rendering', 'Sampling'),
    'nova:rrStartDepth': ('Rendering', 'Limits'),
    'nova:object:velocityBlur': ('Rendering', 'Camera Effects', 'Motion Blur'),
    'nova:object:instanceVelocityBlur': ('Rendering', 'Camera Effects', 'Motion Blur'),
    'nova:object:accelerationSamples': ('Rendering', 'Camera Effects', 'Motion Blur'),
    'nova:lightOverride': ('Rendering', 'Lighting Effects'),
    'nova:autoHeadlight': ('Rendering', 'Lighting Effects'),
    'nova:object:lightSource': ('Rendering', 'Lighting Effects'),
    'nova:defaultMaterial': ('Rendering', 'Geometry and Shading', 'Shading'),
    'nova:object:curveStyle': ('Rendering', 'Geometry and Shading'),
    'nova:object:pointStyle': ('Rendering', 'Geometry and Shading'),
    'nova:object:widthScale': ('Rendering', 'Geometry and Shading'),
    'nova:dicingQuality': ('Rendering', 'Geometry and Shading', 'Dicing'),
    'nova:dicingDepth': ('Rendering', 'Geometry and Shading', 'Dicing'),
    'nova:displacement': ('Rendering', 'Geometry and Shading', 'Dicing'),
    'nova:object:renderVisibility': ('Rendering', 'Geometry and Shading', 'Visibility'),
    'nova:object:holdoutMode': ('Rendering', 'Geometry and Shading', 'Visibility'),
    'nova:backgroundOccludesShadows': ('Rendering', 'Geometry and Shading', 'Visibility'),
    'nova:outline': ('Rendering', 'Outlines'),
    'nova:outlineColor': ('Rendering', 'Outlines'),
    'nova:outlineRadius': ('Rendering', 'Outlines'),
    'nova:outlineIntensity': ('Rendering', 'Outlines'),
    'nova:outlineFitMin': ('Rendering', 'Outlines'),
    'nova:outlineFitMax': ('Rendering', 'Outlines'),
    'nova:outlineDepthThreshold': ('Rendering', 'Outlines'),
    'nova:outlineApply': ('Rendering', 'Outlines'),
    'nova:outlineDepthBlend': ('Rendering', 'Outlines', 'Advanced'),
    'nova:outlineSamples': ('Rendering', 'Outlines', 'Advanced'),
    'nova:outlineStencils': ('Rendering', 'Outlines', 'Advanced'),
    'nova:outlinePattern': ('Rendering', 'Outlines', 'Advanced'),
    'nova:object:outline': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineRadius': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineContour': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineCrease': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineCreaseThreshold': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineDepth': ('Rendering', 'Outlines', 'Objects'),
    'nova:object:outlineDepthThreshold': ('Rendering', 'Outlines', 'Objects'),
    'nova:denoiser': ('Image Output', 'Filters', 'Denoiser'),
    'nova:denoiseQuality': ('Image Output', 'Filters', 'Denoiser'),
    'nova:denoiseAlbedo': ('Image Output', 'Filters', 'Denoiser'),
    'nova:denoiseNormal': ('Image Output', 'Filters', 'Denoiser'),
    'nova:timeLimit': ('Advanced', 'Performance'),
    'nova:threads': ('Advanced', 'Performance'),
    'nova:textureCacheMemory': ('Advanced', 'Textures'),
    'nova:deepUseHitDistance': ('Image Output', 'Deep Output'),
    'nova:deepExcludeHoldouts': ('Image Output', 'Deep Output'),
    'nova:pixelFilter': ('Rendering', 'Sampling'),
    'nova:pixelFilterScale': ('Rendering', 'Sampling'),
    'nova:seed': ('Rendering', 'Sampling'),
    'nova:clampIndirect': ('Rendering', 'Sampling'),
    'nova:clampDirect': ('Rendering', 'Sampling'),
    'nova:lightSamplingMode': ('Rendering', 'Sampling'),
    'nova:maxDepth': ('Rendering', 'Limits'),
    'nova:object:diffuseLimit': ('Rendering', 'Limits'),
    'nova:object:reflectLimit': ('Rendering', 'Limits'),
    'nova:object:refractLimit': ('Rendering', 'Limits'),
    'nova:object:sssLimit': ('Rendering', 'Limits'),
    'nova:object:volumeLimit': ('Rendering', 'Limits'),
    'nova:object:thinWallAtRefractLimit': ('Rendering', 'Limits'),
    'nova:maxMotionSteps': ('Rendering', 'Camera Effects', 'Motion Blur'),
    'nova:object:fakeCaustics': ('Rendering', 'Geometry and Shading', 'Caustics'),
    'nova:object:causticsRoughnessClamp': ('Rendering', 'Geometry and Shading', 'Caustics'),
    'nova:object:internalReflection': ('Rendering', 'Geometry and Shading', 'Shading'),
    'nova:constrainRoughness': ('Rendering', 'Geometry and Shading', 'Shading'),
    'nova:dicingQualityScale': ('Rendering', 'Geometry and Shading', 'Dicing'),
    'nova:offscreenQuality': ('Rendering', 'Geometry and Shading', 'Dicing'),
    'nova:subdivLevel': ('Rendering', 'Geometry and Shading', 'Subdivision'),
    'nova:subdivMaxTriangles': ('Rendering', 'Geometry and Shading', 'Subdivision'),
    'nova:textureAutoTx': ('Advanced', 'Textures'),
    'nova:textureCacheDir': ('Advanced', 'Textures'),
    'nova:textureBlur': ('Advanced', 'Textures'),
    'nova:textureFilterWidth': ('Advanced', 'Textures'),
    'nova:abortOnMissingTexture': ('Advanced', 'Textures'),
    'nova:textureStats': ('Advanced', 'Textures'),
    'nova:gpuTextureBudget': ('Advanced', 'Textures'),
    'nova:denoise': ('Image Output', 'Filters', 'Denoiser'),
    'nova:denoiseAovs': ('Image Output', 'Filters', 'Denoiser'),
    'nova:viewportDenoise': ('Image Output', 'Filters', 'Denoiser'),
    'nova:denoiseStartPasses': ('Image Output', 'Filters', 'Denoiser'),
    'nova:pixelOracle': ('Advanced', 'Sampling'),
    'nova:oracleMinSamples': ('Advanced', 'Sampling'),
    'nova:oracleThreshold': ('Advanced', 'Sampling'),
    'nova:iprProxyDepth': ('Advanced', 'Sampling'),
    # Path guiding, where Karma's LOP has it (Advanced, Sampling).
    'nova:guiding': ('Advanced', 'Sampling', 'Indirect Guiding'),
    'nova:guidingSamples': ('Advanced', 'Sampling', 'Indirect Guiding'),
    'nova:guidingComponents': ('Advanced', 'Sampling', 'Indirect Guiding'),
    'nova:guidingClearBuffers': ('Advanced', 'Sampling', 'Indirect Guiding'),
    'nova:lightStrategy': ('Advanced', 'Debug'),
}
TABS = (('Rendering', ('Sampling', 'Limits', 'Camera Effects', 'Lighting Effects', 'Geometry and Shading', 'Outlines')),
        ('Image Output', ('AOVs (Render Vars)', 'Filters', 'Aspect Ratio', 'Metadata', 'Deep Output')),
        ('Advanced', ('Sampling', 'Performance', 'Checkpointing', 'Textures', 'Debug')))
# What the Karma LOP authors beside its own settings, by sub-tab: (name, label,
# kind, default[, menu]). The inner LOPs take these in build_settings().
EXR_COMPRESSION = (('none', 'No Compression'), ('dwab', 'DWA-B, Lossy and Fast (256-scanline block)'),
                   ('dwaa', 'DWA-A, Lossy and Fast (32-scanline block)'), ('piz', 'PIZ Wavelet'),
                   ('rle', 'Runlength Encoding'), ('zips', 'ZIP (Single scanline)'), ('zip', 'ZIP (Multi-scanline blocks)'),
                   ('pix', 'PXR24 (32 bit float compression, lossy)'), ('b44', 'B44 (4x4 block compression, lossy)'),
                   ('b44a', 'B44A (4x4 block extra compression, lossy)'))
OUTPUT = {
    'Aspect Ratio': (
        ('aspectRatioConformPolicy', 'Aspect Ratio Conform Policy', 'menu', 'expandAperture',
         (('expandAperture', 'Expand Aperture'), ('cropAperture', 'Crop Aperture'),
          ('adjustApertureWidth', 'Adjust Aperture Width'), ('adjustApertureHeight', 'Adjust Aperture Height'),
          ('adjustPixelAspectRatio', 'Adjust Pixel Aspect Ratio')),
         'What gives when the image and the camera\'s aperture have different aspect ratios.'),
        ('dataWindowNDC', 'Data Window NDC', 'float4', (0.0, 0.0, 1.0, 1.0), None,
         'The part of the image that is rendered, as left, bottom, right and top in 0..1: a smaller window is a crop, '
         'values outside 0..1 are overscan.'),
        ('pixelAspectRatio', 'Pixel Aspect Ratio', 'float', 1.0, None, 'The width of a pixel over its height.'),
    ),
    'Metadata': (
        ('image_exr_compression', 'EXR Compression', 'menu', 'zips', EXR_COMPRESSION, 'The compression of the EXR husk writes.'),
        ('image_dwa_compression', 'DWA Compression Level', 'int', 85, None, 'The level of the two DWA compressions.'),
        ('image_artist', 'Artist', 'string', '', None, 'The EXR\'s artist metadata.'),
        ('image_comment', 'Comment', 'string', '', None, 'The EXR\'s comment metadata.'),
        ('image_hostname', 'Hostname', 'string', '', None, 'The EXR\'s hostname metadata.'),
    ),
    'Deep Output': (
        ('dcm', 'Deep Camera Map', 'toggle', False, None,
         'Writes a deep image next to the picture: every sample with its depth, for deep compositing.'),
        ('dcmfilename', 'DCM Filename', 'file', '$HIP/render/$HIPNAME.dcm.$F4.exr', None, 'The deep image.'),
        ('dcmcompression', 'DCM Compression', 'int', 5, None,
         '0 keeps every sample; higher values merge samples that are close in depth (10 about halves their number).'),
        ('dcmofsize', 'DCM Of Size', 'intmenu', 3, (('1', 'Monochrome'), ('3', 'Full Color')),
         'One opacity per sample, or one per colour channel.'),
        ('dcmzbias', 'DCM Z-Bias', 'float', 0.001, None, 'Samples closer in depth than this merge.'),
    ),
    'Checkpointing': (
        ('outputcheckpoint', 'Output Checkpoint Files', 'toggle', False, None,
         'Saves the render\'s state while it renders, so that an interrupted frame can go on where it stopped. '
         'A finished frame removes its checkpoint.'),
        ('checkpointfile', 'Checkpoint File', 'file', '$HIP/render/$HIPNAME.$F4.checkpoint', None, 'The checkpoint.'),
        ('savefrequency', 'Save Frequency', 'float', 60.0, None, 'Seconds between two saves.'),
        ('resume', 'Resume From Checkpoint', 'toggle', True, None,
         'A render that finds its checkpoint goes on from it instead of starting over.'),
    ),
}


def output_templates(subtab):
    out = []
    entries = OUTPUT.get(subtab, ())
    switch = entries[0][0] if entries and entries[0][2] == 'toggle' else None
    for name, label, kind, default, menu, help_text in entries:
        if kind == 'menu':
            t = hou.StringParmTemplate(name, label, 1, default_value=(default,), menu_items=[v for v, _ in menu],
                                       menu_labels=[l for _, l in menu], menu_type=hou.menuType.Normal)
        elif kind == 'intmenu':
            t = hou.IntParmTemplate(name, label, 1, default_value=(default,), menu_items=[v for v, _ in menu],
                                    menu_labels=[l for _, l in menu])
        elif kind == 'float4':
            t = hou.FloatParmTemplate(name, label, 4, default_value=default, naming_scheme=hou.parmNamingScheme.Base1)
        elif kind == 'float':
            t = hou.FloatParmTemplate(name, label, 1, default_value=(default,))
        elif kind == 'int':
            t = hou.IntParmTemplate(name, label, 1, default_value=(default,))
        elif kind == 'toggle':
            t = hou.ToggleParmTemplate(name, label, default_value=default)
        elif kind == 'file':
            t = hou.StringParmTemplate(name, label, 1, default_value=(default,),
                                       string_type=hou.stringParmType.FileReference, file_type=hou.fileType.Image)
        else:
            t = hou.StringParmTemplate(name, label, 1, default_value=(default,))
        t.setHelp(help_text)
        if switch and name != switch:
            t.setConditional(hou.parmCondType.DisableWhen, '{ %s == 0 }' % switch)
        out.append(t)
    return out
# The renderer of the Rendering Engine menu (docs/design/M10.md D1, as the
# Karma LOPs' engine): an expression on a parameter path holding cpu or xpu.
def delegate(engine):
    return '`ifs(strcmp(chs("%s"), "xpu") == 0, "HdNovaXpuRendererPlugin", "HdNovaRendererPlugin")`' % engine


CRYPTO = (('CryptoPrimitives', '__name', 'cryptoprimitives', 'Primitives'),
          ('CryptoMaterials', '__materialname', 'cryptomaterials', 'Materials'))


def friendly(key):
    """nova:object:diffuseLimit -> object_diffuseLimit (a valid parameter name)."""
    return key.split(':', 1)[1].replace(':', '_')


def ident(label):
    return re.sub(r'[^A-Za-z0-9]+', '_', label).strip('_').lower()


def template(setting):
    name, label, kind, default = friendly(setting['key']), setting['label'], setting['type'], setting['default']
    menu = setting.get('menu', ())
    if kind == 'int':
        # With a menu: its values, as the parameter files have them.
        t = hou.IntParmTemplate(name, label, 1, default_value=(int(default),), menu_items=[v for v, _ in menu],
                                menu_labels=[l for _, l in menu])
    elif kind == 'float':
        t = hou.FloatParmTemplate(name, label, 1, default_value=(float(default),))
    elif kind == 'color':
        t = hou.FloatParmTemplate(name, label, 3, default_value=tuple(float(c) for c in default), min=0.0, max=1.0,
                                  look=hou.parmLook.ColorSquare, naming_scheme=hou.parmNamingScheme.RGBA)
    elif kind == 'bool':
        t = hou.ToggleParmTemplate(name, label, default_value=bool(default))
    elif kind == 'token':
        items = [v for v, _ in setting['menu']]
        labels = [l for _, l in setting['menu']]
        t = hou.StringParmTemplate(name, label, 1, default_value=(default,), menu_items=items, menu_labels=labels,
                                   menu_type=hou.menuType.Normal)
    elif setting['key'].endswith('Dir'):
        t = hou.StringParmTemplate(name, label, 1, default_value=(default,),
                                   string_type=hou.stringParmType.FileReference,
                                   file_type=hou.fileType.Directory)
    else:
        # A menu on a string fills the field ("menutype": "replace") or is the choice.
        t = hou.StringParmTemplate(name, label, 1, default_value=(default,), menu_items=[v for v, _ in menu],
                                   menu_labels=[l for _, l in menu],
                                   menu_type=(hou.menuType.StringReplace if setting.get('menutype') == 'replace'
                                              else hou.menuType.Normal))
    if 'range' in setting and kind in ('int', 'float'):
        lo, hi = setting['range']
        t.setMinValue(lo)
        t.setMaxValue(hi)
    t.setHelp(setting['help'])
    return t


def aov_groups(node):
    """The output toggles of Karma's Standard Render Vars LOP in its own
    groups: [(label, [...])] for groups, (name, label) for toggles; without
    the colour-space and per-LPE-tag options."""
    def walk(entries, inside):
        out = []
        for t in entries:
            if t.type() == hou.parmTemplateType.Folder:
                sub = walk(t.parmTemplates(), True)
                if sub:
                    out.append((t.label(), sub))
            elif (inside and t.type() == hou.parmTemplateType.Toggle and not t.isHidden()
                  and not t.name().endswith(('outputcs', 'perlpe'))):
                out.append((t.name(), t.label()))
        return out
    return walk(node.parmTemplateGroup().entries(), False)


def leaves(groups):
    for item in groups:
        if isinstance(item[1], list):
            yield from leaves(item[1])
        else:
            yield item


def aov_templates(groups):
    out = []
    for item in groups:
        if isinstance(item[1], list):
            folder = hou.FolderParmTemplate('aovs_' + ident(item[0]), item[0], folder_type=hou.folderType.Collapsible)
            for t in aov_templates(item[1]):
                folder.addParmTemplate(t)
            out.append(folder)
        else:
            name, label = item
            t = hou.ToggleParmTemplate(name, label, default_value=(name == 'beauty'))
            t.setHelp('Karma Standard Render Vars: %s (the same render var Karma authors).' % label)
            out.append(t)
    return out


def layout(table, groups):
    """The shared parameters: (header templates, tab templates)."""
    by_key = {s['key']: s for s in table}
    missing = [k for k in by_key if k not in PLACE]
    if missing:
        raise RuntimeError('no place in the LOP layout for %s (build_nova_hdas.py PLACE)' % missing)

    prim = hou.FolderParmTemplate('renderprims', 'Render Settings Prim', folder_type=hou.folderType.Collapsible)
    for name, label, default in (('settingspath', 'RenderSettings Primitive Path', '/Render/rendersettings'),
                                 ('productpath', 'RenderProduct Primitive Path', '/Render/Products/nova'),
                                 ('varspath', 'RenderVars Parent Primitive Path', '/Render/Products/Vars')):
        prim.addParmTemplate(hou.StringParmTemplate(name, label, 1, default_value=(default,)))
    picture = hou.StringParmTemplate('picture', 'Output Picture', 1, default_value=('$HIP/render/$HIPNAME.$F4.exr',),
                                     string_type=hou.stringParmType.FileReference, file_type=hou.fileType.Image)
    camera = hou.StringParmTemplate('camera', 'Camera', 1, default_value=('/cameras/camera1',))
    camera.setHelp('The camera prim to render through.')
    camera.setTags({'script_action': 'import loputils\nloputils.selectPrimsInParm(kwargs, False)',
                    'script_action_icon': 'BUTTONS_reselect', 'sidefx::usdpathtype': 'prim'})
    resolution = hou.IntParmTemplate('resolution', 'Resolution', 2, default_value=(1920, 1080),
                                     naming_scheme=hou.parmNamingScheme.XYZW)
    engine = hou.MenuParmTemplate('engine', 'Rendering Engine', ('cpu', 'xpu'), ('CPU', 'XPU'), default_value=0)
    engine.setHelp('The renderer husk and the render buttons use (the render settings prim\'s husk:default_delegate): '
                   'Nova on the CPU, or Nova XPU on an NVIDIA GPU, the CPU taking over when no GPU is usable. '
                   'The viewport renders with the renderer chosen in its menu.')
    header = [prim, picture, camera, resolution, hou.SeparatorParmTemplate('sepheader1'), engine]
    header += [template(by_key[k]) for k in by_key if PLACE[k] == ('header',)]
    header_groups = {}
    for k in by_key:
        if PLACE[k][0] == 'header' and len(PLACE[k]) == 2:
            if PLACE[k][1] not in header_groups:
                header_groups[PLACE[k][1]] = hou.FolderParmTemplate(
                    'group_' + ident(PLACE[k][1]), PLACE[k][1], folder_type=hou.folderType.Collapsible)
            header_groups[PLACE[k][1]].addParmTemplate(template(by_key[k]))
    header += list(header_groups.values())
    header.append(hou.SeparatorParmTemplate('sepheader2'))

    # Tabs and sub-tabs as Karma Render Settings; settings in table order.
    tabs = []
    for tab, subtabs in TABS:
        tab_folder = hou.FolderParmTemplate('tab_' + ident(tab), tab, folder_type=hou.folderType.Tabs)
        for subtab in subtabs:
            sub = hou.FolderParmTemplate('tab_' + ident(tab) + '_' + ident(subtab), subtab,
                                         folder_type=hou.folderType.Tabs)
            if subtab == 'Camera Effects':
                mblur = hou.FolderParmTemplate('group_motionblur', 'Motion Blur', folder_type=hou.folderType.Simple)
                mblur.addParmTemplate(hou.ToggleParmTemplate('enablemblur', 'Enable Motion Blur', default_value=True))
                for k in by_key:
                    if PLACE[k] == (tab, subtab, 'Motion Blur'):
                        mblur.addParmTemplate(template(by_key[k]))
                sub.addParmTemplate(mblur)
                dof = hou.FolderParmTemplate('group_dof', 'Depth of Field', folder_type=hou.folderType.Simple)
                dof.addParmTemplate(hou.ToggleParmTemplate('enabledof', 'Enable Depth of Field', default_value=True))
                sub.addParmTemplate(dof)
            if subtab == 'AOVs (Render Vars)':
                for t in aov_templates(groups):
                    sub.addParmTemplate(t)
                crypto = hou.FolderParmTemplate('aovs_cryptomatte', 'Cryptomatte',
                                                folder_type=hou.folderType.Collapsible)
                for _, _, parm, what in CRYPTO:
                    t = hou.ToggleParmTemplate(parm, what, default_value=False)
                    t.setHelp('A cryptomatte of the %s, as the Karma Cryptomatte LOP authors it (rank 6).' % what.lower())
                    crypto.addParmTemplate(t)
                sub.addParmTemplate(crypto)
            for t in output_templates(subtab):
                sub.addParmTemplate(t)
            groups_here = {}
            for k in by_key:
                place = PLACE[k]
                if place[:2] != (tab, subtab) or (subtab == 'Camera Effects' and place[2:] == ('Motion Blur',)):
                    continue
                if len(place) == 2:
                    t = template(by_key[k])
                    if subtab in OUTPUT and OUTPUT[subtab][0][2] == 'toggle':
                        t.setConditional(hou.parmCondType.DisableWhen, '{ %s == 0 }' % OUTPUT[subtab][0][0])
                    sub.addParmTemplate(t)
                else:
                    if place[2] not in groups_here:
                        groups_here[place[2]] = hou.FolderParmTemplate(
                            'group_' + ident(place[2]), place[2], folder_type=hou.folderType.Collapsible)
                    groups_here[place[2]].addParmTemplate(template(by_key[k]))
            for folder in groups_here.values():
                sub.addParmTemplate(folder)
            tab_folder.addParmTemplate(sub)
        tabs.append(tab_folder)
    return header, tabs


def value_names(templates):
    """The parameter names of value templates (not folders, separators or buttons)."""
    for t in templates:
        if t.type() == hou.parmTemplateType.Folder:
            yield from value_names(t.parmTemplates())
        elif t.type() not in (hou.parmTemplateType.Separator, hou.parmTemplateType.Button,
                              hou.parmTemplateType.Label):
            yield t


def link(node, templates):
    """References node's parameters of these names to its parent's."""
    for t in value_names(templates):
        tup = node.parmTuple(t.name())
        if tup is None:
            raise RuntimeError('%s has no parameter %s' % (node.path(), t.name()))
        for p in tup:
            if t.type() == hou.parmTemplateType.String:
                p.set('`chs("../%s")`' % p.name())
            else:
                p.setExpression('ch("../%s")' % p.name())


def set_icon(definition):
    """Nova's mark as the HDA's icon, embedded (the node and its TAB entry
    show it wherever the HDA is loaded)."""
    with open(ICON_FILE, encoding='utf-8') as f:
        definition.addSection('IconSVG', f.read())
    definition.setIcon('opdef:/%s/%s?IconSVG' % (definition.nodeTypeCategory().name(), definition.nodeTypeName()))


def tool_section(submenu):
    return '''<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>LOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>%s</toolSubmenu>
    <script scriptType="python"><![CDATA[import loptoolutils

loptoolutils.genericTool(kwargs, '$HDA_NAME')]]></script>
  </tool>
</shelfDocument>
''' % submenu


def build_settings(table):
    stage = hou.node('/stage')
    subnet = stage.createNode('subnet', 'nova_rendersettings')
    subnet.setInput(0, None)

    # The AOVs: Karma's Standard Render Vars LOP, its toggles driven by the
    # HDA's (beauty on, the rest off); the beauty in 32-bit float.
    aovs = subnet.createNode('karmastandardrendervars', 'aovs')
    aovs.setInput(0, subnet.indirectInputs()[0])
    aovs.parm('parentprimpath').set('`chs("../varspath")`')
    aovs.parm('beautyprecision').set('color4f')
    groups = aov_groups(aovs)
    for name, _ in leaves(groups):
        aovs.parm(name).setExpression('ch("../%s")' % name)

    # Cryptomattes, as Karma's Cryptomatte LOP authors them.
    last = aovs
    for name, source, parm_name, _ in CRYPTO:
        rv = subnet.createNode('rendervar', name.lower())
        rv.setInput(0, last)
        rv.parm('primpath').set('`chs("../varspath")`/' + name)
        for parm, value in (('sourceName', source), ('dataType', 'float3'), ('sourceType', 'primvar'),
                            (enc('driver:parameters:aov:husk:format'), 'float3'),
                            (enc('driver:parameters:aov:husk:name'), name),
                            (enc('driver:parameters:aov:husk:multiSampled'), 0),
                            (enc('driver:parameters:aov:karma:cryptomatte'), 1),
                            (enc('driver:parameters:aov:karma:cryptomatterank'), 6)):
            if rv.parm(parm) is None:
                raise RuntimeError('the Render Var LOP has no parameter %s' % hou.text.decodeParm(parm))
            control = rv.parm(parm + '_control') or rv.parm(enc(hou.text.decodeParm(parm) + '_control'))
            if control is not None:
                control.set('set')
            rv.parm(parm).set(value)
        switch = subnet.createNode('switch', name.lower() + '_on')
        switch.setInput(0, last)
        switch.setInput(1, rv)
        switch.parm('input').setExpression('ch("../%s")' % parm_name)
        last = switch

    rp = subnet.createNode('renderproduct', 'product')
    rp.setInput(0, last)
    rp.parm('primpath').set('`chs("../productpath")`')
    rp.parm('productName_control').set('set')
    rp.parm('productName').set('`chs("../picture")`')
    rp.parm('orderedVars_control').set('set')
    # The beauty first, then every var in the scope.
    rp.parm('orderedVars').set('`chs("../varspath")`/beauty `chs("../varspath")`/*')

    # husk's EXR options on the product (the Karma LOP's Metadata tab).
    for parm, source, text in (('driver:parameters:husk:OpenEXR:compression', 'image_exr_compression', True),
                               ('driver:parameters:husk:OpenEXR:dwa_compression', 'image_dwa_compression', False),
                               ('driver:parameters:husk:artist', 'image_artist', True),
                               ('driver:parameters:husk:comment', 'image_comment', True),
                               ('driver:parameters:husk:hostname', 'image_hostname', True)):
        control, value = rp.parm(enc(parm + '_control')), rp.parm(enc(parm))
        if control is None or value is None:
            raise RuntimeError('the Render Product LOP has no parameter %s' % parm)
        control.set('set')
        if text:
            value.set('`chs("../%s")`' % source)
        else:
            value.setExpression('ch("../%s")' % source)

    # The deep camera map: a product of the type karma:deep with the picture's
    # vars, as the Karma LOP's Deep Output authors it (docs/design/M11-P6.md).
    deep = subnet.createNode('renderproduct', 'deep')
    deep.setInput(0, rp)
    deep.parm('primpath').set('`chs("../productpath")`_deep')
    deep.parm('productType_control').set('set')
    deep.parm('productType').set('karma:deep')
    deep.parm('productName_control').set('set')
    deep.parm('productName').set('`chs("../dcmfilename")`')
    deep.parm('orderedVars_control').set('set')
    deep.parm('orderedVars').set('`chs("../varspath")`/beauty `chs("../varspath")`/*')
    for parm, source in (('karma:product:dcmcompression', 'dcmcompression'), ('karma:product:dcmofsize', 'dcmofsize'),
                         ('karma:product:dcmzbias', 'dcmzbias')):
        control, value = deep.parm(enc(parm + '_control')), deep.parm(enc(parm))
        if control is None or value is None:
            raise RuntimeError('the Render Product LOP has no parameter %s (Karma\'s product parameters)' % parm)
        control.set('set')
        value.setExpression('ch("../%s")' % source)
    deep_on = subnet.createNode('switch', 'deep_on')
    deep_on.setInput(0, rp)
    deep_on.setInput(1, deep)
    deep_on.parm('input').setExpression('ch("../dcm")')

    # Checkpoints: a product of the type karma:checkpoint; its two values are
    # plain attributes of the prim, which a Python LOP authors.
    checkpoint = subnet.createNode('renderproduct', 'checkpoint')
    checkpoint.setInput(0, deep_on)
    checkpoint.parm('primpath').set('`chs("../productpath")`_checkpoint')
    checkpoint.parm('productType_control').set('set')
    checkpoint.parm('productType').set('karma:checkpoint')
    checkpoint.parm('productName_control').set('set')
    checkpoint.parm('productName').set('`chs("../checkpointfile")`')
    checkpoint.parm('orderedVars_control').set('set')
    checkpoint.parm('orderedVars').set('`chs("../varspath")`/beauty')
    values = subnet.createNode('pythonscript', 'checkpoint_values')
    values.setInput(0, checkpoint)
    values.parm('python').set('''from pxr import Sdf
node = hou.pwd()
hda = node.parent()
prim = node.editableStage().GetPrimAtPath(hda.evalParm('productpath') + '_checkpoint')
if prim:
    prim.CreateAttribute('saveFrequency', Sdf.ValueTypeNames.Float).Set(float(hda.evalParm('savefrequency')))
    prim.CreateAttribute('resume', Sdf.ValueTypeNames.Bool).Set(bool(hda.evalParm('resume')))
''')
    checkpoint_on = subnet.createNode('switch', 'checkpoint_on')
    checkpoint_on.setInput(0, deep_on)
    checkpoint_on.setInput(1, values)
    checkpoint_on.parm('input').setExpression('ch("../outputcheckpoint")')

    rs = subnet.createNode('rendersettings', 'settings')
    rs.setInput(0, checkpoint_on)
    rs.parm('primpath').set('`chs("../settingspath")`')
    rs.parm('camera_control').set('set')
    rs.parm('camera').set('`chs("../camera")`')
    rs.parm('products_control').set('set')
    # The picture's product, and the deep and checkpoint products while they are on.
    rs.parm('products').set('`chs("../productpath")` `ifs(ch("../dcm"), chs("../productpath") + "_deep", "")` '
                            '`ifs(ch("../outputcheckpoint"), chs("../productpath") + "_checkpoint", "")`')
    # UsdRender's framing (the Karma LOP's Aspect Ratio tab).
    rs.parm('aspectRatioConformPolicy_control').set('set')
    rs.parm('aspectRatioConformPolicy').set('`chs("../aspectRatioConformPolicy")`')
    rs.parm('dataWindowNDC_control').set('set')
    for i in '1234':
        rs.parm('dataWindowNDC' + i).setExpression('ch("../dataWindowNDC%s")' % i)
    rs.parm('pixelAspectRatio_control').set('set')
    rs.parm('pixelAspectRatio').setExpression('ch("../pixelAspectRatio")')
    rs.parm('res_mode').set('manual')
    for name in ('resolution1', 'resolution2'): # locked by the default auto-height mode
        rs.parm(name).lock(False)
    rs.parm('resolution_control').set('set')
    rs.parm('resolution1').setExpression('ch("../resolutionx")')
    rs.parm('resolution2').setExpression('ch("../resolutiony")')
    rs.parm(enc('husk:default_delegate_control')).set('set')
    rs.parm(enc('husk:default_delegate')).set(delegate('../engine'))
    for usd, toggle in (('disableMotionBlur', 'enablemblur'), ('disableDepthOfField', 'enabledof')):
        if rs.parm(usd) is None:
            raise RuntimeError('the Render Settings LOP has no %s parameter' % usd)
        rs.parm(usd + '_control').set('set')
        rs.parm(usd).setExpression('1 - ch("../%s")' % toggle)
    for s in table:
        control, value = rs.parm(enc(s['key'] + '_control')), rs.parmTuple(enc(s['key']))
        if control is None or value is None:
            raise RuntimeError('the Render Settings LOP has no Nova parameter for %s (is the Nova tab there?)' % s['key'])
        control.set('set')
        if s['type'] in ('token', 'string'):
            value[0].set('`chs("../%s")`' % friendly(s['key']))
        elif s['type'] == 'color':
            for component, suffix in zip(value, 'rgb'):
                component.setExpression('ch("../%s%s")' % (friendly(s['key']), suffix))
        else:
            value[0].setExpression('ch("../%s")' % friendly(s['key']))
    subnet.node('output0').setInput(0, rs)
    subnet.layoutChildren()

    node = subnet.createDigitalAsset(name='nova_rendersettings', hda_file_name=HDA,
                                     description='Nova Render Settings', min_num_inputs=0, max_num_inputs=1,
                                     version='1.0')
    definition = node.type().definition()
    header, tabs = layout(table, groups)
    group = hou.ParmTemplateGroup()
    for t in header + tabs:
        group.append(t)
    definition.setParmTemplateGroup(group)
    definition.setComment('Nova Render Settings: a complete Nova render setup (generated by build_nova_hdas.py).')
    set_icon(definition)
    definition.addSection('Tools.shelf', tool_section('Rendering/Nova'))
    definition.save(HDA, node)
    node.destroy()
    return groups


def button(name, label, rop_parm, help_text):
    t = hou.ButtonParmTemplate(name, label, script_callback="kwargs['node'].node('render').parm('%s').pressButton()" % rop_parm,
                               script_callback_language=hou.scriptLanguage.Python)
    t.setHelp(help_text)
    return t


def build_all_in_one(table, groups):
    hou.hda.installFile(HDA)
    stage = hou.node('/stage')
    subnet = stage.createNode('subnet', 'nova')
    subnet.setInput(0, None)
    settings = subnet.createNode('nova_rendersettings', 'settings')
    settings.setInput(0, subnet.indirectInputs()[0])
    rop = subnet.createNode('usdrender_rop', 'render')
    rop.setInput(0, settings)
    rop.parm('renderer').set(delegate('../engine'))
    rop.parm('rendersettings').set('`chs("../settingspath")`')
    rop.parm('trange').setExpression('ch("../trange")')
    for p in ('f1', 'f2', 'f3'):
        rop.parm(p).setExpression('ch("../%s")' % p)
    rop.parm('allframesatonce').setExpression('ch("../allframesatonce")')
    rop.parm('husk_instantshutter').setExpression('1 - ch("../enablemblur")')
    subnet.node('output0').setInput(0, settings)
    subnet.layoutChildren()

    node = subnet.createDigitalAsset(name='nova', hda_file_name=HDA, description='Nova', min_num_inputs=0,
                                     max_num_inputs=1, version='1.0')
    definition = node.type().definition()
    header, tabs = layout(table, groups)
    group = hou.ParmTemplateGroup()
    group.append(button('execute', 'Render to Disk', 'execute', 'Renders the frames to the output picture with husk.'))
    group.append(button('renderpreview', 'Render to MPlay', 'renderpreview', 'Renders into MPlay.'))
    group.append(button('executebackground', 'Render to Disk in Background', 'executebackground',
                        'Renders to disk in a background husk process.'))
    group.append(button('renderdialog', 'Controls...', 'renderdialog', 'The render dialog of the frame range.'))
    group.append(hou.MenuParmTemplate('trange', 'Valid Frame Range', ('off', 'normal', 'on', 'stage'),
                                      ('Render Current Frame', 'Render Specific Frame Range Only',
                                       'Render Specific Frame Range', 'Render Frame Range from Stage'),
                                      default_value=0))
    f = hou.FloatParmTemplate('f', 'Start/End/Inc', 3, default_value=(1.0, 240.0, 1.0),
                              default_expression=('$FSTART', '$FEND', ''),
                              naming_scheme=hou.parmNamingScheme.Base1)
    f.setConditional(hou.parmCondType.DisableWhen, '{ trange == off } { trange == stage }')
    group.append(f)
    group.append(hou.ToggleParmTemplate('allframesatonce', 'Render All Frames with a Single Process', default_value=False))
    common = hou.FolderParmTemplate('common', 'Common Settings', folder_type=hou.folderType.Simple)
    for t in header:
        common.addParmTemplate(t)
    group.append(common)
    for t in tabs:
        group.append(t)
    definition.setParmTemplateGroup(group)
    # The inner settings follow the node's parameters.
    link(node.node('settings'), header + tabs)
    definition.setComment('Nova: Nova render settings and render buttons in one node (generated by build_nova_hdas.py).')
    set_icon(definition)
    definition.addSection('Tools.shelf', tool_section('Rendering'))
    definition.updateFromNode(node)
    definition.save(HDA, node)


def main():
    with open(os.path.join(HERE, 'nova_settings.json')) as f:
        table = [s for s in json.load(f)['settings'] if 'global' in s['scopes'] and s.get('hda', True)]
    os.makedirs(os.path.dirname(os.path.abspath(HDA)), exist_ok=True)
    if os.path.exists(HDA):
        os.remove(HDA)
    groups = build_settings(table)
    build_all_in_one(table, groups)
    # Houdini keeps numbered copies of an overwritten HDA; they are not part
    # of the package.
    shutil.rmtree(os.path.join(os.path.dirname(HDA), 'backup'), ignore_errors=True)
    print('wrote', os.path.normpath(HDA), 'with', len(table), 'Nova settings and', len(list(leaves(groups))) + 2,
          'AOV toggles: Nova Render Settings and Nova')


main()
