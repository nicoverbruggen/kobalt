"""Check APK rejection and source coverage without Android tools or containers."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ci_android
import release_sources

LIBRARY = 'lib/arm64-v8a/libkobalt_android_arm64-v8a.so'
VERSION = 'v1.0-beta.4'
BADGING = ("package: name='com.nicoverbruggen.kobalt.debug' versionCode='42' versionName='v1.0-beta.4-debug'\n"
           "minSdkVersion:'28'\ntargetSdkVersion:'36'\nnative-code: 'arm64-v8a'\napplication-debuggable\n")


def elf(alignment=16384, architecture=183):
    data = bytearray(120)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', data, 18, architecture)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 1)
    struct.pack_into('<I', data, 64, 1)
    struct.pack_into('<Q', data, 112, alignment)
    return data


class AndroidArtifactTests(unittest.TestCase):
    def validate(self, libraries=None, badging=BADGING, certificate=ci_android.CERTIFICATE, tools_fail=False, version=VERSION):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'app.apk'
            with zipfile.ZipFile(path, 'w') as archive:
                for name, data in (libraries if libraries is not None else {LIBRARY: elf()}).items():
                    archive.writestr(name, data)
            certificates = 'Signer #1 certificate SHA-256 digest: ' + certificate + '\n'
            with patch.object(ci_android.subprocess, 'check_output', side_effect=[badging, certificates]), \
                    patch.object(ci_android.subprocess, 'run') as run:
                if tools_fail:
                    run.side_effect = ci_android.subprocess.CalledProcessError(1, 'zipalign')
                return ci_android.validate_apk(path, Path('/sdk'), version, 42)

    def test_verified_apk_records_the_testing_identity(self):
        record = self.validate()
        self.assertEqual(record['version_code'], 42)
        self.assertEqual(record['certificate_sha256'], ci_android.CERTIFICATE)
        self.assertEqual(record['build_type'], 'debug')

    def test_wrong_metadata_or_signer_is_rejected(self):
        for old, new in (('.debug', ''), ("versionCode='42'", "versionCode='1'"),
                         ('v1.0-beta.4-debug', 'v1.0-beta.3-debug'), ('28', '29'), ('36', '35'),
                         ('application-debuggable', ''), ('arm64-v8a', 'x86_64')):
            with self.subTest(old=old), self.assertRaises(ValueError):
                self.validate(badging=BADGING.replace(old, new))
        with self.assertRaisesRegex(ValueError, 'certificate'):
            self.validate(certificate='f' * 64)

    def test_source_version_already_ending_in_debug_is_not_doubled(self):
        self.validate(version='v1.0-debug', badging=BADGING.replace('v1.0-beta.4-debug', 'v1.0-debug'))

    def test_invalid_native_libraries_are_rejected(self):
        for libraries in ({}, {LIBRARY: elf(4096)}, {LIBRARY: elf(architecture=62)},
                          {LIBRARY: b'broken'}, {LIBRARY: elf(), 'lib/x86_64/extra.so': elf()}):
            with self.subTest(libraries=list(libraries)), self.assertRaises(ValueError):
                self.validate(libraries=libraries)

    def test_alignment_tool_failure_is_not_ignored(self):
        with self.assertRaises(ci_android.subprocess.CalledProcessError):
            self.validate(tools_fail=True)


class AndroidSourceTests(unittest.TestCase):
    def test_source_bundle_keeps_android_overrides_and_matching_qt_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for directory in ('dependencies', 'android', 'toolchains/qt6/source', 'resources/fonts'):
                (root / directory).mkdir(parents=True)
            manifests = {
                'dependencies/manifest.json': {'libpng': {'url': 'kobo-libpng'}},
                'android/dependencies.json': {'libpng': {'url': 'android-libpng'}, 'openssl': {'url': 'openssl'}},
                'dependencies/release-sources.json': json.loads((ROOT / 'dependencies/release-sources.json').read_text()),
                'toolchains/qt6/source/source.json': {'version': '6.5.2', 'url': 'qt-kobo'},
                'resources/fonts/ebook-core.json': {'revision': '', 'files': []},
            }
            manifests['resources/fonts/ebook-core.json']['revision'] = manifests['dependencies/release-sources.json']['ebook-fonts']['url'].rsplit('/', 1)[-1]
            for name, value in manifests.items():
                (root / name).write_text(json.dumps(value))

            def download(spec, destination, offline):
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(spec['url'])

            with patch.object(release_sources.subprocess, 'check_output', return_value=b''), \
                    patch.object(release_sources, 'download', side_effect=download), \
                    patch.object(ci_android.fetch_fonts, 'fetch'):
                files = release_sources.inputs(root, offline=True)
            self.assertEqual(files['.cache/kobalt/libpng.tar.gz'].read_text(), 'kobo-libpng')
            self.assertEqual(files['.cache/android-dependencies/libpng.tar.gz'].read_text(), 'android-libpng')
            self.assertIn('.cache/android-dependencies/openssl.tar.gz', files)
            for name in ('qtbase-everywhere-src-6.11.3.tar.xz', 'qtsensors-everywhere-src-6.11.3.tar.xz'):
                self.assertIn('.cache/release-sources/' + name, files)


if __name__ == '__main__':
    unittest.main()
