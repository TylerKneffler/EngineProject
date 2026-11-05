"""Import and compose the Oak Meadow scene from the supplied glTF packs.
Requires numpy/scipy for whole-blade grass thinning. Originals are never changed.
Run from the repository root with a built Debug Editor.exe.
"""
from pathlib import Path
import copy, json, os, re, subprocess, sys, xml.etree.ElementTree as ET
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
ROOT=Path(__file__).resolve().parents[2]
ASSETS=ROOT/'Engine/Core/Assets'
STAGE=ROOT/'.local/woodland-import'
OUTPUT=ASSETS/'Models/Woodland'
NS='urn:engine-components'
ET.register_namespace('components',NS)

def read_pack(name):
    folder=ASSETS/'Other'/name
    data=json.loads((folder/'scene.gltf').read_text())
    buffers=[(folder/b['uri']).read_bytes() for b in data['buffers']]
    def accessor(index):
        a=data['accessors'][index];v=data['bufferViews'][a['bufferView']]
        types={5126:'<f4',5125:'<u4',5123:'<u2',5121:'u1',5122:'<i2'}
        width={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
        dtype=np.dtype(types[a['componentType']]);offset=v.get('byteOffset',0)+a.get('byteOffset',0)
        result=np.ndarray((a['count'],width),dtype=dtype,buffer=buffers[v['buffer']],offset=offset,
            strides=(v.get('byteStride',width*dtype.itemsize),dtype.itemsize)).copy()
        return result[:,0] if width==1 else result
    def mesh(index,scale=1.,base=0.,center=(0,0)):
        result=[]
        for p in data['meshes'][index]['primitives']:
            attrs={k:accessor(v).astype(np.float32) for k,v in p['attributes'].items()
                if k in ['POSITION','NORMAL','TANGENT','TEXCOORD_0','COLOR_0']}
            for name in ['POSITION','NORMAL','TANGENT']:
                if name not in attrs:continue
                old=attrs[name].copy();attrs[name][:,:3]=old[:,[0,2,1]]*np.array([1,1,-1])
                if name=='POSITION':attrs[name][:,:3]=(attrs[name][:,:3]-[center[0],base,center[1]])*scale
            result.append((attrs,accessor(p['indices']).astype(np.uint32).reshape(-1,3),p.get('material',0)))
        return result
    return folder,data,mesh

def stage_pack(name,folder,source,models):
    destination=STAGE/name;destination.mkdir(parents=True,exist_ok=True)
    gltf={'asset':{'version':'2.0','generator':'Engine Oak Meadow asset preparation','extras':source['asset'].get('extras',{})},
        'scene':0,'scenes':[{'nodes':[]}],'nodes':[],'meshes':[],'accessors':[],'bufferViews':[],
        'buffers':[],'materials':[],'textures':[],'images':[],'samplers':source.get('samplers',[])}
    binary=bytearray();material_map={};texture_map={};image_map={}
    def texture(old):
        if old in texture_map:return texture_map[old]
        value=copy.deepcopy(source['textures'][old]);oldimage=value['source']
        if oldimage not in image_map:
            image=copy.deepcopy(source['images'][oldimage]);image['mimeType']='image/png';image['uri']=os.path.relpath(folder/image['uri'],destination).replace('\\','/')
            image_map[oldimage]=len(gltf['images']);gltf['images'].append(image)
        value['source']=image_map[oldimage];texture_map[old]=len(gltf['textures']);gltf['textures'].append(value)
        return texture_map[old]
    def material(old):
        if old in material_map:return material_map[old]
        value=copy.deepcopy(source['materials'][old])
        def walk(obj):
            if not isinstance(obj,dict):return
            for key,child in obj.items():
                if key.endswith('Texture') and isinstance(child,dict) and 'index' in child:child['index']=texture(child['index'])
                else:walk(child)
        walk(value)
        if value.get('alphaMode')=='BLEND':value['alphaMode']='MASK';value['alphaCutoff']=.4;value['doubleSided']=True
        material_map[old]=len(gltf['materials']);gltf['materials'].append(value);return material_map[old]
    def append(array,position=False):
        array=np.ascontiguousarray(array);binary.extend(b'\0'*((-len(binary))%4));offset=len(binary);binary.extend(array.tobytes())
        view=len(gltf['bufferViews']);gltf['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':array.nbytes})
        width=1 if array.ndim==1 else array.shape[1]
        a={'bufferView':view,'componentType':5125 if array.dtype==np.uint32 else 5126,'count':len(array),
           'type':{1:'SCALAR',2:'VEC2',3:'VEC3',4:'VEC4'}[width]}
        if position:a.update(min=array.min(0).tolist(),max=array.max(0).tolist())
        index=len(gltf['accessors']);gltf['accessors'].append(a);return index
    for model,primitives in models:
        out=[]
        for attrs,indices,mat in primitives:
            used,inverse=np.unique(indices,return_inverse=True)
            out.append({'attributes':{k:append(v[used],k=='POSITION') for k,v in attrs.items()},
                'indices':append(inverse.astype(np.uint32).reshape(-1)),'material':material(mat)})
        gltf['meshes'].append({'name':model,'primitives':out})
        gltf['scenes'][0]['nodes'].append(len(gltf['nodes']));gltf['nodes'].append({'name':model,'mesh':len(gltf['meshes'])-1})
    gltf['buffers']=[{'uri':name+'.bin','byteLength':len(binary)}]
    (destination/(name+'.bin')).write_bytes(binary)
    path=destination/(name+'.gltf');path.write_text(json.dumps(gltf,separators=(',',':')))
    return path

