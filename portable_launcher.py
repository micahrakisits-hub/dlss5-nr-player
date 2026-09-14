"""Single-file distribution entry point; payload is extracted by PyInstaller."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

def show_error(message, title):
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, message, title, 0x10)
    except ImportError:
        log_path = Path(tempfile.gettempdir()) / 'DLSS5-NR-Player.log'
        log_path.write_text(message, encoding='utf-8')

def main():
    runtime = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))
    environment = os.environ.copy()
    environment['PATH'] = str(runtime) + os.pathsep + environment.get('PATH', '')
    log_path = Path(tempfile.gettempdir()) / 'DLSS5-NR-Player.log'
    # The native application owns the window and stays alive across file opens.
    # Waiting keeps the extracted runtime available until the player closes.
    with log_path.open('w', encoding='utf-8') as log:
        process = subprocess.Popen(
            [str(runtime / 'nr_player.exe'), '--gui', *sys.argv[1:]],
            cwd=runtime, env=environment, stdin=subprocess.DEVNULL,
            stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        code = process.wait()
    if code:
        show_error(f'The player exited with an error. Details: {log_path}', 'DLSS 5 NR Player')
    return code

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        show_error(str(error), 'Cannot start DLSS 5 NR Player')
        sys.exit(1)
