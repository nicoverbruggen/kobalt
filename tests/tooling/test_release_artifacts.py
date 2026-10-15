"""Check draft attachment policy without Git, containers or release services."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import draft_release as github
import release_sources

BETA = 'v1.0-beta.1'
COMMIT = 'a' * 40


class ReleaseSourceKeyTests(unittest.TestCase):
    def test_shared_debug_key_is_allowed(self):
        self.assertEqual(release_sources.safe_source(ROOT, 'android/debug.keystore'),
                         ROOT / 'android/debug.keystore')

    def test_other_keys_are_rejected(self):
        for name in ('android/release.keystore', 'android/release.p12', 'android/debug.jks'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                release_sources.safe_source(ROOT, name)

    def test_replaced_debug_key_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            root = Path(temporary)
            (root / 'android').mkdir()
            (root / 'android/debug.keystore').write_bytes(b'other signing key')
            with self.assertRaisesRegex(ValueError, 'Unexpected shared'):
                release_sources.safe_source(root, 'android/debug.keystore')


class ReleaseArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / 'artifacts'
        self.output.mkdir()
        (self.root / 'CHANGELOG.md').write_text('## ' + BETA + '\n\nFirst beta.\n')
        (self.root / 'docs').mkdir()
        self.template = self.root / 'docs/release-installation.md'
        self.template.write_text((ROOT / 'docs/release-installation.md').read_text(encoding='utf-8'), encoding='utf-8')
        self.prepare(BETA)
        checkout = patch.object(github, 'checkout', return_value={'commit': COMMIT, 'dirty': False})
        checkout.start(); self.addCleanup(checkout.stop)
        commands = patch.object(github.subprocess, 'check_output', return_value=COMMIT)
        commands.start(); self.addCleanup(commands.stop)

    def prepare(self, tag):
        for key, name in github.ARTIFACTS.items():
            path = self.output / name
            path.write_bytes(key.encode())
            record = {'commit': COMMIT, 'dirty': False, 'file': name, 'target': key,
                      'version': tag, 'sha256': github.digest(path)}
            (self.output / (key + '.json')).write_text(json.dumps(record))

    def remove_qt6(self):
        (self.output / github.ARTIFACTS['qt6']).unlink()
        (self.output / 'qt6.json').unlink()

    def github_draft(self, tag=BETA, existing=None):
        result = subprocess.CompletedProcess([], 1)
        if existing is not None:
            result = subprocess.CompletedProcess([], 0, json.dumps(existing))
        with patch.object(github.subprocess, 'run', return_value=result) as run:
            github.draft(self.root, self.output, tag, 'owner/kobalt')
        return [call.args[0] for call in run.call_args_list]

    def assert_inventory(self, keys):
        records = json.loads((self.output / 'BUILD-INFO.json').read_text())['artifacts']
        self.assertEqual(set(records), set(keys))
        self.assertEqual((self.output / 'SHA256SUMS').read_text(), ''.join(
            github.digest(self.output / github.ARTIFACTS[key]) + '  ' + github.ARTIFACTS[key] + '\n'
            for key in keys))

    def test_beta_validation_needs_only_qt5_and_source(self):
        self.remove_qt6()
        self.assertEqual(set(github.validate(self.root, self.output, BETA)), {'qt5', 'source'})
        (self.output / github.ARTIFACTS['qt5']).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Artifact does not match'):
            github.validate(self.root, self.output, BETA)

    def test_beta_four_requires_android_and_includes_testing_policy(self):
        tag = 'v1.0-beta.4'
        self.prepare(tag)
        (self.root / 'CHANGELOG.md').write_text('## ' + tag + '\n\nAndroid testing build.\n')
        creation = self.github_draft(tag)[-1]
        self.assertIn(str(self.output / github.ARTIFACTS['android']), creation)
        self.assert_inventory(('qt5', 'qt6', 'android', 'source'))
        notes = (self.output / 'release-notes.md').read_text()
        self.assertIn('/' + tag + '/Kobalt-android-arm64-debug.apk)', notes)
        self.assertIn('shared public test key', notes)
        self.assertIn('signature does not prove', notes)
        (self.output / 'android.json').unlink()
        with self.assertRaises(FileNotFoundError):
            github.validate(self.root, self.output, tag)

    def test_previous_betas_do_not_require_or_advertise_android(self):
        for tag in ('v1.0-beta.1', 'v1.0-beta.2', 'v1.0-beta.3'):
            with self.subTest(tag=tag):
                self.prepare(tag)
                (self.root / 'CHANGELOG.md').write_text('## ' + tag + '\n\nExisting beta.\n')
                (self.output / 'android.json').unlink()
                (self.output / github.ARTIFACTS['android']).unlink()
                self.assertNotIn('android', github.validate(self.root, self.output, tag))
                self.assertNotIn('Android debug build', github.release_notes(self.root, tag, 'owner/kobalt'))

    def test_github_beta_creation_uploads_qt5_source_and_matching_metadata(self):
        self.remove_qt6()
        commands = self.github_draft()
        creation = commands[-1]
        self.assertEqual(creation[:3], ['gh', 'release', 'create'])
        self.assertIn('--draft', creation)
        self.assertIn('--prerelease', creation)
        self.assertIn('--verify-tag', creation)
        paths = {Path(arg).name for arg in creation if str(self.output) in arg}
        self.assertEqual(paths, {'Kobalt-qt5.zip', 'Kobalt-source.tar.gz', 'SHA256SUMS',
                                 'BUILD-INFO.json', 'release-notes.md'})
        self.assert_inventory(('qt5', 'source'))
        notes = (self.output / 'release-notes.md').read_text(encoding='utf-8')
        self.assertTrue(notes.startswith('First beta.\n\n## Installation\n'))
        base = 'https://github.com/owner/kobalt/releases/download/' + BETA + '/'
        for name in ('Kobalt-qt5.zip', 'Kobalt-source.tar.gz', 'SHA256SUMS', 'BUILD-INFO.json'):
            self.assertIn('](' + base + name + ')', notes)
        self.assertNotIn('Kobalt-qt6.zip', notes)
        self.assertNotIn('{{', notes)

    def test_github_beta_rerun_removes_only_the_excluded_package(self):
        commands = self.github_draft(existing={'isDraft': True, 'tagName': BETA,
            'assets': [{'name': 'Kobalt-qt6.zip'}, {'name': 'custom.txt'}]})
        deletes = [command for command in commands if command[2] == 'delete-asset']
        self.assertEqual(deletes, [['gh', 'release', 'delete-asset', BETA, 'Kobalt-qt6.zip',
                                   '--repo', 'owner/kobalt', '--yes']])
        self.assertEqual(commands[-1][:3], ['gh', 'release', 'upload'])
        self.assertNotIn(str(self.output / 'Kobalt-qt6.zip'), commands[-1])
        self.assert_inventory(('qt5', 'source'))
        notes = (self.output / 'release-notes.md').read_text(encoding='utf-8')
        edit = next(command for command in commands if command[2] == 'edit')
        self.assertEqual(edit[edit.index('--notes-file') + 1], str(self.output / 'release-notes.md'))
        self.assertIn('## Installation', notes)

    def test_editable_installation_fields_use_the_selected_repository_and_tag(self):
        self.template.write_text('Install {{ tag }} from [Qt5]({{qt5_url}}).\n'
                                 '{{repository}} {{release_url}} {{download_url}}\n'
                                 'Custom instructions: café.\n', encoding='utf-8')
        notes = github.release_notes(self.root, BETA, 'some-owner/reader')
        self.assertIn('Install ' + BETA + ' from [Qt5](https://github.com/some-owner/reader/releases/download/'
                      + BETA + '/Kobalt-qt5.zip).', notes)
        self.assertIn('some-owner/reader https://github.com/some-owner/reader/releases/tag/' + BETA, notes)
        self.assertIn('Custom instructions: café.', notes)

    def test_installation_rejects_unknown_unavailable_and_malformed_fields_before_github_calls(self):
        for field in ('{{typo}}', '{{qt6_url}}', '{{tag', 'tag}}'):
            with self.subTest(field=field):
                self.template.write_text('Install ' + field)
                with patch.object(github.subprocess, 'run') as run:
                    with self.assertRaisesRegex(ValueError, 'release installation field'):
                        github.draft(self.root, self.output, BETA, 'owner/kobalt')
                run.assert_not_called()
                self.assertFalse((self.output / 'release-notes.md').exists())

    def test_missing_or_empty_installation_template_fails(self):
        self.template.write_text(' \n')
        with self.assertRaisesRegex(ValueError, 'Empty release installation template'):
            github.release_notes(self.root, BETA, 'owner/kobalt')
        self.template.unlink()
        with self.assertRaises(FileNotFoundError):
            github.release_notes(self.root, BETA, 'owner/kobalt')

    def test_github_published_beta_is_never_changed(self):
        release = {'isDraft': False, 'tagName': BETA, 'assets': [{'name': 'Kobalt-qt6.zip'}]}
        with patch.object(github.subprocess, 'run', return_value=
                          subprocess.CompletedProcess([], 0, json.dumps(release))) as run:
            with self.assertRaisesRegex(ValueError, 'published release'):
                github.draft(self.root, self.output, BETA, 'owner/kobalt')
        self.assertEqual([call.args[0][2] for call in run.call_args_list], ['view'])

    def test_github_stops_if_beta_is_published_before_asset_removal(self):
        draft = {'isDraft': True, 'tagName': BETA, 'assets': [{'name': 'Kobalt-qt6.zip'}]}
        published = {'isDraft': False, 'tagName': BETA}
        responses = [subprocess.CompletedProcess([], 0, json.dumps(value)) for value in (draft, published)]
        with patch.object(github.subprocess, 'run', side_effect=responses) as run:
            with self.assertRaisesRegex(ValueError, 'published while uploading'):
                github.draft(self.root, self.output, BETA, 'owner/kobalt')
        self.assertEqual([call.args[0][2] for call in run.call_args_list], ['view', 'view'])


    def test_other_tags_keep_both_packages(self):
        for tag in ('v1.0-beta.2', 'v1.0'):
            with self.subTest(tag=tag):
                self.prepare(tag)
                (self.root / 'CHANGELOG.md').write_text('## ' + tag + '\n\nRelease notes.\n')
                creation = self.github_draft(tag)[-1]
                self.assertIn(str(self.output / 'Kobalt-qt6.zip'), creation)
                self.assertIn('](https://github.com/owner/kobalt/releases/download/' + tag + '/Kobalt-qt6.zip)',
                              (self.output / 'release-notes.md').read_text(encoding='utf-8'))
                self.assert_inventory(('qt5', 'qt6', *(['android'] if tag == 'v1.0' else []), 'source'))


if __name__ == '__main__':
    unittest.main()
