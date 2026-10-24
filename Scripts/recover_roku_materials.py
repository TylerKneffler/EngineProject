"""Recover Roku's packed Blender materials without re-exporting its rig or animation.

Run with Blender 5.x: blender --background --python Scripts/recover_roku_materials.py
    -- source.blend Engine/Core/Assets/Models/mmd/Roku/Roku_V1_packed.fbx
Custom Blender BSDFs are approximated by baked base color, roughness and normal
maps. Hair opacity is baked into base-color alpha. Original files stay intact.
"""
import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import numpy as np


def source_socket(tree, socket):
    if socket.is_linked:
        return socket.links[0].from_socket
    node = tree.nodes.new('ShaderNodeRGB' if socket.type == 'RGBA' else 'ShaderNodeValue')
    node.outputs[0].default_value = socket.default_value
    return node.outputs[0]


def expose(group, name, socket):
    tree = group.node_tree
    tree.interface.new_socket(name=name, in_out='OUTPUT', socket_type='NodeSocketColor')
    output = next(n for n in tree.nodes if n.type == 'GROUP_OUTPUT' and n.is_active_output)
    tree.links.new(socket, output.inputs[name])
    return group.outputs[name]


def mix(tree, factor, a, b):
    node = tree.nodes.new('ShaderNodeMixRGB')
    for socket, value in zip(node.inputs, (factor, a, b)):
        tree.links.new(value, socket)
    return node.outputs[0]


