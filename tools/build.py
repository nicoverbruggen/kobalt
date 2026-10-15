#!/usr/bin/env python3
"""Build Kobalt for the selected application target."""
import argparse
import tarfile
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
from build_engine import detect, ensure_toolchain5, ensure_toolchain6, run
from session_lock import acquire
from targets import DEFAULT_TARGET, SUPPORTED_TARGETS, target, output_path, lock_path
import assets
import fetch_fonts
import source_state


ENGINE_PATCHES = tuple((ROOT / 'dependencies/crengine-patches.txt').read_text().splitlines())
LUNASVG_PATCHES = ('lunasvg-resource-limits.patch',)


def fetch(name, spec, cache, offline=False):
    archive = cache / (name + '.tar.gz')
    if not archive.exists():
        if offline:
            raise ValueError('Offline, but there is no cached archive for ' + name)
        with urllib.request.urlopen(spec['url'], timeout=60) as response:
            content = response.read(128 * 1024 * 1024 + 1)
        if len(content) > 128 * 1024 * 1024:
            raise ValueError('Dependency archive exceeds 128 MiB')
        if hashlib.sha256(content).hexdigest() != spec['sha256']:
            raise ValueError('Dependency hash mismatch: ' + name)
        # Publish only complete, verified files. A failed write must remain retryable.
        with tempfile.TemporaryDirectory(prefix='download-', dir=cache) as temporary:
            partial = Path(temporary) / archive.name
            partial.write_bytes(content)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != spec['sha256']:
                raise ValueError('Downloaded dependency hash mismatch: ' + name)
            partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
        raise ValueError('Cached dependency hash mismatch: ' + name + '. Remove ' + str(archive) + ' and retry.')
    destination = cache / spec['directory']
    stamp = destination / '.kobalt-source'
    if stamp.exists() and stamp.read_text() == spec['sha256'] + ':metadata-v1':
        return
    if destination.exists():
        # An existing manual extraction can be used without erasing it only
        # after every regular source file matches the verified archive.
        with tarfile.open(archive) as source:
            for member in source.getmembers():
                path = PurePosixPath(member.name)
                if path.is_absolute() or '..' in path.parts or path.parts[0] != spec['directory']:
                    raise ValueError('Invalid dependency member')
                if member.isfile():
                    target = cache / member.name
                    target.resolve().relative_to(destination.resolve())
                    with source.extractfile(member) as stream:
                        if not target.is_file() or target.read_bytes() != stream.read():
                            raise ValueError('Existing source differs: ' + str(target))
                    target.chmod(member.mode & 0o777)
                    os.utime(target, (member.mtime, member.mtime))
        stamp.write_text(spec['sha256'] + ':metadata-v1')
        return
    with tempfile.TemporaryDirectory(prefix='extract-', dir=cache) as temporary:
        with tarfile.open(archive) as source:
            for member in source.getmembers():
                path = PurePosixPath(member.name)
                if path.is_absolute() or '..' in path.parts or path.parts[0] != spec['directory']:
                    raise ValueError('Invalid dependency member')
                target = Path(temporary) / member.name
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as src, target.open('wb') as dst:
                        shutil.copyfileobj(src, dst)
                    target.chmod(member.mode & 0o777)
                    os.utime(target, (member.mtime, member.mtime))
                # Source-build symlinks and special files are not needed.
        extracted = Path(temporary) / spec['directory']
        (extracted / '.kobalt-source').write_text(spec['sha256'] + ':metadata-v1')
        extracted.rename(destination)


def source_commit():
    """Short commit of the checkout and whether it has uncommitted changes, or ('', False)."""
    try:
        top = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--show-toplevel'],
                             capture_output=True, text=True, check=True).stdout.strip()
        if Path(top).resolve() != ROOT.resolve():
            return '', False
        commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'],
                                capture_output=True, text=True, check=True).stdout.strip()
        changes = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain'],
                                 capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return '', False
    return commit, bool(changes.strip())


def verify_public_library(path):
    """Reject a public binary containing native resource-service entry points."""
    data = path.read_bytes()
    for symbol in (b'retrieveKeys', b'retrieveKey', b'getDeviceKey', b'serveFileFromEpubImpl', b'ReaderServer6create', b'EpubNetworkAccessManager'):
        if symbol in data:
            raise ValueError('Binary contains an unsupported resource service: ' + symbol.decode())


