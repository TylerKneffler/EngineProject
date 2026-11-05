"""Import the pack's authored 28-triangle tree impostors; preserve atlas UVs."""
from pathlib import Path
import json, re, struct, xml.etree.ElementTree as ET
import numpy as np
from build_oak_meadow import read_pack, ASSETS, NS, write_xml

def main():
    folder,data,read_mesh=read_pack('oak_trees_pack_17var_lods_seasons_gameready')
    template=json.loads((ASSETS/'Models/Woodland/Oaks/Materials/Cluster_Mat 2.material').read_text())
    for index,mesh in enumerate(data['meshes']):
        if '_Billboard_' not in mesh.get('name',''): continue
        name=re.sub('_+','_',mesh['name'].split('_Billboard_')[0])
        # Match the root pivot used by this variety's existing near meshes.
        near=[m for m in data['meshes'] if re.sub('_+','_',m.get('name','')).startswith(name+'_LOD0_')]
        base=min(data['accessors'][p['attributes']['POSITION']]['min'][2] for m in near for p in m['primitives'])
        parts=read_mesh(index,base=base)
        assert len(parts)==1
        attrs,triangles,material_index=parts[0]
        ids=triangles.reshape(-1)
        vertices=np.zeros((len(ids),66),dtype='<f4')
        vertices[:,0:3]=attrs['POSITION'][ids];vertices[:,3:6]=attrs['NORMAL'][ids]
        vertices[:,6:8]=attrs['TEXCOORD_0'][ids]
        vertices[:,8:12]=attrs.get('TANGENT',np.tile([1,0,0,1],(len(attrs['POSITION']),1)))[ids]
        vertices[:,14:18]=1
        vertices[:,34:36]=vertices[:,6:8]
        assert len(ids)==84
        mesh_path=ASSETS/'Models/Woodland/Oaks/Meshes'/(name+'_Impostor.mesh')
        mesh_path.write_bytes(struct.pack('<4I',0x4853454d,7,len(ids),0)+vertices.tobytes())
        material=dict(template)
        source=data['materials'][material_index]
        tex=source['pbrMetallicRoughness']['baseColorTexture']['index']
        image=data['images'][data['textures'][tex]['source']]['uri']
        material['baseColorTexture']='Assets/Other/'+folder.name+'/'+image
        for key in ['normalTexture','metallicRoughnessTexture','occlusionTexture']: material[key]=''
        material['alphaCutoff']=.32;material['useVertexColors']=False
        material['specularModel']='BlinnPhong';material['reflectionStrength']=0
        mat_path=ASSETS/'Models/Woodland/Oaks/Materials'/(name+'_Impostor.material')
        mat_path.write_text(json.dumps(material,indent=2)+'\n')
        root=ET.Element('Prefab',{'version':'1','type':'prefab'})
        obj=ET.SubElement(root,'object',{'name':name+' impostor','enabled':'true'})
        components=ET.SubElement(obj,'components')
        ET.SubElement(components,'{'+NS+'}Mesh',{'Mesh.file':'Assets/'+str(mesh_path.relative_to(ASSETS)).replace('\\','/')})
        ET.SubElement(components,'{'+NS+'}Material',{'Material.materialAsset':str(mat_path.relative_to(ASSETS.parent.parent.parent)).replace('\\','/')})
        ET.SubElement(obj,'children')
        write_xml(ASSETS/'Prefabs/Woodland/Oaks'/(name+'_Impostor.prefab'),root)
        print(name, '28 triangles')
    scene=ASSETS/'Scenes/terrain/oak_meadow.scene'
    tree=ET.parse(scene)
    for layer in tree.iter('{'+NS+'}TerrainObjectLayer'):
        if layer.get('TerrainObjectLayer.layerName')!='Oak forest': continue
        for placement in layer.find('TerrainObjectLayer.placements'):
            placement.set('asset',re.sub(r'_LOD[012]\.prefab$', '_LOD0.prefab',placement.get('asset')))
        for chain in layer.find('TerrainObjectLayer.objectLods'):
            items=chain.find('meshes').findall('item')
            items[-1].text=items[0].text.replace('_LOD0.prefab','_Impostor.prefab')
    write_xml(scene,tree.getroot())

if __name__=='__main__': main()
