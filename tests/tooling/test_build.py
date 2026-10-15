#!/usr/bin/env python3
"""Tests for verified dependency extraction."""
import ast
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
from pathlib import Path
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import fetch
import assets
import source_state
import build
from dependency_cache import fetch_source


class DependencyExtractionTest(unittest.TestCase):
    def make_archive(self, cache):
        payload = b'#!/bin/sh\nexit 0\n'
        archive = cache / 'sample.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            info = tarfile.TarInfo('sample-1/configure')
            info.mode = 0o755
            info.mtime = 1_700_000_000
            info.size = len(payload)
            tar.addfile(info, io.BytesIO(payload))
        return archive, payload

    def test_extraction_restores_executable_mode_and_mtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive, payload = self.make_archive(cache)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            fetch('sample', {'sha256': digest, 'directory': 'sample-1'}, cache, offline=True)

            configure = cache / 'sample-1/configure'
            self.assertEqual(configure.read_bytes(), payload)
            self.assertEqual(configure.stat().st_mode & 0o777, 0o755)
            self.assertEqual(configure.stat().st_mtime, 1_700_000_000)

    def test_legacy_verified_extraction_gets_metadata_restored(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive, payload = self.make_archive(cache)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            source = cache / 'sample-1'
            source.mkdir()
            configure = source / 'configure'
            configure.write_bytes(payload)
            configure.chmod(0o644)
            (source / '.kobalt-source').write_text(digest)

            fetch('sample', {'sha256': digest, 'directory': 'sample-1'}, cache, offline=True)

            self.assertEqual(configure.stat().st_mode & 0o777, 0o755)
            self.assertEqual(configure.stat().st_mtime, 1_700_000_000)
            self.assertEqual((source / '.kobalt-source').read_text(), digest + ':metadata-v1')


class AutotoolsBuildTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('make'), 'Make is required for the library-only build check')
    def test_library_only_selection_applies_to_build_and_staged_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / 'source'
            source.mkdir()
            # The default build needs an unavailable target program to make
            # its manual. Library-only builds must skip it during install too.
            (source / 'configure').write_text(
                '#!/bin/sh\nset -eu\n'
                'for argument in "$@"; do\n'
                '  case "$argument" in --prefix=*) prefix=${argument#--prefix=};; esac\n'
                'done\n'
                'printf "PREFIX := %s\\n" "$prefix" > Makefile\n'
                "cat >> Makefile <<'MAKEFILE'\n"
                'SUBDIRS = lib bin\n'
                '.PHONY: all install\n'
                'all install:\n'
                '\t@test "$(SUBDIRS)" = lib || { echo "Cannot run target program for its manual" >&2; exit 1; }\n'
                '\t@mkdir -p "$(DESTDIR)$(PREFIX)/lib"\n'
                '\t@printf "library\\n" > "$(DESTDIR)$(PREFIX)/lib/libsample.a"\n'
                'MAKEFILE\n')
            prefix = root / 'prefix'
            with self.assertRaises(subprocess.CalledProcessError):
                build.build_autotools(source, root / 'default-build', prefix)
            stage = root / 'stage'
            build.build_autotools(source, root / 'library-build', prefix,
                                  environment={'DESTDIR': str(stage)}, make_options=('SUBDIRS=lib',))
            self.assertEqual((stage / str(prefix).lstrip('/') / 'lib/libsample.a').read_text(), 'library\n')
            self.assertFalse(prefix.exists())


