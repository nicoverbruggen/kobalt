#!/usr/bin/env python3
"""Build the standalone Android reader from the public source checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import inspect
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build import ENGINE_PATCHES, LUNASVG_PATCHES, build_autotools, source_commit, verify_public_library
import assets
from sign import sign_interactively
from session_lock import acquire
from dependency_cache import DependencyCache, cache_root, stage_cmake, gradle_cache, fetch_source


def version_code(value):
    try:
        code = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError('Version code must be an integer') from None
    if not 1 <= code <= 2100000000:
        raise argparse.ArgumentTypeError('Version code must be between 1 and 2100000000')
    return code


def patched(source, patches, destination):
    digest = hashlib.sha256((source / '.kobalt-source').read_bytes() + b''.join(p.read_bytes() for p in patches)).hexdigest()[:16]
    target = destination / (source.name + '-' + digest)
    if not target.exists():
        with tempfile.TemporaryDirectory(dir=destination) as temporary:
            copy = Path(temporary) / 'source'
            shutil.copytree(source, copy)
            for patch in patches:
                subprocess.run(['patch', '--batch', '--forward', '-p1', '-d', str(copy), '-i', str(patch)], check=True)
            copy.rename(target)
    return target


def build_openssl(source, directory, ndk, abi, jobs):
    # Qt loads suffixed libraries so Android's system TLS cannot shadow them.
    libraries = [directory / name for name in ('libcrypto_3.so', 'libssl_3.so')]
    for path in libraries:
        path.unlink(missing_ok=True)
    if not all(path.is_file() for path in libraries):
        directory.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        environment['ANDROID_NDK_ROOT'] = str(ndk)
        toolchain = ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
        if not toolchain.is_dir(): toolchain = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
        environment['PATH'] = str(toolchain) + os.pathsep + environment['PATH']
        architecture = 'android-arm64' if abi == 'arm64-v8a' else 'android-x86_64'
        subprocess.run(['perl', str(source / 'Configure'), architecture, 'shared', 'no-tests', 'no-apps',
                        '-U__ANDROID_API__', '-D__ANDROID_API__=28', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384'],
                       cwd=directory, env=environment, check=True)
        subprocess.run(['make', '-j' + str(jobs), 'build_generated'], cwd=directory, env=environment, check=True)
        subprocess.run(['make', '-j' + str(jobs), 'SHLIB_VERSION_NUMBER=', 'libcrypto_3.so', 'libssl_3.so'],
                       cwd=directory, env=environment, check=True)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--abi', choices=['arm64-v8a', 'x86_64'], default='arm64-v8a')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--offline', action='store_true', help='require cached dependency archives')
    parser.add_argument('--native-only', action='store_true', help='compile the reader without packaging an APK')
    parser.add_argument('--release', action='store_true', help='build a release APK; unsigned unless --keystore is supplied')
    parser.add_argument('--version-code', type=version_code, default=1, help='Android update number; increase it for each distributed release')
    parser.add_argument('--commit', help='source commit recorded by the container build helper')
    parser.add_argument('--modified', action='store_true', help='mark a copied source snapshot as modified')
    parser.add_argument('--keystore', type=Path, help='sign the release APK with this external PKCS#12 file')
    parser.add_argument('--key-alias', default='kobalt-release', help='key alias inside the keystore')
    args = parser.parse_args()
    if args.keystore:
        if not args.release or args.native_only:
            parser.error('--keystore requires --release and APK packaging')
        args.keystore = args.keystore.expanduser().resolve()
        if not args.keystore.is_file():
            parser.error('Keystore does not exist: ' + str(args.keystore))
        if not sys.stdin.isatty():
            parser.error('Run signing in your terminal so the passphrase can be entered without echo')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    required = ['ANDROID_NDK_ROOT', 'ANDROID_HOME', 'QT_HOST_PATH',
                'QT_ANDROID_ARM64_PATH' if args.abi == 'arm64-v8a' else 'QT_ANDROID_PATH']
    for name in required:
        if not os.environ.get(name):
            parser.error('Set the toolchain paths described in android/README.md; missing ' + name)
    ndk = Path(os.environ['ANDROID_NDK_ROOT'])
    qt = Path(os.environ['QT_ANDROID_ARM64_PATH'] if args.abi == 'arm64-v8a' else os.environ['QT_ANDROID_PATH'])
    if args.abi == 'x86_64' and qt.name != 'android_x86_64':
        parser.error('Install the Qt android_x86_64 kit before selecting x86_64')
    output = ROOT / 'build/android' / (args.abi + ('-release' if args.release else ''))
    prefix = output / 'deps'
    cache = ROOT / '.cache/android-dependencies'
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    if 'GRADLE_USER_HOME' not in os.environ:
        os.environ['GRADLE_USER_HOME'] = str(gradle_cache(ROOT))
    os.environ.setdefault('ANDROID_USER_HOME', str(output.parent / 'user-home'))
    lock = acquire(output.parent / 'build.lock')
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    specs.update(json.loads((ROOT / 'android/dependencies.json').read_text()))
    names = ['crengine', 'koreader-base', 'lunasvg', 'libpng', 'freetype', 'utf8proc', 'libjpeg-turbo', 'xxhash', 'harfbuzz',
             'fribidi', 'libunibreak', 'openssl']
    for name in names:
        # Reuse verified archives, never another target's writable build tree.
        archive = ROOT / '.cache/kobalt' / (name + '.tar.gz')
        if archive.exists() and not (cache / archive.name).exists():
            shutil.copy2(archive, cache / archive.name)
        fetch_source(name, specs[name], cache, cache_root(ROOT) / 'sources', args.offline)
    source = {name: cache / specs[name]['directory'] for name in names}
    artifacts = DependencyCache(cache_root(ROOT), 'android-' + args.abi + '-' + sys.platform + '-' + platform.machine())
    prebuilt = ndk / 'toolchains/llvm/prebuilt'
    host_tags = (('darwin-x86_64', 'darwin-arm64') if sys.platform == 'darwin' else
                 ('linux-x86_64', 'linux-aarch64'))
    host_tag = next((tag for tag in host_tags if (prebuilt / tag / 'bin').is_dir()), None)
    if host_tag is None:
        parser.error('Android NDK has no compiler tools for this host: ' + str(prebuilt))
    ndk_bin = prebuilt / host_tag / 'bin'
    toolchain = {'ndk': (ndk / 'source.properties').read_text(), 'abi': args.abi,
                 'compiler': hashlib.sha256((ndk_bin / 'clang').read_bytes()).hexdigest(),
                 'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0],
                 'environment': {name: os.environ.get(name, '') for name in
                                 ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS')},
                 'image': os.environ.get('KOBALT_SDK_IMAGE', '')}
    keys = {}
    env = os.environ.copy()
    env['PKG_CONFIG_LIBDIR'] = str(prefix / 'lib/pkgconfig')
    env['PKG_CONFIG_PATH'] = ''
    env.pop('PKG_CONFIG_SYSROOT_DIR', None)
    if not shutil.which('pkg-config'):
        parser.error('Install pkg-config with your package manager')
    pc = prefix / 'lib/pkgconfig'
    pc.mkdir(parents=True, exist_ok=True)
    # zlib belongs to the Android platform. The compiler supplies its headers
    # and library; never let pkg-config find a macOS or Linux host library.
    zlib_pc = 'Name: zlib\nDescription: Android platform zlib\nVersion: 1.2.13\nLibs: -lz\n'
    if not (pc / 'zlib.pc').exists() or (pc / 'zlib.pc').read_text() != zlib_pc:
        (pc / 'zlib.pc').write_text(zlib_pc)
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
              '-DBUILD_SHARED_LIBS=OFF', '-DCMAKE_INSTALL_LIBDIR=lib', f'-DCMAKE_INSTALL_PREFIX={prefix}',
              f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake',
              f'-DANDROID_ABI={args.abi}', '-DANDROID_PLATFORM=android-28',
              '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
              f'-DCMAKE_PREFIX_PATH={prefix}']

    def run(*command):
        subprocess.run([str(c) for c in command], env=env, check=True)

    def build(name, src, *options, install=True, dependencies=(), patches=()):
        inputs = {'toolchain': toolchain, 'source': specs[name]['sha256'],
                  'patches': [hashlib.sha256(path.read_bytes()).hexdigest() for path in patches],
                  'dependencies': {name: keys[name] for name in dependencies},
                  'options': [str(option).replace(str(ROOT), '$ROOT').replace(str(ndk), '$NDK')
                              for option in [*common, *options]],
                  'recipe': inspect.getsource(build), 'installer': inspect.getsource(stage_cmake)}
        def compile(files, key):
            directory = output / (name + '-' + key[:16])
            run('cmake', '-S', src, '-B', directory, *common, *options)
            run('cmake', '--build', directory, '--parallel', args.jobs)
            if install:
                stage_cmake(directory, prefix, files, env)
            else:
                destination = files / directory.name
                destination.mkdir()
                for filename in ('libcrengine.a', 'crsetup.h'):
                    shutil.copy2(directory / filename, destination / filename)
        keys[name] = artifacts.build(name, inputs, output, compile)
        return output / (name + '-' + keys[name][:16])

    build('libpng', source['libpng'], '-DPNG_SHARED=OFF', '-DPNG_TESTS=OFF', '-DPNG_TOOLS=OFF')
    build('freetype', source['freetype'], '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_BROTLI=ON', dependencies=('libpng',))
    build('utf8proc', source['utf8proc'], '-DUTF8PROC_ENABLE_TESTING=OFF')
    build('libjpeg-turbo', source['libjpeg-turbo'], '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON', '-DWITH_TURBOJPEG=OFF', '-DWITH_TOOLS=OFF', '-DWITH_TESTS=OFF')
    build('xxhash', source['xxhash'] / 'cmake_unofficial', '-DXXHASH_BUILD_XXHSUM=OFF')
    build('harfbuzz', source['harfbuzz'], '-DHB_BUILD_UTILS=OFF', '-DHB_BUILD_SUBSET=OFF', '-DHB_BUILD_RASTER=OFF',
        '-DHB_BUILD_VECTOR=OFF', '-DHB_BUILD_GPU=OFF', '-DHB_HAVE_FREETYPE=ON', dependencies=('freetype',))
    host_triple = 'aarch64-linux-android' if args.abi == 'arm64-v8a' else 'x86_64-linux-android'
    api_compiler = ndk_bin / f'{host_triple}28-clang'
    if not api_compiler.is_file():
        parser.error('Android NDK is missing the compiler: ' + str(api_compiler))
    autotools_environment = dict(env)
    autotools_environment.update({'CC': str(api_compiler), 'AR': str(ndk_bin / 'llvm-ar'),
                                  'RANLIB': str(ndk_bin / 'llvm-ranlib')})
    for name in ('fribidi', 'libunibreak'):
        def compile_autotools(files, key):
            directory = output / (name + '-' + key[:16])
            directory.mkdir(exist_ok=True)
            environment = dict(autotools_environment, DESTDIR=str(files / 'stage'))
            make_options = ('SUBDIRS=lib',) if name == 'fribidi' else ()
            build_autotools(source[name], directory, prefix, args.jobs,
                            host=host_triple, environment=environment, make_options=make_options)
            shutil.move(str(files / 'stage' / str(prefix).lstrip('/')), files / 'deps')
            shutil.rmtree(files / 'stage')
        keys[name] = artifacts.build(name, {'toolchain': toolchain, 'source': specs[name]['sha256'],
                                          'host': host_triple, 'recipe': inspect.getsource(build_autotools)
                                          + inspect.getsource(compile_autotools)}, output, compile_autotools)
    extensions = source['koreader-base'] / 'thirdparty/lunasvg'
    svg_patches = [extensions / name for name in ('cmake_tweaks.patch', 'extended.patch', 'pkgconfig.patch')]
    svg_patches += [ROOT / 'dependencies/patches' / name for name in LUNASVG_PATCHES]
    svg = patched(source['lunasvg'], svg_patches, output)
    build('lunasvg', svg, f'-DXTENDED_DIR={extensions}/xtended', '-DLUNASVG_BUILD_EXAMPLES=OFF',
          patches=svg_patches + sorted((extensions / 'xtended').rglob('*.cpp')) + sorted((extensions / 'xtended').rglob('*.h')))
    engine = patched(source['crengine'], [ROOT / 'dependencies/patches' / p for p in ENGINE_PATCHES], output)
    # A CMake build directory belongs to one source directory. A changed patch
    # set gets a new patched source, so it also needs a new build directory.
    engine_build = build('crengine', engine, '-DAUTO=OFF', '-DFREETYPE=ON', '-DHARFBUZZ=ON', '-DLIBPNG=ON',
                         '-DLIBJPEG=ON', '-DGIF=ON', '-DLUNASVG=ON', '-DZLIB=ON', '-DUTF8PROC=ON', '-DFRIBIDI=ON',
                         '-DLIBUNIBREAK=ON', '-DMATHML=ON', install=False,
                         dependencies=tuple(keys), patches=[ROOT / 'dependencies/patches' / p for p in ENGINE_PATCHES])
    assets.fetch_fonts.fetch(ROOT, offline=args.offline)
    manifest = output / 'asset-manifest.json'
    app_version = assets.release_version()
    if not args.release and not app_version.endswith('-debug'):
        app_version += '-debug'
    manifest_data = assets.manifest(version=app_version)
    if not manifest.is_file() or manifest.read_bytes() != manifest_data:
        manifest.write_bytes(manifest_data)
    ssl_source = patched(source['openssl'], [ROOT / 'dependencies/patches/openssl-android-libraries.patch'], output)
    def compile_openssl(files, key):
        directory = output / ('openssl-' + key[:16])
        build_openssl(ssl_source, directory, ndk, args.abi, args.jobs)
        destination = files / directory.name
        destination.mkdir()
        for filename in ('libcrypto_3.so', 'libssl_3.so'):
            shutil.copy2(directory / filename, destination / filename)
    ssl_key = artifacts.build('openssl', {'toolchain': toolchain,
                             'source': specs['openssl']['sha256'],
                             'patch': hashlib.sha256((ROOT / 'dependencies/patches/openssl-android-libraries.patch').read_bytes()).hexdigest(),
                             'recipe': inspect.getsource(build_openssl)}, output, compile_openssl)
    openssl = output / ('openssl-' + ssl_key[:16])
    (output / 'dependency-timings.json').write_text(json.dumps(artifacts.events, indent=2) + '\n')
    commit, modified = (args.commit, args.modified) if args.commit is not None else source_commit()
    run(qt / 'bin/qt-cmake', '-S', ROOT / 'android', '-B', output / 'app', '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=' + ('Release' if args.release else 'Debug'), f'-DQT_HOST_PATH={os.environ["QT_HOST_PATH"]}',
        f'-DANDROID_SDK_ROOT={os.environ["ANDROID_HOME"]}', f'-DANDROID_NDK_ROOT={ndk}',
        '-DQT_ANDROID_SDK_BUILD_TOOLS_REVISION=36.0.0', '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON',
        f'-DCRENGINE_SOURCE={engine}', f'-DCRENGINE_BUILD={engine_build}', f'-DDEPENDENCY_PREFIX={prefix}',
        f'-DANDROID_OPENSSL_DIR={openssl}', f'-DASSET_MANIFEST={manifest}', f'-DKOBALT_VERSION={app_version}',
        f'-DKOBALT_ANDROID_VERSION_CODE={args.version_code}',
        f'-DKOBALT_COMMIT={commit}', '-DKOBALT_COMMIT_MODIFIED=' + ('ON' if modified else 'OFF'))
    run('cmake', '--build', output / 'app', '--target', 'kobalt_android', '--parallel', args.jobs)
    verify_public_library(output / 'app' / ('libkobalt_android_' + args.abi + '.so'))
    if not args.native_only:
        run('cmake', '--build', output / 'app', '--target', 'apk', '--parallel', args.jobs)
        if args.keystore:
            sign_interactively(output / 'app/android-build/kobalt_android.apk', args.keystore, args.key_alias)


if __name__ == '__main__':
    main()
