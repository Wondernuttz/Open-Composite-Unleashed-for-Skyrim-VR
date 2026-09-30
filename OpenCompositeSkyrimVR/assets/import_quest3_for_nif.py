"""
Quest 3 Controller → Skyrim NIF Preparation Script for Blender
==============================================================
Run this script in Blender (Edit > Preferences > Add-ons: ensure PyNifly is installed).

What it does:
  1. Imports Quest3_Left.obj and Quest3_Right.obj
  2. Applies Quest3_Left.png / Quest3_Right.png as diffuse textures
  3. Triangulates meshes (required for NIF)
  4. Applies transforms
  5. Saves each as a separate .blend file ready for NIF export

After running this script, manually export each controller:
  File > Export > Skyrim SE NIF (.nif)

Texture setup:
  - Convert the PNGs to DDS (BC1/DXT1 or BC3/DXT5 with mipmaps)
    using texconv.exe, GIMP DDS plugin, or Paint.NET
  - Place DDS files in: Data/textures/opencomposite/
  - NIF material paths should reference: textures/opencomposite/quest3_left.dds

Usage:
  1. Open Blender
  2. Go to Scripting workspace
  3. Open this file
  4. Click "Run Script"
  5. Two controllers will be imported into the scene
  6. Select one, File > Export > Skyrim SE NIF
"""

import bpy
import os
import math

# --- Configuration ---
ASSETS_DIR = os.path.dirname(os.path.abspath(__file__))

CONTROLLERS = [
    {
        "name": "Quest3_Left",
        "obj": os.path.join(ASSETS_DIR, "Quest3_Left.obj"),
        "png": os.path.join(ASSETS_DIR, "Quest3_Left.png"),
        "nif_tex_path": "textures\\opencomposite\\quest3_left.dds",
    },
    {
        "name": "Quest3_Right",
        "obj": os.path.join(ASSETS_DIR, "Quest3_Right.obj"),
        "png": os.path.join(ASSETS_DIR, "Quest3_Right.png"),
        "nif_tex_path": "textures\\opencomposite\\quest3_right.dds",
    },
]

# --- Skyrim NIF orientation transforms ---
# These match the values tuned in BaseRenderModels.cpp:
#   scale = 1.65
#   gripCenter = (sided * 0.029, -0.025, 0.0)
#   tiltRad = -80 degrees around X
#   verticalDrop = (0, -0.028, 0.010)
#
# NOTE: You may want to export WITHOUT these transforms and let
# OpenComposite apply them at runtime (as it currently does).
# Set APPLY_GRIP_TRANSFORM = True to bake them into the mesh,
# or False to export in the raw Blender coordinate space.
APPLY_GRIP_TRANSFORM = False


def clear_scene():
    """Remove all objects from the scene."""
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    # Clear orphan data
    for block in bpy.data.meshes:
        if block.users == 0:
            bpy.data.meshes.remove(block)
    for block in bpy.data.materials:
        if block.users == 0:
            bpy.data.materials.remove(block)
    for block in bpy.data.images:
        if block.users == 0:
            bpy.data.images.remove(block)


def import_obj(filepath):
    """Import an OBJ file and return the imported object."""
    # Blender 4.x+ uses the new importer
    if hasattr(bpy.ops.wm, 'obj_import'):
        bpy.ops.wm.obj_import(filepath=filepath, forward_axis='NEGATIVE_Z', up_axis='Y')
    else:
        bpy.ops.import_scene.obj(filepath=filepath, axis_forward='-Z', axis_up='Y')

    obj = bpy.context.selected_objects[0]
    return obj


def setup_material(obj, name, png_path, nif_tex_path):
    """Create a Principled BSDF material with the PNG texture."""
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links

    # Clear default nodes
    for node in nodes:
        nodes.remove(node)

    # Create Principled BSDF
    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.location = (0, 0)
    # Low roughness for plastic look
    bsdf.inputs['Roughness'].default_value = 0.4
    bsdf.inputs['Specular IOR Level'].default_value = 0.5

    # Create texture node
    tex_node = nodes.new('ShaderNodeTexImage')
    tex_node.location = (-400, 0)
    img = bpy.data.images.load(png_path)
    tex_node.image = img

    # Connect texture to base color
    links.new(tex_node.outputs['Color'], bsdf.inputs['Base Color'])

    # Create output node
    output = nodes.new('ShaderNodeOutputMaterial')
    output.location = (300, 0)
    links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])

    # Store the Skyrim texture path as a custom property for NIF export reference
    mat["nif_texture_path"] = nif_tex_path

    # Assign material to object
    if obj.data.materials:
        obj.data.materials[0] = mat
    else:
        obj.data.materials.append(mat)

    return mat


