#!/usr/bin/env python3
"""Build and run the host tests in tests/ against a Qt 6 SDK for this machine.

Without --no-engine, this also builds CREngine and its pinned dependencies for
the host, so the tests can open real books.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build import ENGINE_PATCHES, LUNASVG_PATCHES, build_autotools, fetch
import fetch_fonts
from session_lock import acquire

HOST = ('macos' if platform.system() == 'Darwin' else 'linux') + '-' + platform.machine()
ENGINE_DEPENDENCIES = ('crengine', 'koreader-base', 'lunasvg', 'freetype', 'utf8proc', 'libjpeg-turbo', 'xxhash', 'harfbuzz',
                       'fribidi', 'libunibreak')



def patched(source, patches, output, suffix=''):
    """A patched copy of a verified source tree. The verified cache stays untouched."""
    digest = hashlib.sha256((source / '.kobalt-source').read_bytes() + b''.join(p.read_bytes() for p in patches)
                            + suffix.encode()).hexdigest()[:16]
    destination = output / (source.name + '-' + digest)
    if not destination.exists():
        with tempfile.TemporaryDirectory(prefix='source-', dir=output) as temporary:
            copy = Path(temporary) / 'source'
            shutil.copytree(source, copy)
            for patch in patches:
                subprocess.run(['patch', '--batch', '--forward', '-p1', '-d', str(copy), '-i', str(patch)], check=True)
            if suffix:
                with (copy / 'CMakeLists.txt').open('a') as file:
                    file.write(suffix)
            copy.rename(destination)
    return destination


def bundled_png(qt):
    """The libpng inside the Qt SDK, so the engine and Qt decode with the same library."""
    library = qt / 'lib/libQt6BundledLibpng.a'
    header = qt / 'include/QtPng/png.h'
    if not library.is_file() or not header.is_file():
        raise SystemExit('The Qt SDK must be built with its bundled libpng (-DINPUT_libpng=qt). See docs/development.md.')
    match = re.search(r'^#define PNG_LIBPNG_VER_STRING "([^"]+)"', header.read_text(), re.MULTILINE)
    if not match:
        raise SystemExit('Cannot read the libpng version from ' + str(header))
    return library, header.parent, match.group(1)


DEFAULT_CACHE = ROOT / '.cache/kobalt'


def dependency_identity(qt, specs, text_only):
    """Inputs that can change a host dependency's ABI or generated code."""
    files = [Path(__file__).resolve(), ROOT / 'tools/build.py', ROOT / 'dependencies/crengine-patches.txt']
    if not text_only:
        files += [ROOT / 'dependencies/patches' / name for name in ENGINE_PATCHES]
        files += [ROOT / 'dependencies/patches' / name for name in LUNASVG_PATCHES]
        png, header, _ = bundled_png(qt)
        files += [png, header / 'png.h']
        files += [qt / 'lib/cmake/Qt6/Qt6Config.cmake']
    environment = {name: os.environ.get(name, '') for name in
                   ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'SDKROOT',
                    'MACOSX_DEPLOYMENT_TARGET', 'CMAKE_TOOLCHAIN_FILE')}
    tools = {}
    for name in ('cc', 'c++', 'cmake', 'ninja', 'make', 'pkg-config'):
        variable = {'cc': 'CC', 'c++': 'CXX'}.get(name)
        command = shlex.split(os.environ.get(variable, '') or name) if variable else [name]
        executable = shutil.which(command[0])
        if not executable:
            raise ValueError('Required dependency build tool missing: ' + command[0])
        command[0] = executable
        tools[name] = [command, subprocess.check_output([*command, '--version'], text=True).splitlines()[0]]
    return {'qt': str(qt), 'host': platform.platform(), 'text_only': text_only,
            'sources': specs, 'environment': environment, 'tools': tools,
            'files': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}


