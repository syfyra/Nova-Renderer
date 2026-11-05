"""Nova's Houdini helpers (docs/design/M4.md 3.7, M4.8).

The Nova (Setup) tool builds what Karma's "Karma (Setup)" builds, for Nova:
a Nova Render Settings LOP, a Render Pass LOP that uses its settings, and a
USD Render ROP set to its renderer (Nova or Nova XPU) that renders the pass.

The Nova Material Builder is a Karma-Material-Builder-style subnet (D1): it
writes MaterialX and the Karma nodes Nova renders into the `kma` render
context, so the same material renders in Nova and in Karma. Its tab menu is
Karma's minus the Karma nodes Nova does not render yet (hair, fur and volumes
come with M8/M9; light filters and lenses are not materials; point clouds,
room maps and path switches later).
"""

import hou
import voptoolutils

RENDER_CONTEXT = 'kma'
FOLDER_LABEL = 'Nova Material Builder'
UNSUPPORTED_KARMA_NODES = (
    'kma_fur*', 'kma_hair', 'kma_volume', 'kma_whitewater', 'kma_pyro*',
    'kma_lfilter_*', 'kma_light_projection', 'kma_physicallens*', 'kma_roomlens', 'kma_uvlens',
    'kma_pcread', 'kma_roommap',
)
NOVA_TAB_MASK = voptoolutils.KARMAMTLX_TAB_MASK.replace(
    'karma USD ', 'karma USD ' + ' '.join('^' + n for n in UNSUPPORTED_KARMA_NODES) + ' ', 1)


def setupBuilder(subnet):
    """Turns a fresh VOP subnet into a Nova Material Builder: Standard
    Surface, a displacement and the material properties wired to the outputs
    (as Karma's builder), the kma context and Nova's tab mask."""
    voptoolutils._setupMtlXBuilderSubnet(subnet_node=subnet, mask=voptoolutils.KARMAMTLX_TAB_MASK,
                                         folder_label=FOLDER_LABEL, render_context=RENDER_CONTEXT)
    subnet.parm('tabmenumask').set(NOVA_TAB_MASK)
    return subnet


def createBuilder(parent, name='novamaterial'):
    """A Nova Material Builder in `parent` (a material network or library)."""
    subnet = parent.createNode('subnet', name)
    subnet.setShaderLanguageName('MaterialX')
    return setupBuilder(subnet)


def connectSetup(settings, rpass, rop):
    """Wires a Nova render setup: the pass renders the settings, the ROP
    renders the pass with the settings' renderer (Nova or Nova XPU, their
    Rendering Engine; without motion blur: an instant shutter)."""
    rpass.setInput(0, settings)
    rpass.parm('renderSource_control').set('set')
    rpass.parm('renderSource').setExpression('chs("../%s/settingspath")' % settings.name())
    rop.setInput(0, rpass)
    rop.parm('renderer').set('`ifs(strcmp(chs("../%s/engine"), "xpu") == 0, "HdNovaXpuRendererPlugin", "HdNovaRendererPlugin")`'
                             % settings.name())
    rop.parm('renderpass').setExpression('chs("../%s/primpath")' % rpass.name())
    rop.parm('husk_instantshutter').setExpression('1 - ch("../%s/enablemblur")' % settings.name())


def createSetup(parent, input_node=None):
    """The Nova (Setup) network in `parent` below `input_node`: (settings,
    render pass, ROP)."""
    settings = parent.createNode('nova_rendersettings', 'novarendersettings')
    if input_node is not None:
        settings.setInput(0, input_node)
        settings.setPosition(input_node.position() + hou.Vector2(0, -1))
    rpass = parent.createNode('renderpass')
    rop = parent.createNode('usdrender_rop')
    connectSetup(settings, rpass, rop)
    rpass.setPosition(settings.position() + hou.Vector2(0, -1))
    rop.setPosition(rpass.position() + hou.Vector2(0, -1))
    return settings, rpass, rop


def createSetupTool(kwargs):
    """The TAB tool: the settings where the user clicked (or wired), the
    pass and the ROP below it."""
    import loptoolutils
    settings = loptoolutils.genericTool(kwargs, 'nova_rendersettings', 'novarendersettings')
    parent = settings.parent()
    rpass = parent.createNode('renderpass')
    rop = parent.createNode('usdrender_rop')
    connectSetup(settings, rpass, rop)
    rpass.setPosition(settings.position() + hou.Vector2(0, -1))
    rop.setPosition(rpass.position() + hou.Vector2(0, -1))
    settings.setCurrent(True, clear_all_selected=True)
    return settings, rpass, rop


def createBuilderTool(kwargs):
    """The shelf/tab tool: placed where the user clicked."""
    subnet = voptoolutils.genericTool(kwargs, 'subnet', 'novamaterial')
    subnet.setShaderLanguageName('MaterialX')
    return setupBuilder(subnet)
