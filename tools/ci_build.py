#!/usr/bin/env python3
"""Build a device package in a disposable SDK container without host bind mounts."""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import uuid
import io
import zipfile

from build import fetch
from build_engine import detect, ensure_toolchain5, ensure_toolchain6, native_platform
from release_sources import checkout, digest, safe_source
from targets import target, output_path
import fetch_fonts
import assets
import source_state
from version import release_version

ROOT = Path(__file__).resolve().parents[1]


def validate_package(package, selected, version, mini=False):
    assets.check_package_size(package, mini=mini)
    with zipfile.ZipFile(package) as archive:
        manifest = json.loads(archive.read('.adds/kobalt/assets/manifest.json'))
        if manifest.get('version') != version:
            raise ValueError('Package assets do not match the release version')
        with tarfile.open(fileobj=io.BytesIO(archive.read('.kobo/' + selected.package_name))) as installer:
            members = installer.getmembers()
            if len(members) != 1 or members[0].name != selected.package_member or not members[0].isfile():
                raise ValueError('Installer must contain only the target reader library')
            header = installer.extractfile(members[0]).read(20)
            if header[:6] != b'\x7fELF\x01\x01' or header[18:20] != b'\x28\x00':
                raise ValueError('Installer is not a little-endian ARM32 library')


def build(selected, jobs, allow_dirty=False, sdk_image=None, cpus=None, mini=False):
    provenance = checkout(ROOT, allow_dirty)
    version = release_version(ROOT)
    engine = detect()
    if sdk_image:
        # An explicit SDK must be available; do not rebuild Qt after a failed pull.
        subprocess.run([engine, 'pull', '--platform', native_platform(engine), sdk_image], check=True)
    else:
        (ensure_toolchain5 if selected.qt_major == 5 else ensure_toolchain6)(engine, ROOT)
    image = subprocess.check_output([engine, 'image', 'inspect', '--format', '{{.Id}}', sdk_image or selected.toolchain_image], text=True).strip()
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    if not mini:
        specs.pop('fonttools')
    cache = ROOT / '.cache/kobalt'; cache.mkdir(parents=True, exist_ok=True)
    for name, spec in specs.items(): fetch(name, spec, cache)
    fetch_fonts.fetch(ROOT)
    sources = source_state.source_state(ROOT)
    names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z', '--cached',
                                    *(['--others', '--exclude-standard'] if allow_dirty else [])]).decode().split('\0')
    files = {name: safe_source(ROOT, name) for name in names if name and (ROOT / name).exists()}
    for name in specs: files['.cache/kobalt/' + name + '.tar.gz'] = cache / (name + '.tar.gz')
    for item in json.loads((ROOT / 'resources/fonts/ebook-core.json').read_text())['files']:
        name = 'vendor/ebook-fonts/' + item['file']; files[name] = safe_source(ROOT, name)
    output = ROOT / ('build/mini' if mini else 'build/release'); output.mkdir(parents=True, exist_ok=True)
    container = 'kobalt-build-' + uuid.uuid4().hex
    command = ['python3', 'tools/build.py', '--in-toolchain', '--hardware', '--target', selected.name,
               '--jobs', str(jobs), '--version', version, '--commit', provenance['commit'][:12], *(['--modified'] if provenance['dirty'] else [])]
    if mini:
        command.append('--mini')
    limits = ['--cpus', str(cpus)] if cpus is not None else []
    subprocess.run([engine, 'create', '--name', container, *limits, '--network', 'none', '--workdir', '/work', image, *command], check=True)
    try:
        with tempfile.TemporaryFile() as stream:
            with tarfile.open(fileobj=stream, mode='w') as archive:
                for name, path in sorted(files.items()): archive.add(path, arcname=name, recursive=False)
            stream.seek(0)
            subprocess.run([engine, 'cp', '-', container + ':/work'], stdin=stream, check=True)
        subprocess.run([engine, 'start', '--attach', container], check=True)
        code = subprocess.check_output([engine, 'inspect', '--format', '{{.State.ExitCode}}', container], text=True).strip()
        if code != '0': raise RuntimeError('SDK build exited with status ' + code)
        package = output / ('Kobalt-' + selected.name + ('-mini' if mini else '') + '.zip')
        subprocess.run([engine, 'cp', container + ':/work/' + str(output_path(selected, hardware=True, mini=mini) / 'Kobalt.zip'), str(package)], check=True)
        validate_package(package, selected, version, mini=mini)
        record = {**provenance, 'target': selected.name, 'version': version, 'sdk_image': image, 'file': package.name, 'sha256': digest(package)}
        if mini:
            record['profile'] = 'mini'
            record['size'] = package.stat().st_size
        if checkout(ROOT, allow_dirty) != provenance: raise ValueError('Checkout changed during build')
        if source_state.source_state(ROOT) != sources:
            raise ValueError('Build sources changed during build')
        (output / (selected.name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    finally:
        subprocess.run([engine, 'rm', '--force', container], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('qt5', 'qt6'), required=True)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cpus', type=int, help='limit the build container to this many CPUs')
    parser.add_argument('--sdk-image', help='pull a prebuilt SDK instead of building one locally')
    parser.add_argument('--allow-dirty', action='store_true', help='local validation only')
    parser.add_argument('--mini', action='store_true', help='build a test ZIP capped at 28 MB in build/mini')
    args = parser.parse_args()
    if args.jobs < 1: parser.error('--jobs must be positive')
    if args.cpus is not None and args.cpus < 1: parser.error('--cpus must be positive')
    build(target(args.target), args.jobs, args.allow_dirty, args.sdk_image, args.cpus, args.mini)
