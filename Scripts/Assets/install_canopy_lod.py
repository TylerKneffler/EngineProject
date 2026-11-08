"""Install a fully validated canopy study without changing authored distances."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from audit_oak_lods import ROOT, ASSETS, resolve

ET.register_namespace('components','urn:engine-components')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate',type=Path)
    args=parser.parse_args()
    candidate=args.candidate.resolve()
    report=json.loads((candidate/'report.json').read_text())
    evidence=json.loads((candidate/'validation/timing-provenance.json').read_text())
    if not all(evidence.get(k) for k in ('complete','inputs_unchanged','candidates_unchanged','runtime_unchanged')):
        raise ValueError('Complete matched study provenance required')
    timings=json.loads((candidate/'validation/timing-summary.json').read_text())
    if len(timings)!=12 or {r['backend'] for r in timings}!={'dx11','dx12','vulkan'}:
        raise ValueError('All three backends and all four timing views are required')
    if len(report['results'])!=4 or not report['coverage_constrained']:
        raise ValueError('All four coverage-constrained varieties required')
    for backend in ('dx11','dx12','vulkan'):
        text=(candidate/'renderer'/backend/'1'/(backend+'.log')).read_text()
        if 'Canopy candidate varieties replaced=4' not in text or 'Canopy experiment captured' not in text:
            raise ValueError('Missing complete backend quality comparison')
    scene=ASSETS/'Scenes/terrain/oak_meadow.scene'
    scene_bytes=scene.read_bytes()
    writes=[]; manifest=[]
    for row in report['results']:
        name=row['name']
        if row['max_missing_coverage_fraction']>.01: raise ValueError('Coverage gate failed')
        tree=ET.parse(candidate/name/(name+'_CanopyCandidate.prefab'))
        nodes=[n for n in tree.getroot().iter() if n.tag.endswith('}Mesh')]
        if len(nodes)!=2: raise ValueError('Both bark and leaves required')
        for part,node in zip(row['parts'],nodes):
            source=ROOT/part['source']
            if hashlib.sha256(source.read_bytes()).hexdigest()!=part['sha256']:
                raise ValueError('Original geometry changed since screening')
            mesh=resolve(node.get('Mesh.file'))
            raw=mesh.read_bytes()
            relative=str(mesh.relative_to(ROOT))
            if evidence['candidates'].get(relative)!=hashlib.sha256(raw).hexdigest():
                raise ValueError('Candidate geometry changed since validation')
            target=ASSETS/'Models/Woodland/Oaks/Meshes/Canopy'/mesh.name.replace('CanopyCandidate','Canopy')
            writes.append((target,raw))
            node.set('Mesh.file','Assets/'+target.relative_to(ASSETS).as_posix())
            manifest.append(dict(original=part['source'],original_sha256=part['sha256'],
                installed=target.relative_to(ROOT).as_posix(),sha256=hashlib.sha256(raw).hexdigest(),
                original_triangles=part['original_triangles'],triangles=part['candidate_triangles']))
        tree.getroot().find('object').set('name',name+'_Canopy')
        ET.indent(tree)
        path=ASSETS/'Prefabs/Woodland/Oaks'/(name+'_Canopy.prefab')
        writes.append((path,ET.tostring(tree.getroot(),encoding='utf-8',xml_declaration=True)))
        old=(name+'_LOD1.prefab').encode(); new=(name+'_Canopy.prefab').encode()
        if scene_bytes.count(old)==1: scene_bytes=scene_bytes.replace(old,new)
        elif scene_bytes.count(new)!=1: raise ValueError('Unexpected scene LOD chain')
    for path,data in writes:
        path.parent.mkdir(parents=True,exist_ok=True)
        if not path.exists() or path.read_bytes()!=data: path.write_bytes(data)
    if scene.read_bytes()!=scene_bytes: scene.write_bytes(scene_bytes)
    path=ASSETS/'Models/Woodland/Oaks/Meshes/Canopy/manifest.json'
    data=json.dumps(dict(generator='Scripts/Assets/build_canopy_lod.py',
        training_size=report['training_size'],retention=report['retention'],
        distance_boundaries=[28,90],parts=manifest),indent=2)+'\n'
    if not path.exists() or path.read_text()!=data: path.write_text(data)
    print('Installed four canopy prefabs and eight meshes; retained 28/90 m')


if __name__=='__main__': main()
