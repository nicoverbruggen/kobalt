"""Record which sources a build used, so checks never run an older build."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Include code generators and toolchain recipes: changing either can change
# the binary without editing C++. Test runners and generated test fixtures
# run from the checkout and need no rebuild.
INPUTS = ('VERSION', 'CMakeLists.txt', 'cmake', 'targets', 'src', 'resources', 'dependencies',
          'toolchains/nickelhook-qt6', 'toolchains/qt5', 'toolchains/qt6',
          'vendor/ebook-fonts', 'tools/fetch_fonts.py', 'tools/build.py', 'tools/build_engine.py', 'tools/assets.py', 'tools/ui_strings.py', 'tools/ui_icon_vectors.py', 'tools/version.py',
          'tools/mini_fonts.py', 'tools/dependency_cache.py', 'tests/Checks.cmake', 'tests/model', 'tests/unit')
STAMP = 'source-state'


def source_state(root=ROOT):
    """A hash of the path and content of every build input."""
    digest = hashlib.sha256()
    for name in INPUTS:
        path = root / name
        files = [path] if path.is_file() else sorted(path.rglob('*'))
        for file in files:
            if not file.is_file() or '__pycache__' in file.parts or file.suffix == '.pyc' or file.name == '.DS_Store':
                continue
            digest.update(file.relative_to(root).as_posix().encode() + b'\0')
            digest.update(hashlib.sha256(file.read_bytes()).digest())
    return digest.hexdigest()


def clear(output):
    """Forget the recorded sources before a build, so a failed build has none."""
    (output / STAMP).unlink(missing_ok=True)


def record(output, state):
    (output / STAMP).write_text(state + '\n')


def current(output, root=ROOT):
    """Whether the build in output was made from the sources in root."""
    stamp = output / STAMP
    return stamp.is_file() and stamp.read_text().strip() == source_state(root)
