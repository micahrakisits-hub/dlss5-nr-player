"""Package an already-built player with locally supplied runtime files."""
import argparse
from pathlib import Path
import sys


def main():
    project = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist-dir', type=Path, default=project / 'dist')
    options = parser.parse_args()
    files = ['nr_player.exe', 'ffmpeg.exe', 'ffprobe.exe', '_nvngx.dll',
             'nvngx_dlssnr.dll', 'caller/nvngx.dll', 'runtime40/nvngx_dlssnr.dll']
    missing = [name for name in files if not (project / name).is_file()]
    if missing:
        parser.error('Missing local dependencies: ' + ', '.join(missing) + '. See README.md.')
    ffi_candidates = [Path(sys.prefix) / 'Library' / 'bin' / 'ffi.dll',
                      Path(sys.prefix) / 'ffi.dll']
    ffi_dll = next((path for path in ffi_candidates if path.is_file()), None)
    if ffi_dll is None:
        parser.error('Could not find ffi.dll required by Python ctypes. '
                     'Install libffi in the active Python environment.')
    try:
        from PyInstaller.__main__ import run
    except ImportError:
        parser.error('Install PyInstaller: python -m pip install pyinstaller')
    args = ['--noconfirm', '--clean', '--onefile', '--windowed', '--noupx',
            '--name', 'DLSS 5 NR Player', '--distpath', str(options.dist_dir.resolve()),
            '--workpath', str(project / 'build' / 'portable'),
            '--specpath', str(project / 'build')]
    for name in files:
        args += ['--add-data', str(project / name) + ';' + str(Path(name).parent)]
    args += ['--add-binary', str(ffi_dll) + ';.']
    args.append(str(project / 'portable_launcher.py'))
    run(args)


if __name__ == '__main__':
    main()
