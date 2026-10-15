"""Share verified dependency artifacts without sharing writable build trees."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from session_lock import acquire


def cache_root(project):
    if os.environ.get('KOBALT_DEPENDENCY_CACHE'):
        return Path(os.environ['KOBALT_DEPENDENCY_CACHE']).expanduser().resolve()
    # Linked worktrees use the original checkout's parent, too.
    result = subprocess.run(['git', '-C', str(project), 'rev-parse', '--git-common-dir'],
                            capture_output=True, text=True)
    if result.returncode == 0:
        common = (project / result.stdout.strip()).resolve()
        return common.parent.parent / '.cache/kobalt'
    return project.parent / '.cache/kobalt'


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def fetch_source(name, spec, cache, shared, offline=False):
    from build import fetch
    archive = cache / (name + '.tar.gz')
    shared = Path(shared) / spec['sha256'] / archive.name
    if shared.is_file() and (not archive.is_file()
                            or hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']):
        if hashlib.sha256(shared.read_bytes()).hexdigest() != spec['sha256']:
            raise ValueError('Shared dependency archive hash mismatch: ' + name)
        with tempfile.TemporaryDirectory(prefix='shared-', dir=cache) as temporary:
            temporary = Path(temporary) / archive.name
            shutil.copy2(shared, temporary)
            temporary.replace(archive)
    fetch(name, spec, cache, offline)
    if not shared.is_file():
        shared.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='archive-', dir=shared.parent) as temporary:
            temporary = Path(temporary) / archive.name
            shutil.copy2(archive, temporary)
            temporary.replace(shared)


def gradle_cache(project):
    root = cache_root(project)
    root.mkdir(parents=True, exist_ok=True)
    directory = root / 'gradle'
    with acquire(root / 'gradle-import.lock', wait=True):
        if not directory.exists():
            legacy = project / '.cache/android-gradle'
            if legacy.is_dir():
                with tempfile.TemporaryDirectory(prefix='gradle-', dir=root) as temporary:
                    imported = Path(temporary) / 'cache'
                    shutil.copytree(legacy, imported)
                    imported.rename(directory)
            else:
                directory.mkdir()
    return directory


def stage_cmake(directory, prefix, files, environment=None):
    """Capture absolute install destinations as well as prefix-relative ones."""
    stage = files / 'stage'
    env = dict(os.environ if environment is None else environment, DESTDIR=str(stage))
    subprocess.run(['cmake', '--install', str(directory)], env=env, check=True)
    shutil.move(str(stage / str(prefix).lstrip('/')), files / 'deps')
    shutil.rmtree(stage)


def safe_path(root, name):
    path = Path(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('Invalid dependency artifact path: ' + str(name))
    result = root / path
    result.resolve().relative_to(root.resolve())
    return result


class DependencyCache:
    def __init__(self, root, target):
        self.root = Path(root) / 'artifacts' / target
        self.root.mkdir(parents=True, exist_ok=True)
        self.events = []

    def read(self, directory, identity):
        try:
            record = json.loads((directory / 'artifact.json').read_text())
            if record['identity'] != identity or not isinstance(record['files'], dict) or not record['files']:
                return None
            if not isinstance(record['output'], str):
                return None
            for name, digest in record['files'].items():
                path = safe_path(directory / 'files', name)
                if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    return None
            return record
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def install(self, directory, record, output, name):
        prefix = str(output)
        output = output.resolve()
        state = output / '.dependency-cache' / (name + '.json')
        state.parent.mkdir(parents=True, exist_ok=True)
        try:
            old = json.loads(state.read_text())
            obsolete = set(old['files']) - set(record['files'])
        except (OSError, ValueError, KeyError, TypeError):
            obsolete = set()
        for relative in obsolete:
            safe_path(output, relative).unlink(missing_ok=True)
        for relative in record['files']:
            source = safe_path(directory / 'files', relative)
            destination = safe_path(output, relative)
            data = source.read_bytes()
            if destination.suffix in ('.pc', '.cmake', '.la'):
                data = data.replace(record['output'].encode(), prefix.encode())
            if destination.is_file() and destination.read_bytes() == data:
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            shutil.copymode(source, destination)
        state.write_text(json.dumps({'files': list(record['files'])}) + '\n')

    def build(self, name, inputs, output, builder):
        identity = json.loads(json.dumps({'schema': 1, 'name': name, 'inputs': inputs}))
        key = fingerprint(identity)
        parent = self.root / name
        parent.mkdir(exist_ok=True)
        start = time.monotonic()
        with acquire(parent / (key + '.lock'), wait=True):
            directory = parent / key
            record = self.read(directory, identity)
            hit = record is not None
            if not hit:
                with tempfile.TemporaryDirectory(prefix=key + '-', dir=parent) as temporary:
                    temporary = Path(temporary)
                    files = temporary / 'files'
                    files.mkdir()
                    builder(files, key)
                    # Installers use aliases such as libpng.a -> libpng16.a.
                    # Store their bytes so an artifact has no external link targets.
                    for path in files.rglob('*'):
                        if path.is_symlink():
                            path.resolve().relative_to(files.resolve())
                            data, mode = path.read_bytes(), path.stat().st_mode & 0o777
                            path.unlink()
                            path.write_bytes(data)
                            path.chmod(mode)
                    hashes = {path.relative_to(files).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in files.rglob('*') if path.is_file()}
                    if not hashes:
                        raise ValueError('Dependency build produced no artifacts: ' + name)
                    record = {'identity': identity, 'output': str(output), 'files': hashes}
                    (temporary / 'artifact.json').write_text(json.dumps(record, indent=2) + '\n')
                    if directory.exists():
                        shutil.rmtree(directory)
                    temporary.rename(directory)
            self.install(directory, record, output, name)
        event = {'dependency': name, 'key': key, 'cache_hit': hit,
                 'seconds': round(time.monotonic() - start, 3)}
        self.events.append(event)
        print('Dependency ' + name + ': ' + ('reused' if hit else 'built')
              + ' (' + str(event['seconds']) + 's)', flush=True)
        return key
