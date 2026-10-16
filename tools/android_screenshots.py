#!/usr/bin/env python3
"""Capture Android-only UI on a disposable emulator in the shared gallery."""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import shutil
import tempfile
import subprocess
import time
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'com.nicoverbruggen.kobalt'
ACTIVITY = PACKAGE + '/org.kobalt.android.KobaltActivity'
PROFILES = {'phone': (1080, 2400, 420, 0), 'reader-seven-inch': (1264, 1680, 300, 0),
            'tablet': (1600, 2560, 320, 0), 'phone-landscape': (1080, 2400, 420, 1)}
READER_PROFILES = {**PROFILES, 'phone-landscape': (1080, 2400, 380, 1),
                   'phone-hole-cutout': (1080, 2400, 440, 0),
                   'phone-corner-cutout': (1080, 2400, 400, 0),
                   'tablet-landscape': (1600, 2560, 280, 1)}
CUTOUTS = {'phone-hole-cutout': 'com.android.internal.display.cutout.emulation.hole',
           'phone-corner-cutout': 'com.android.internal.display.cutout.emulation.corner'}
GESTURAL_NAV = 'com.android.internal.systemui.navbar.gestural'
GROUP = 'android-device'


def display_size(profile):
    width, height, _, turn = profile
    return (height, width) if turn % 2 else (width, height)


def cutout_insets(text):
    matches = re.findall(r'DisplayCutout\{insets=Rect\((\d+),\s*(\d+)\s*-\s*(\d+),\s*(\d+)\)', text)
    if not matches:
        raise ValueError('Android did not report current display cutout safe insets')
    values = {tuple(map(int, match)) for match in matches}
    if len(values) != 1:
        raise ValueError('Android reported ambiguous display cutout safe insets')
    return values.pop()


SLOT_NAMES = ('top_left', 'top_center', 'top_right', 'bottom_left', 'bottom_center', 'bottom_right')
PRESET_SLOTS = {
    'default': ('time', 'chapter', 'chapter_page', 'nothing', 'book_page', 'nothing'),
    'alternative_a': ('time', 'chapter_and_page', 'nothing', 'nothing', 'title_and_page', 'nothing'),
    'alternative_b': ('nothing', 'time', 'nothing', 'chapter_pages_left', 'nothing', 'nothing'),
}


def reader_chrome(settings):
    """Return (header size, footer size, progress bar), as headerfooter::select does.

    A size is None when its strip is not drawn: Off, or no slot shows anything.
    """
    def layout(slots, header='small', footer='small', bar=True):
        return dict(slots=list(slots), header_size=header, footer_size=footer, progress_bar=bar)

    def custom(fallback):
        saved = settings.get('header_footer_custom')
        if not isinstance(saved, dict):
            return fallback
        result = dict(fallback, slots=[saved.get(name, slot) for name, slot in zip(SLOT_NAMES, fallback['slots'])])
        for key in ('header_size', 'footer_size'):
            if saved.get(key) in ('off', 'small', 'medium', 'large'):
                result[key] = saved[key]
        if isinstance(saved.get('progress_bar'), bool):
            result['progress_bar'] = saved['progress_bar']
        return result

    standard = layout(PRESET_SLOTS['default'])
    preset = settings.get('header_footer')
    if isinstance(preset, str):
        chosen = custom(standard) if preset == 'custom' else layout(
            PRESET_SLOTS.get(preset, PRESET_SLOTS['default']), bar=preset != 'alternative_b')
    else:
        header, clock = settings.get('show_header', True), settings.get('show_clock', True)
        def shown(key): return settings.get(key) if settings.get(key) in ('medium', 'large') else 'small'
        slots = ['time' if clock else 'nothing'] + (['chapter', 'chapter_page'] if header else ['nothing'] * 2)
        legacy = layout(slots + ['nothing', 'book_page', 'nothing'],
                        shown('header_text_size') if header or clock else 'off',
                        shown('footer_text_size') if settings.get('show_footer', True) else 'off',
                        settings.get('show_progress_bar', True))
        chosen = legacy if legacy == standard else custom(legacy)
    def strip(size, slots): return size if size != 'off' and any(slot != 'nothing' for slot in slots) else None
    return (strip(chosen['header_size'], chosen['slots'][:3]), strip(chosen['footer_size'], chosen['slots'][3:]),
            chosen['progress_bar'])


