# DaDirt - build the two assets that cannot be written as text.
#
# Run from a shell (the editor launches, builds the assets, saves, exits):
#
#   "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
#       "C:\Users\alex\Desktop\DaDirt\DaDirt.uproject" ^
#       -ExecutePythonScript="C:\Users\alex\Desktop\DaDirt\Tools\BuildDirtAssets.py"
#
# It creates:
#   /Game/Dirt/RT_DirtPlaceholder  - a tiny linear render target used as the default
#                                    texture of each parameter (a linear-colour
#                                    sampler refuses the engine's sRGB default texture)
#   /Game/Dirt/M_DirtGround        - the ground material described in
#                                    docs/DirtboxSetup.md, node for node
#   /Game/Maps/L_Dirtbox           - the Basic template level with its Floor removed
#
# Re-running is safe: existing assets are deleted and rebuilt.

import unreal

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

DIRT_DIR = "/Game/Dirt"
MAPS_DIR = "/Game/Maps"
RT_PATH = DIRT_DIR + "/RT_DirtPlaceholder"
MAT_PATH = DIRT_DIR + "/M_DirtGround"
MAP_PATH = MAPS_DIR + "/L_Dirtbox"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"


def log(msg):
    unreal.log("[BuildDirtAssets] " + msg)


def fresh(path):
    if EAL.does_asset_exist(path):
        if not EAL.delete_asset(path):
            raise RuntimeError("could not delete existing " + path)
        log("deleted existing " + path)


# Delete in dependency order: the level and material reference the placeholder.
fresh(MAP_PATH)
fresh(MAT_PATH)
fresh(RT_PATH)


# ---------------------------------------------------------------------------
# Placeholder render target (linear, so the LinearColor samplers compile)
# ---------------------------------------------------------------------------

fresh(RT_PATH)
rt = TOOLS.create_asset("RT_DirtPlaceholder", DIRT_DIR, unreal.TextureRenderTarget2D,
                        unreal.TextureRenderTargetFactoryNew())
rt.set_editor_property("render_target_format", unreal.TextureRenderTargetFormat.RTF_RGBA16F)
rt.set_editor_property("size_x", 4)
rt.set_editor_property("size_y", 4)
rt.set_editor_property("clear_color", unreal.LinearColor(0.5, 0.5, 1.0, 0.0))
EAL.save_asset(RT_PATH)
log("placeholder render target saved")


# ---------------------------------------------------------------------------
# Ground material
# ---------------------------------------------------------------------------

fresh(MAT_PATH)
mat = TOOLS.create_asset("M_DirtGround", DIRT_DIR, unreal.Material, unreal.MaterialFactoryNew())

# Our normal texture is world space, not tangent space.
mat.set_editor_property("tangent_space_normal", False)


def tex_param(name, x, y, mip_level_mode=False):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("texture", rt)
    node.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    if mip_level_mode:
        node.set_editor_property("mip_value_mode", unreal.TextureMipValueMode.TMVM_MIP_LEVEL)
    return node


def const(value, x, y):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property("r", value)
    return node


def connect(src, src_out, dst, dst_in):
    ok = MEL.connect_material_expressions(src, src_out, dst, dst_in)
    if not ok:
        raise RuntimeError("could not connect %s.%s -> %s.%s" % (src.get_name(), src_out, dst.get_name(), dst_in))


def connect_prop(src, src_out, prop):
    ok = MEL.connect_material_property(src, src_out, prop)
    if not ok:
        raise RuntimeError("could not connect %s.%s -> %s" % (src.get_name(), src_out, prop))


# Node 1 - height -> World Position Offset (0, 0, height)
disp = tex_param("DirtDisplay", -900, -300, mip_level_mode=True)
mip0 = const(0.0, -1150, -200)
# The mip input is named after its mode; try the names the engine has used.
for in_name in ("MipLevel", "Level", "MipValue"):
    if MEL.connect_material_expressions(mip0, "", disp, in_name):
        log("mip level input connected as '%s'" % in_name)
        break
else:
    raise RuntimeError("could not find the mip level input on the DirtDisplay sampler")

xy0 = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant2Vector, -650, -450)
xy0.set_editor_property("r", 0.0)
xy0.set_editor_property("g", 0.0)
append = MEL.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -400, -350)
connect(xy0, "", append, "A")
connect(disp, "A", append, "B")
connect_prop(append, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

# Node 2 - colour
debug = tex_param("DirtDebug", -900, 0)
connect_prop(debug, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)

# Node 3 - world-space normal, unpacked from 0..1 to -1..1
normal = tex_param("DirtNormal", -900, 300)
mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -600, 320)
mul.set_editor_property("const_b", 2.0)
connect(normal, "RGB", mul, "A")
sub = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, -400, 320)
sub.set_editor_property("const_b", 1.0)
connect(mul, "", sub, "A")
connect_prop(sub, "", unreal.MaterialProperty.MP_NORMAL)

# Node 4 - not plastic
connect_prop(const(0.9, -400, 550), "", unreal.MaterialProperty.MP_ROUGHNESS)
connect_prop(const(0.05, -400, 650), "", unreal.MaterialProperty.MP_SPECULAR)

MEL.recompile_material(mat)
EAL.save_asset(MAT_PATH)
log("material saved: %s (%d expressions)" % (MAT_PATH, MEL.get_num_material_expressions(mat)))


# ---------------------------------------------------------------------------
# Level
# ---------------------------------------------------------------------------

level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

fresh(MAP_PATH)
if not level_sub.new_level_from_template(MAP_PATH, TEMPLATE):
    raise RuntimeError("could not create level from template")

# The Floor goes because the Dirtbox is the ground. The volumetric cloud goes
# because it costs ~3 ms of GPU on the Arc iGPU - more than the whole dirt sim -
# and the sandbox map is meant to keep heavy rendering features off.
REMOVE_LABELS = ("Floor", "VolumetricCloud")
removed = 0
for actor in actor_sub.get_all_level_actors():
    label = actor.get_actor_label()
    if label in REMOVE_LABELS or isinstance(actor, unreal.VolumetricCloud):
        actor_sub.destroy_actor(actor)
        removed += 1
        log("removed level actor '%s'" % label)

if not level_sub.save_current_level():
    raise RuntimeError("could not save level")
log("level saved: %s (%d actors removed)" % (MAP_PATH, removed))

log("DONE")