class BuildInputTest(unittest.TestCase):
    def test_android_uses_the_same_engine_patches(self):
        expected = tuple((ROOT / 'dependencies/crengine-patches.txt').read_text().splitlines())
        self.assertIn('crengine-svg-resources.patch', expected)
        self.assertEqual(build.ENGINE_PATCHES, expected)
        for name in ('tools/build.py', 'android/build.py', 'tools/run_tests.py'):
            with self.subTest(builder=name):
                tree = ast.parse((ROOT / name).read_text())
                self.assertTrue(any(isinstance(node, ast.comprehension)
                                    and isinstance(node.iter, ast.Name)
                                    and node.iter.id == 'ENGINE_PATCHES' for node in ast.walk(tree)))
                literals = [node.value for node in ast.walk(tree) if isinstance(node, ast.Constant)
                            and isinstance(node.value, str) and node.value.startswith('crengine-')
                            and node.value.endswith('.patch')]
                self.assertEqual(literals, [])

    def test_android_uses_content_derived_layout_policy(self):
        android = (ROOT / 'android/CMakeLists.txt').read_text()
        self.assertIn('LayoutPolicy.cmake', android)
        self.assertIn('"${LAYOUT_POLICY}"', android)
        self.assertNotIn('android-public-v1', android)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake is required for policy configuration')
    def test_layout_policy_tracks_patch_and_resource_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            helper = ROOT / 'cmake/LayoutPolicy.cmake'
            names = set(re.findall(r'(?:src|dependencies)/[\w/]+\.(?:cpp|h|json)', helper.read_text()))
            names.add('src/publication/plain_archive.cpp')
            names.update('dependencies/patches/' + name for name in build.ENGINE_PATCHES)
            names.update('dependencies/patches/' + name for name in build.LUNASVG_PATCHES)
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('initial content')
            inventory = root / 'dependencies/crengine-patches.txt'
            inventory.write_text('\n'.join(build.ENGINE_PATCHES) + '\n')
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.16)\nproject(Policy NONE)\n'
                + f'include("{helper}")\n'
                + 'kobalt_layout_policy(policy "${CMAKE_CURRENT_SOURCE_DIR}" "src/publication/plain_archive.cpp")\n'
                + 'file(WRITE "${CMAKE_BINARY_DIR}/policy" "${policy}")\n'
                + 'get_property(inputs DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)\n'
                + 'file(WRITE "${CMAKE_BINARY_DIR}/inputs" "${inputs}")\n')
            def configure():
                generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
                subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'), *generator],
                               check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                return (root / 'build/policy').read_text()
            initial = configure()
            patch_path = root / 'dependencies/patches/crengine-svg-resources.patch'
            patch_path.write_text('changed SVG handling')
            patched = configure()
            self.assertNotEqual(initial, patched)
            svg_patch = root / 'dependencies/patches/lunasvg-resource-limits.patch'
            svg_patch.write_text('changed SVG nesting limits')
            limited = configure()
            self.assertNotEqual(patched, limited)
            (root / 'src/publication/native_resources.cpp').write_text('changed resource handling')
            self.assertNotEqual(limited, configure())
            dependencies = (root / 'build/inputs').read_text().split(';')
            self.assertIn(str(patch_path), dependencies)
            self.assertIn(str(inventory), dependencies)

    def test_release_version_change_invalidates_recorded_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            version = root / 'VERSION'
            version.write_text('v1.0-beta.1\n')
            source_state.record(root, source_state.source_state(root))
            self.assertTrue(source_state.current(root, root))
            version.write_text('v1.0-beta.2\n')
            self.assertFalse(source_state.current(root, root))

    def test_icon_generator_edit_invalidates_recorded_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generator = root / 'tools/ui_icon_vectors.py'
            generator.parent.mkdir()
            generator.write_text('first generator')
            source_state.record(root, source_state.source_state(root))
            self.assertTrue(source_state.current(root, root))
            generator.write_text('changed generator')
            self.assertFalse(source_state.current(root, root))


class DeviceDependencyCacheTest(unittest.TestCase):
    def test_sdk_source_pins_and_build_recipe_change_cache_identity(self):
        selected = build.target('qt5')
        specs = {'crengine': {'sha256': 'first source'}}
        first = build.device_dependency_inputs(ROOT, selected, specs, 'first SDK')
        self.assertNotEqual(first, build.device_dependency_inputs(ROOT, selected, specs, 'second SDK'))
        self.assertNotEqual(first, build.device_dependency_inputs(ROOT, selected,
                            {'crengine': {'sha256': 'second source'}}, 'first SDK'))
        original = Path.read_bytes
        def changed(path):
            data = original(path)
            return data + b'\n# changed recipe' if path == ROOT / 'tools/build.py' else data
        with patch.object(Path, 'read_bytes', changed):
            self.assertNotEqual(first, build.device_dependency_inputs(ROOT, selected, specs, 'first SDK'))

    def test_reuse_requires_matching_sdk_inputs_and_intact_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / 'deps/lib/libtext.a'
            library.parent.mkdir(parents=True)
            library.write_bytes(b'installed library')
            engine = root / 'engine/libcrengine.a'
            engine.parent.mkdir()
            engine.write_bytes(b'engine library')
            header = engine.parent / 'crsetup.h'
            header.write_text('engine configuration')
            identity = {'image': 'immutable-first', 'recipes': 'first'}
            build.record_device_dependencies(root, identity, engine)
            self.assertTrue(build.device_dependencies_match(root, identity))
            self.assertFalse(build.device_dependencies_match(root, dict(identity, image='immutable-second')))
            self.assertFalse(build.device_dependencies_match(root, dict(identity, recipes='second')))
            for output in (library, engine, header):
                original = output.read_bytes()
                output.write_bytes(b'damaged')
                self.assertFalse(build.device_dependencies_match(root, identity))
                output.unlink()
                self.assertFalse(build.device_dependencies_match(root, identity))
                output.write_bytes(original)
            for record in ('null', '{', '{}', '{"identity":{},"files":{}}'):
                (root / 'dependencies.json').write_text(record)
                self.assertFalse(build.device_dependencies_match(root, identity))
            self.assertFalse(build.device_dependencies_match(root, None))

    def test_cache_record_cannot_read_outside_build_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('../outside', '/outside'):
                (root / 'dependencies.json').write_text(json.dumps({'identity': {}, 'files': {name: 'hash'}}))
                self.assertFalse(build.device_dependencies_match(root, {}))