def reader_geometry(event, density, settings):
    """Reconstruct the surface from resizeText's engine viewport and reading chrome.

    ReadingPage::topHeight/bottomHeight and metrics::reading own these rules.
    Android uses densityDpi, clamped to 100..600, for ui::px before construction.
    """
    if event.get('event') != 'reader-resize-text' or event.get('target') != 'main':
        raise ValueError('Geometry evidence is not a main reader resize')
    density = max(100, min(600, density))
    def px(design): return int(design * density / 300 + .5)
    def strip(size): return px({'medium': 64, 'large': 80}.get(size, 48)) if size else 0
    header, footer, bar = reader_chrome(settings)
    top = strip(header)
    bottom = strip(footer) + (px(24) if bar else 0)
    return dict(event, width=event.get('viewport_width', 0),
                height=event.get('viewport_height', 0) + top + bottom,
                reading_top=top, reading_bottom=bottom,
                geometry_source='reader-resize-text plus source-defined reading chrome')


def validate_reader_geometry(size, insets, geometry, require_cutout=False):
    width, height = size
    left, top, right, bottom = insets
    expected = (width - left - right, height - top - bottom)
    actual = (geometry.get('width', 0), geometry.get('height', 0))
    if min(expected) <= 0 or min(insets) < 0 or (require_cutout and not any(insets)):
        raise ValueError('Invalid or absent required cutout safe area')
    # Qt rounds logical safe margins back to physical pixels at fractional DPR.
    if any(abs(a - b) > 2 for a, b in zip(actual, expected)):
        raise ValueError(f'Reader viewport {actual} does not match safe display area {expected}')
    if geometry.get('viewport_width') != actual[0] or not 0 < geometry.get('viewport_height', 0) <= actual[1]:
        raise ValueError('Reading image viewport is inconsistent with the reader surface')
    return (left, top, left + actual[0], top + actual[1])


