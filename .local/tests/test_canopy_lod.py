"""Check the alpha/occlusion measurements used to screen canopy candidates."""
from pathlib import Path
import sys
import unittest
import contextlib
import io
import xml.etree.ElementTree as ET

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'Scripts/Assets'))
from build_canopy_lod import render
from build_oak_meadow import ASSETS, NS, populate_forest


def card(z, color, alpha=255):
    vertices=np.zeros((4,66), dtype=np.float32)
    vertices[:,:3]=[[-1,-1,z],[1,-1,z],[1,1,z],[-1,1,z]]
    vertices[:,3:6]=[0,0,1]
    vertices[:,6:8]=[[0,0],[1,0],[1,1],[0,1]]
    return dict(triangles=vertices[[[0,1,2],[0,2,3]]], groups=np.array([0,0]),
                count=1, texture=np.full((4,4,4),[*color,alpha],dtype=np.uint8), cutoff=.32)


class CanopyRasterTests(unittest.TestCase):
    def test_regeneration_selects_approved_canopies_without_extra_varieties(self):
        path=ASSETS/'Scenes/terrain/oak_meadow.scene'
        before=path.read_bytes()
        scene=ET.fromstring(before)
        library={p.stem:p for p in (ASSETS/'Prefabs/Woodland/Oaks').glob('*.prefab')}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(populate_forest(scene,library),2048)
        forest=next(n for n in scene.iter('{'+NS+'}TerrainObjectLayer')
                    if n.get('TerrainObjectLayer.layerName')=='Oak forest')
        chains=list(forest.find('TerrainObjectLayer.objectLods'))
        self.assertEqual(len(chains),17)
        self.assertEqual(sum('_Canopy.prefab' in n.find('meshes')[1].text for n in chains),4)
        self.assertTrue(all([n.text for n in chain.find('distances')]==['28','90'] for chain in chains))
        self.assertEqual(len(forest.find('TerrainObjectLayer.placements')),2048)
        self.assertEqual(path.read_bytes(),before)

    def test_native_uv_uses_top_left_texture_origin(self):
        part=card(0,[255,0,0])
        part['triangles'][:,:,6:8]=[.25,.25]
        part['texture'][2:,:,3]=0
        _,owner,_=render([part],np.array([0.,0.,5.]),np.zeros(3),60,32)
        self.assertEqual(owner[16,16],0)
        part['triangles'][:,:,7]=.75
        _,owner,_=render([part],np.array([0.,0.,5.]),np.zeros(3),60,32)
        self.assertEqual(owner[16,16],-1)

    def test_occluded_layers_still_contribute_to_coverage_constraint(self):
        footprints=[[],[]]
        parts=[card(0,[0,0,255]),card(1,[255,0,0])]
        rgb,owner,depth=render(parts,np.array([0.,0.,5.]),np.zeros(3),60,32,footprints=footprints)
        self.assertEqual(owner[16,16],1)
        self.assertAlmostEqual(depth[16,16],4)
        self.assertGreater(rgb[16,16,0],0)
        self.assertEqual(rgb[16,16,2],0)
        self.assertTrue(all(len(p)>0 for p in footprints))
        self.assertIn(16*32+16,np.concatenate(footprints[0]))

    def test_alpha_hole_exposes_underlying_layer_and_mask_removes_geometry(self):
        far,near=card(0,[0,0,255]),card(1,[255,0,0],alpha=0)
        eye=np.array([0.,0.,5.])
        _,owner,depth=render([far,near],eye,np.zeros(3),60,32)
        self.assertEqual(owner[16,16],0)
        self.assertAlmostEqual(depth[16,16],5)
        _,owner,depth=render([far,near],eye,np.zeros(3),60,32,
                             [np.zeros(2,dtype=bool),np.ones(2,dtype=bool)])
        self.assertTrue((owner==-1).all())
        self.assertTrue(np.isinf(depth).all())


if __name__=='__main__': unittest.main()
