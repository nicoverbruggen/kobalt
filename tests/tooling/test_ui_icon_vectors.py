import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ui_icon_vectors as vectors


class VectorAssets(unittest.TestCase):
    def test_all_pinned_shapes_compile_deterministically(self):
        directory = ROOT / 'resources/icons/phosphor'
        generated = vectors.generate(directory)
        self.assertEqual(generated, vectors.generate(directory))
        manifest = json.loads((directory / 'manifest.json').read_text())
        for item in manifest['files']:
            self.assertIn('QStringLiteral("' + Path(item['file']).stem + '")', generated)
        self.assertEqual(generated.count('Qt::WindingFill'), len(manifest['files']))

    def test_arcs_keep_endpoints_and_sweep(self):
        # A half circle splits into two quarter cubics that end on the arc's endpoint.
        commands = vectors.path_commands('M0 0A1 1 0 0 1 2 0')
        self.assertEqual(len(commands), 3)
        self.assertTrue(commands[-1].endswith(', 2, 0);'))
        # The sweep flag chooses the side: clockwise in SVG's y-down space goes up.
        upper = [float(value) for value in commands[1][len('path.cubicTo('):-2].split(', ')]
        self.assertLess(upper[1], 0)
        lower = vectors.path_commands('M0 0A1 1 0 0 0 2 0')[1]
        self.assertGreater(float(lower[len('path.cubicTo('):-2].split(', ')[1]), 0)
        # A radius too small for the endpoints scales up instead of failing.
        self.assertTrue(vectors.path_commands('M0 0a.1 .1 0 0 1 4 0')[-1].endswith(', 4, 0);'))

    def test_smooth_curves_reflect_the_previous_control(self):
        commands = vectors.path_commands('M0 0C0 1 2 1 2 0S4 -1 4 0Z')
        self.assertEqual(commands[2], 'path.cubicTo(2, -1, 4, -1, 4, 0);')
        self.assertEqual(commands[-1], 'path.closeSubpath();')
        # Without a previous cubic, the first control point is the current point.
        self.assertEqual(vectors.path_commands('M1 1S2 2 3 3')[-1], 'path.cubicTo(1, 1, 2, 2, 3, 3);')

    def test_relative_commands_continue_after_close(self):
        commands = vectors.path_commands('M10 10h2v2zm1 1l1 0')
        self.assertEqual(commands[4], 'path.moveTo(11, 11);')
        self.assertEqual(commands[5], 'path.lineTo(12, 11);')

    def test_unsupported_or_incomplete_paths_fail(self):
        for path in ['M0 0Q1 1 2 2', 'M0 0T2 2', 'M0', 'L1 1', 'M0 0a1 1 0 2 1 1 1', 'M0 0a1 1 0 0 2 1 1',
                     'M0 0L1e999 1', 'M0 0Z1 1', 'M0 0C1 1 2']:
            with self.subTest(path=path), self.assertRaises(ValueError):
                vectors.path_commands(path)

    def test_changed_svg_and_unsupported_elements_fail(self):
        source = ROOT / 'resources/icons/phosphor/x.svg'
        data = source.read_bytes()
        entry = {'sha256': hashlib.sha256(data).hexdigest()}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'x.svg'
            path.write_bytes(data + b' ')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                vectors.geometry(path, entry)
            unsupported = data.replace(b'<path ', b'<image href="external.svg"/><path ')
            path.write_bytes(unsupported)
            with self.assertRaisesRegex(ValueError, 'unsupported SVG element'):
                vectors.geometry(path, {'sha256': hashlib.sha256(unsupported).hexdigest()})
            stroked = data.replace(b'fill="currentColor"', b'fill="none" stroke="currentColor"')
            path.write_bytes(stroked)
            with self.assertRaisesRegex(ValueError, 'unsupported SVG root'):
                vectors.geometry(path, {'sha256': hashlib.sha256(stroked).hexdigest()})

    def test_manifest_paths_cannot_escape_asset_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            for filename in ['../x.svg', '/tmp/x.svg']:
                (path / 'manifest.json').write_text(json.dumps({'files': [{'file': filename}]}))
                with self.assertRaisesRegex(ValueError, 'filename'):
                    vectors.generate(path)


if __name__ == '__main__':
    unittest.main()