def prepare():
    manifest={}
    folder,data,mesh=read_pack('oak_trees_pack_17var_lods_seasons_gameready');models=[]
    names={}
    for i,m in enumerate(data['meshes']):
        match=re.match(r'^(.+)_LOD([012])_(Bark_Mat|Cluster_Mat)_0$',m['name'])
        if not match:continue
        species,lod,part=match.groups();key=re.sub('_+','_',species).strip('_')+'_LOD'+lod
        names.setdefault(key,[]).append(i)
    for name,parts in names.items():
        base=min(data['accessors'][p['attributes']['POSITION']]['min'][2] for i in parts for p in data['meshes'][i]['primitives'])
        models.append((name,[prim for i in parts for prim in mesh(i,base=base)]))
    manifest['Oaks']={'source':str(stage_pack('Oaks',folder,data,models)),'models':[n for n,_ in models]}
    folder,data,mesh=read_pack('real_bush');models=[]
    for i,m in enumerate(data['meshes']):
        name=m['name'].split('_phong')[0];base=min(data['accessors'][p['attributes']['POSITION']]['min'][2] for p in m['primitives'])
        models.append((name,mesh(i,scale=.02,base=base)))
    manifest['Bushes']={'source':str(stage_pack('Bushes',folder,data,models)),'models':[n for n,_ in models]}
    folder,data,mesh=read_pack('grass_patches_-_circle');pieces=[p for i in range(len(data['meshes'])) for p in mesh(i)]
    attrs={k:np.concatenate([p[0][k] for p in pieces]) for k in pieces[0][0]};offset=0;triangles=[]
    for a,indices,mat in pieces:triangles.append(indices+offset);offset+=len(a['POSITION'])
    triangles=np.concatenate(triangles);edges=np.concatenate([triangles[:,[0,1]],triangles[:,[1,2]],triangles[:,[2,0]]])
    count,labels=connected_components(coo_matrix((np.ones(len(edges)),(edges[:,0],edges[:,1])),shape=(offset,offset)),directed=False)

    # Retopologize each existing connected leaf into a six-triangle ribbon.
    # Positions/UVs come from the supplied blade, preserving its curvature and atlas.
    ribbons={k:[] for k in attrs};ribbon_labels=[];ribbon_triangles=[];vertex_count=0
    order=np.argsort(labels);starts=np.searchsorted(labels[order],np.arange(count+1))
    for leaf in range(count):
        ids=order[starts[leaf]:starts[leaf+1]]
        if len(ids)<3:continue
        points=attrs['POSITION'][ids];center=points.mean(0)
        _,_,axes=np.linalg.svd(points-center,full_matrices=False)
        length_axis=axes[0];width_axis=axes[1]
        if length_axis[1]<0:length_axis=-length_axis
        along=(points-center)@length_axis;across=(points-center)@width_axis
        span=max(float(np.ptp(along)),.00001);chosen=[]
        for target in np.linspace(float(along.min()),float(along.max()),4):
            distance=np.abs(along-target);local=np.where(distance<=span*.18)[0]
            if len(local)<2:local=np.argsort(distance)[:min(4,len(ids))]
            chosen.extend([ids[local[np.argmin(across[local])]],ids[local[np.argmax(across[local])]]])
        for key,values in attrs.items():ribbons[key].append(values[chosen])
        ribbon_labels.extend([leaf]*8)
        for step in range(3):
            a=vertex_count+step*2;ribbon_triangles.extend([[a,a+2,a+1],[a+1,a+2,a+3]])
        vertex_count+=8
    attrs={k:np.concatenate(v) for k,v in ribbons.items()};labels=np.asarray(ribbon_labels)
    triangles=np.asarray(ribbon_triangles,dtype=np.uint32)
    roots=np.full(count,np.inf);np.minimum.at(roots,labels,attrs['POSITION'][:,1])
    tri_labels=labels[triangles[:,0]];rng=np.random.default_rng(18731);models=[]
    # Whole connected blades are retained: never remove arbitrary triangles/cards.
    for variant in range(4):
        order=rng.permutation(count)
        for lod,keep in enumerate([512,192,64]):
            selected=triangles[np.isin(tri_labels,order[:keep])]
            transformed={k:v.copy() for k,v in attrs.items()}
            normals=transformed['NORMAL'];normals[:,1]=np.abs(normals[:,1])*.25+.75;normals/=np.linalg.norm(normals,axis=1,keepdims=True)
            transformed['POSITION']=(transformed['POSITION']-np.array([-.07,-.36,-4.15],np.float32))*.45
            transformed['POSITION'][:,1]=(attrs['POSITION'][:,1]-roots[labels])*.45*2.4
            # Each connected blade is grounded independently before scaling its height.
            models.append((f'MeadowPatch_{variant+1:02}_LOD{lod}',[(transformed,selected,0)]))
    manifest['Grass']={'source':str(stage_pack('Grass',folder,data,models)),'models':[n for n,_ in models]}
    STAGE.mkdir(parents=True,exist_ok=True);(STAGE/'manifest.json').write_text(json.dumps(manifest,indent=2))
    return manifest