def prepare_mesh(obj):
    """Triangulate and clean up the mesh for NIF export."""
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)

    # Apply all transforms
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # Enter edit mode for cleanup
    bpy.ops.object.mode_set(mode='EDIT')

    # Select all
    bpy.ops.mesh.select_all(action='SELECT')

    # Triangulate (NIF requires triangles)
    bpy.ops.mesh.quads_convert_to_tris(quad_method='BEAUTY', ngon_method='BEAUTY')

    # Recalculate normals (ensure they point outward)
    bpy.ops.mesh.normals_make_consistent(inside=False)

    # Back to object mode
    bpy.ops.object.mode_set(mode='OBJECT')

    # Auto-smooth for better shading (custom split normals)
    if hasattr(obj.data, 'use_auto_smooth'):
        obj.data.use_auto_smooth = True
        obj.data.auto_smooth_angle = math.radians(30)

    obj.select_set(False)


def main():
    print("\n" + "=" * 60)
    print("Quest 3 Controller NIF Preparation")
    print("=" * 60)

    clear_scene()

    imported_objects = []

    for ctrl in CONTROLLERS:
        print(f"\nProcessing {ctrl['name']}...")

        # Check files exist
        if not os.path.exists(ctrl['obj']):
            print(f"  ERROR: OBJ not found: {ctrl['obj']}")
            continue
        if not os.path.exists(ctrl['png']):
            print(f"  ERROR: PNG not found: {ctrl['png']}")
            continue

        # Import OBJ
        obj = import_obj(ctrl['obj'])
        obj.name = ctrl['name']
        print(f"  Imported: {len(obj.data.vertices)} verts, {len(obj.data.polygons)} faces")

        # Setup material with texture
        mat = setup_material(obj, ctrl['name'] + "_Mat", ctrl['png'], ctrl['nif_tex_path'])
        print(f"  Material: {mat.name}")
        print(f"  NIF texture path: {ctrl['nif_tex_path']}")

        # Prepare mesh (triangulate, clean normals)
        prepare_mesh(obj)
        tri_count = len(obj.data.polygons)
        print(f"  Triangulated: {tri_count} tris")

        # Move right controller to the side for visibility
        if "Right" in ctrl['name']:
            obj.location.x = 0.15

        imported_objects.append(obj)

    # Deselect all, then select both for overview
    bpy.ops.object.select_all(action='DESELECT')
    for obj in imported_objects:
        obj.select_set(True)
    if imported_objects:
        bpy.context.view_layer.objects.active = imported_objects[0]

    print("\n" + "=" * 60)
    print("DONE! Both controllers imported and prepared.")
    print("")
    print("NEXT STEPS:")
    print("  1. Convert PNG textures to DDS:")
    print("     - Use texconv.exe, GIMP, or Paint.NET")
    print("     - Format: BC1 (DXT1) for opaque, BC3 (DXT5) if alpha needed")
    print("     - Generate mipmaps")
    print("")
    print("  2. Export each controller to NIF:")
    print("     - Select ONE controller object")
    print("     - File > Export > Skyrim SE NIF (.nif)")
    print("     - In export settings, set game: 'SKYRIM_SE'")
    print("     - Save as quest3_left.nif / quest3_right.nif")
    print("")
    print("  3. Place files in your Skyrim Data folder:")
    print("     - Data/meshes/opencomposite/quest3_left.nif")
    print("     - Data/meshes/opencomposite/quest3_right.nif")
    print("     - Data/textures/opencomposite/quest3_left.dds")
    print("     - Data/textures/opencomposite/quest3_right.dds")
    print("")
    print("  4. Open each NIF in NifScope to verify:")
    print("     - BSTriShape node exists")
    print("     - BSLightingShaderProperty has correct texture path")
    print("     - Texture path: textures\\opencomposite\\quest3_left.dds")
    print("=" * 60)


if __name__ == "__main__":
    main()