class DependencyDownloadTest(unittest.TestCase):
    def test_shared_archive_is_reused_offline_in_another_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / 'first', root / 'second'
            first.mkdir(); second.mkdir()
            archive, payload = DependencyExtractionTest().make_archive(first)
            spec = {'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'directory': 'sample-1'}
            fetch_source('sample', spec, first, root / 'shared', offline=True)
            with patch('urllib.request.urlopen', side_effect=AssertionError('Unexpected download')):
                fetch_source('sample', spec, second, root / 'shared', offline=True)
            self.assertEqual((second / 'sample-1/configure').read_bytes(), payload)

    def test_corrupt_shared_archive_is_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / 'checkout'; cache.mkdir()
            spec = {'sha256': '0' * 64, 'directory': 'sample-1'}
            shared = root / 'shared' / spec['sha256']
            shared.mkdir(parents=True)
            (shared / 'sample.tar.gz').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'Shared dependency archive hash mismatch'):
                fetch_source('sample', spec, cache, root / 'shared', offline=True)
            self.assertFalse((cache / 'sample.tar.gz').exists())

    def test_interrupted_write_leaves_no_cache_entry_or_partial(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            payload = b'checked archive bytes'
            spec = {'url': 'https://example.invalid/archive', 'sha256': hashlib.sha256(payload).hexdigest()}
            write_bytes = Path.write_bytes
            def interrupted(path, content):
                write_bytes(path, content[:3])
                raise OSError('disk full')
            with patch('build.urllib.request.urlopen', return_value=io.BytesIO(payload)), patch.object(Path, 'write_bytes', interrupted):
                with self.assertRaisesRegex(OSError, 'disk full'):
                    fetch('sample', spec, cache)
            self.assertEqual(list(cache.iterdir()), [])

    def test_verified_download_publishes_and_extracts_without_leftovers(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive, payload = DependencyExtractionTest().make_archive(cache)
            content = archive.read_bytes()
            archive.unlink()
            spec = {'url': 'https://example.invalid/archive', 'sha256': hashlib.sha256(content).hexdigest(),
                    'directory': 'sample-1'}
            with patch('build.urllib.request.urlopen', return_value=io.BytesIO(content)):
                fetch('sample', spec, cache)
            self.assertEqual(archive.read_bytes(), content)
            self.assertEqual((cache / 'sample-1/configure').read_bytes(), payload)
            self.assertEqual(sorted(p.name for p in cache.iterdir()), ['sample-1', 'sample.tar.gz'])

    def test_corrupt_existing_download_names_recovery_and_stays_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive = cache / 'sample.tar.gz'
            archive.write_bytes(b'broken')
            with patch('build.urllib.request.urlopen') as download:
                with self.assertRaisesRegex(ValueError, 'Remove .*sample.tar.gz'):
                    fetch('sample', {'sha256': '0' * 64}, cache)
                download.assert_not_called()
            self.assertEqual(archive.read_bytes(), b'broken')


class AssetStagingTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cmake'), 'CMake is required for incremental asset configuration')
    def test_incremental_build_refreshes_ttc_and_hyphenation_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('resources/fonts/fallback.ttc', 'resources/hyph/English.pattern',
                         'resources/fonts/ebook-core.json', 'tools/fetch_fonts.py', 'tools/version.py'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('initial')
            (root / 'tools/assets.py').write_text(
                'import hashlib, pathlib, sys\n'
                'root = pathlib.Path(__file__).resolve().parents[1]\n'
                'data = b"".join(p.read_bytes() for p in sorted((root / "resources").rglob("*")) if p.is_file())\n'
                'pathlib.Path(sys.argv[2]).write_text(hashlib.sha256(data).hexdigest())\n')
            source = (ROOT / 'CMakeLists.txt').read_text()
            block = source[source.index('find_package(Python3 REQUIRED'):source.index('add_library(reader-fonts')]
            (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(Assets NONE)\n'
                                                'set(KOBALT_VERSION 1.0.0)\n' + block)
            output = root / 'build'
            generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
            subprocess.run(['cmake', '-S', str(root), '-B', str(output), *generator], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            manifest = output / 'asset-manifest.json'
            before = manifest.read_text()
            for name in ('resources/fonts/fallback.ttc', 'resources/hyph/English.pattern'):
                with self.subTest(asset=name):
                    asset = root / name
                    asset.write_text('changed asset')
                    stamp = time.time() + 2
                    os.utime(asset, (stamp, stamp))
                    subprocess.run(['cmake', '--build', str(output)], check=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    os.utime(asset, None)
                    self.assertNotEqual(before, manifest.read_text())
                    before = manifest.read_text()

    def test_restage_drops_removed_font(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            source = output / 'font.ttf'
            source.write_bytes(b'font')
            destination = output / 'install/.adds/kobalt/assets'
            obsolete = destination / 'fonts/ebook-core/Cartisse-Regular.ttf'
            obsolete.parent.mkdir(parents=True)
            obsolete.write_bytes(b'old-font')
            data = json.dumps({'files': [{'path': 'fonts/Ancizar.ttf'}]}).encode()
            with patch.object(assets, 'manifest', return_value=data), patch.object(assets, 'source_path', return_value=source):
                assets.stage(output)
            self.assertFalse(obsolete.exists())
            self.assertEqual((destination / 'fonts/Ancizar.ttf').read_bytes(), b'font')
            self.assertEqual((destination / 'manifest.json').read_bytes(), data)


if __name__ == '__main__':
    unittest.main()