def cached_dependencies(output, identity):
    """Reuse only a complete build whose installed files still match their hashes."""
    try:
        record = json.loads((output / 'dependencies.json').read_text())
        if not isinstance(record, dict) or not isinstance(record.get('files'), dict) or not isinstance(record.get('options'), list):
            return None
        if record['identity'] != identity or not record['files']:
            return None
        expected_mode = '-DKOBALT_TEST_ENGINE=' + ('OFF' if identity.get('text_only') else 'ON')
        if not record['options'] or record['options'][0] != expected_mode or not all(isinstance(option, str) for option in record['options']):
            return None
        for name, expected in record['files'].items():
            path = Path(name)
            if path.is_absolute() or '..' in path.parts:
                return None
            if hashlib.sha256((output / path).read_bytes()).hexdigest() != expected:
                return None
        return record['options']
    except (OSError, ValueError, KeyError, TypeError):
        return None


def record_dependencies(output, identity, options, extra=()):
    files = [path for path in (output / 'deps').rglob('*') if path.is_file()]
    files += list(extra)
    record = {'identity': identity, 'options': options,
              'files': {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in files}}
    (output / 'dependencies.json').write_text(json.dumps(record, indent=2) + '\n')
    return options


def fetch_sources(names, specs, cache=DEFAULT_CACHE, offline=False):
    """Verified, extracted sources. The default cache is the one tools/build.py uses."""
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    # tools/build.py fetches into the default cache under the same lock.
    lock = ROOT / 'build/fetch.lock' if cache == DEFAULT_CACHE else cache / 'fetch.lock'
    lock.parent.mkdir(parents=True, exist_ok=True)
    with acquire(lock, wait=True):
        # Name every missing archive at once. fetch() refuses each one too.
        if offline:
            missing = [name for name in names if not (cache / (name + '.tar.gz')).is_file()]
            if missing:
                raise SystemExit(f'Offline, but {cache} has no archive for: ' + ', '.join(missing))
        for name in names:
            fetch(name, specs[name], cache, offline)
    return {name: cache / specs[name]['directory'] for name in names}


def build_qt(output, jobs, cache=DEFAULT_CACHE, offline=False):
    """Build the pinned Qt Base and Qt Sensors for the host, without the Kobo patches."""
    install = output / 'qt'
    if (install / 'lib/cmake/Qt6Sensors/Qt6SensorsConfig.cmake').is_file():
        return install
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    specs['qtbase'] = json.loads((ROOT / 'toolchains/qt6/source/source.json').read_text())
    sources = fetch_sources(('qtbase', 'qtsensors'), specs, cache, offline)
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={install}',
              '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF']
    # Only the modules the tests use. Bundled third-party libraries keep the
    # SDK independent of system packages, and the tests need its libpng.
    base = ['-DFEATURE_openssl=OFF', '-DFEATURE_printsupport=OFF', '-DFEATURE_dbus=OFF', '-DFEATURE_glib=OFF',
            '-DINPUT_opengl=no', '-DFEATURE_opengl=OFF', '-DFEATURE_sql=OFF',
            '-DINPUT_pcre=qt', '-DINPUT_freetype=qt', '-DINPUT_harfbuzz=qt', '-DINPUT_libjpeg=qt', '-DINPUT_libpng=qt']
    if platform.system() == 'Darwin':
        # Qt 6.5.2 needs the macOS 14 standard library. The math.h pre-include
        # stops the bundled libpng from including the obsolete fp.h.
        common += ['-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', f'-DCMAKE_OSX_ARCHITECTURES={platform.machine()}']
        base += ['-DFEATURE_framework=OFF', '-DFEATURE_securetransport=ON', '-DFEATURE_opengl_desktop=OFF',
                 '-DCMAKE_C_FLAGS=-include math.h']
    else:
        base += ['-DFEATURE_xcb=OFF', '-DFEATURE_wayland=OFF']
    for name, options in (('qtbase', base), ('qtsensors', [f'-DCMAKE_PREFIX_PATH={install}'])):
        directory = output / ('qt-build-' + name)
        subprocess.run(['cmake', '-S', str(sources[name]), '-B', str(directory), *common, *options], check=True)
        subprocess.run(['cmake', '--build', str(directory), '--parallel', str(jobs)], check=True)
        subprocess.run(['cmake', '--install', str(directory)], check=True)
    return install


