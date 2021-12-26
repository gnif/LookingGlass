# -- Path setup --------------------------------------------------------------
from pathlib import Path

try:
    with open(Path(__file__).parent.parent / 'VERSION') as f:
        release = f.read().strip()
except IOError:
    import subprocess
    try:
        release = subprocess.check_output([
            'git', 'describe', '--always', '--abbrev=10', '--dirty=+', '--tags'
        ]).decode('utf-8').strip()
    except (subprocess.CalledProcessError, OSError):
        release = '(unknown version)'
    del subprocess
