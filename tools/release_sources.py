#!/usr/bin/env python3
"""Bundle release source, pinned dependency archives and installed font assets."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
import urllib.request
from version import release_version

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def download(spec, destination, offline=False):
    if destination.is_symlink():
        raise ValueError('Source cache entry must not be a symlink')
    if destination.exists():
        if digest(destination) != spec['sha256']:
            raise ValueError('Cached source checksum mismatch: ' + destination.name)
        return
    if offline:
        raise ValueError('Source archive is not cached: ' + destination.name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
        partial = Path(temporary) / 'download'
        with urllib.request.urlopen(spec['url'], timeout=120) as response, partial.open('wb') as output:
            size = 0
            while block := response.read(1024 * 1024):
                size += len(block)
                if size > 1024 * 1024 * 1024:
                    raise ValueError('Source download exceeds 1 GiB')
                output.write(block)
        if digest(partial) != spec['sha256']:
            raise ValueError('Source checksum mismatch: ' + destination.name)
        partial.replace(destination)


def checkout(root, allow_dirty=False):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args]).decode().strip()
    if Path(git('rev-parse', '--show-toplevel')).resolve() != root.resolve():
        raise ValueError('Run from a Kobalt source checkout')
    dirty = bool(git('status', '--porcelain', '--untracked-files=all'))
    if dirty and not allow_dirty:
        raise ValueError('Release inputs must be committed and clean')
    return {'commit': git('rev-parse', 'HEAD'), 'dirty': dirty}


def safe_source(root, name):
    path = PurePosixPath(name)
    shared_debug_key = name == 'android/debug.keystore'
    if (not path.parts or path.is_absolute() or any(p in ('..', '.git') or p.startswith('.env') for p in path.parts)
            or '\\' in name or any(ord(c) < 32 for c in name)
            or path.parts[0] in ('build', 'tmp', 'state')
            or (path.suffix.lower() in ('.p12', '.pem', '.key', '.jks', '.keystore', '.epub', '.kepub', '.db', '.sqlite')
                and not shared_debug_key)):
        raise ValueError('Unexpected release source path: ' + name)
    source = root / name
    if not source.is_file() or any(p.is_symlink() for p in (source, *source.parents) if p != root and p.is_relative_to(root)):
        raise ValueError('Release source must be a regular file: ' + name)
    if shared_debug_key and digest(source) != 'e7b7893c7abd2ee65d0bc98645e3d5dd88e6c092da451fd1c1bfea548fd31912':
        raise ValueError('Unexpected shared Android debug keystore')
    return source


def inputs(root, offline=False, allow_dirty=False):
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '--cached',
                                    *(['--others', '--exclude-standard'] if allow_dirty else [])]).decode().split('\0')
    files = {name: safe_source(root, name) for name in names if name and (root / name).exists()}
    specs = json.loads((root / 'dependencies/manifest.json').read_text())
    for name, spec in specs.items():
        relative = '.cache/kobalt/' + name + '.tar.gz'
        download(spec, root / relative, offline)
        files[relative] = root / relative
    # Android overrides some Kobo dependency versions. Keep both source sets.
    for name, spec in json.loads((root / 'android/dependencies.json').read_text()).items():
        relative = '.cache/android-dependencies/' + name + '.tar.gz'
        download(spec, root / relative, offline)
        files[relative] = root / relative
    extra = json.loads((root / 'dependencies/release-sources.json').read_text())
    qt = json.loads((root / 'toolchains/qt6/source/source.json').read_text())
    extra['qt6-base'] = {**qt, 'file': 'qtbase-' + qt['version'] + '.tar.xz'}
    fonts = json.loads((root / 'resources/fonts/ebook-core.json').read_text())
    if not extra['ebook-fonts']['url'].endswith('/' + fonts['revision']):
        raise ValueError('Update the font source archive pin with the reading font revision')
    for spec in extra.values():
        filename = spec['file']
        if Path(filename).name != filename or not re.fullmatch(r'[a-zA-Z0-9_.-]+', filename):
            raise ValueError('Invalid source archive filename')
        relative = '.cache/release-sources/' + filename
        download(spec, root / relative, offline)
        files[relative] = root / relative
    import fetch_fonts
    fetch_fonts.fetch(root, offline=offline)
    for font in fonts['files']:
        name = 'vendor/ebook-fonts/' + font['file']
        files[name] = safe_source(root, name)
    return files


def bundle(root, output, *, offline=False, allow_dirty=False):
    provenance = checkout(root, allow_dirty)
    files = inputs(root, offline, allow_dirty)
    record = {**provenance, 'version': release_version(root), 'format': 1, 'targets': ['qt5', 'qt6', 'android'],
              'files': {name: {'sha256': digest(path), 'size': path.stat().st_size} for name, path in sorted(files.items())}}
    if checkout(root, allow_dirty) != provenance:
        raise ValueError('Checkout changed while collecting release inputs')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        partial = Path(temporary) / 'bundle.tar.gz'
        with partial.open('wb') as stream, gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w|') as archive:
                for name, path in sorted(files.items()):
                    info = tarfile.TarInfo('kobalt/' + name)
                    info.size = record['files'][name]['size']
                    info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                    with path.open('rb') as source:
                        archive.addfile(info, source)
                    if digest(path) != record['files'][name]['sha256']:
                        raise ValueError('Source changed during packaging: ' + name)
                data = (json.dumps(record, indent=2, sort_keys=True) + '\n').encode()
                info = tarfile.TarInfo('kobalt/SOURCE-BUNDLE.json'); info.size = len(data); info.mode = 0o644
                archive.addfile(info, io.BytesIO(data))
        partial.replace(output)
    return {**provenance, 'version': record['version'], 'file': output.name, 'sha256': digest(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--allow-dirty', action='store_true', help='local packaging checks only; drafts reject this output')
    args = parser.parse_args()
    output = ROOT / 'build/release/Kobalt-source.tar.gz'
    record = bundle(ROOT, output, offline=args.offline, allow_dirty=args.allow_dirty)
    output.with_name('source.json').write_text(json.dumps(record, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