def build_engine(qt, output, jobs, cache=DEFAULT_CACHE, offline=False, text_only=False):
    """Build the pinned text dependencies and optionally CREngine for the host."""
    prefix = output / 'deps'
    specs = json.loads((ROOT / 'dependencies/manifest.json').read_text())
    names = ('freetype', 'harfbuzz', 'fribidi') if text_only else ENGINE_DEPENDENCIES
    sources = fetch_sources(names, specs, cache, offline)
    identity = dependency_identity(qt, {name: specs[name] for name in names}, text_only)
    identity['output'] = str(output.resolve())
    identity['source_directories'] = {name: str(path) for name, path in sources.items()}
    cached = cached_dependencies(output, identity)
    if cached is not None:
        print('Reusing verified host dependencies', flush=True)
        return cached
    environment = os.environ.copy()
    environment['PKG_CONFIG_PATH'] = environment['PKG_CONFIG_LIBDIR'] = str(prefix / 'lib/pkgconfig')
    environment.pop('PKG_CONFIG_SYSROOT_DIR', None)
    if platform.system() == 'Darwin':
        environment['MACOSX_DEPLOYMENT_TARGET'] = '14.0'
    pkgconfig = prefix / 'lib/pkgconfig'
    pkgconfig.mkdir(parents=True, exist_ok=True)
    png, png_include, png_version = bundled_png(qt) if not text_only else (None, None, None)
    (pkgconfig / 'zlib.pc').write_text('Name: zlib\nDescription: Host zlib\nVersion: 1\nLibs: -lz\n')
    if png:
        (pkgconfig / 'libpng.pc').write_text(f'Name: libpng\nDescription: PNG from the Qt SDK\nVersion: {png_version}\n'
                                             f'Libs: {png} -lz\nCflags: -I{png_include}\n')
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
              '-DBUILD_SHARED_LIBS=OFF', '-DCMAKE_INSTALL_LIBDIR=lib', f'-DCMAKE_INSTALL_PREFIX={prefix}',
              f'-DCMAKE_PREFIX_PATH={prefix}', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5']
    if platform.system() == 'Darwin':
        common += ['-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', f'-DCMAKE_OSX_ARCHITECTURES={platform.machine()}']

    def dependency(name, source, *options, install=True):
        directory = output / name
        # CMake reads compiler environment flags only on first configuration.
        # A cache miss must discard both that configuration and its objects.
        if directory.exists():
            shutil.rmtree(directory)
        for command in (['cmake', '-S', source, '-B', directory, *common, *options],
                        ['cmake', '--build', directory, '--parallel', jobs],
                        ['cmake', '--install', directory] if install else None):
            if command:
                subprocess.run([str(part) for part in command], env=environment, check=True)
        return directory

    def autotools(name, source, make_options=()):
        directory = output / name
        # Make does not rebuild objects just because compiler flags changed.
        if directory.exists():
            shutil.rmtree(directory)
        build_autotools(source, directory, prefix, jobs, environment=environment, make_options=make_options)

    png_options = ['-DFT_DISABLE_PNG=ON'] if text_only else ['-DFT_DISABLE_PNG=OFF', f'-DPNG_LIBRARY={png}', f'-DPNG_PNG_INCLUDE_DIR={png_include}']
    # Keep the portable font set independent of Apple's optional HVF library.
    dependency('freetype-' + sources['freetype'].name, sources['freetype'], '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BZIP2=ON',
               '-DFT_DISABLE_BROTLI=ON', '-DFT_DISABLE_HVF=ON', *png_options)
    dependency('harfbuzz-' + sources['harfbuzz'].name, sources['harfbuzz'], '-DHB_BUILD_UTILS=OFF', '-DHB_BUILD_SUBSET=OFF', '-DHB_BUILD_RASTER=OFF',
        '-DHB_BUILD_VECTOR=OFF', '-DHB_BUILD_GPU=OFF', '-DHB_HAVE_FREETYPE=ON')
    autotools('fribidi-autotools', sources['fribidi'], make_options=('SUBDIRS=lib',))
    if text_only:
        return record_dependencies(output, identity,
                                   ['-DKOBALT_TEST_ENGINE=OFF', f'-DKOBALT_TEST_DEPENDENCIES={prefix}', '-DKOBALT_TEST_PNG='])
    dependency('utf8proc', sources['utf8proc'], '-DUTF8PROC_ENABLE_TESTING=OFF')
    dependency('jpeg', sources['libjpeg-turbo'], '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON',
               '-DWITH_TURBOJPEG=OFF', '-DWITH_TOOLS=OFF', '-DWITH_TESTS=OFF')
    dependency('xxhash', sources['xxhash'] / 'cmake_unofficial', '-DXXHASH_BUILD_XXHSUM=OFF')
    autotools('libunibreak-autotools', sources['libunibreak'])
    extensions = sources['koreader-base'] / 'thirdparty/lunasvg'
    svg_patches = [extensions / name for name in ('cmake_tweaks.patch', 'extended.patch', 'pkgconfig.patch')]
    svg_patches += [ROOT / 'dependencies/patches' / name for name in LUNASVG_PATCHES]
    svg = patched(sources['lunasvg'], svg_patches, output)
    dependency('lunasvg-build-' + svg.name.rsplit('-', 1)[-1], svg, f'-DXTENDED_DIR={extensions / "xtended"}',
               '-DLUNASVG_BUILD_EXAMPLES=OFF')
    # The device build renames CREngine's FreeType glyph calls with objcopy,
    # which Mach-O archives do not support. Rename them at compilation instead,
    # so font_weight.cpp still reaches the original FreeType functions.
    definitions = ('\ntarget_compile_definitions(crengine PRIVATE '
                   'FT_Load_Glyph=kobalt_font_load_glyph FT_Render_Glyph=kobalt_font_render_glyph)\n')
    engine = patched(sources['crengine'], [ROOT / 'dependencies/patches' / name for name in ENGINE_PATCHES], output, definitions)
    engine_build = dependency('crengine-build-' + engine.name.rsplit('-', 1)[-1], engine, '-DAUTO=OFF', '-DFREETYPE=ON',
                              '-DHARFBUZZ=ON', '-DLIBPNG=ON', '-DLIBJPEG=ON', '-DGIF=ON', '-DLUNASVG=ON', '-DZLIB=ON', '-DUTF8PROC=ON',
                              '-DFRIBIDI=ON', '-DLIBUNIBREAK=ON', '-DMATHML=ON',
                              install=False)
    options = ['-DKOBALT_TEST_ENGINE=ON', f'-DCRENGINE_SOURCE={engine}', f'-DCRENGINE_BUILD={engine_build}',
            f'-DKOBALT_TEST_DEPENDENCIES={prefix}', f'-DKOBALT_TEST_PNG={png}', f'-DKOBALT_TEST_PNG_INCLUDE={png_include}',
            f'-DKOBALT_TEST_VARIABLE_FONT_SOURCE={sources["harfbuzz"] / "test/subset/data/fonts/Fraunces.ttf"}']
    return record_dependencies(output, identity, options, [engine_build / 'libcrengine.a', engine / '.kobalt-source'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt', type=Path, help='an existing Qt 6.5 SDK for this machine, built with its bundled '
                        'libpng; without it, the pinned Qt is built once into the output directory')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests' / HOST, help='build directory')
    parser.add_argument('--jobs', type=int, default=2, help='parallel compiler jobs (default: 2)')
    parser.add_argument('--test-jobs', type=int, default=min(8, os.cpu_count() or 2), help='parallel tests (default: up to 8 host CPUs)')
    parser.add_argument('--offline', action='store_true', help='use cached fonts and source archives only')
    parser.add_argument('--cache', type=Path, default=DEFAULT_CACHE,
                        help='directory for verified source archives (default: .cache/kobalt)')
    parser.add_argument('--no-engine', action='store_true', help='skip the tests that need CREngine')
    parser.add_argument('--build-only', action='store_true', help='build the tests without running them')
    parser.add_argument('--coverage', action='store_true', help='instrument the tests for llvm-cov (Clang only)')
    parser.add_argument('--mini', action='store_true', help='test the mini Kobo asset bundle in a separate build directory')
    parser.add_argument('ctest', nargs='*', help='extra arguments for ctest, after --')
    args = parser.parse_args()
    if args.jobs < 1 or args.test_jobs < 1:
        parser.error('--jobs and --test-jobs must be positive')
    output = args.output.resolve()
    if args.qt and not (args.qt / 'lib/cmake/Qt6/Qt6Config.cmake').is_file():
        parser.error(f'No Qt 6 SDK at {args.qt}. See the testing section of docs/development.md.')
    for command in ('cmake', 'ninja', 'ctest', 'make', 'pkg-config') + (() if args.no_engine else ('patch',)):
        if not shutil.which(command):
            parser.error(f'{command} is required; this tool does not install system packages')
    if args.coverage and not args.build_only:
        from test_coverage import llvm_tool
        try:
            llvm_tool('llvm-profdata')
            llvm_tool('llvm-cov')
        except ValueError as error:
            parser.error(str(error))
    output.mkdir(parents=True, exist_ok=True)
    # The tests stage the release asset bundle, which includes the reading
    # fonts. Check them before the long SDK and engine builds.
    fetch_fonts.fetch(ROOT, offline=args.offline)
    if args.mini:
        import mini_fonts
        mini_fonts.prepare(ROOT, cache=args.cache.resolve(), offline=args.offline)
    with acquire(output / 'build.lock', wait=True):
        cache = args.cache.resolve()
        qt = args.qt.resolve() if args.qt else build_qt(output, args.jobs, cache, args.offline)
        engine = build_engine(qt, output, args.jobs, cache, args.offline, text_only=args.no_engine)
        # Instrumented objects get their own build directory, so switching
        # --coverage on or off never reuses objects built the other way.
        tests = output / ('tests' + ('-mini' if args.mini else '') + ('-coverage' if args.coverage else ''))
        configure = ['cmake', '-S', ROOT / 'tests', '-B', tests, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                     f'-DCMAKE_PREFIX_PATH={qt}', '-DKOBALT_MINI=' + ('ON' if args.mini else 'OFF'), *engine]
        if platform.system() == 'Darwin':
            configure.append('-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0')
        if args.coverage:
            flags = '-fprofile-instr-generate -fcoverage-mapping'
            configure += [f'-DCMAKE_CXX_FLAGS={flags}', f'-DCMAKE_EXE_LINKER_FLAGS={flags}']
        subprocess.run([str(part) for part in configure], check=True)
        subprocess.run(['cmake', '--build', str(tests), '--parallel', str(args.jobs)], check=True)
        if args.build_only:
            return
        environment = os.environ.copy()
        if args.coverage:
            coverage = tests / 'coverage'
            coverage.mkdir(exist_ok=True)
            profiles = Path(tempfile.mkdtemp(prefix='run-', dir=coverage))
            environment['LLVM_PROFILE_FILE'] = str(profiles / '%m-%p.profraw')
        result = subprocess.run(['ctest', '--test-dir', str(tests), '--output-on-failure', '--parallel', str(args.test_jobs), *args.ctest], env=environment)
        if args.coverage:
            from test_coverage import report
            metadata = dict(test_exit_code=result.returncode, ctest_arguments=args.ctest, host=HOST)
            variable_font = tests / 'fixtures/variable-fonts/Fraunces.ttf'
            if not args.no_engine and variable_font.is_file():
                metadata['variable_font_sha256'] = hashlib.sha256(variable_font.read_bytes()).hexdigest()
            (profiles / 'run.json').write_text(json.dumps(metadata, indent=2) + '\n')
            report(tests, profiles, profiles)
        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
