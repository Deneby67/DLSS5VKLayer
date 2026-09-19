import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('sr',Path(__file__).resolve().parents[1]/'tools/control-dlss-sr.py')
sr=importlib.util.module_from_spec(spec);spec.loader.exec_module(sr)

class Control(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.home=Path(self.tmp.name);self.proc=self.home/'proc';self.proc.mkdir()
        self.game=self.home/'.steam/debian-installation/steamapps/common/Red Dead Redemption 2/nvngx_dlss.dll'
        self.game.parent.mkdir(parents=True);self.game.write_bytes(b'tested-SR-fixture')
        digest=hashlib.sha256(self.game.read_bytes()).hexdigest()
        original=sr.LATEST_SHA;sr.LATEST_SHA=digest;self.addCleanup(setattr,sr,'LATEST_SHA',original)
        record=self.home/'.config/dlssnr/native-inline-install.json';record.parent.mkdir(parents=True)
        record.write_text(json.dumps({'sr_source':{'version':'310.9.1.0','sha256':digest}}))
        self.arm=self.home/'.local/state/dlssnr/native-inline/run-123/arm.txt'
        self.arm.parent.mkdir(parents=True)
    def test_default_is_transformer_k_without_writing(self):
        status=sr.status(self.home,self.proc)
        self.assertEqual(status['config'],sr.DEFAULT);self.assertTrue(status['installed'])
        self.assertFalse(sr.config_path(self.home).exists())
    def test_save_does_not_replace_dll_or_arm_nr(self):
        before=self.game.read_bytes()
        sr.save(self.home,'cnn','F')
        self.assertEqual(sr.load(self.home)['preset'],'F');self.assertEqual(self.game.read_bytes(),before)
        self.assertFalse(self.arm.exists())
    def test_cross_family_and_reserved_presets_rejected(self):
        for model,preset in [('cnn','K'),('transformer','E'),('game','K'),('cnn','A'),('transformer','G'),('unknown','K')]:
            with self.assertRaises(ValueError):sr.save(self.home,model,preset)
        self.assertFalse(sr.config_path(self.home).exists())
    def test_launch_freezes_selection_preserves_other_environment(self):
        sr.save(self.home,'cnn','E')
        incoming={'DLSSNR_ARM_FILE':'Z:'+str(self.arm),'KEEP':'yes'}
        env=sr.launch_environment(self.home,incoming)
        self.assertEqual(env['DLSSNR_SR_PRESET'],'5');self.assertEqual(env['KEEP'],'yes')
        sr.save(self.home,'transformer','M')
        self.assertEqual(env['DLSSNR_SR_PRESET'],'5')
        self.assertNotIn('DLSSNR_SR_PRESET',incoming)
    def test_unknown_dll_is_preserved_and_launch_refused(self):
        self.game.write_bytes(b'other modification')
        with self.assertRaises(ValueError):sr.launch_environment(self.home,{'DLSSNR_ARM_FILE':'Z:'+str(self.arm)})
        self.assertEqual(self.game.read_bytes(),b'other modification')
    def test_requires_private_session(self):
        with self.assertRaises(ValueError):sr.launch_environment(self.home,{})
    def test_runtime_and_pending_status(self):
        proc=self.proc/'123';proc.mkdir();(proc/'comm').write_text('RDR2.exe\n')
        (proc/'environ').write_bytes(('DLSSNR_ARM_FILE=Z:'+str(self.arm)+'\0DLSSNR_SR_PRESET=11\0').encode())
        report=self.arm.with_name('sr-status.json')
        report.write_text(json.dumps(dict(schema=1,preset=11,preset_reads=2,result=1)))
        self.assertIn('created DLSS',sr.status(self.home,self.proc)['message'])
        sr.save(self.home,'cnn','E')
        self.assertTrue(sr.status(self.home,self.proc)['restart_required'])
        # A launcher or an isolated fixture must not look like an active game session.
        (proc/'comm').write_text('Launcher.exe\n')
        self.assertEqual(sr.status(self.home,self.proc)['sessions'],[])

if __name__=='__main__':unittest.main()
