"""Package an already-built player with locally supplied runtime files."""
import argparse
from pathlib import Path


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
    try:
        from PyInstaller.__main__ import run
    except ImportError:
        parser.error('Install PyInstaller: python -m pip install pyinstaller')
    args = ['--noconfirm', '--clean', '--onefile', '--windowed',
            '--name', 'DLSS 5 NR Player', '--distpath', str(options.dist_dir.resolve()),
            '--workpath', str(project / 'build' / 'portable'),
            '--specpath', str(project / 'build')]
    for name in files:
        args += ['--add-data', str(project / name) + ';' + str(Path(name).parent)]
    args.append(str(project / 'portable_launcher.py'))
    run(args)


if __name__ == '__main__':
    main()