def build_autotools(source, build, prefix, jobs=2, host=None, environment=None,
                    engine=None, container_command=None, make_options=()):
    """Build a pinned Autotools library out of tree as a static dependency."""
    source, build, prefix = (Path(path).resolve() for path in (source, build, prefix))
    build.mkdir(parents=True, exist_ok=True)
    configure = ['sh', str(source / 'configure'), f'--prefix={prefix}', '--disable-shared', '--enable-static', '--with-pic']
    if host:
        configure.append(f'--host={host}')
    env = dict(environment or {})
    if engine is None:
        process_env = os.environ.copy()
        process_env.update(env)
        commands = ((configure, build), (['make', '-j', str(jobs), *make_options], build),
                    (['make', 'install', *make_options], build))
        for command, cwd in commands:
            subprocess.run(command, cwd=cwd, env=process_env, check=True)
        return

    if not container_command:
        raise ValueError('container_command is required for a container build')
    try:
        source_arg = '/work/' + str(source.relative_to(ROOT))
        build_arg = '/work/' + str(build.relative_to(ROOT))
        prefix_arg = '/work/' + str(prefix.relative_to(ROOT))
    except ValueError as error:
        raise ValueError('Autotools paths must be inside the checkout for a container build') from error
    configure[1] = source_arg + '/configure'
    configure[2] = f'--prefix={prefix_arg}'
    command = list(container_command)
    workdir = command.index('-w') + 1
    command[workdir] = build_arg
    env_command = ['env', *(f'{name}={value}' for name, value in env.items())]
    for args in (configure, ['make', '-j', str(jobs), *make_options], ['make', 'install', *make_options]):
        run(engine, command + env_command + args)


def device_dependency_inputs(root, selected, specs, image):
    files = [root / 'tools/build.py', root / 'tools/build_engine.py', root / 'targets' / (selected.name + '.json'),
             root / 'dependencies/crengine-patches.txt']
    files += [root / 'dependencies/patches' / name for name in ENGINE_PATCHES]
    files += [root / 'dependencies/patches' / name for name in LUNASVG_PATCHES]
    return {'target': selected.name, 'image': image, 'root': str(root.resolve()),
            'sources': {name: item['sha256'] for name, item in specs.items() if name != 'fonttools'},
            'recipes': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}


