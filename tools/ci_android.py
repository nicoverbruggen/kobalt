#!/usr/bin/env python3
"""Build and verify a debug APK in a disposable Android SDK container."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile
import time
import uuid
import zipfile

from build_engine import detect, native_platform
import fetch_fonts
from release_sources import checkout, digest, download, safe_source
from session_lock import acquire
from version import release_version
from dependency_cache import cache_root, gradle_cache

ROOT = Path(__file__).resolve().parents[1]
SDK_IMAGE = 'ghcr.io/nicoverbruggen/kobuild:android-v0.2.0'
APK = 'Kobalt-android-arm64-debug.apk'
CERTIFICATE = '874630ab00cefbeb01d39d625b596db4e55e333563d7521478f682cad18466f5'
DEPENDENCY_TARGET = 'android-arm64-v8a-linux-x86_64'


def validate_apk(path, sdk, version, version_code):
    tools = sdk / 'build-tools/36.0.0'
    app_version = version if version.endswith('-debug') else version + '-debug'
    badging = subprocess.check_output([str(tools / 'aapt2'), 'dump', 'badging', str(path)], text=True)
    for expected in ("name='com.nicoverbruggen.kobalt.debug'", f"versionCode='{version_code}'",
                     f"versionName='{app_version}'", "targetSdkVersion:'36'", "native-code: 'arm64-v8a'",
                     'application-debuggable'):
        if expected not in badging:
            raise ValueError('Unexpected Android package metadata; missing ' + expected)
    if not re.search(r"^(?:minSdkVersion|sdkVersion):'28'$", badging, re.MULTILINE):
        raise ValueError('Android minimum SDK must be 28')
    subprocess.run([str(tools / 'zipalign'), '-c', '-P', '16', '4', str(path)], check=True)
    certificates = subprocess.check_output([str(tools / 'apksigner'), 'verify', '--verbose', '--print-certs',
                                            str(path)], text=True)
    signers = re.findall(r'^Signer #[0-9]+ certificate SHA-256 digest: ([0-9a-f]+)$', certificates, re.MULTILINE)
    if signers != [CERTIFICATE]:
        raise ValueError('APK must use only the shared Kobalt debug signing certificate')
    with zipfile.ZipFile(path) as archive:
        libraries = [name for name in archive.namelist() if name.startswith('lib/') and name.endswith('.so')]
        if 'lib/arm64-v8a/libkobalt_android_arm64-v8a.so' not in libraries:
            raise ValueError('APK is missing the reader library')
        for name in libraries:
            data = archive.read(name)
            if (not name.startswith('lib/arm64-v8a/') or len(data) < 64 or data[:6] != b'\x7fELF\x02\x01'
                    or struct.unpack_from('<H', data, 18)[0] != 183):
                raise ValueError('APK library is not ARM64: ' + name)
            offset = struct.unpack_from('<Q', data, 32)[0]
            size, count = struct.unpack_from('<HH', data, 54)
            if size < 56 or not count or offset + size * count > len(data):
                raise ValueError('Invalid ELF program headers: ' + name)
            loads = 0
            for index in range(count):
                header = offset + index * size
                if struct.unpack_from('<I', data, header)[0] != 1:
                    continue
                loads += 1
                file_offset, address = struct.unpack_from('<QQ', data, header + 8)
                alignment = struct.unpack_from('<Q', data, header + 48)[0]
                if alignment < 16384 or file_offset % 16384 != address % 16384:
                    raise ValueError('APK library lacks 16 KiB page alignment: ' + name)
            if not loads:
                raise ValueError('ELF has no loadable segments: ' + name)
    return {'application_id': 'com.nicoverbruggen.kobalt.debug', 'abi': 'arm64-v8a', 'build_type': 'debug',
            'version_code': version_code, 'certificate_sha256': CERTIFICATE, 'sha256': digest(path)}


def in_container(args):
    Path('/gradle').mkdir(exist_ok=True)
    (cache_root(ROOT) / 'artifacts' / DEPENDENCY_TARGET).mkdir(parents=True, exist_ok=True)
    command = ['python3', 'android/build.py', '--offline', '--jobs', str(args.jobs),
               '--version-code', str(args.version_code), '--commit', args.commit]
    if args.modified:
        command.append('--modified')
    start = time.monotonic()
    subprocess.run(command, cwd=ROOT, check=True)
    elapsed = round(time.monotonic() - start, 3)
    path = ROOT / 'build/android/arm64-v8a/app/android-build/kobalt_android.apk'
    record = validate_apk(path, Path(os.environ['ANDROID_HOME']), release_version(ROOT), args.version_code)
    record['build_seconds'] = elapsed
    record['dependencies'] = json.loads((ROOT / 'build/android/arm64-v8a/dependency-timings.json').read_text())
    (ROOT / 'build/android/artifact.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Android debug APK metadata, certificate, ARM64 libraries and 16 KiB alignment verified', flush=True)


def build(args):
    output = ROOT / 'build/release'
    output.mkdir(parents=True, exist_ok=True)
    lock = acquire(output / 'android.lock')
    provenance = checkout(ROOT, args.allow_dirty)
    version = release_version(ROOT)
    engine = detect()
    if native_platform(engine) != 'linux/amd64':
        print('The Android SDK image uses AMD64 emulation on this host. '
              'Use android/build.py with the native Qt and Android SDKs for local builds when available.', flush=True)
    if not args.no_pull:
        subprocess.run([engine, 'pull', '--platform', 'linux/amd64', args.sdk_image], check=True)
    image = json.loads(subprocess.check_output([engine, 'image', 'inspect', args.sdk_image], text=True))[0]
    if image['Architecture'] != 'amd64' or image['Os'] != 'linux':
        raise ValueError('The Android SDK requires a Linux AMD64 image')
    names = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z', '--cached',
                                    *(['--others', '--exclude-standard'] if args.allow_dirty else [])]).decode().split('\0')
    files = {name: safe_source(ROOT, name) for name in names if name and (ROOT / name).exists()}
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    specs.update(json.loads((ROOT / 'android/dependencies.json').read_text()))
    for name, spec in specs.items():
        relative = '.cache/android-dependencies/' + name + '.tar.gz'
        download(spec, ROOT / relative)
        files[relative] = ROOT / relative
    fetch_fonts.fetch(ROOT)
    for item in json.loads((ROOT / 'resources/fonts/ebook-core.json').read_text())['files']:
        name = 'vendor/ebook-fonts/' + item['file']
        files[name] = safe_source(ROOT, name)
    fingerprints = {name: digest(path) for name, path in files.items()}
    gradle = gradle_cache(ROOT)
    dependencies = cache_root(ROOT) / 'artifacts' / DEPENDENCY_TARGET
    dependencies.mkdir(parents=True, exist_ok=True)
    container = 'kobalt-android-' + uuid.uuid4().hex
    command = ['python3', 'tools/ci_android.py', '--in-container', '--jobs', str(args.jobs),
               '--version-code', str(args.version_code), '--commit', provenance['commit'][:12]]
    if provenance['dirty']:
        command.append('--modified')
    subprocess.run([engine, 'create', '--platform', 'linux/amd64', '--name', container, '--workdir', '/work',
                    '--env', 'HOME=/tmp', '--env', 'GRADLE_USER_HOME=/gradle',
                    '--env', 'KOBALT_DEPENDENCY_CACHE=/work/.cache/shared-dependencies',
                    '--env', 'KOBALT_SDK_IMAGE=' + image['Id'],
                    '--env', 'KOBALT_RELEASE_VERSION=' + version, image['Id'], *command], check=True)
    try:
        with tempfile.TemporaryFile() as stream:
            with tarfile.open(fileobj=stream, mode='w') as archive:
                for name, path in sorted(files.items()):
                    archive.add(path, arcname=name, recursive=False)
                    if digest(path) != fingerprints[name]:
                        raise ValueError('Source changed while copying: ' + name)
                archive.add(dependencies, arcname='.cache/shared-dependencies/artifacts/' + DEPENDENCY_TARGET)
            stream.seek(0)
            subprocess.run([engine, 'cp', '-', container + ':/work'], stdin=stream, check=True)
        subprocess.run([engine, 'cp', str(gradle), container + ':/gradle'], check=True)
        subprocess.run([engine, 'start', '--attach', container], check=True)
        code = subprocess.check_output([engine, 'inspect', '--format', '{{.State.ExitCode}}', container], text=True).strip()
        if code != '0':
            raise RuntimeError('Android SDK build exited with status ' + code)
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            temporary = Path(temporary)
            subprocess.run([engine, 'cp', container + ':/work/build/android/artifact.json', str(temporary / 'android.json')], check=True)
            package = temporary / APK
            subprocess.run([engine, 'cp', container + ':/work/build/android/arm64-v8a/app/android-build/kobalt_android.apk',
                            str(package)], check=True)
            record = json.loads((temporary / 'android.json').read_text())
            if record['sha256'] != digest(package):
                raise ValueError('APK changed while copying from the container')
            if (checkout(ROOT, args.allow_dirty) != provenance
                    or any(digest(path) != fingerprints[name] for name, path in files.items())):
                raise ValueError('Checkout changed during the Android build')
            record.update({**provenance, 'target': 'android', 'version': version, 'file': APK,
                           'sdk_image': image['Id'], 'sdk_reference': args.sdk_image})
            (temporary / 'android.json').write_text(json.dumps(record, indent=2) + '\n')
            package.replace(output / APK)
            (temporary / 'android.json').replace(output / 'android.json')
    finally:
        try:
            subprocess.run([engine, 'cp', container + ':/gradle/.', str(gradle)], check=True)
            subprocess.run([engine, 'cp', container + ':/work/.cache/shared-dependencies/artifacts/'
                            + DEPENDENCY_TARGET + '/.', str(dependencies)], check=True)
        finally:
            subprocess.run([engine, 'rm', '--force', container], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--version-code', type=int, required=True)
    parser.add_argument('--sdk-image', default=SDK_IMAGE)
    parser.add_argument('--no-pull', action='store_true', help='use an existing local SDK image for validation')
    parser.add_argument('--allow-dirty', action='store_true', help='local validation only; drafts reject this output')
    parser.add_argument('--in-container', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--commit', default='', help=argparse.SUPPRESS)
    parser.add_argument('--modified', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.jobs < 1 or not 1 <= args.version_code <= 2100000000:
        parser.error('Jobs must be positive and version code must be between 1 and 2100000000')
    if args.in_container:
        in_container(args)
    else:
        build(args)


if __name__ == '__main__':
    main()
