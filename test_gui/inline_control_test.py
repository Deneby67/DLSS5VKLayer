import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('control',Path(__file__).resolve().parents[1]/'tools/control-nr-inline.py')
control=importlib.util.module_from_spec(spec);spec.loader.exec_module(control)

class Sessions(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.home=Path(self.tmp.name);self.proc=self.home/'proc';self.proc.mkdir()
        self.log=self.home/'.local/state/dlssnr/native-inline/run-test/adapter.log'
        self.log.parent.mkdir(parents=True);self.log.write_text('[nr-inline] arm-ready pid=42 token=900; NR disabled until matching request\n')
        self.p=self.proc/'123';(self.p/'fd').mkdir(parents=True)
        (self.p/'comm').write_text('RDR2.exe\n')
        (self.p/'environ').write_bytes(('DLSSNR_ARM_FILE=Z:'+str(self.log.with_name('arm.txt'))).encode()+b'\0')
        (self.p/'fd/7').symlink_to(self.log)
        dll=self.home/'vulkan-1.dll';dll.write_bytes(b'fixture');st=dll.stat()
        self.mapping=f'1000-2000 r--p 0 {os.major(st.st_dev):02x}:{os.minor(st.st_dev):02x} {st.st_ino} {dll}\n'
        (self.p/'maps').write_text(self.mapping)
        record=self.home/'.config/dlssnr/native-inline-install.json';record.parent.mkdir(parents=True)
        record.write_text(json.dumps({'installed_hashes':{str(dll):'fixture'}}))
    def status(self):return control.status(self.home,proc_root=self.proc)
    def command(self,mode,token='900'):return control.control(self.home,str(self.log),mode,self.proc,token)
    def test_discovery_does_not_arm(self):
        s=self.status();self.assertEqual(s['status'],'off');self.assertTrue(s['can_enable']);self.assertFalse(self.log.with_name('arm.txt').exists())
    def test_request_ack_and_disable(self):
        self.assertEqual(self.command('on')['status'],'pending')
        with self.log.open('a') as f:f.write('[nr-inline] arm-state enabled\n[nr-inline] recorded NR-before-SR calls=1\n')
        self.assertEqual(self.status()['status'],'recording')
        self.assertEqual(self.command('off')['status'],'pending')
    def test_wrong_session_rejected(self):
        with self.assertRaises(ValueError):self.command('on','901')
        self.assertFalse(self.log.with_name('arm.txt').exists())
    def test_stale_mapping_requires_restart(self):
        (self.p/'maps').write_text(self.mapping.rstrip()+' (deleted)\n')
        self.assertEqual(self.status()['status'],'restart')
        with self.assertRaises(ValueError):self.command('on')
    def test_dead_process_cannot_be_controlled(self):
        (self.p/'fd/7').unlink()
        self.assertFalse(self.status()['ready'])
        with self.assertRaises(ValueError):self.command('on')
    def test_terminal_failure_allows_turning_off(self):
        self.command('on')
        with self.log.open('a') as f:f.write('[nr-inline] arm-state enabled\n[nr-inline] initialization failed; original SR retained\n')
        self.assertFalse(self.status()['can_enable'])
        with self.assertRaises(ValueError):self.command('on')
        self.assertFalse(self.command('off')['requested_on'])
    def test_bypass_is_explained(self):
        self.command('on')
        with self.log.open('a') as f:f.write('[nr-inline] arm-state enabled\n[nr-inline] bypass: unsupported resource formats or extents\n')
        self.assertEqual(self.status()['status'],'bypassed')
        self.assertIn('formats',self.status()['reason'])
    def test_ambiguous_sessions_rejected(self):
        import shutil
        shutil.copytree(self.p,self.proc/'124',symlinks=True)
        self.assertEqual(self.status()['status'],'ambiguous')
        with self.assertRaises(ValueError):self.command('on')
    def test_oversized_log_still_allows_off(self):
        self.command('on')
        with self.log.open('a') as f:f.write(' '*1100000)
        self.assertFalse(self.status()['can_enable'])
        self.assertFalse(self.command('off')['requested_on'])
    def test_auto_never_selects_a_write_target(self):
        with self.assertRaises(ValueError):control.control(self.home,'auto','on',self.proc)

if __name__=='__main__':unittest.main()
