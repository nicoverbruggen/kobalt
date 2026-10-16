import hashlib
import io
import json
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import native_screenshots as runner


class Response(io.BytesIO):
    def geturl(self):
        return 'https://example.org/book.epub'


class ScreenshotRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.cache = Path(self.temporary.name)
        self.payload = b'pinned fixture content'
        self.spec = dict(filename='book.epub', download='https://example.org/book.epub',
                         sha256=hashlib.sha256(self.payload).hexdigest())

    def test_real_android_captures_replace_native_startup_previews(self):
        shots = []
        for group, width, height in [('android-startup', 1264, 1680), ('android-device', 1080, 2400)]:
            filename = group + '/startup.png'
            image = self.cache / filename; image.parent.mkdir(); image.write_bytes(b'PNG')
            shots.append(dict(name=filename, group=group, file=filename, theme='light', width=width, height=height))
        runner.report(self.cache, dict(complete=True, groups=['android-startup', 'android-device'],
            screens=shots, width=1264, height=1680))
        page = (self.cache / 'index.html').read_text()
        self.assertIn('width="1080" height="2400"', page)
        self.assertNotIn('data-scene="android-startup/startup"', page)
        self.assertIn('data-scene="android-device/startup"', page)

    def test_android_capture_refuses_physical_devices_before_running_adb(self):
        import android_screenshots
        with patch.object(android_screenshots.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'never a physical device'):
                android_screenshots.capture('personal-phone', self.cache / 'app.apk', self.cache / 'book.epub', self.cache)
            run.assert_not_called()

    def test_android_capture_restores_owned_state_when_startup_fails(self):
        import android_screenshots as android
        apk = self.cache / 'app.apk'; apk.write_bytes(b'APK')
        book = self.cache / 'book.epub'; book.write_bytes(b'public fixture')
        commands = []
        def adb(command, **kwargs):
            args = command[3:]; commands.append(args)
            if args[:4] == ['shell', 'am', 'start', '-W']:
                raise subprocess.CalledProcessError(1, command)
            responses = {
                ('shell', 'getprop', 'sys.boot_completed'): '1',
                ('shell', 'wm', 'size'): 'Physical size: 1080x2400\nOverride size: 900x1600',
                ('shell', 'wm', 'density'): 'Physical density: 420\nOverride density: 360',
                ('shell', 'cmd', 'window', 'user-rotation'): 'lock 3',
                ('shell', 'settings', 'get', 'system', 'user_rotation'): '3',
                ('shell', 'settings', 'get', 'system', 'accelerometer_rotation'): '1',
                ('shell', 'settings', 'get', 'system', 'screen_off_timeout'): '60000',
                ('shell', 'settings', 'get', 'secure', 'immersive_mode_confirmations'): 'null',
                ('shell', 'cmd', 'overlay', 'list', '--user', '0'): '[x] com.android.internal.display.cutout.emulation.corner\n[ ] com.android.internal.display.cutout.emulation.hole\n[x] com.android.internal.systemui.navbar.threebutton\n[ ] com.android.internal.systemui.navbar.gestural',
            }
            return subprocess.CompletedProcess(command, 0, stdout=responses.get(tuple(args), ''), stderr='')
        with patch.dict('os.environ', {'ANDROID_HOME': '/sdk'}), patch.object(android.subprocess, 'run', side_effect=adb):
            with self.assertRaises(subprocess.CalledProcessError):
                android.capture('emulator-5554', apk, book, self.cache, reader_checks=True)
        self.assertIn(['shell', 'wm', 'size', '900x1600'], commands)
        self.assertIn(['shell', 'wm', 'density', '360'], commands)
        self.assertIn(['shell', 'cmd', 'window', 'user-rotation', 'lock', '3'], commands)
        self.assertIn(['shell', 'settings', 'put', 'system', 'screen_off_timeout', '60000'], commands)
        self.assertIn(['shell', 'settings', 'delete', 'secure', 'immersive_mode_confirmations'], commands)
        self.assertIn(['shell', 'cmd', 'overlay', 'enable', '--user', '0', 'com.android.internal.display.cutout.emulation.corner'], commands)
        self.assertIn(['shell', 'cmd', 'overlay', 'disable', '--user', '0', 'com.android.internal.display.cutout.emulation.hole'], commands)
        self.assertIn(['shell', 'cmd', 'overlay', 'enable', '--user', '0', 'com.android.internal.systemui.navbar.threebutton'], commands)
        self.assertIn(['shell', 'cmd', 'overlay', 'disable', '--user', '0', 'com.android.internal.systemui.navbar.gestural'], commands)
        report = json.loads((self.cache / 'android-reader-validation.json').read_text())
        self.assertEqual(report['profiles'][0]['status'], 'failed')

    def test_android_reader_profiles_do_not_expand_default_startup_inventory(self):
        import android_screenshots as android
        self.assertEqual(tuple(android.PROFILES), ('phone', 'reader-seven-inch', 'tablet', 'phone-landscape'))
        self.assertEqual(len(android.READER_PROFILES), 7)
        self.assertEqual(len({profile[2] for profile in android.READER_PROFILES.values()}), 7)
        self.assertEqual(android.display_size(android.READER_PROFILES['tablet-landscape']), (2560, 1600))

    def test_android_proxy_geometry_uses_text_viewport_and_source_chrome(self):
        import android_screenshots as android
        event = dict(event='reader-resize-text', target='main', viewport_width=2263, viewport_height=911)
        geometry = android.reader_geometry(event, 420, {})
        self.assertEqual((geometry['width'], geometry['height']), (2263, 1079))
        # Pixel 7's native hole remains when emulator cutout overlays are off.
        android.validate_reader_geometry((2400, 1080), (137, 0, 0, 0), geometry)
        no_strips = android.reader_geometry(event, 212, dict(show_header=False, show_footer=False, show_progress_bar=False, show_clock=False))
        self.assertEqual(no_strips['height'], 911)
        # The old clock without a header now keeps a small header strip.
        clock = android.reader_geometry(event, 212, dict(show_header=False, show_footer=False, show_progress_bar=False))
        self.assertEqual(clock['height'], 945)
        quiet = android.reader_geometry(event, 212, dict(header_footer='alternative_b'))
        self.assertEqual(quiet['height'], 911 + 2 * 34)
        custom = android.reader_geometry(event, 212, dict(header_footer='custom', header_footer_custom=dict(
            header_size='large', footer_size='off', progress_bar=False)))
        self.assertEqual(custom['height'], 911 + 57)
        with self.assertRaisesRegex(ValueError, 'not a main reader resize'):
            android.reader_geometry({**event, 'target': 'preview'}, 420, {})

    def test_android_reader_geometry_requires_safe_cutout_and_image_viewport(self):
        import android_screenshots as android
        geometry = dict(width=1080, height=2320, viewport_width=1080, viewport_height=2200)
        bounds = android.validate_reader_geometry((1080, 2400), (0, 80, 0, 0), geometry, True)
        self.assertEqual(bounds, (0, 80, 1080, 2400))
        self.assertEqual(android.input_point(bounds, .85), (918, 1240))
        with self.assertRaisesRegex(ValueError, 'absent required cutout'):
            android.validate_reader_geometry((1080, 2400), (0, 0, 0, 0), geometry, True)
        with self.assertRaisesRegex(ValueError, 'safe display area'):
            android.validate_reader_geometry((1080, 2400), (0, 0, 0, 0), geometry)
        with self.assertRaisesRegex(ValueError, 'inconsistent'):
            android.validate_reader_geometry((1080, 2400), (0, 80, 0, 0), {**geometry, 'viewport_height': 2400})
        rounded = {**geometry, 'width': 1079, 'viewport_width': 1079}
        android.validate_reader_geometry((1080, 2400), (0, 80, 0, 0), rounded, True)

    def test_android_cutout_evidence_fails_closed_when_missing_or_ambiguous(self):
        import android_screenshots as android
        self.assertEqual(android.cutout_insets('mDisplayCutout=DisplayCutout{insets=Rect(0, 80 - 0, 0)'), (0, 80, 0, 0))
        with self.assertRaisesRegex(ValueError, 'did not report'):
            android.cutout_insets('unrecognized display layout')
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            android.cutout_insets('DisplayCutout{insets=Rect(0, 80 - 0, 0) DisplayCutout{insets=Rect(0, 0 - 0, 0)')

    def test_android_scrolled_capture_requires_both_actions_on_screen(self):
        import android_screenshots
        import xml.etree.ElementTree as ET
        def nodes(open_bounds, exit_bounds):
            return [ET.Element('node', {'resource-id': 'app.' + name, 'bounds': bounds})
                    for name, bounds in [('androidOpenBook', open_bounds), ('androidExit', exit_bounds)]]
        visible = android_screenshots.startup_actions_visible
        self.assertFalse(visible(nodes('[0,0][0,0]', '[0,0][0,0]'), (2400, 1080)))
        self.assertFalse(visible(nodes('[71,696][1750,842]', '[71,1010][1750,1156]'), (2400, 1080)))
        self.assertFalse(visible(nodes('[71,696][1750,842]', '[-10,869][1750,1015]'), (2400, 1080)))
        self.assertTrue(visible(nodes('[71,696][1750,842]', '[71,869][1750,1015]'), (2400, 1080)))

    def test_prune_removes_obsolete_groups_and_screens(self):
        current = self.cache / 'light/reading/current.png'
        current.parent.mkdir(parents=True)
        current.write_bytes(b'current')
        stale = current.with_name('obsolete.png')
        stale.write_bytes(b'old')
        stale.with_suffix('.json').write_text('{}')
        old_group = self.cache / 'dark/removed-group'
        old_group.mkdir(parents=True)
        (old_group / 'run.log').write_text('old log')
        runner.prune_captures(self.cache, {'screens': [{'file': 'light/reading/current.png'}]})
        self.assertTrue(current.exists())
        self.assertFalse(stale.exists())
        self.assertFalse(stale.with_suffix('.json').exists())
        self.assertFalse(old_group.exists())

    def test_download_once_then_reuse_without_network(self):
        with patch.object(runner.urllib.request, 'urlopen', return_value=Response(self.payload)) as request:
            path = runner.cached_book(self.spec, self.cache)
            self.assertEqual(path.read_bytes(), self.payload)
            self.assertEqual(runner.cached_book(self.spec, self.cache), path)
            self.assertEqual(runner.cached_book(self.spec, self.cache, offline=True), path)
            request.assert_called_once()

    def test_corrupt_cache_is_not_silently_replaced(self):
        (self.cache / 'book.epub').write_bytes(b'changed')
        with patch.object(runner.urllib.request, 'urlopen') as request:
            with self.assertRaisesRegex(ValueError, 'differs'):
                runner.cached_book(self.spec, self.cache)
            request.assert_not_called()

    def test_failed_checksum_leaves_no_cached_or_partial_file(self):
        with patch.object(runner.urllib.request, 'urlopen', return_value=Response(b'upstream changed')):
            with self.assertRaisesRegex(ValueError, 'changed upstream'):
                runner.cached_book(self.spec, self.cache)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_offline_missing_book_never_downloads(self):
        with patch.object(runner.urllib.request, 'urlopen') as request:
            with self.assertRaisesRegex(ValueError, 'not cached'):
                runner.cached_book(self.spec, self.cache, offline=True)
            request.assert_not_called()

    def test_groups_and_individual_screens_keep_inventory_order(self):
        self.assertEqual(runner.selected_groups(['chapters', 'settings'], ['dictionary/definition']),
                         ['settings', 'chapters', 'dictionary'])
        self.assertEqual(runner.selected_groups([], []), [group for group in runner.GROUPS if group != 'android-startup'])
        with self.assertRaises(ValueError):
            runner.selected_groups([], ['settings/../escape'])
        with self.assertRaises(ValueError):
            runner.selected_groups(['unknown'], [])

    def test_android_previews_require_explicit_selection(self):
        defaults = runner.selected_groups([], [])
        self.assertNotIn('android-startup', defaults)
        self.assertEqual(runner.selected_groups(['all'], []), defaults)
        self.assertEqual(runner.selected_groups([], [], include_android=True), list(runner.GROUPS))
        self.assertEqual(runner.selected_groups(['all'], [], include_android=True), list(runner.GROUPS))
        self.assertEqual(runner.selected_groups(['reading'], [], include_android=True), ['android-startup', 'reading'])
        self.assertEqual(runner.selected_groups(['android-startup'], []), ['android-startup'])
        self.assertEqual(runner.selected_groups([], ['android-startup/with-cover']), ['android-startup'])

    def test_full_sweep_includes_every_international_fixture(self):
        references = json.loads(runner.BOOKS.read_text())
        inventory = json.loads((runner.ROOT / 'tests/fixtures/public-domain/catalog.json').read_text())
        groups = runner.selected_groups([], [])
        captured = {references[runner.GROUPS[group][0]]['language'] for group in groups if group.startswith('international-')}
        self.assertEqual({book['language'] for book in inventory}, captured)
        for group in groups:
            if not group.startswith('international-'): continue
            book = references[runner.GROUPS[group][0]]
            self.assertEqual(book['rights'], 'Public-domain original; Wikisource transcription CC BY-SA 4.0')
            self.assertTrue(book['source'].startswith('https://'))
        self.assertEqual(runner.selected_groups(['international'], []), [group for group in groups if group.startswith('international-')])

    def test_gallery_excludes_obsolete_native_ui_component_captures(self):
        self.assertNotIn('native-ui', runner.selected_groups([], []))
        with self.assertRaisesRegex(ValueError, 'Unknown screenshot groups'):
            runner.selected_groups(['native-ui'], [])
        filename = 'light/reading/menu.png'
        image = self.cache / filename
        image.parent.mkdir(parents=True)
        image.write_bytes(b'PNG')
        manifest = dict(complete=True, groups=['reading', 'native-ui'], width=1264, height=1680,
                        screens=[dict(name='menu', group='reading', file=filename),
                                 dict(name='components', group='native-ui', file='light/native-ui/components.png')])
        runner.report(self.cache, manifest)
        page = (self.cache / 'index.html').read_text()
        self.assertNotIn('native-ui', page)
        self.assertNotIn('Native ui', page)
        self.assertIn('Overflow and settings · Reading menu', page)
        saved = json.loads((self.cache / 'manifest.json').read_text())
        self.assertEqual(saved['groups'], ['reading'])
        self.assertEqual(len(saved['screens']), 1)
        self.assertEqual(manifest['groups'], ['reading', 'native-ui'])

    def test_report_separates_multilingual_and_vertical_reading(self):
        groups = ['international-arabic', 'international-greek', 'international-writing-modes']
        shots = []
        for group in groups:
            filename = f'light/{group}/reading.png'
            path = self.cache / filename; path.parent.mkdir(parents=True); path.write_bytes(b'PNG')
            shots.append(dict(name=filename, group=group, file=filename))
        runner.report(self.cache, dict(complete=True, groups=groups, screens=shots, width=1264, height=1680))
        page = (self.cache / 'index.html').read_text()
        self.assertEqual(page.count('<h2>Reading · Multilingual</h2>'), 1)
        self.assertEqual(page.count('<h2>CJK · Vertical</h2>'), 1)
        self.assertEqual(page.count('data-group="reading-multilingual"'), 2)
        self.assertEqual(page.count('data-group="cjk-vertical"'), 1)
        self.assertIn('Arabic · Reading', page)
        self.assertIn('Greek · Reading', page)
        self.assertIn('Japanese writing modes · Reading', page)
        self.assertIn('data-scene="international-arabic/reading"', page)
        self.assertIn('data-scene="international-greek/reading"', page)
        self.assertNotIn('Save PNG', page)
        self.assertIn('<option value="reading-multilingual">Reading · Multilingual</option>', page)

    def test_report_orders_main_view_before_controls_and_names_book_languages(self):
        shots = []
        for group, name in [('reading', 'menu'), ('reading', 'reading'),
                            ('international-latin', 'reading'), ('international-cyrillic', 'reading')]:
            filename = f'light/{group}/{name}.png'
            image = self.cache / filename
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(b'PNG')
            shots.append(dict(name=filename, group=group, file=filename))
        runner.report(self.cache, dict(complete=True, groups=['reading', 'international-latin', 'international-cyrillic'],
                                      screens=shots, width=1264, height=1680))
        page = (self.cache / 'index.html').read_text()
        self.assertLess(page.index('alt="Reading · Basic · Alice · Reading'), page.index('alt="Overflow and settings · Reading menu'))
        self.assertLess(page.index('Russian · Reading'), page.index('Vietnamese · Reading'))

    def test_report_places_international_ui_after_reading_and_interactions(self):
        inventory = [('settings', 'display'), ('international-arabic', 'supplemental-fonts'),
                     ('international-arabic', 'chapters'), ('dictionary', 'definition'), ('footnotes', 'reference'),
                     ('links', 'actions'), ('reading', 'menu'), ('maths', 'equations-1'),
                     ('international-arabic', 'reading'), ('reading', 'reading'), ('search', 'query')]
        shots = []
        for group, scene in inventory:
            filename = f'light/{group}/{scene}.png'
            image = self.cache / filename
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(b'PNG')
            shots.append(dict(name=filename, group=group, file=filename))
        manifest = dict(complete=True, groups=list(dict.fromkeys(group for group, _ in inventory)),
                        screens=shots, width=1264, height=1680)
        runner.report(self.cache, manifest)
        page = (self.cache / 'index.html').read_text()
        headings = ['Reading · Basic', 'Reading · Multilingual', 'Reading · Mathematics', 'Links', 'Footnotes', 'Dictionary', 'Chapters', 'Text', 'Search', 'Overflow and settings']
        positions = [page.index(f'<h2>{heading}</h2>') for heading in headings]
        self.assertEqual(positions, sorted(positions))
        def section_before(scene):
            return page[:page.index(f'data-scene="{scene}"')].rsplit('<h2>', 1)[1].split('</h2>', 1)[0]
        self.assertEqual(section_before('international-arabic/reading'), 'Reading · Multilingual')
        self.assertEqual(section_before('maths/equations-1'), 'Reading · Mathematics')
        self.assertEqual(section_before('international-arabic/chapters'), 'Chapters')
        self.assertEqual(section_before('international-arabic/supplemental-fonts'), 'Text')
        self.assertEqual(section_before('reading/menu'), 'Overflow and settings')
        saved = json.loads((self.cache / 'manifest.json').read_text())
        self.assertEqual(saved['groups'], manifest['groups'])

    @unittest.skipUnless(shutil.which('node'), 'Node is required for gallery controls')
    def test_viewer_theme_and_keyboard_follow_filtered_gallery(self):
        script = (runner.ROOT / 'tools/native_screenshot_gallery.html').read_text().split('<script>')[1].split('</script>')[0]
        harness = r"""
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
function element() {
  return {value: '', hidden: false, listeners: {}, attrs: {},
    addEventListener(name, callback) { this.listeners[name] = callback; },
    setAttribute(name, value) { this.attrs[name] = value; },
    removeAttribute(name) { delete this[name]; },
    closest() { return null; }, focus() { this.focused = true; }};
}
const elements = Object.fromEntries(['filters','group','theme','query','count','empty','reset','viewer','image','viewer-title','viewer-theme','gallery','close'].map(id => [id, element()]));
elements.viewer.showModal = function() { this.open = true; };
elements.viewer.close = function() { this.open = false; this.listeners.close(); };
function card(scene, theme, group = 'reading') {
  const result = element();
  result.dataset = {scene, theme, group, search: scene};
  result.image = {src: scene + theme, alt: scene + theme};
  result.button = element();
  result.button.querySelector = () => result.image;
  result.button.closest = selector => selector === 'figure' ? result : result.button;
  result.querySelector = () => result.button;
  return result;
}
const cards = [card('first','light'), card('first','dark'), card('second','light'), card('second','dark'), card('other','light','settings'), card('unpaired','light')];
const document = element();
document.getElementById = id => elements[id];
document.querySelectorAll = selector => selector === 'figure' ? cards : [];
vm.runInNewContext(fs.readFileSync(0, 'utf8'), {document, Set});
function key(key, target = element()) {
  let prevented = false;
  document.listeners.keydown({key, target, preventDefault() { prevented = true; }});
  return prevented;
}
function open(index) { elements.gallery.listeners.click({target: cards[index].button}); }
assert.equal(key(' '), false);
elements.group.value = 'reading';
elements.filters.listeners.change();
open(0);
assert.equal(elements.image.src, 'firstlight');
assert.equal(key('ArrowLeft'), true);
assert.equal(elements.image.src, 'firstlight');
key('ArrowRight');
assert.equal(elements.image.src, 'secondlight');
key(' ');
assert.equal(elements.image.src, 'seconddark');
assert.equal(elements.theme.attrs['aria-pressed'], 'true');
key('ArrowRight');
assert.equal(elements.image.src, 'seconddark');
key('ArrowLeft');
assert.equal(elements.image.src, 'firstdark');
elements['viewer-theme'].listeners.click();
assert.equal(elements.image.src, 'firstlight');
const input = element(); input.closest = () => input;
assert.equal(key('ArrowRight', input), false);
assert.equal(elements.image.src, 'firstlight');
key('ArrowRight'); key('ArrowRight');
assert.equal(elements.image.src, 'unpairedlight');
assert.equal(elements['viewer-theme'].disabled, true);
key(' ');
assert.equal(elements.image.src, 'unpairedlight');
elements.close.listeners.click();
assert.equal(cards[5].button.focused, true);
assert.equal(key('ArrowLeft'), false);
elements.query.value = 'second';
elements.filters.listeners.input();
open(2); key('ArrowLeft');
assert.equal(elements.image.src, 'secondlight');
key(' ');
assert.equal(elements.image.src, 'seconddark');
"""
        subprocess.run(['node', '-e', harness], input=script, text=True, check=True, capture_output=True)

    def test_cached_default_build_refreshes_engine_configuration(self):
        from run_tests import HOST
        build = self.cache / 'build/tests' / HOST / 'tests'
        build.mkdir(parents=True)
        (build / 'CMakeCache.txt').write_text('existing configuration')
        (build / 'reader-screenshot-tour').write_bytes(b'existing executable')
        with patch.object(runner, 'ROOT', self.cache), patch.object(runner, 'selected_groups', return_value=[]), \
                patch.object(runner, 'report'), patch.object(runner.subprocess, 'run') as run:
            runner.main(['--offline'])
        configure = run.call_args_list[0].args[0]
        self.assertIn(str(self.cache / 'tools/run_tests.py'), configure)
        self.assertIn('--build-only', configure)
        self.assertIn('--offline', configure)
        self.assertIn('reader-screenshot-tour', run.call_args_list[1].args[0])

    def test_partial_refresh_keeps_other_groups_and_themes(self):
        def shot(theme, group, name):
            return dict(theme=theme, group=group, file=f'{theme}/{group}/{name}.png')
        old = dict(complete=True, width=1264, height=1680, books={}, dictionaries={'en-en': {'sha256': 'pinned'}}, screens=[
            shot('light', 'links', 'actions'), shot('light', 'links', 'returned'),
            shot('dark', 'links', 'actions'), shot('light', 'settings', 'display')])
        fresh = dict(complete=True, width=1264, height=1680, books={}, groups=['links'], themes=['light'],
                     screens=[dict(shot('light', 'links', 'actions'), sha256='new')])
        merged = runner.merge_captures(old, fresh, ['links/actions'])
        self.assertEqual(len(merged['screens']), 4)
        self.assertEqual(merged['screens'][-1]['sha256'], 'new')
        self.assertEqual(merged['themes'], ['light', 'dark'])
        self.assertEqual(merged['dictionaries'], old['dictionaries'])
        merged = runner.merge_captures(old, fresh, [])
        self.assertEqual(len(merged['screens']), 3)
        self.assertFalse(any(shot['file'].endswith('returned.png') for shot in merged['screens']))
        fresh['width'] = 600
        with self.assertRaisesRegex(ValueError, 'Viewport differs'):
            runner.merge_captures(old, fresh, [])

    def test_density_paths_keep_the_default_and_isolate_other_densities(self):
        self.assertEqual(runner.capture_folder(300, 'light', 'settings'), Path('light/settings'))
        self.assertEqual(runner.capture_folder(212, 'light', 'settings'), Path('ppi-212/light/settings'))

    def test_capture_cli_default_and_requested_densities(self):
        build = self.cache / 'native'
        build.mkdir()
        (build / 'CMakeCache.txt').write_text('configuration')
        (build / 'reader-screenshot-tour').write_bytes(b'binary')
        book = self.cache / 'book.epub'
        book.write_bytes(self.payload)
        def capture(command, **kwargs):
            folder = Path(command[command.index('--output') + 1])
            ppi = int(command[command.index('--ppi') + 1])
            (folder / 'display.png').write_bytes(b'unchanged PNG')
            (folder / 'display.json').write_text(json.dumps(dict(ppi=ppi, reading_ppi=ppi, dpr=1)))
        for options, expected in [([], [300]), (['--ppi', '300', '--ppi', '212'], [300, 212])]:
            with patch.object(runner, 'ROOT', self.cache), patch.object(runner, 'cached_book', return_value=book), \
                    patch.object(runner, 'report'), patch.object(runner.subprocess, 'run', side_effect=capture) as run:
                runner.main(['--no-build', '--build-dir', str(build), '--group', 'settings', '--theme', 'light', *options])
            self.assertEqual(sorted(int(call.args[0][call.args[0].index('--ppi') + 1]) for call in run.call_args_list), sorted(expected))
            self.assertEqual((self.cache / 'build/screenshots/light/settings/display.png').read_bytes(), b'unchanged PNG')
            if 212 in expected:
                self.assertTrue((self.cache / 'build/screenshots/ppi-212/light/settings/display.png').exists())
            shutil.rmtree(self.cache / 'build/screenshots')

    def test_partial_density_refresh_keeps_other_density(self):
        old = dict(complete=True, width=1264, height=1680, books={}, screens=[
            dict(theme='light', group='settings', ppi=300, file='light/settings/display.png'),
            dict(theme='light', group='settings', ppi=212, file='ppi-212/light/settings/display.png')])
        fresh = dict(complete=True, width=1264, height=1680, books={}, groups=['settings'], themes=['light'], densities=[212], screens=[
            dict(theme='light', group='settings', ppi=212, file='ppi-212/light/settings/display.png', sha256='new')])
        merged = runner.merge_captures(old, fresh, [])
        self.assertEqual(len(merged['screens']), 2)
        self.assertEqual(merged['screens'][0]['ppi'], 300)
        self.assertEqual(merged['screens'][1]['sha256'], 'new')
        self.assertEqual(merged['densities'], [212, 300])
        for shot in merged['screens']:
            path = self.cache / shot['file']
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'PNG')
        stale = self.cache / 'ppi-212/dark/settings/obsolete.png'
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b'old')
        runner.prune_captures(self.cache, merged)
        self.assertTrue(all((self.cache / shot['file']).exists() for shot in merged['screens']))
        self.assertFalse(stale.parent.exists())

    def test_failed_capture_preserves_gallery_and_releases_lock(self):
        import subprocess
        build = self.cache / 'build'
        build.mkdir()
        (build / 'CMakeCache.txt').write_text('configuration')
        (build / 'reader-screenshot-tour').write_bytes(b'binary')
        output = self.cache / 'build/screenshots'
        output.mkdir(parents=True)
        previous = dict(width=1264, height=1680, screens=[], complete=True)
        (output / 'manifest.json').write_text(json.dumps(previous))
        (output / 'index.html').write_text('previous gallery')
        book = self.cache / 'book.epub'
        book.write_bytes(self.payload)
        with patch.object(runner, 'ROOT', self.cache), patch.object(runner, 'cached_book', return_value=book), \
                patch.object(runner.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'capture')):
            with self.assertRaisesRegex(ValueError, 'reading failed'):
                runner.main(['--no-build', '--build-dir', str(build),
                             '--group', 'reading', '--theme', 'light'])
        self.assertEqual((output / 'index.html').read_text(), 'previous gallery')
        self.assertEqual(json.loads((output / 'manifest.json').read_text()), previous)
        self.assertFalse((output / '.capture.lock').exists())
        self.assertTrue((output / 'capture-error.log').is_file())
        self.assertFalse(list(output.glob('.capture-*')))

    def test_report_marks_partial_run_and_escapes_labels(self):
        manifest = dict(complete=False, groups=['settings'], width=1264, height=1680,
                        screens=[dict(name='<unfinished>', group='settings', file='light/settings/display.png')])
        image = self.cache / "light/settings/display.png"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"fake PNG for HTML embedding")
        runner.report(self.cache, manifest)
        self.assertEqual(json.loads((self.cache / 'manifest.json').read_text()), manifest)
        page = (self.cache / 'index.html').read_text()
        self.assertIn('This capture run is incomplete.', page)
        self.assertIn('&lt;unfinished&gt;', page)
        self.assertIn('data:image/png;base64,', page)
        self.assertIn('data:font/ttf;base64,', page)
        self.assertNotRegex(page, r'(?:src|href)="(?:https?:|light/|dark/)')
        self.assertNotIn('@@', page)


if __name__ == '__main__':
    unittest.main()
