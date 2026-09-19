#!/usr/bin/env python3
"""Build the Qt GUI using local Qt dev files and the patched Clang (24 workers)."""
import concurrent.futures
import os
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[1];os.chdir(repo)
out=repo/'build/gui-native';out.mkdir(parents=True,exist_ok=True)
qt=Path(os.environ.get('DLSSNR_QT_DEV_ROOT',out/'qt'))
include=qt/'usr/include/x86_64-linux-gnu/qt6'
moc=qt/'usr/lib/qt6/libexec/moc'
clang=Path(os.environ.get('LLVM_BIN','/opt/llvm-23.1.1-bp18/bin'))/'clang++'
flags=['-std=c++17','-O2','-fPIC','-pthread','-Icommon','-Igui','-Ilayer_linux/src','-Ibuild-layer-fix/deps/usr/include']
for name in ('','QtCore','QtGui','QtWidgets'):flags+=['-I'+str(include/name)]
sources=['gui/main.cpp','gui/mainwindow.cpp','gui/inline_nr_panel.cpp','gui/dlss_sr_panel.cpp','gui/passdialog.cpp','gui/shm_binder.cpp','layer_linux/src/hotkey.cpp','common/runner_discovery.cpp']
for name in ('mainwindow','passdialog','shm_binder'):
    generated=out/f'moc_{name}.cpp'
    subprocess.run([str(moc),f'gui/{name}.h','-o',str(generated)],check=True);sources.append(str(generated))
sources+=['test_gui/inline_panel_test.cpp','test_gui/binder_test.cpp','test_gui/dlss_sr_panel_test.cpp']
def compile(source):
    obj=out/(Path(source).stem+'.o')
    subprocess.run([str(clang),*flags,'-c',source,'-o',str(obj)],check=True)
    return source,obj
with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:objects=dict(pool.map(compile,sources))
libs=[f'/usr/lib/x86_64-linux-gnu/libQt6{name}.so.6' for name in ('Widgets','Gui','Core')]
def link(name,selected):
    subprocess.run([str(clang),'-fuse-ld=lld','-Wl,--threads=24','-pthread',*[str(objects[s]) for s in selected],*libs,'-ldl','-o',str(out/name)],check=True)
link('dlssnr_gui',sources[:-3])
link('inline_panel_test',['test_gui/inline_panel_test.cpp','gui/inline_nr_panel.cpp'])
link('binder_test',['test_gui/binder_test.cpp','gui/shm_binder.cpp',str(out/'moc_shm_binder.cpp')])

link('dlss_sr_panel_test',['test_gui/dlss_sr_panel_test.cpp','gui/dlss_sr_panel.cpp'])
