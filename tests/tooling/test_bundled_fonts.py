import hashlib
import io
import zipfile
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import fetch_fonts
import assets


class BundledFonts(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'resources/fonts/Libron-Regular.ttf'
        self.source.parent.mkdir(parents=True)
        self.data = b'local release font'
        self.source.write_bytes(self.data)
        self.spec = dict(repository='https://github.com/nicoverbruggen/ebook-fonts',
                         revision='a' * 40, collection='core', files=[dict(file=self.source.name,
                         bundled=True, sha256=hashlib.sha256(self.data).hexdigest())])
        self.save()

    def save(self):
        (self.source.parent / 'ebook-core.json').write_text(json.dumps(self.spec))

    def test_offline_build_replaces_old_cache_from_verified_source(self):
        cached = fetch_fonts.font_path(self.source.name, self.root)
        cached.parent.mkdir(parents=True)
        cached.write_bytes(b'old upstream font')
        with patch.object(fetch_fonts.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            fetch_fonts.fetch(self.root, offline=True)
        self.assertEqual(cached.read_bytes(), self.data)
        self.assertEqual(self.source.read_bytes(), self.data)
        fetch_fonts.verify(self.root)

    def test_corrupt_source_is_rejected_even_with_a_valid_cache(self):
        fetch_fonts.fetch(self.root, offline=True)
        self.source.write_bytes(b'changed font')
        for action in [fetch_fonts.fetch, fetch_fonts.verify]:
            with self.assertRaisesRegex(ValueError, 'changed bundled font'):
                action(self.root)
        self.assertEqual(self.source.read_bytes(), b'changed font')
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing or changed bundled font'):
            fetch_fonts.fetch(self.root)

    def test_bundled_source_cannot_be_a_symlink(self):
        target = self.root / 'other.ttf'
        target.write_bytes(self.data)
        self.source.unlink()
        self.source.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'Linked'):
            fetch_fonts.fetch(self.root, offline=True)

    def test_bundled_flag_is_boolean(self):
        self.spec['files'][0]['bundled'] = 'true'
        self.save()
        with self.assertRaisesRegex(ValueError, 'bundled font flag'):
            fetch_fonts.collection(self.root)

    def test_release_download_checks_archive_and_font(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as package:
            package.writestr(self.source.name, self.data)
        archive = buffer.getvalue()
        self.source.unlink()
        item = self.spec['files'][0]
        item.pop('bundled')
        item['release'] = 'Libron'
        self.spec['releases'] = {'Libron': dict(version='0.31',
            url='https://github.com/nicoverbruggen/libron/releases/download/v0.31/Libron.zip',
            sha256=hashlib.sha256(archive).hexdigest())}
        self.save()
        with self.assertRaisesRegex(ValueError, 'offline mode'):
            fetch_fonts.fetch(self.root, offline=True)
        with patch.object(fetch_fonts.urllib.request, 'urlopen', return_value=io.BytesIO(b'corrupt')):
            with self.assertRaisesRegex(ValueError, 'release checksum'):
                fetch_fonts.fetch(self.root)
        with patch.object(fetch_fonts.urllib.request, 'urlopen', return_value=io.BytesIO(archive)) as download:
            fetch_fonts.fetch(self.root)
            download.assert_called_once_with(self.spec['releases']['Libron']['url'], timeout=60)
        with patch.object(fetch_fonts.urllib.request, 'urlopen', side_effect=AssertionError('network')):
            fetch_fonts.fetch(self.root, offline=True)
        self.assertEqual(fetch_fonts.font_path(self.source.name, self.root).read_bytes(), self.data)

    def test_libron_release_is_pinned(self):
        spec = fetch_fonts.collection(ROOT)
        self.assertEqual(spec['releases']['Libron']['version'], '0.31')
        fonts = [item for item in spec['files'] if item['file'].startswith('Libron-')]
        self.assertEqual(len(fonts), 4)
        for item in fonts:
            self.assertEqual(item['release'], 'Libron')
            self.assertFalse(item.get('bundled'))
            self.assertFalse((ROOT / 'resources/fonts' / item['file']).exists())

    def test_fallback_assets_match_pinned_files(self):
        files = []
        for manifest in ('noto.json', 'noto-scripts.json', 'noto-cjk.json'):
            spec = json.loads((ROOT / 'resources/fonts' / manifest).read_text())
            for item in spec['files']:
                data = (ROOT / 'resources/fonts' / item['file']).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), item['sha256'], item['file'])
                self.assertIn('/' + spec['revision'] + '/', item['url'])
                files.append(item['file'])
        self.assertCountEqual(files, assets.FALLBACK_FONTS)


if __name__ == '__main__':
    unittest.main()