def import_packs(manifest):
    editor=ROOT/'build/Debug/Debug/Editor.exe'
    for name,info in manifest.items():
        expected=OUTPUT/name/(name+'.prefab')
        if expected.exists():print('Using imported',expected,flush=True);continue
        args=[str(editor),'--import-gltf',os.path.relpath(info['source'],ROOT),os.path.relpath(OUTPUT,ROOT)]
        result=subprocess.run(args,cwd=ROOT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if result.returncode:raise RuntimeError('Native import failed: '+name)
        print('Imported',name,flush=True)


def component(node,name):
    return node.find('components').find('{'+NS+'}'+name)

def vector(parent,name,values):
    old=parent.find(name)
    if old is not None:parent.remove(old)
    node=ET.SubElement(parent,name)
    for value in values:ET.SubElement(node,'item').text=f'{value:.7g}'
    return node

def transform(node,position=(0,0,0),rotation=(0,0,0),scale=(1,1,1)):
    old=node.find('transform')
    if old is not None:node.remove(old)
    t=ET.SubElement(node,'transform')
    for name,value in [('position',position),('rotation',rotation),('scale',scale)]:vector(t,'Transform.'+name,value)

def write_xml(path,root):
    path.parent.mkdir(parents=True,exist_ok=True);ET.indent(root,space='  ')
    ET.ElementTree(root).write(path,encoding='utf-8',xml_declaration=True)

def portable(value):return value.replace('Engine/Core/Assets/','Assets/')

def make_prefabs():
    library={}
    for pack in ['Oaks','Bushes','Grass']:
        folder=OUTPUT/pack
        # The native importer owns texture/mesh conversion. Materials here add
        # cutout rendering and wind-friendly foliage lighting for this environment.
        for path in (folder/'Materials').glob('*.material'):
            d=json.loads(path.read_text())
            for key,value in list(d.items()):
                if isinstance(value,str):d[key]=portable(value)
            leaf=d.get('alphaMode')=='Mask' or pack in ['Bushes','Grass']
            d.update(useVertexColors=False,ambientColor=[.025,.03,.02],metallic=0,roughness=.86)
            if pack=='Oaks' and not leaf:d['doubleSided']=False
            if leaf:d.update(alphaMode='Mask',alphaCutoff=.32,doubleSided=True,castsShadows=True,ambientColor=[.20,.23,.13],normalScale=.55,reflectionStrength=.18)
            if pack in ['Grass','Bushes']:d['shaders']=[{'path':'Assets/Shaders/Foliage.hlsl','enabled':True,'blendMode':'Auto'}]
            path.write_text(json.dumps(d,indent=2)+'\n')
        root=ET.parse(folder/(pack+'.prefab')).getroot().find('object')
        for source in root.find('children'):
            obj=copy.deepcopy(source);obj.tag='object';model=obj.get('name')
            for node in obj.iter():
                for key,value in list(node.attrib.items()):node.set(key,portable(value))
                if node.text and 'Engine/Core/Assets/' in node.text:node.text=portable(node.text)
                if node.tag=='{'+NS+'}Mesh':node.set('Mesh.foliageWindStrength',str(.018 if pack=='Oaks' else .16 if pack=='Bushes' else .48))
                if node.tag=='{'+NS+'}Material':
                    asset=node.get('Material.materialAsset')
                    node.clear();node.set('Material.materialAsset',asset.replace('Assets/','Engine/Core/Assets/',1))
            transform(obj)
            # Native object names and separate bark/leaf slots stay editable.
            prefab=ET.Element('Prefab',{'version':'1','type':'prefab'});prefab.append(obj)
            path=ASSETS/'Prefabs/Woodland'/pack/(model+'.prefab');write_xml(path,prefab)
            library[model]=path
    return library

def add_grass_layer(terrain, template, grove_half_extent=48.0):
    """Transfer the flat scene's authored grass pattern onto this terrain."""
    import math
    source=next(n for n in template.iter('item') if n.find('components/{'+NS+'}GrassField') is not None)
    grass=copy.deepcopy(source);grass.set('name','Oak meadow grass')
    children=grass.find('children')
    if children is not None:children.clear()
    transform(grass)
    field=component(grass,'GrassField')
    field.set('GrassField.viewerObjectName','Meadow Explorer')
    field.set('GrassField.useEditorCamera','true')
    field.set('GrassField.allowProceduralGeneration','false')
    field.set('GrassField.retainDormantTiles','false')
    layer=component(grass,'TerrainObjectLayer')
    layer.attrib.pop('TerrainObjectLayer.bakedInstances',None)  # Flat transforms are invalid here.
    layer.set('TerrainObjectLayer.layerName','Meadow grass')
    layer.set('TerrainObjectLayer.viewDistance','36')
    placements=layer.find('TerrainObjectLayer.placements')
    originals=list(placements);placements.clear()
    def numbers(node):
        return [float(n.text) for n in node] if len(node) else [float(n) for n in node.text.split()]
    t=component(terrain,'Terrain')
    cells_x=int(t.get('Terrain.cellsX'));cells_z=int(t.get('Terrain.cellsY'))
    width=float(t.get('Terrain.cellWidth'));depth=float(t.get('Terrain.cellDepth'))
    heights=[float(n.text) for n in t.find('Terrain.heights')]
    weights=[float(n.text) for n in t.find('Terrain.weights')]
    layer_count=len(t.find('Terrain.layers'))
    origin=numbers(terrain.find('transform/Transform.position'))
    def sample(x,z):
        gx=x/width;gz=z/depth;col=min(int(gx),cells_x-1);row=min(int(gz),cells_z-1)
        u=gx-col;v=gz-row;a=row*(cells_x+1)+col;b=a+cells_x+1
        # Match Terrain's a,b,a+1 / a+1,b,b+1 triangle diagonal exactly.
        ids,blend=([a,a+1,b],[1-u-v,u,v]) if u+v<=1 else ([a+1,b,b+1],[1-v,1-u,u+v-1])
        y=sum(heights[i]*w for i,w in zip(ids,blend))
        dirt=sum(weights[i*layer_count+1]*w for i,w in zip(ids,blend))
        dx=((heights[a+1]-heights[a]) if u+v<=1 else (heights[b+1]-heights[b]))/width
        dz=((heights[b]-heights[a]) if u+v<=1 else (heights[b+1]-heights[a+1]))/depth
        length=math.sqrt(dx*dx+1+dz*dz)
        return y,[-dx/length,1/length,-dz/length],dirt
    # The authored grove covers 96 m; the remaining ground supplies the horizon.
    first=int(math.floor(-grove_half_extent/16));last=int(math.ceil(grove_half_extent/16))
    for tile_z in range(first,last):
        for tile_x in range(first,last):
            tile=(tile_z-first)*(last-first)+(tile_x-first)
            for index,original in enumerate(originals):
                position=numbers(original.find('position'))
                x=position[0]+tile_x*16;z=position[2]+tile_z*16
                if abs(x)>grove_half_extent or abs(z)>grove_half_extent:continue
                local_x=x-origin[0];local_z=z-origin[2]
                y,normal,dirt=sample(local_x,local_z)
                if dirt>=.35:continue
                placement=ET.SubElement(placements,'item',dict(original.attrib))
                placement.set('id',str(tile*len(originals)+index+1))
                for name,values in [('position',[local_x,y,local_z]),('normal',normal)]:
                    node=ET.SubElement(placement,name,{'_arrayEncoding':'spaceSeparatedNumbers'})
                    node.text=' '.join(f'{value:.7g}' for value in values)
    terrain_children=terrain.find('children')
    if terrain_children is None:terrain_children=ET.SubElement(terrain,'children')
    for child in list(terrain_children):
        if child.get('name')=='Oak meadow grass' or child.get('prefab')=='Assets/Prefabs/Woodland/OakMeadowGrass.prefab':terrain_children.remove(child)
    prefab=ET.Element('Prefab',{'version':'1','type':'prefab'})
    grass.tag='object';prefab.append(grass)
    write_xml(ASSETS/'Prefabs/Woodland/OakMeadowGrass.prefab',prefab)
    instance=ET.SubElement(terrain_children,'item',{'prefab':'Assets/Prefabs/Woodland/OakMeadowGrass.prefab','enabled':'true'})
    transform(instance)
    return len(placements)


def populate_forest(scene, library, count=2048):
    """Replace only oak placements; preserve authored terrain, grass and settings."""
    import math, random
    rng=random.Random(18731)
    objects=scene.find('objects')
    for node in list(objects):
        if node.get('name','').startswith(('Oak groves','Oak forest')):
            objects.remove(node)
    terrain=next(n for n in objects if component(n,'Terrain') is not None)
    children=terrain.find('children')
    if children is None:children=ET.SubElement(terrain,'children')
    for node in list(children):
        if node.get('name')=='Oak forest':children.remove(node)
    group=ET.SubElement(children,'item',{'name':'Oak forest','enabled':'true'})
    transform(group);ET.SubElement(group,'children');cs=ET.SubElement(group,'components')
    layer=ET.SubElement(cs,'{'+NS+'}TerrainObjectLayer',{
        'TerrainObjectLayer.layerName':'Oak forest','TerrainObjectLayer.viewDistance':'220',
        'TerrainObjectLayer.lod1Distance':'28','TerrainObjectLayer.lod2Distance':'90',
        'TerrainObjectLayer.shadowDistance':'90'})
    trees=ET.SubElement(layer,'TerrainObjectLayer.placements')
    surface=component(terrain,'Terrain')
    cells_x=int(surface.get('Terrain.cellsX'));cells_z=int(surface.get('Terrain.cellsY'))
    step_x=float(surface.get('Terrain.cellWidth'));step_z=float(surface.get('Terrain.cellDepth'))
    origin=[float(n.text) for n in terrain.find('transform/Transform.position')]
    heights=[float(n.text) for n in surface.find('Terrain.heights')]
    def ground(x,z):
        gx=(x-origin[0])/step_x;gz=(z-origin[2])/step_z
        col=min(cells_x-1,max(0,int(math.floor(gx))))
        row=min(cells_z-1,max(0,int(math.floor(gz))))
        u=gx-col;v=gz-row;i=row*(cells_x+1)+col
        a,b,c,d=heights[i],heights[i+1],heights[i+cells_x+1],heights[i+cells_x+2]
        return origin[1]+((1-u-v)*a+u*b+v*c if u+v<=1 else
            (1-v)*b+(1-u)*c+(u+v-1)*d)-.025
    path_x=lambda z:2.4*math.sin(z*.095)+1.3*math.sin(z*.19)
    species=sorted({re.sub(r'_LOD[012]$','',n) for n in library if '_oak_tree_' in n})
    if len(species)!=17:raise RuntimeError('Expected all seventeen native oak varieties')
    lods=ET.SubElement(layer,'TerrainObjectLayer.objectLods')
    for name in species:
        paths=[portable(str(library[name+f'_LOD{lod}'].relative_to(ROOT)).replace('\\','/')) for lod in range(3)]
        impostor=ASSETS/'Prefabs/Woodland/Oaks'/(name+'_Impostor.prefab')
        if impostor.exists(): paths[2]='Assets/'+impostor.relative_to(ASSETS).as_posix()
        entry=ET.SubElement(lods,'item',{'asset':paths[0]})
        meshes=ET.SubElement(entry,'meshes')
        for path in paths:ET.SubElement(meshes,'item').text=path
        distances=ET.SubElement(entry,'distances')
        for distance in [28,90]:ET.SubElement(distances,'item').text=str(distance)
    centers=[(rng.uniform(-156,156),rng.uniform(-156,156)) for _ in range(96)]
    positions=[];buckets={};pairs=0
    def accept(x,z,spacing):
        if len(positions)>=count or max(abs(x),abs(z))>164 or abs(x-path_x(z))<3.4:return False
        key=(math.floor(x/4.2),math.floor(z/4.2))
        for dz in range(-1,2):
            for dx in range(-1,2):
                for a,b in buckets.get((key[0]+dx,key[1]+dz),[]):
                    if (x-a)**2+(z-b)**2<spacing**2:return False
        positions.append((x,z));buckets.setdefault(key,[]).append((x,z));return True
    for attempt in range(count*200):
        if len(positions)>=count:break
        if rng.random()<.75:
            cx,cz=rng.choice(centers);x,z=rng.gauss(cx,13),rng.gauss(cz,13)
        else:x,z=rng.uniform(-164,164),rng.uniform(-164,164)
        if not accept(x,z,4.2):continue
        if rng.random()<.38:
            angle=rng.uniform(-math.pi,math.pi);distance=rng.uniform(1.9,2.8)
            pairs+=accept(x+math.cos(angle)*distance,z+math.sin(angle)*distance,1.85)
    if len(positions)!=count:raise RuntimeError(f'Could only place {len(positions)} of {count} oaks')
    for i,(x,z) in enumerate(positions):
        name=species[i%len(species)]+'_LOD0'
        scale=rng.uniform(.70,1.08)
        yaw=rng.uniform(-math.pi,math.pi)
        obj=ET.SubElement(trees,'item',{'asset':portable(str(library[name].relative_to(ROOT)).replace('\\','/')),
            'id':str(i+1),'yawDegrees':f'{math.degrees(yaw):.7g}','scale':f'{scale:.7g}'})
        for key,values in [('position',[x-origin[0],ground(x,z)-origin[1],z-origin[2]]),('normal',[0,1,0])]:
            ET.SubElement(obj,key,{'_arrayEncoding':'spaceSeparatedNumbers'}).text=' '.join(f'{value:.7g}' for value in values)
    print(f'Forest: {count} oaks, {pairs} close companions, 17 runtime LOD varieties, 328 x 328 m coverage',flush=True)
    return count


def compose(library):
    import math
    template=ET.parse(ASSETS/'Scenes/terrain/grass_flat_test.scene').getroot()
    lookup={n.get('name'):n for n in template.iter('item') if n.get('name')}
    scene=ET.Element('Scene',{'version':'1'});settings=copy.deepcopy(template.find('settings'))
    settings.set('skyboxTexture','Assets/Textures/Skyboxes/editor-default-sky.png')
    for key,value in [('textureLodEnabled','true'),('textureLodNearDistance','12'),('textureLodFarDistance','48'),('textureLodMipBias','4'),('textureLodSimplifyShading','true')]:settings.set(key,value)
    settings.set('hdriLightingEnabled','true');settings.set('hdriIntensity','.55');settings.set('outputExposure','-.2')
    vector(settings,'ambientColor',[.10,.13,.15]);scene.append(settings);objects=ET.SubElement(scene,'objects')
    path_x=lambda z:2.4*math.sin(z*.095)+1.3*math.sin(z*.19)
    height=lambda x,z:.4*math.sin(x*.065)*math.cos(z*.08)+.23*math.sin(z*.11)+.08*math.sin((x+z)*.2)
    player=copy.deepcopy(lookup['Grass Test Capsule']);player.set('name','Meadow Explorer')
    transform(player,(path_x(-26),height(path_x(-26),-26)+1.08,-26))
    camera=component(player,'Camera');camera.set('Camera.fov','67');camera.set('Camera.far','220')
    vector(camera,'Camera.target',[path_x(5),1.35,5]);objects.append(player)
    sun=copy.deepcopy(lookup['Grass Sun']);sun.set('name','Late afternoon sun')
    transform(sun,(0,20,-10),(.70,2.40,0));light=component(sun,'Light');light.set('Light.intensity','3.2')
    light.set('Light.castsShadows','false')
    light.set('Light.shadowResolutionScale','.0625');light.set('Light.shadowFilterScale','.05')
    vector(light,'Light.color',[1,.91,.77]);objects.append(sun)
    materials=ASSETS/'Materials/Woodland';materials.mkdir(parents=True,exist_ok=True)
    for name,texture,color in [('Meadow','meadow_grass.png',[.48,.53,.36]),('Path','warm_dirt.png',[.83,.75,.62]),('WoodlandFloor','shaded_moss.png',[.48,.55,.40])]:
        (materials/(name+'.material')).write_text(json.dumps({'version':1,'baseColor':color,'metallic':0,'roughness':1,
            'useVertexColors':True,'baseColorTexture':'Assets/Textures/Terrain/'+texture,'castsShadows':False},indent=2)+'\n')
    terrain=copy.deepcopy(lookup['Flat Grass Terrain']);terrain.set('name','Oak meadow ground')
    # One continuous surface covers both the grove and the distant horizon.
    terrain_half_extent=180.0
    terrain_cells=60
    terrain.find('children').clear();transform(terrain,(-terrain_half_extent,0,-terrain_half_extent))
    terrain_cell_size=(terrain_half_extent*2.0)/terrain_cells
    t=component(terrain,'Terrain');t.clear();t.attrib.update({
        'Terrain.cellsX':str(terrain_cells),'Terrain.cellsY':str(terrain_cells),
        'Terrain.cellWidth':f'{terrain_cell_size:g}','Terrain.cellDepth':f'{terrain_cell_size:g}','Terrain.textureTiling':'180'})
    heights=ET.SubElement(t,'Terrain.heights');weights=ET.SubElement(t,'Terrain.weights');layers=ET.SubElement(t,'Terrain.layers')
    for name,col in [('Meadow',[.37,.47,.22]),('Path',[.40,.32,.20]),('WoodlandFloor',[.22,.30,.16])]:
        l=ET.SubElement(layers,'item',{'material':f'Assets/Materials/Woodland/{name}.material','minHeight':'-100','maxHeight':'100'})
        vector(l,'color',col)
    for row in range(terrain_cells+1):
        z=-terrain_half_extent+row*terrain_cell_size
        for column in range(terrain_cells+1):
            x=-terrain_half_extent+column*terrain_cell_size;ET.SubElement(heights,'item').text=f'{height(x,z):.5f}'
            distance=abs(x-path_x(z));dirt=max(0,min(1,(2.15-distance)/1.1))
            moss=max(0,min(.75,(abs(x)-16)/25))*(1-dirt)
            for w in [1-dirt-moss,dirt,moss]:ET.SubElement(weights,'item').text=f'{w:.5f}'
    collider=component(terrain,'MeshObjectCollider');collider.set('MeshObjectCollider.collisionEnabled','true')
    grass_count=add_grass_layer(terrain,template)
    objects.append(terrain)
    tree_count=populate_forest(scene,library)
    path=ASSETS/'Scenes/terrain/oak_meadow.scene';write_xml(path,scene)
    print(f'Created {path}: {tree_count} oaks, {grass_count} grounded grass patches',flush=True)
    credits=[]
    for pack in (ASSETS/'Other').glob('*/scene.gltf'):
        info=json.loads(pack.read_text())['asset']['extras'];credits.append(f"- {info['title']} by {info['author']}\n  Source: {info['source']}\n  License: {info['license']}")
    (OUTPUT/'ATTRIBUTION.md').write_text('# Oak Meadow source assets\n\n'+'\n\n'.join(credits)+'\n\nModified for this scene: Y-up metre-scale geometry, root pivots, reusable native prefabs, alpha-tested materials, grounded whole-blade ribbon retopology and density LODs. Original packs remain in Assets/Other.\n')
    (OUTPUT/'README.md').write_text('# Oak Meadow\n\nOpen `Assets/Scenes/terrain/oak_meadow.scene` and press Play. WASD moves, mouse looks, Shift sprints.\n\nThe forest stress scene uses 2,048 oaks across a 328 x 328 m area, a painted three-layer ground, a winding footpath and seventeen oak varieties. Irregular canopy clusters include 569 close companions at roughly 2-3 m spacing; the narrow footpath remains clear. Trees select shared resident geometry as the camera moves: LOD0 within 28 m, LOD1 to 90 m, and an authored 28-triangle impostor beyond. Complementary spatial dithering blends each boundary over a one-metre interval. Placements reference the base variety, rather than a starting-camera LOD. Trees are compact shared-prefab placements in the terrain child `Oak forest` (`TerrainObjectLayer`), referencing editable assets under `Assets/Prefabs/Woodland/Oaks`. Prefab parts share source geometry and materials instead of expanding a full object hierarchy for every tree. Oak prefabs currently contain no collider or rigid body. Add a primitive collider to the LOD0/base prefab to apply it to every placement of that oak variety, including its configured visual LOD and impostor variants; saved prefab edits are polled once per second. The terrain Objects brush stores prefab references in these layers.\n\nOne continuous 60x60 terrain grid covers the grove and horizon without overlapping surfaces. Grass from `grass_flat_test.scene` is authored across the 96 m grove and streamed to a 36 m view distance using its existing LODs and wind settings. Grass roots and normals follow the actual terrain triangles, leaving the painted footpath clear. Grass placement data lives in `Assets/Prefabs/Woodland/OakMeadowGrass.prefab`, attached directly beneath the terrain. There is no shrub-prefab layer in this scene.\n\n`Mesh.foliageWindStrength` enables the shared root-pinned wind field for authored meshes. The oak meadow scene intentionally keeps tree trunks and canopies at zero wind so they remain static and do not trigger shadow-map churn from shader-driven vertex motion. The same vertex path drives color and shadows, with bounds padding for wind. Bark and canopies keep source PBR shading. The scene enables generic distance texture LOD: full quality within 12 m, smoothly reaching +4 mip bias at 48 m, with normal/material detail and reflections fading over that interval. Adjust Texture LOD in Scene Properties or enable a material override. Bark is one-sided; leaf cards remain two-sided to preserve canopy coverage. Terrain renders only its upward-facing surface.\n\nRebuild only tree placements with `python Scripts/Assets/build_oak_meadow.py --forest-only`, preserving the current terrain, grass and scene settings. Regenerate all assets with `python Scripts/Assets/build_oak_meadow.py --prepare-only`, then `python Scripts/Assets/build_oak_meadow.py` (numpy/scipy and a built Debug Editor required). Existing imported packs are reused. Imports use the native glTF importer; generated intermediates live under `.local/woodland-import`. See ATTRIBUTION.md for CC-BY-4.0 credits.\n')

if __name__=='__main__':
    os.chdir(ROOT)
    if '--forest-only' in sys.argv:
        library={p.stem:p for p in (ASSETS/'Prefabs/Woodland/Oaks').glob('*.prefab')}
        path=ASSETS/'Scenes/terrain/oak_meadow.scene'
        scene=ET.parse(path).getroot();populate_forest(scene,library);write_xml(path,scene)
    elif '--prepare-only' in sys.argv:prepare()
    else:
        manifest=json.loads((STAGE/'manifest.json').read_text()) if (STAGE/'manifest.json').exists() else prepare()
        import_packs(manifest)
        compose(make_prefabs())
