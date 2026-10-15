"""Check artifact reuse across worktrees and dependency invalidation."""
import json
from pathlib import Path
import sys
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from dependency_cache import DependencyCache, stage_cmake


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cache = DependencyCache(self.root / 'shared', 'android-test')
        self.output = self.root / 'first/build'
        self.calls = 0

    def build(self, files, key):
        self.calls += 1
        library = files / 'deps/lib/text.a'
        library.parent.mkdir(parents=True)
        library.write_bytes(b'compiled text')
        pc = files / 'deps/lib/pkgconfig/text.pc'
        pc.parent.mkdir()
        pc.write_text('prefix=' + str(self.output / 'deps') + '\n')

    def test_two_worktrees_reuse_artifact_and_relocate_metadata(self):
        first = self.cache.build('text', {'compiler': 'one'}, self.output, self.build)
        other = self.root / 'second/build'
        second = self.cache.build('text', {'compiler': 'one'}, other, self.build)
        self.assertEqual(first, second)
        self.assertEqual(self.calls, 1)
        self.assertEqual((other / 'deps/lib/text.a').read_bytes(), b'compiled text')
        self.assertEqual((other / 'deps/lib/pkgconfig/text.pc').read_text(), 'prefix=' + str(other / 'deps') + '\n')

    def test_reuse_preserves_timestamps_and_repairs_damaged_install(self):
        self.cache.build('text', {}, self.output, self.build)
        library = self.output / 'deps/lib/text.a'
        before = library.stat().st_mtime_ns
        self.cache.build('text', {}, self.output, self.build)
        self.assertEqual(library.stat().st_mtime_ns, before)
        library.write_bytes(b'damaged')
        self.cache.build('text', {}, self.output, self.build)
        self.assertEqual(library.read_bytes(), b'compiled text')
        self.assertEqual(self.calls, 1)

    def test_only_changed_dependency_and_its_consumers_miss(self):
        def graph(pin):
            text = self.cache.build('text', {'source': pin}, self.output, self.build)
            tls = self.cache.build('tls', {'source': 'unchanged'}, self.output, self.build)
            engine = self.cache.build('engine', {'dependencies': {'text': text}}, self.output, self.build)
            return text, tls, engine
        first = graph('first')
        self.assertEqual(self.calls, 3)
        second = graph('second')
        self.assertEqual(self.calls, 5)
        self.assertNotEqual(first[0], second[0])
        self.assertEqual(first[1], second[1])
        self.assertNotEqual(first[2], second[2])

    def test_corrupt_shared_artifact_is_rebuilt(self):
        key = self.cache.build('text', {}, self.output, self.build)
        library = self.cache.root / 'text' / key / 'files/deps/lib/text.a'
        library.write_bytes(b'damaged')
        self.cache.build('text', {}, self.output, self.build)
        self.assertEqual(self.calls, 2)
        self.assertEqual(library.read_bytes(), b'compiled text')

    def test_installer_library_alias_is_cached_as_a_regular_file(self):
        def aliases(files, key):
            self.calls += 1
            (files / 'versioned.a').write_bytes(b'compiled archive')
            (files / 'alias.a').symlink_to('versioned.a')
        self.cache.build('text', {}, self.output, aliases)
        self.cache.build('text', {}, self.output, aliases)
        self.assertEqual(self.calls, 1)
        self.assertEqual((self.output / 'alias.a').read_bytes(), b'compiled archive')

    @unittest.skipUnless(shutil.which('cmake'), 'CMake is required')
    def test_cmake_staging_captures_absolute_and_relative_install_paths(self):
        project = self.root / 'cmake'
        project.mkdir()
        (project / 'library.a').write_bytes(b'archive')
        (project / 'library.h').write_text('header')
        (project / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.19)
project(InstallTest NONE)
install(FILES library.a DESTINATION "${CMAKE_INSTALL_PREFIX}/lib")
install(FILES library.h DESTINATION include)
''')
        directory = project / 'build'
        subprocess.run(['cmake', '-S', str(project), '-B', str(directory),
                        '-DCMAKE_INSTALL_PREFIX=' + str(self.output / 'deps')],
                       stdout=subprocess.DEVNULL, check=True)
        def compile(files, key):
            stage_cmake(directory, self.output / 'deps', files)
        self.cache.build('text', {}, self.output, compile)
        other = self.root / 'second/build'
        self.cache.build('text', {}, other, compile)
        self.assertEqual((other / 'deps/lib/library.a').read_bytes(), b'archive')
        self.assertEqual((other / 'deps/include/library.h').read_text(), 'header')

    def test_failed_build_is_not_reusable(self):
        def fail(files, key):
            self.build(files, key)
            raise RuntimeError('interrupted')
        with self.assertRaises(RuntimeError):
            self.cache.build('text', {}, self.output, fail)
        self.cache.build('text', {}, self.output, self.build)
        self.assertEqual(self.calls, 2)

    def test_removed_installed_files_do_not_survive_dependency_update(self):
        self.cache.build('text', {'source': 'old'}, self.output, self.build)
        def changed(files, key):
            (files / 'new.a').write_bytes(b'new library')
        self.cache.build('text', {'source': 'new'}, self.output, changed)
        self.assertFalse((self.output / 'deps/lib/text.a').exists())
        self.assertTrue((self.output / 'new.a').exists())

    def test_artifact_record_cannot_install_outside_output(self):
        key = self.cache.build('text', {}, self.output, self.build)
        path = self.cache.root / 'text' / key / 'artifact.json'
        record = json.loads(path.read_text())
        record['files'] = {'../../outside': 'digest'}
        path.write_text(json.dumps(record))
        self.cache.build('text', {}, self.output, self.build)
        self.assertEqual(self.calls, 2)
        self.assertFalse((self.root / 'outside').exists())


if __name__ == '__main__':
    unittest.main()