def parameters(material):
    """Resolve the authored skin/latex mixture before reducing to engine PBR."""
    tree = material.node_tree
    skin = next((n for n in tree.nodes if n.type == 'GROUP' and 'latex mask' in n.inputs), None)
    if skin:
        skin.node_tree = skin.node_tree.copy()
        inner = skin.node_tree
        blend = inner.nodes['Mix Shader']
        a, b = (i.links[0].from_node for i in list(blend.inputs)[1:3])
        fac = source_socket(inner, blend.inputs[0])
        color = expose(skin, 'EngineColor', mix(inner, fac, source_socket(inner, a.inputs['Base Color']), source_socket(inner, b.inputs['Base Color'])))
        rough = expose(skin, 'EngineRoughness', mix(inner, fac, source_socket(inner, a.inputs['Roughness']), source_socket(inner, b.inputs['Roughness'])))
        # Combine tangent-space inputs rather than baking world-space normals.
        normal_a = source_socket(tree, skin.inputs['Normal'])
        normal_b = source_socket(tree, skin.inputs['latex mask Normal'])
        factor = expose(skin, 'EngineLatex', fac)
        normal = mix(tree, factor, normal_a, normal_b)
        return color, rough, normal, None, 0.0
    hair = tree.nodes.get('Hair')
    if hair:
        return source_socket(tree, hair.inputs['Base Color']), None, None, source_socket(tree, hair.inputs['Alpha']), .26335877
    principled = next((n for n in tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if not principled:
        raise RuntimeError('No supported shader in ' + material.name)
    normal = None
    if principled.inputs['Normal'].is_linked:
        n = principled.inputs['Normal'].links[0].from_node
        if n.type == 'NORMAL_MAP':
            normal = source_socket(tree, n.inputs['Color'])
    return (source_socket(tree, principled.inputs['Base Color']),
            source_socket(tree, principled.inputs['Roughness']), normal,
            source_socket(tree, principled.inputs['Alpha']),
            principled.inputs['Metallic'].default_value)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('blend', type=Path)
    parser.add_argument('fbx', type=Path)
    parser.add_argument('--size', type=int, default=2048)
    parser.add_argument('--material', help='Regenerate only this source material')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    bpy.ops.wm.open_mainfile(filepath=str(args.blend.resolve()))
    originals = list(bpy.data.materials)
    # Preserve the authored UV tile used by each material (including UDIM arms).
    tiles = {}
    for obj in bpy.data.objects:
        if obj.type != 'MESH' or not obj.data.uv_layers.active:
            continue
        uv = obj.data.uv_layers.active.data
        for poly in obj.data.polygons:
            if poly.material_index >= len(obj.material_slots):
                continue
            mat = obj.material_slots[poly.material_index].material
            if mat:
                center = sum((uv[i].uv.x for i in poly.loop_indices)) / len(poly.loop_indices)
                tiles.setdefault(mat.name, []).append(math.floor(center + 1e-5))
    tile_by_name = {k: max(set(v), key=v.count) for k,v in tiles.items()}
    scene = bpy.data.scenes.new('Engine material recovery')
    bpy.context.window.scene = scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 1
    scene.cycles.device = 'CPU'
    scene.render.bake.margin = 0
    bpy.ops.mesh.primitive_plane_add(size=2)
    plane = bpy.context.object
    plane.name = 'Material sampling plane'
    original_uv = plane.data.uv_layers.active
    original_uv.name = 'SourceUV'
    bake_uv = plane.data.uv_layers.new(name='BakeUV')
    for a,b in zip(original_uv.data, bake_uv.data):
        b.uv = a.uv
    plane.data.uv_layers.active = original_uv
    original_uv.active_render = True
    destination = args.fbx.resolve().parent / 'Roku_RecoveredTextures'
    destination.mkdir(parents=True, exist_ok=True)
    entries = []
    for original in originals:
        if original.name == 'Backdrop' or original.name not in tile_by_name:
            continue
        if args.material and original.name != args.material:
            continue
        material = original.copy()
        tree = material.node_tree
        plane.data.materials.clear()
        plane.data.materials.append(material)
        tile = tile_by_name[original.name]
        for a,b in zip(original_uv.data,bake_uv.data):
            a.uv = (b.uv.x + tile, b.uv.y)
        color,rough,normal,alpha,metallic = parameters(material)
        # Unlinked texture coordinates always sample the authored UV map.
        uv_node = tree.nodes.new('ShaderNodeUVMap')
        uv_node.uv_map = original_uv.name
        def set_uvs(t):
            for node in t.nodes:
                if node.type == 'TEX_IMAGE' and not node.inputs['Vector'].is_linked:
                    u = t.nodes.new('ShaderNodeUVMap'); u.uv_map = original_uv.name
                    t.links.new(u.outputs['UV'], node.inputs['Vector'])
                elif node.type == 'GROUP':
                    node.node_tree = node.node_tree.copy()
                    set_uvs(node.node_tree)
        set_uvs(tree)
        output = next(n for n in tree.nodes if n.type=='OUTPUT_MATERIAL' and n.is_active_output)
        emission = tree.nodes.new('ShaderNodeEmission')
        tree.links.new(emission.outputs[0], output.inputs['Surface'])
        for link in list(output.inputs['Displacement'].links):
            tree.links.remove(link)
        target = tree.nodes.new('ShaderNodeTexImage')
        tree.nodes.active = target
        safe = original.name.replace(' ', '_')
        def bake(socket, channel, data=False):
            image = bpy.data.images.new(safe+'_'+channel, width=args.size, height=args.size, alpha=True, float_buffer=False)
            image.colorspace_settings.name = 'Non-Color' if data else 'sRGB'
            target.image = image
            tree.links.new(socket, emission.inputs['Color'])
            bpy.ops.object.bake(type='EMIT', uv_layer=bake_uv.name)
            pixels = np.empty(args.size*args.size*4, dtype=np.float32)
            image.pixels.foreach_get(pixels)
            return image, pixels.reshape((-1,4))
        base, base_pixels = bake(color,'BaseColor')
        if alpha is not None:
            alpha_image, alpha_pixels = bake(alpha,'Alpha',True)
            base_pixels[:,3] = alpha_pixels[:,0]
            bpy.data.images.remove(alpha_image)
        base.pixels.foreach_set(base_pixels.ravel())
        def save(image, suffix):
            path = destination / (safe+'_'+suffix+'.png')
            image.filepath_raw = str(path); image.file_format='PNG'
            image.save()
            return path.relative_to(args.fbx.resolve().parent).as_posix()
        entry = {'name': original.name, 'baseColor':[1,1,1,1], 'baseColorTexture':save(base,'BaseColor'),
                 'metallic':float(metallic), 'roughness':1.0, 'doubleSided':bool(hair_name(original.name)),
                 'alphaMode':'Mask' if hair_name(original.name) or original.name.startswith('M_Roku_Access') else 'Opaque',
                 'normalTexture':'', 'metallicRoughnessTexture':'', 'occlusionTexture':'', 'emissiveTexture':''}
        if rough is not None:
            image,pixels = bake(rough,'MetallicRoughness',True)
            pixels[:,1] = pixels[:,0]; pixels[:,0] = 1; pixels[:,2] = metallic; pixels[:,3] = 1
            image.pixels.foreach_set(pixels.ravel())
            entry['metallicRoughnessTexture'] = save(image,'MetallicRoughness')
            entry['metallic'] = 1.0
            bpy.data.images.remove(image)
        else:
            entry['roughness'] = .66793895
        if normal is not None:
            image,_ = bake(normal,'Normal',True)
            entry['normalTexture'] = save(image,'Normal')
            bpy.data.images.remove(image)
        if original.name == 'eyemat':
            shader = tree.nodes['Principled BSDF']
            glow = tree.nodes.new('ShaderNodeVectorMath'); glow.operation='SCALE'
            tree.links.new(source_socket(tree,shader.inputs['Emission Color']),glow.inputs[0])
            tree.links.new(source_socket(tree,shader.inputs['Emission Strength']),glow.inputs['Scale'])
            image,_ = bake(glow.outputs[0],'Emissive')
            entry['emissiveTexture']=save(image,'Emissive')
            entry['emissiveColor']=[1,1,1]
            bpy.data.images.remove(image)
        # The engine approximates refractive tear shells with a faint blend.
        if original.name.startswith('Tear'):
            entry['alphaMode']='Blend'; entry['baseColor'][3]=.035
        entries.append(entry)
        print('RECOVERED',original.name,'tile',tile,flush=True)
        bpy.data.images.remove(base)
        bpy.data.materials.remove(material)
    sidecar = Path(str(args.fbx.resolve())+'.materials.json')
    if args.material and sidecar.exists():
        prior=json.loads(sidecar.read_text())['materials']
        replaced={m['name'] for m in entries}
        entries=[m for m in prior if m['name'] not in replaced]+entries
    sidecar.write_text(json.dumps({'version':1,'materials':entries},indent=2)+'\n')
    print('WROTE',sidecar,flush=True)


def hair_name(name):
    return name.startswith('HAIRMAT') or name == 'Material.003'


if __name__ == '__main__':
    main()
