import importlib.util
from pathlib import Path
import unittest
import numpy as np

spec=importlib.util.spec_from_file_location('analysis',Path(__file__).resolve().parents[1]/'tools/analyze-nr-hdr.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)

class HdrAnalysis(unittest.TestCase):
    def images(self):
        original=np.full((64,64,4),9,dtype=np.float32);original[:,:,3]=.5
        before=original.copy();before[:,:,:3]=a.srgb(np.full((64,64,3),.9))
        after=original.copy();after[:,:,:3]=a.srgb(np.full((64,64,3),.95))
        restored=original.copy();restored[:,:,:3]=9.5
        return [original,before,after,restored]
    def test_attenuation(self):
        report,previews=a.metrics(self.images(),np.array([1,1,.4,1]))
        self.assertAlmostEqual(report['retained_linear_edit_projection'],2/21,places=4)
        self.assertLess(report['restored_shader_reference_max_error'],.01)
        self.assertAlmostEqual(report['input_proxy_reference_mae'],0,places=6)
        self.assertEqual(report['alpha_changed_fraction'],0)
        self.assertEqual(len(previews),4)
    def test_identity(self):
        images=self.images();images[2]=images[1].copy();images[3]=images[0].copy()
        report,_=a.metrics(images,np.array([1,1,0,1]))
        self.assertIsNone(report['retained_linear_edit_projection'])
        self.assertEqual(report['hdr_changed_pixel_fraction'],0)
    def test_bad_exposure_is_not_treated_as_valid(self):
        for ev in (0,-1,float('nan'),float('inf')):
            report,previews=a.metrics(self.images(),np.array([ev,1,0,1]))
            self.assertFalse(report['exposure_valid']);self.assertIsNone(previews)
            self.assertNotIn('retained_linear_edit_projection',report)
    def test_nonfinite_output_not_hidden(self):
        images=self.images();images[2][0,0,0]=float('nan')
        report,previews=a.metrics(images,np.array([1,1,0,1]))
        self.assertEqual(report['nonfinite_components'][2],1);self.assertIsNone(previews)

if __name__=='__main__':unittest.main()