def device_dependencies_match(directory, identity):
    if identity is None:
        return False
    try:
        record = json.loads((directory / 'dependencies.json').read_text())
        if record['identity'] != identity or not isinstance(record['files'], dict) or not record['files']:
            return False
        for name, digest in record['files'].items():
            path = Path(name)
            if path.is_absolute() or '..' in path.parts or hashlib.sha256((directory / path).read_bytes()).hexdigest() != digest:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def record_device_dependencies(directory, identity, engine_library):
    if identity is None:
        return
    files = [path for path in (directory / 'deps').rglob('*') if path.is_file()]
    files += [engine_library, engine_library.parent / 'crsetup.h']
    record = {'identity': identity, 'files': {path.relative_to(directory).as_posix():
              hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (directory / 'dependencies.json').write_text(json.dumps(record, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=SUPPORTED_TARGETS, default=DEFAULT_TARGET, help='application integration (default: qt6)')
    parser.add_argument('--build-target', action='append', default=[], help='build a named CMake target without packaging')
    parser.add_argument('--jobs', type=int, default=2, help='compiler jobs per build (default: 2)')
    parser.add_argument('--page-animations', action=argparse.BooleanOptionalAction, default=True, help='include optional page animations')
    parser.add_argument('--hardware', action='store_true', default=True, help='persistent device package')
    parser.add_argument('--temporary', action='store_true', help='remove on the next Nickel restart, requires --hardware')
    parser.add_argument('--mini', action='store_true', help='keep compatibility assets, Libron and NV Bitter; cap the ZIP at 25 MB')
    parser.add_argument('--in-toolchain', action='store_true', help='run directly inside the SDK at /work, without nested containers')
    parser.add_argument('--version', help='release version override; defaults to the tag or development fallback')
    parser.add_argument('--commit', help='commit to show in About (default: the checkout HEAD, if Git can read it)')
    parser.add_argument('--modified', action='store_true', help='with --commit: the sources had uncommitted changes')
    args = parser.parse_args()
    version = assets.validate_version(args.version) if args.version else assets.release_version()
    selected = target(args.target)
    build_root = Path(selected.build_root)
    if args.in_toolchain and (ROOT != Path('/work') or not Path(selected.toolchain_file).is_file()):
        parser.error('--in-toolchain requires the Kobalt SDK with sources at /work')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.temporary and not args.hardware:
        parser.error('--temporary requires --hardware')
    if args.modified and not args.commit:
        parser.error('--modified requires --commit')
    commit, modified = (args.commit, args.modified) if args.commit else source_commit()
    output = ROOT / output_path(selected, hardware=args.hardware, temporary=args.temporary, mini=args.mini)
    output.mkdir(parents=True, exist_ok=True)
    fetch_fonts.fetch(ROOT)
    # Hash before compiling: an edit made during the build must count as newer.
    sources = source_state.source_state()
    source_state.clear(output)
    build_path = str(output.relative_to(ROOT))
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    cache = ROOT / '.cache/kobalt'
    cache.mkdir(parents=True, exist_ok=True)
    with acquire(lock_path(selected, 'build'), wait=True):
        # Both targets download into the same cache.
        with acquire(ROOT / 'build/fetch.lock', wait=True):
            for name, spec in specs.items():
                if (name != 'qtsensors' or selected.qt_major == 6) and (name != 'fonttools' or args.mini):
                    fetch(name, spec, cache)
        engine = None if args.in_toolchain else detect()
        if engine is not None:
            (ensure_toolchain6 if selected.qt_major == 6 else ensure_toolchain5)(engine, ROOT)
        prefix = '/work/' + str(build_root / 'deps')
        environment = {'PKG_CONFIG_PATH': f'{prefix}/lib/pkgconfig',
                       'PKG_CONFIG_LIBDIR': '/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig' if selected.qt_major == 6 else '/tc/qt5/sysroot/usr/lib/pkgconfig'}
        if args.in_toolchain:
            os.environ.update(environment)
            os.chdir(ROOT)
            command = []
        else:
            command = ['run', '--rm', '--network', 'none', '-v', f'{ROOT}:/work:rw,z', '-w', '/work']
            for key, value in environment.items():
                command.extend(['-e', f'{key}={value}'])
            image = subprocess.check_output([engine, 'image', 'inspect', '--format', '{{.Id}}', selected.toolchain_image], text=True).strip()
            command.append(image)
        common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
                  '-DBUILD_SHARED_LIBS=OFF', f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_INSTALL_LIBDIR=lib',
                  '-DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config', f'-DCMAKE_PREFIX_PATH={prefix}']
        if selected.qt_major == 5:
            common += [f'-DCMAKE_FIND_ROOT_PATH={prefix};/tc/qt5/sysroot', f'-DCMAKE_PREFIX_PATH={prefix};/tc/qt5/sysroot/usr']
        patches = [ROOT / 'dependencies/patches' / name for name in ENGINE_PATCHES]
        revision = hashlib.sha256(specs['crengine']['sha256'].encode()
                                  + b''.join(patch.read_bytes() for patch in patches)).hexdigest()[:16]
        patched_source = ROOT / build_root / ('crengine-source-' + revision)
        source = str(patched_source.relative_to(ROOT))
        engine_build = str(build_root / ('crengine-' + revision))
        dependency_inputs = None if args.in_toolchain else device_dependency_inputs(ROOT, selected, specs, image)
        if device_dependencies_match(ROOT / build_root, dependency_inputs):
            print('Reusing verified device dependencies', flush=True)
        else:
            # Discard configured compilers and objects when their SDK or recipe
            # changes. Installed files must also be repaired after corruption.
            if dependency_inputs is not None:
                directories = ['deps', 'lunasvg', 'utf8proc', 'libjpeg-turbo', 'xxhash', 'qtsensors',
                               specs['freetype']['directory'], specs['harfbuzz']['directory'],
                               'fribidi-autotools', 'libunibreak-autotools', 'crengine-' + revision]
                for name in directories:
                    directory = ROOT / build_root / name
                    if directory.exists():
                        shutil.rmtree(directory)
            # CREngine uses KOReader's text and image callbacks, not stock LunaSVG.
            # Patch a build copy; keep the verified upstream source unchanged.
            extensions = cache / specs['koreader-base']['directory'] / 'thirdparty/lunasvg'
            svg_patches = [extensions / name for name in ('cmake_tweaks.patch', 'extended.patch', 'pkgconfig.patch')]
            svg_patches += [ROOT / 'dependencies/patches' / name for name in LUNASVG_PATCHES]
            svg_revision = hashlib.sha256((specs['lunasvg']['sha256'] + specs['koreader-base']['sha256']).encode()
                                         + b''.join(patch.read_bytes() for patch in svg_patches)).hexdigest()[:16]
            svg_source = ROOT / build_root / ('lunasvg-source-' + svg_revision)
            svg_extensions = '/work/.cache/kobalt/' + specs['koreader-base']['directory'] + '/thirdparty/lunasvg'
            if not svg_source.exists():
                with tempfile.TemporaryDirectory(prefix='lunasvg-', dir=svg_source.parent) as temporary:
                    patched = Path(temporary) / 'source'
                    shutil.copytree(cache / specs['lunasvg']['directory'], patched)
                    for patch in svg_patches:
                        run(engine, command + ['patch', '--batch', '--forward', '-p1',
                                               '-d', '/work/' + str(patched.relative_to(ROOT)),
                                               '-i', '/work/' + str(patch.relative_to(ROOT))])
                    patched.rename(svg_source)
            svg_build = str(build_root / 'lunasvg')
            run(engine, command + ['cmake', '-S', str(svg_source.relative_to(ROOT)), '-B', svg_build, *common,
                                   '-DXTENDED_DIR=' + svg_extensions + '/xtended', '-DLUNASVG_BUILD_EXAMPLES=OFF',
                                   '-DCMAKE_C_VISIBILITY_PRESET=hidden', '-DCMAKE_CXX_VISIBILITY_PRESET=hidden',
                                   '-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON'])
            run(engine, command + ['cmake', '--build', svg_build, '--parallel', str(args.jobs)])
            run(engine, command + ['cmake', '--install', svg_build])
            extra = [('freetype', '', ['-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_BROTLI=ON'])]
            for name, suffix, options in extra + [
                ('utf8proc', '', ['-DUTF8PROC_ENABLE_TESTING=OFF', '-DCMAKE_C_VISIBILITY_PRESET=hidden']),
                ('libjpeg-turbo', '', ['-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON', '-DWITH_TURBOJPEG=OFF',
                                      '-DWITH_TOOLS=OFF', '-DWITH_TESTS=OFF', '-DCMAKE_C_VISIBILITY_PRESET=hidden']),
                ('xxhash', '/cmake_unofficial', ['-DXXHASH_BUILD_XXHSUM=OFF']),
                ('harfbuzz', '', ['-DHB_BUILD_UTILS=OFF', '-DHB_BUILD_SUBSET=OFF', '-DHB_BUILD_RASTER=OFF',
                    '-DHB_BUILD_VECTOR=OFF', '-DHB_BUILD_GPU=OFF', '-DHB_HAVE_FREETYPE=ON', f'-DFREETYPE_LIBRARY={prefix}/lib/libfreetype.a',
                    f'-DFREETYPE_LIBRARY_RELEASE={prefix}/lib/libfreetype.a',
                    f'-DFREETYPE_INCLUDE_DIR_freetype2={prefix}/include/freetype2',
                    f'-DFREETYPE_INCLUDE_DIR_ft2build={prefix}/include/freetype2'])]:
                build = str(build_root / (specs[name]['directory'] if name in ('freetype', 'harfbuzz') else name))
                run(engine, command + ['cmake', '-S', '.cache/kobalt/' + specs[name]['directory'] + suffix,
                                       '-B', build, *common, *options])
                run(engine, command + ['cmake', '--build', build, '--parallel', str(args.jobs)])
                run(engine, command + ['cmake', '--install', build])
            if selected.qt_major == 6:
                sensors = '.cache/kobalt/' + specs['qtsensors']['directory']
                sensors_build = str(build_root / 'qtsensors')
                run(engine, command + ['cmake', '-S', sensors, '-B', sensors_build, '-G', 'Ninja',
                                       '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={prefix}',
                                       '-DCMAKE_INSTALL_LIBDIR=lib', '-DQT_BUILD_EXAMPLES=OFF', '-DQT_BUILD_TESTS=OFF'])
                run(engine, command + ['cmake', '--build', sensors_build, '--parallel', str(args.jobs)])
                run(engine, command + ['cmake', '--install', sensors_build])
            autotools_host = ('arm-kobo-linux-gnueabihf' if selected.qt_major == 6 else 'arm-linux-gnueabihf')
            autotools_env = ({'CC': 'arm-kobo-linux-gnueabihf-gcc', 'AR': 'arm-kobo-linux-gnueabihf-ar',
                              'RANLIB': 'arm-kobo-linux-gnueabihf-ranlib'} if selected.qt_major == 6 else
                             {'CC': '/tc/qt5/bin/gcc', 'AR': '/usr/bin/arm-linux-gnueabihf-ar',
                              'RANLIB': '/usr/bin/arm-linux-gnueabihf-ranlib'})
            for name in ('fribidi', 'libunibreak'):
                # FriBidi's CLI manual requires running the target binary.
                make_options = ('SUBDIRS=lib',) if name == 'fribidi' else ()
                build_autotools(cache / specs[name]['directory'], ROOT / build_root / (name + '-autotools'),
                                ROOT / build_root / 'deps', args.jobs, autotools_host, autotools_env,
                                engine=engine, container_command=command, make_options=make_options)
            # Keep the verified dependency intact. Apply reader-specific resource
            # and image-memory changes only to a build copy.
            if not patched_source.exists():
                with tempfile.TemporaryDirectory(prefix='crengine-', dir=patched_source.parent) as temporary:
                    patched = Path(temporary) / 'source'
                    shutil.copytree(cache / specs['crengine']['directory'], patched)
                    for patch in patches:
                        run(engine, command + ['patch', '--batch', '--forward', '-p1',
                                               '-d', '/work/' + str(patched.relative_to(ROOT)),
                                               '-i', '/work/' + str(patch.relative_to(ROOT))])
                    patched.rename(patched_source)
            run(engine, command + ['cmake', '-S', source, '-B', engine_build, *common,
                                   '-DAUTO=OFF', '-DFREETYPE=ON', '-DHARFBUZZ=ON', '-DLIBPNG=ON', '-DLIBJPEG=ON', '-DGIF=ON',
                                   '-DLUNASVG=ON', '-DZLIB=ON', '-DUTF8PROC=ON', '-DFRIBIDI=ON',
                                   '-DLIBUNIBREAK=ON', '-DMATHML=ON',
                                   '-U__pkg_config_checked_FREETYPE', '-UFREETYPE_*'])
            run(engine, command + ['cmake', '--build', engine_build, '--parallel', str(args.jobs)])
            record_device_dependencies(ROOT / build_root, dependency_inputs, ROOT / engine_build / 'libcrengine.a')
        run(engine, command + ['cmake', '-S', '.',
                               '-B', build_path, '-G', 'Ninja',
                               f'-DKOBALT_TARGET={selected.name}', f'-DKOBALT_VERSION={version}',
                               '-DKOBALT_READER_HARDWARE=' + ('ON' if args.hardware else 'OFF'),
                               '-DKOBALT_READER_TEMPORARY=' + ('ON' if args.temporary else 'OFF'),
                               '-DKOBALT_PAGE_ANIMATIONS=' + ('ON' if args.page_animations else 'OFF'),
                               '-DKOBALT_MINI=' + ('ON' if args.mini else 'OFF'),
                               f'-DKOBALT_COMMIT={commit}', '-DKOBALT_COMMIT_MODIFIED=' + ('ON' if modified else 'OFF'),
                               '-DCMAKE_BUILD_TYPE=Release', f'-DCRENGINE_SOURCE=/work/{source}', f'-DCRENGINE_BUILD=/work/{engine_build}',
                               f'-DQt{selected.qt_major}Sensors_DIR=' + (f'{prefix}/lib/cmake/Qt6Sensors' if selected.qt_major == 6 else '/tc/qt5/sysroot/usr/lib/cmake/Qt5Sensors')])
        run(engine, command + ['cmake', '--build', build_path, '--parallel', str(args.jobs)] + (['--target', *args.build_target] if args.build_target else []))

        if not args.build_target:
            verify_public_library(output / 'libkobalt.so')
            # Strip only the packaged copy; keep the original for crash symbols.
            library = output / 'libkobalt-packaged.so'
            shutil.copy2(output / 'libkobalt.so', library)
            run(engine, command + ['arm-linux-gnueabihf-strip', '--strip-unneeded',
                                  '/work/' + str(library.relative_to(ROOT))])
            with tarfile.open(output / selected.package_name, 'w:gz') as archive:
                info = archive.gettarinfo(str(library), arcname=selected.package_member)
                info.uid = info.gid = 0
                info.uname = info.gname = 'root'
                info.mode = 0o755
                with library.open('rb') as stream:
                    archive.addfile(info, stream)
            if assets.manifest(version=version, mini=args.mini) != (output / 'asset-manifest.json').read_bytes():
                raise RuntimeError('Assets changed during the build; rebuild before packaging')
            install = output / 'install'
            if install.exists():
                shutil.rmtree(install)
            staged = assets.stage(output, version=version, mini=args.mini)
            assets.verify_assets(staged, (output / 'asset-manifest.json').read_bytes())
            (install / '.kobo').mkdir()
            shutil.copy2(output / selected.package_name, install / '.kobo' / selected.package_name)
            with zipfile.ZipFile(output / 'Kobalt.zip', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(install.rglob('*')):
                    if path.is_file():
                        archive.write(path, path.relative_to(install).as_posix())
            assets.check_package_size(output / 'Kobalt.zip', mini=args.mini)
            source_state.record(output, sources)
            print(output / selected.package_name)
            print(output / 'Kobalt.zip')


if __name__ == '__main__':
    main()