def input_point(bounds, fraction):
    left, top, right, bottom = bounds
    if not 0 < fraction < 1 or right <= left or bottom <= top:
        raise ValueError('Invalid reader input bounds')
    return (round(left + (right - left) * fraction), (top + bottom) // 2)



def startup_actions_visible(nodes, size):
    width, height = size
    actions = {name: False for name in ('androidOpenBook', 'androidExit')}
    for node in nodes:
        for name in actions:
            if not node.get('resource-id', '').endswith('.' + name): continue
            bounds = list(map(int, re.findall(r'-?\d+', node.get('bounds', ''))))
            actions[name] = (len(bounds) == 4 and 0 <= bounds[0] < bounds[2] <= width
                             and 0 <= bounds[1] < bounds[3] <= height)
    return all(actions.values())


def capture(serial, apk, book, output, profiles=tuple(PROFILES), reader_checks=False):
    if not serial.startswith('emulator-'):
        raise ValueError('Android screenshot captures require a disposable emulator, never a physical device')
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if not sdk:
        raise ValueError('Set ANDROID_HOME to the installed Android SDK')
    adb = [str(Path(sdk) / 'platform-tools/adb'), '-s', serial]

    def run(*args, check=True):
        return subprocess.run([*adb, *args], check=check, capture_output=True, text=True, timeout=40).stdout.strip()

    def wait(predicate, description, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = predicate()
            if isinstance(result, ET.Element) or result: return result
            time.sleep(.2)
        raise ValueError('Timed out waiting for ' + description)

    def nodes():
        run('shell', 'uiautomator', 'dump', '/sdcard/kobalt-gallery.xml')
        return list(ET.fromstring(run('shell', 'cat', '/sdcard/kobalt-gallery.xml')).iter('node'))

    def find(name):
        return next((n for n in nodes() if n.get('resource-id', '').endswith('.' + name)), None)

    def tap(name, hold=False):
        node = wait(lambda: find(name), name)
        area = list(map(int, re.findall(r'-?\d+', node.get('bounds', ''))))
        display = struct.unpack('>II', subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=20)[16:24])
        if (len(area) != 4 or node.get('enabled') == 'false'
                or not 0 <= area[0] < area[2] <= display[0]
                or not 0 <= area[1] < area[3] <= display[1]):
            raise ValueError('Action is outside the visible screen: ' + name)
        x, y = str((area[0]+area[2])//2), str((area[1]+area[3])//2)
        if hold: run('shell', 'input', 'swipe', x, y, x, y, '1000')
        else: run('shell', 'input', 'tap', x, y)

    def write(path, data):
        code = 'printf %s ' + shlex.quote(base64.b64encode(data).decode()) + ' | base64 -d > ' + shlex.quote(path)
        run('shell', 'run-as', PACKAGE, 'sh', '-c', shlex.quote(code))

    def recent(metadata):
        write('files/config/last-book.json', json.dumps(metadata).encode())

    current_profile = None
    validations = []
    active_reader_profile = 'startup'

    def launch():
        run('shell', 'am', 'force-stop', PACKAGE)
        run('shell', 'input', 'keyevent', '224')
        run('shell', 'am', 'start', '-W', '-n', ACTIVITY)
        wait(lambda: find('androidOpenBook'), 'startup actions')
        if current_profile:
            width, height, _, turn = current_profile
            run('shell', 'cmd', 'window', 'user-rotation', 'lock', str(turn))
            expected = (height, width) if turn % 2 else (width, height)
            wait(lambda: struct.unpack('>II', subprocess.check_output(
                [*adb, 'exec-out', 'screencap', '-p'], timeout=20)[16:24]) == expected,
                'profile display dimensions')

    shots = []
    created = datetime.now(timezone.utc).isoformat()
    apk_hash = hashlib.sha256(apk.read_bytes()).hexdigest()

    def validation_report(error=None):
        records = validations + ([dict(profile=active_reader_profile, status='failed', error=str(error))] if error else [])
        output.mkdir(parents=True, exist_ok=True)
        (output / 'android-reader-validation.json').write_text(json.dumps(dict(
            created_at=created, binary_sha256=apk_hash, book_sha256=hashlib.sha256(book.read_bytes()).hexdigest(),
            profiles=records), indent=2) + '\n')

    def shot(profile, scene):
        previous = None
        end = time.monotonic() + 15
        while time.monotonic() < end:
            pixels = subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=20)
            if pixels == previous: break
            previous = pixels
            time.sleep(.25)
        else: raise ValueError('Screen did not settle: ' + scene)
        width, height = struct.unpack('>II', pixels[16:24])
        file = Path('android') / GROUP / (profile + '-' + scene + '.png')
        (output / file).parent.mkdir(parents=True, exist_ok=True)
        (output / file).write_bytes(pixels)
        shots.append(dict(group=GROUP, file=file.as_posix(), name=profile + ' · ' + scene,
            theme='light', width=width, height=height, platform='android-emulator',
            captured_at=created, binary_sha256=apk_hash, sha256=hashlib.sha256(pixels).hexdigest(),
            display_density=current_profile[2] if current_profile else None))
        print('CAPTURED Android ' + profile + '/' + scene, flush=True)

    wait(lambda: run('shell', 'getprop', 'sys.boot_completed') == '1', 'Android boot')
    run('install', '-r', str(apk))
    original = {kind: run('shell', 'wm', kind) for kind in ('size', 'density')}
    original_rotation = run('shell', 'cmd', 'window', 'user-rotation').split()
    rotation = {key: run('shell', 'settings', 'get', 'system', key)
                for key in ('accelerometer_rotation', 'user_rotation', 'screen_off_timeout')}
    immersive = run('shell', 'settings', 'get', 'secure', 'immersive_mode_confirmations')
    overlays = run('shell', 'cmd', 'overlay', 'list', '--user', '0') if reader_checks else ''
    cutout_overlays = {name: enabled == 'x' for enabled, name in
                      re.findall(r'\[([ x])\]\s+(com\.android\.internal\.display\.cutout\.emulation\.\S+)', overlays)}
    navigation_overlays = {name: enabled == 'x' for enabled, name in
                           re.findall(r'\[([ x])\]\s+(com\.android\.internal\.systemui\.navbar\.\S+)', overlays)}
    try:
        run('shell', 'settings', 'put', 'system', 'screen_off_timeout', '1800000')
        run('shell', 'settings', 'put', 'secure', 'immersive_mode_confirmations', 'confirmed')
        launch()
        run('shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files/books', 'files/config')
        run('push', str(book), '/data/local/tmp/kobalt-gallery.epub')
        run('shell', 'run-as', PACKAGE, 'cp', '/data/local/tmp/kobalt-gallery.epub', 'files/books/gallery.epub')
        with zipfile.ZipFile(book) as archive:
            container = ET.fromstring(archive.read('META-INF/container.xml'))
            opf = next(n.attrib['full-path'] for n in container.iter() if n.tag.endswith('rootfile'))
            metadata = ET.fromstring(archive.read(opf))
            title = next((n.text for n in metadata.iter() if n.tag.endswith('}title')), book.stem)
            author = next((n.text for n in metadata.iter() if n.tag.endswith('}creator')), '')
        last = {'path': '/data/user/0/' + PACKAGE + '/files/books/gallery.epub', 'title': title, 'author': author}
        # Let the app decode and save the real cover, rather than fabricating one.
        run('shell', 'wm', 'size', '1264x1680'); run('shell', 'wm', 'density', '300')
        recent(last); launch(); tap('androidLastBook')
        wait(lambda: 'Kobalt frame ready' in run('logcat', '-d', '--pid=' + run('shell', 'pidof', PACKAGE), '-s', 'default:I'), 'first reading page', 60)
        last = json.loads(run('shell', 'run-as', PACKAGE, 'cat', 'files/config/last-book.json'))
        run('shell', 'am', 'force-stop', PACKAGE)
        for profile in profiles:
            current_profile = PROFILES[profile]
            width, height, density, turn = PROFILES[profile]
            run('shell', 'wm', 'size', f'{width}x{height}'); run('shell', 'wm', 'density', str(density))
            run('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
            run('shell', 'settings', 'put', 'system', 'user_rotation', str(turn))
            run('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/config/last-book.json')
            launch(); shot(profile, 'no-recent-book')
            recent({key: value for key, value in last.items() if key != 'cover'})
            launch(); shot(profile, 'last-book-no-cover')
            recent(last); launch(); shot(profile, 'last-book-cover')
            if turn:
                run('shell', 'input', 'swipe', str(height//2), str(width*3//4),
                    str(height//2), str(width//4), '400')
                wait(lambda: startup_actions_visible(nodes(), (height, width)), 'scrolled startup actions')
                shot(profile, 'startup-actions-scrolled')
            # Portrait profiles have the card visible without scrolling.
            if turn == 0:
                before = subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=20)
                tap('androidLastBook', hold=True)
                # Qt's graphics proxy does not export this overlay to Android accessibility.
                wait(lambda: subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=20) != before, 'removal overlay paint')
                shot(profile, 'remove-recent-confirmation')
            if profile == 'phone':
                write('files/books/gallery-invalid.epub', b'not an EPUB')
                recent({'path': last['path'].replace('gallery.epub', 'gallery-invalid.epub'), 'title': 'Unreadable book'})
                launch(); tap('androidLastBook')
                wait(lambda: any(n.get('text') == "Couldn't open this book" or n.get('content-desc') == "Couldn't open this book" for n in nodes()), 'opening error')
                shot(profile, 'opening-error')
                recent(last); launch(); tap('androidOpenBook')
                wait(lambda: any('documentsui' in n.get('package', '') for n in nodes()), 'system document picker')
                shot(profile, 'choose-book')
                # The picker is a separate activity and survives force-stopping Kobalt.
                run('shell', 'input', 'keyevent', '4')
                wait(lambda: find('androidOpenBook'), 'startup after closing the document picker')
        if reader_checks:
            checkpoint_name = hashlib.sha256(('file://' + last['path']).encode()).hexdigest() + '.json'

            def checkpoint():
                text = run('shell', 'run-as', PACKAGE, 'cat', 'files/config/' + checkpoint_name, check=False)
                try: return json.loads(text)
                except json.JSONDecodeError: return {}

            def diagnostics():
                text = run('shell', 'run-as', PACKAGE, 'cat', 'files/diagnostics/events.jsonl')
                return [json.loads(line) for line in text.splitlines() if line.strip()]

            def rotation_to(turn):
                run('shell', 'cmd', 'window', 'user-rotation', 'lock', str(turn))
                expected = display_size((*current_profile[:3], turn))
                wait(lambda: struct.unpack('>II', subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=20)[16:24]) == expected,
                     'reader rotation dimensions')

            def reader_ready():
                pid = run('shell', 'pidof', PACKAGE)
                return pid and 'Kobalt frame ready' in run('logcat', '-d', '--pid=' + pid, '-s', 'default:I')

            for profile, current_profile in READER_PROFILES.items():
                active_reader_profile = profile
                width, height, density, turn = current_profile
                for overlay in cutout_overlays:
                    run('shell', 'cmd', 'overlay', 'disable', '--user', '0', overlay)
                gestural = profile == 'phone-hole-cutout' or profile.endswith('-landscape')
                if gestural and GESTURAL_NAV not in navigation_overlays:
                    raise ValueError('Required gestural navigation overlay is unavailable')
                for overlay, enabled in navigation_overlays.items():
                    desired_navigation = overlay == GESTURAL_NAV if gestural else enabled
                    run('shell', 'cmd', 'overlay', 'enable' if desired_navigation else 'disable', '--user', '0', overlay)
                desired = CUTOUTS.get(profile)
                if desired:
                    if desired not in cutout_overlays: raise ValueError('Required emulator cutout overlay is unavailable: ' + desired)
                    run('shell', 'cmd', 'overlay', 'enable', '--user', '0', desired)
                run('shell', 'wm', 'size', f'{width}x{height}'); run('shell', 'wm', 'density', str(density))
                recent(last); launch()
                run('logcat', '-c'); tap('androidLastBook')
                wait(reader_ready, 'reader frame ready', 90)
                # A roundtrip produces existing reader geometry diagnostics; the
                # checkpoint path intentionally does not export UI geometry.
                event_count = len(diagnostics())
                rotation_to(1 if turn == 0 else 0)
                wait(lambda: any(e.get('event') == 'reader-resize-text' and e.get('target') == 'main'
                                 for e in diagnostics()[event_count:]), 'reader rotation relayout')
                wait(lambda: any(e.get('event') == 'render-end' and not e.get('failed')
                                 for e in diagnostics()[event_count:]), 'rotated reader frame')
                event_count = len(diagnostics()); rotation_to(turn)
                wait(lambda: any(e.get('event') == 'render-end' and not e.get('failed')
                                 for e in diagnostics()[event_count:]), 'restored reader frame')
                safe = cutout_insets(run('shell', 'dumpsys', 'window', 'displays'))
                settings_text = run('shell', 'run-as', PACKAGE, 'cat', 'files/config/reader-settings.json', check=False)
                settings = json.loads(settings_text) if settings_text else {}
                observed_geometry = {}
                def safe_geometry():
                    nonlocal observed_geometry
                    events = diagnostics()[event_count:]
                    event = next((e for e in reversed(events)
                                  if e.get('event') == 'reader-resize-text' and e.get('target') == 'main'), None)
                    if not event: return None
                    observed_geometry = reader_geometry(event, density, settings)
                    try: validate_reader_geometry(display_size(current_profile), safe, observed_geometry, bool(desired))
                    except ValueError: return None
                    return observed_geometry
                try: geometry = wait(safe_geometry, 'restored safe reader geometry')
                except ValueError as error:
                    raise ValueError(f'{error}: observed {observed_geometry}, cutout {safe}') from error
                bounds = validate_reader_geometry(display_size(current_profile), safe, geometry, bool(desired))
                shot(profile, 'reader-ready')
                origin = wait(lambda: checkpoint().get('position'), 'saved reader origin')
                forward = input_point(bounds, .85)
                run('shell', 'input', 'tap', *map(str, forward))
                def advanced_position():
                    value = checkpoint().get('position')
                    return value if value and value != origin else None
                advanced = wait(advanced_position, 'page tap checkpoint')
                shot(profile, 'reader-page-forward')
                backward = input_point(bounds, .15)
                run('shell', 'input', 'tap', *map(str, backward))
                wait(lambda: checkpoint().get('position') == origin, 'reverse page tap restores position')
                shot(profile, 'reader-page-returned')
                # Force-stop only after the saved checkpoint above has arrived.
                recent(last); launch(); run('logcat', '-c'); tap('androidLastBook')
                wait(reader_ready, 'reopened reader ready', 90)
                shot(profile, 'reader-reopened')
                # A retained file alone cannot prove the reopened reader loaded
                # it. Repeat the same actual touch transition from that reader.
                run('shell', 'input', 'tap', *map(str, forward))
                wait(lambda: checkpoint().get('position') == advanced, 'reopened reader advances from saved origin')
                run('shell', 'input', 'tap', *map(str, backward))
                wait(lambda: checkpoint().get('position') == origin, 'reopened reader returns to saved origin')
                validations.append(dict(profile=profile, status='passed', density=density,
                    navigation='gestural' if gestural else 'original',
                    display=list(display_size(current_profile)), cutout_insets=list(safe), reader_geometry=geometry,
                    input_points={'forward': forward, 'backward': backward},
                    origin=origin, advanced=advanced,
                    assertions=['reader-safe-viewport', 'physical-touch-forward', 'physical-touch-backward', 'saved-position-reopen']))
            validation_report()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        if reader_checks: validation_report(error)
        raise
    finally:
        run('shell', 'am', 'force-stop', PACKAGE, check=False)
        if reader_checks:
            for overlay, enabled in navigation_overlays.items():
                run('shell', 'cmd', 'overlay', 'enable' if enabled else 'disable', '--user', '0', overlay)
            for overlay, enabled in cutout_overlays.items():
                run('shell', 'cmd', 'overlay', 'enable' if enabled else 'disable', '--user', '0', overlay)
        run('shell', 'settings', 'delete' if immersive == 'null' else 'put', 'secure',
            'immersive_mode_confirmations', *([] if immersive == 'null' else [immersive]))
        for key, value in rotation.items():
            run('shell', 'settings', 'delete' if value == 'null' else 'put', 'system', key,
                *([] if value == 'null' else [value]))
        for kind, value in original.items():
            override = re.search(r'Override \w+: (\S+)', value)
            run('shell', 'wm', kind, override.group(1) if override else 'reset')
        run('shell', 'cmd', 'window', 'user-rotation', *original_rotation)
    return shots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True, help='disposable emulator serial; its Kobalt recent-book entry is replaced')
    parser.add_argument('--apk', type=Path, default=ROOT / 'build/android/arm64-v8a/app/android-build/kobalt_android.apk')
    parser.add_argument('--book', type=Path, required=True, help='public-domain EPUB with a real cover')
    parser.add_argument('--reader-checks', action='store_true', help='validate physical reader touch, safe viewport and saved reopening on seven emulator profiles')
    args = parser.parse_args()
    from native_screenshots import report
    output = ROOT / 'build/screenshots'
    manifest = json.loads((output / 'manifest.json').read_text())
    lock = output / '.capture.lock'
    try: fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError: raise ValueError('Another capture owns ' + str(lock))
    os.close(fd)
    try:
        with tempfile.TemporaryDirectory(prefix='kobalt-android-captures-') as temporary:
            staging = Path(temporary)
            try:
                shots = capture(args.serial, args.apk, args.book, staging, reader_checks=args.reader_checks)
            except (ValueError, OSError, subprocess.SubprocessError):
                validation = staging / 'android-reader-validation.json'
                if validation.is_file(): shutil.copy2(validation, output / validation.name)
                raise
            shutil.copytree(staging / 'android', output / 'android', dirs_exist_ok=True)
            if args.reader_checks:
                shutil.copy2(staging / 'android-reader-validation.json', output / 'android-reader-validation.json')
        manifest['screens'] = [s for s in manifest['screens'] if s['group'] != GROUP] + shots
        manifest['groups'] = list(dict.fromkeys(manifest['groups'] + [GROUP]))
        report(output, manifest)
    finally: lock.unlink()
    print(output / 'index.html')


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, subprocess.SubprocessError) as error: raise SystemExit(str(error))
