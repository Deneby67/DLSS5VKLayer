import importlib.util
from pathlib import Path
import unittest
import numpy as np

spec=importlib.util.spec_from_file_location('study',Path(__file__).resolve().parents[1]/'tools/run-nr-color-study.py')
s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)

class ColorInputs(unittest.TestCase):
    def setUp(self):
        original=np.zeros((64,64,4),dtype='f4');original[:,:,:3]=(.001,.01,.1);original[:,:,3]=-2
        encoded=original.copy();encoded[:,:,:3]=s.a.proxy(original[:,:,:3],100)
        self.planes=[original,encoded,encoded.copy(),original.copy()]
        self.cases={name:(x,hdr) for name,x,hdr,_ in s.inputs(self.planes,np.array([100,1,.1,1]))}
    def test_baseline_exact_and_opaque_changes_only_alpha(self):
        np.testing.assert_array_equal(self.cases['baseline'][0],self.planes[1])
        np.testing.assert_array_equal(self.cases['opaque-alpha'][0][:,:,:3],self.planes[1][:,:,:3])
        self.assertTrue(np.all(self.cases['opaque-alpha'][0][:,:,3]==1))
    def test_inputs_finite_and_hdr_explicit(self):
        for name,(x,hdr) in self.cases.items():
            self.assertTrue(np.isfinite(x).all(),name)
            self.assertEqual(hdr,name=='native-hdr')
            if name!='baseline':self.assertTrue(np.all(x[:,:,3]==1))
            if not hdr:self.assertTrue(np.all((x[:,:,:3]>=0)&(x[:,:,:3]<=1)),name)
    def test_original_not_mutated(self):
        self.assertTrue(np.all(self.planes[0][:,:,3]==-2))
        np.testing.assert_array_equal(self.cases['native-hdr'][0][:,:,:3],np.maximum(self.planes[0][:,:,:3],0)*100)

if __name__=='__main__':unittest.main()
