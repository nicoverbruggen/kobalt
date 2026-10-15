#!/usr/bin/env python3
"""Capture the real reader widgets and engine with isolated, deterministic host fixtures."""
import argparse
import base64
import hashlib
import html
import json
import os
import re
import shutil
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.request
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
BOOKS = ROOT / 'tests/fixtures/screenshot_books.json'
GROUPS = {
    'android-startup': ('alice', 'Android startup, last-book cover and no-cover states'),
    'reading': ('alice', 'Reading page and menu'),
    'bluetooth': ('alice', 'Bluetooth status beside the battery, with controls shown and hidden'),
    'settings': ('alice', 'Every settings tab, restore confirmation and refresh picker'),
    'bionic': ('alice', 'Word-prefix emphasis with bold text'),
    'typography': ('alice', 'Text panel, font picker and appearance previews'),
    'embedded-fonts': ('embedded-fonts', 'Publisher font count and selection with a real embedded family'),
    'layout': ('alice', 'Page layout, header/footer size pickers and margin preview'),
    'layout-system-rotation': ('alice', 'Layout controls without manual rotation, as on Android'),
    'chapters': ('alice', 'Every page of the chapter picker'),
    'links': ('alice', 'Book contents, link actions, destination and return row'),
    'footnotes': ('maths', 'Footnote reference, preview, full passage and return row'),
    'dictionary': ('alice', 'English selection, definition, dictionary picker and error states'),
    'search': ('alice', 'Search editor, results, pagination and no matches'),
    'bookmarks': ('alice', 'Empty list, ribbon, saved bookmark and removal prompt'),
    'light': ('alice', 'Emulated reader frontlight controls'),
    'stats': ('alice', 'Chapter graph'),
    'stats-dense': ('moby-dick', 'Moby Dick with more than one hundred chapters in the dense graph'),
    'about': ('alice', 'Acknowledgments and an example license'),
    'dialogs': ('alice', 'Book-finished and book-opening failure confirmations'),
    'sync': ('alice', 'Emulated incoming position, missing position and sync error'),
    'maths': ('maths', 'Four consecutive pages with equations'),
    'international-arabic': ('public-ar', 'Arabic prose and right-to-left chapters'),
    'international-hebrew': ('public-he', 'Hebrew prose and vowel marks'),
    'international-writing-modes': ('public-ja-writing-modes', 'Japanese vertical and horizontal chapters'),
    'international-greek': ('public-el', 'Greek prose and left-to-right chapters'),
    'international-cyrillic': ('public-ru', 'Russian prose'),
    'international-armenian': ('public-hy', 'Armenian prose'),
    'international-georgian': ('public-ka', 'Georgian prose'),
    'international-hindi': ('public-hi', 'Hindi prose'),
    'international-bengali': ('public-bn', 'Bengali prose'),
    'international-tamil': ('public-ta', 'Tamil prose'),
    'international-thai': ('public-th', 'Thai prose'),
    'international-latin': ('public-vi', 'Vietnamese prose'),
    'international-japanese': ('public-ja', 'Japanese prose and ruby'),
    'international-korean': ('public-ko', 'Korean prose'),
    'international-chinese-simplified': ('public-zh-Hans', 'Simplified Chinese prose'),
    'international-chinese-traditional': ('public-zh-Hant', 'Traditional Chinese prose'),
    'international-german': ('public-de', 'German prose'),
    'international-korean-vertical': ('public-ko-writing-modes', 'Korean prose in vertical columns'),
    'international-chinese-simplified-vertical': ('public-zh-Hans-writing-modes', 'Simplified Chinese prose in vertical columns'),
    'international-chinese-traditional-vertical': ('public-zh-Hant-writing-modes', 'Traditional Chinese prose in vertical columns'),

}
EXCLUSIONS = {
    'platform_ui': 'Native OS file pickers, keyboards, sleep, USB and PIN screens belong to the host platform.',
    'transient_ui': 'Opening progress depends on worker timing. Delayed relayout indicators use controlled elapsed-time captures and behavioral tests.',
    'platform_failures': 'Platform authorization and book-opening failures remain in the integration tests.',
    'content_variations': 'Font picker captures cover its first three pages; licenses use one example. These are UI states, not every possible item.',
}


def digest(path):
    with path.open('rb') as stream:
        checksum = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
        return checksum.hexdigest()


def cached_book(spec, cache, offline=False, maximum_bytes=64 * 1024 * 1024):
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / spec['filename']
    if destination.exists():
        if destination.is_symlink() or digest(destination) != spec['sha256']:
            raise ValueError(f'Cached EPUB differs from its pinned reference: {destination}')
        return destination
    if offline:
        raise ValueError(f'EPUB is not cached: {destination}. Run once without --offline.')
    if not spec['download'].startswith('https://'):
        raise ValueError('Fixture downloads require HTTPS')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=cache, prefix='.epub-', delete=False) as stream:
            temporary = Path(stream.name)
            with urllib.request.urlopen(spec['download'], timeout=60) as response:
                if not response.geturl().startswith('https://'):
                    raise ValueError('Fixture download redirected away from HTTPS')
                total = 0
                while chunk := response.read(1024 * 1024):
                    total += len(chunk)
                    if total > maximum_bytes:
                        raise ValueError('Fixture download exceeds its size limit')
                    stream.write(chunk)
        if digest(temporary) != spec['sha256']:
            raise ValueError('Downloaded EPUB changed upstream. Review the book and update its pinned reference; no screenshot run was started.')
        temporary.replace(destination)
        return destination
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def selected_groups(groups, screens, include_android=False):
    defaults = [group for group in GROUPS if group != 'android-startup']
    selected = set(groups)
    if 'all' in selected:
        selected.remove('all')
        selected.update(defaults)
    if 'international' in selected:
        selected.remove('international')
        selected.update(group for group in GROUPS if group.startswith('international-'))
    for screen in screens:
        parts = screen.split('/')
        if len(parts) != 2 or not all(parts) or any(part in ('.', '..') for part in parts):
            raise ValueError('Use --screen GROUP/NAME')
        selected.add(parts[0])
    unknown = selected - GROUPS.keys()
    if unknown:
        raise ValueError('Unknown screenshot groups: ' + ', '.join(sorted(unknown)))
    if not selected:
        selected.update(defaults)
    if include_android:
        selected.add('android-startup')
    return [group for group in GROUPS if group in selected]


def capture_folder(ppi, theme, group):
    # Keep the original default paths for existing gallery links.
    return Path(theme) / group if ppi == 300 else Path(f'ppi-{ppi}') / theme / group


def report(output, manifest):
    # Older manifests include a component-only tour that repeats the reader captures.
    manifest = dict(manifest)
    manifest['groups'] = [group for group in manifest['groups'] if group != 'native-ui']
    actual_android = any(shot['group'] == 'android-device' for shot in manifest['screens'])
    manifest['screens'] = [shot for shot in manifest['screens'] if shot['group'] != 'native-ui'
        and not (actual_android and shot['group'] == 'android-startup')]

    def embedded(path, mime):
        return 'data:' + mime + ';base64,' + base64.b64encode(path.read_bytes()).decode('ascii')

    def label(value):
        names = {'light': 'Light', 'native-ui': 'Native UI', 'stats': 'Reading statistics',
                 'dialogs': 'Confirmations', 'maths': 'Equations', 'international': 'International books',
                 'cyrillic': 'Russian', 'latin': 'Vietnamese', 'writing-modes': 'Japanese writing modes'}
        return names.get(value, value.replace('-', ' ').capitalize())

    def section(shot):
        group, scene = shot['group'], Path(shot['file']).stem
        if group == 'android-device': return 'android-startup'
        if group.startswith('international-'):
            if scene == 'chapters': return 'chapters'
            if scene == 'supplemental-fonts': return 'text'
            if any(word in group for word in ('japanese', 'korean', 'chinese', 'writing-modes')):
                return 'cjk-vertical' if 'vertical' in group or group == 'international-writing-modes' else 'cjk-horizontal'
            return 'reading-multilingual'
        if group == 'reading':
            return 'settings' if scene == 'menu' else 'text' if scene == 'supplemental-fonts' else 'reading'
        if group == 'maths':
            return 'reading-mathematics'
        if group == 'stats-dense': return 'stats'
        if group in ('typography', 'embedded-fonts', 'bionic'):
            return 'text'
        if group in ('about', 'dialogs', 'sync'):
            return 'settings'
        return group

    section_names = {'android-startup': 'Android · Startup', 'reading': 'Reading · Basic', 'reading-multilingual': 'Reading · Multilingual',
                     'reading-mathematics': 'Reading · Mathematics', 'cjk-horizontal': 'CJK · Horizontal',
                     'cjk-vertical': 'CJK · Vertical', 'links': 'Links', 'footnotes': 'Footnotes', 'dictionary': 'Dictionary',
                     'bookmarks': 'Bookmarks', 'chapters': 'Chapters', 'text': 'Text', 'layout': 'Layout',
                     'light': 'Light', 'stats': 'Stats', 'search': 'Search', 'settings': 'Overflow and settings'}
    section_descriptions = {'android-startup': ('Android emulator captures of startup, recent-book removal, errors and document picking' if actual_android else 'Native previews of Android startup and last-book states'), 'reading': 'Basic reading controls and page layouts',
                            'reading-multilingual': 'One reading page per language',
                            'reading-mathematics': 'Equations from Calculus Made Easy',
                            'cjk-horizontal': 'Chinese, Japanese and Korean prose in horizontal layout',
                            'cjk-vertical': 'Public-domain Chinese, Japanese and Korean chapters presented vertically',
                            'text': 'Text controls, reading fonts and language font choices',
                            'chapters': 'Chapter lists from the reading and international fixtures',
                            'settings': 'Reading menu, settings, confirmations, sync and acknowledgments'}
    present = {section(shot) for shot in manifest['screens']}
    display_groups = [group for group in section_names if group in present]
    display_groups.extend(sorted(present - section_names.keys()))
    sections = []
    primary_screens = {'reading': 'reading', 'typography': 'text', 'layout': 'layout',
                       'links': 'contents', 'footnotes': 'reference', 'dictionary': 'selection-page',
                       'search': 'query', 'embedded-fonts': 'detected'}
    group_order = {group: index for index, group in enumerate(GROUPS)}
    for group in display_groups:
        cards = []
        shots = sorted((shot for shot in manifest['screens'] if section(shot) == group),
                       key=lambda shot: (group_order.get(shot['group'], len(GROUPS)),
                                         Path(shot['file']).stem != primary_screens.get(shot['group'],
                                             'vertical-chapter' if shot['group'] == 'international-writing-modes' else 'reading'),
                                         Path(shot['file']).stem, shot.get('theme', shot['file'].split('/')[0]) == 'dark'))
        for shot in shots:
            theme = shot.get('theme', shot['file'].split('/')[0])
            name = label(Path(shot['file']).stem)
            if shot['group'].startswith('international-'):
                name = label(shot['group'].removeprefix('international-')) + ' · ' + name
            elif shot['group'] == 'reading' and group == 'reading':
                name = 'Alice · ' + name
            elif shot['group'] == 'reading' and Path(shot['file']).stem == 'menu':
                name = 'Reading menu'
            elif shot['group'] == 'maths':
                name = 'Calculus Made Easy ' + Path(shot['file']).stem.removeprefix('equations-')
            if shot['group'] == 'stats-dense': name = 'Moby Dick · Dense chapter graph'
            density = shot.get('ppi', 300)
            density_label = f' · {density} ppi' if density != 300 else ''
            title = html.escape(f'{section_names.get(group, label(group))} · {name} · {label(theme)}{density_label}', quote=True)
            capture_group = shot['group']
            description = GROUPS.get(capture_group, ('', ''))[1]
            search = html.escape(f'{group} {name} {theme} {capture_group} {description} {density} ppi'.lower(), quote=True)
            scene = html.escape(capture_group + '/' + Path(shot['file']).stem + (f'@{density}' if density != 300 else ''), quote=True)
            filename = html.escape(capture_group + '-' + Path(shot['file']).name, quote=True)
            image = embedded(output / shot['file'], 'image/png')
            cards.append(f'<figure data-group="{html.escape(group, quote=True)}" data-theme="{html.escape(theme, quote=True)}" data-scene="{scene}" data-search="{search}">'
                         f'<button class="capture" type="button" data-filename="{filename}" aria-label="Inspect {title}">'
                         f'<img loading="lazy" src="{image}" alt="{title}" width="{shot.get("width", manifest["width"])}" height="{shot.get("height", manifest["height"])}"></button>'
                         f'<div class="caption"><figcaption>{html.escape(name)}<small>{html.escape(shot["name"])}</small></figcaption>'
                         f'<span class="tag {html.escape(theme, quote=True)}">{html.escape(label(theme) + density_label)}</span></div></figure>')
        description = html.escape(section_descriptions.get(group, GROUPS.get(group, ('', ''))[1]))
        sections.append(f'<section class="group"><div class="group-title"><h2>{html.escape(section_names.get(group, label(group)))}</h2><p>{description}</p></div><div class="screens">{"".join(cards)}</div></section>')
    fonts = []
    for family, filename in (('GallerySans', 'IBMPlexSans-Regular.ttf'), ('GalleryMono', 'IBMPlexMono-Regular.ttf'),
                             ('GallerySerif', 'InstrumentSerif-Regular.ttf')):
        source = embedded(ROOT / 'resources/fonts' / filename, 'font/ttf')
        fonts.append('@font-face{font-family:' + family + ';src:url("' + source + '") format("truetype");font-display:swap}')
    licences = ''.join('<pre>' + html.escape((ROOT / 'resources/notices' / name).read_text()) + '</pre>'
                       for name in ('IBM-Plex-OFL', 'Instrument-Serif-OFL'))
    books = []
    for name, book in manifest.get('books', {}).items():
        title = html.escape(book.get('title', name))
        source = book.get('source', '')
        if source.startswith('https://'):
            title = '<a href="' + html.escape(source, quote=True) + '">' + title + '</a>'
        credit = ' · '.join(html.escape(book[key]) for key in ('author', 'rights') if book.get(key))
        books.append('<p>' + title + ('<br>' + credit if credit else '') + '</p>')
    for entry in manifest.get('dictionaries', {}).values():
        books.append('<p><a href="https://github.com/DanielGregorini/koreader-dicts">' + html.escape(entry['name']) + '</a><br>WordNet and Wiktionary · koreader-dicts v1.2.0 · CC BY-SA 4.0</p>')
    books = ''.join(books)
    values = dict(FONTS='\n'.join(fonts), GROUPS=''.join(sections), COUNT=str(len(manifest['screens'])),
                  NOTICE='' if manifest['complete'] else '<p class="notice">This capture run is incomplete. ' + html.escape(manifest.get('error', 'Captured screens are shown below.')) + '</p>',
                  OPTIONS=''.join(f'<option value="{html.escape(group, quote=True)}">{html.escape(section_names.get(group, label(group)))}</option>' for group in display_groups),
                  CREATED=html.escape(manifest.get('created', 'during this run')),
                  EXCLUSIONS=''.join('<li>' + html.escape(value) + '</li>' for value in manifest.get('exclusions', EXCLUSIONS).values()),
                  BOOKS=books, LICENCES=licences)
    template = Path(__file__).with_name('native_screenshot_gallery.html').read_text()
    page = re.sub(r'@@([A-Z]+)@@', lambda match: values[match[1]], template)
    page = page.replace('<body>', '<body><nav><a href="ui-review.html">Visual review</a> · <a href="../refactor-journal/index.html">Refactor journal</a></nav>', 1)
    temporary = output / '.index.html.tmp'
    temporary.write_text(page, encoding='utf-8')
    temporary.replace(output / 'index.html')
    temporary = output / '.manifest.json.tmp'
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(output / 'manifest.json')


def merge_captures(previous, current, screens):
    if previous is None:
        return current
    if any(previous[key] != current[key] for key in ('width', 'height')):
        raise ValueError('Viewport differs from the current gallery. Run a full sweep to replace it.')
    def replaced(shot):
        selected = {screen for screen in screens if screen.startswith(shot['group'] + '/')}
        return (shot['group'] in current['groups'] and shot['theme'] in current['themes']
                and shot.get('ppi', 300) in current.get('densities', [300])
                and (not selected or shot['group'] + '/' + Path(shot['file']).stem in selected))
    retained = [shot for shot in previous['screens'] if shot['group'] in GROUPS and not replaced(shot)]
    result = dict(current)
    result['screens'] = retained + current['screens']
    result['groups'] = [group for group in GROUPS if any(shot['group'] == group for shot in result['screens'])]
    result['themes'] = [theme for theme in ('light', 'dark') if any(shot['theme'] == theme for shot in result['screens'])]
    result['densities'] = sorted({shot.get('ppi', 300) for shot in result['screens'] if shot['group'] != 'android-device'})
    result['books'] = {**previous.get('books', {}), **current['books']}
    result['dictionaries'] = {**previous.get('dictionaries', {}), **current.get('dictionaries', {})}
    # Retained screenshots keep their own capture time and executable checksum.
    result['complete'] = current['complete'] and (previous['complete'] or not retained)
    return result


def prune_captures(output, manifest):
    keep = {shot['file'] for shot in manifest['screens']}
    for relative in ('light', 'dark', *(path.name for path in output.glob('ppi-*') if path.is_dir())):
        root = output / relative
        if not root.exists():
            continue
        for path in root.rglob('*.png'):
            if path.relative_to(output).as_posix() not in keep:
                path.unlink()
                path.with_suffix('.json').unlink(missing_ok=True)
        for folder in sorted((path for path in root.rglob('*') if path.is_dir()), key=lambda path: len(path.parts), reverse=True):
            if not any(name.startswith(folder.relative_to(output).as_posix() + '/') for name in keep):
                shutil.rmtree(folder)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-android', action='store_true', help='also capture native previews of Android startup screens')
    parser.add_argument('--android-serial', help='also capture Android-only screens on a disposable emulator')
    parser.add_argument('--android-apk', type=Path, default=ROOT / 'build/android/arm64-v8a/app/android-build/kobalt_android.apk')
    parser.add_argument('--gallery', action='store_true', help='regenerate build/screenshots/index.html from its current manifest')
    parser.add_argument('--list', action='store_true', help='list capture groups without building or downloading')
    parser.add_argument('--group', action='append', default=[], help='group to capture; repeat for several, or all')
    parser.add_argument('--screen', action='append', default=[], help='one GROUP/NAME from a previous manifest; repeat as needed')
    parser.add_argument('--theme', choices=('light', 'dark', 'both'), default='both')
    parser.add_argument('--book-cache', type=Path, default=Path(tempfile.gettempdir()) / 'kobalt-screenshot-books')
    parser.add_argument('--offline', action='store_true', help='require previously cached books and build dependencies')
    parser.add_argument('--build-dir', type=Path, help='existing native tests build directory')
    parser.add_argument('--dependency-cache', type=Path, default=ROOT / '.cache/kobalt')
    parser.add_argument('--jobs', type=int, default=2, help='parallel capture processes, default 2; use 1 for a serial run')
    parser.add_argument('--no-build', action='store_true', help='use the existing screenshot executable')
    parser.add_argument('--ppi', type=int, action='append', help='UI and reading density; repeat for multiple densities, default 300')
    parser.add_argument('--width', type=int, default=1264)
    parser.add_argument('--height', type=int, default=1680)
    args = parser.parse_args(argv)
    started = time.monotonic()
    densities = list(dict.fromkeys(args.ppi or [300]))
    if any(ppi <= 0 for ppi in densities): parser.error('--ppi must be positive')
    if args.jobs < 1: parser.error('--jobs must be at least 1')
    output = (ROOT / "build/screenshots").resolve()
    if args.gallery:
        report(output, json.loads((output / "manifest.json").read_text()))
        print(output / "index.html")
        return
    if args.list:
        for group, (_, description) in GROUPS.items(): print(f'{group:22} {description}')
        return
    if args.width < 600 or args.height < 800: parser.error('Use a reader-sized viewport, at least 600 × 800 pixels')
    groups = selected_groups(args.group, args.screen, args.include_android)
    full_groups = selected_groups([], [], args.include_android)
    from run_tests import HOST
    build = (args.build_dir or ROOT / 'build/tests' / HOST / 'tests').resolve()
    if args.build_dir and not (build / 'CMakeCache.txt').is_file():
        raise ValueError('--build-dir must name an existing native tests build')
    if not args.no_build:
        if not args.build_dir:
            # Reconfigure cached builds too, so engine patch changes select a fresh build.
            command = [sys.executable, str(ROOT / 'tools/run_tests.py'), '--output', str(build.parent), '--build-only', '--cache', str(args.dependency_cache)]
            if args.offline: command.append('--offline')
            subprocess.run(command, check=True)
        subprocess.run(['cmake', '--build', str(build), '--target', 'reader-screenshot-tour', '--parallel', '2'], check=True)
    binary = build / 'reader-screenshot-tour'
    if not binary.is_file(): raise ValueError('Build the native screenshot executable first')
    if output.exists() and not (output / 'manifest.json').is_file():
        if any(not item.name.startswith('.capture-') and item.name != '.capture.lock' for item in output.iterdir()):
            raise ValueError('Existing output must be a generated screenshot gallery')
    output.mkdir(parents=True, exist_ok=True)
    references = json.loads(BOOKS.read_text())
    needed = {GROUPS[group][0] for group in groups}
    if args.android_serial: needed.add('alice')
    books = {}
    for name in sorted(needed):
        spec = references[name]
        if 'fixture' in spec:
            path = build / 'fixtures' / spec['fixture']
            if not path.is_file(): raise ValueError('Generated book missing; rebuild the native tests: ' + str(path))
            books[name] = path
            spec['sha256'] = digest(path)
        else:
            books[name] = cached_book(spec, args.book_cache, args.offline)
    dictionary = None
    if 'dictionary' in groups:
        dictionary = next(entry for entry in json.loads((ROOT / 'resources/dictionaries.json').read_text())['dictionaries'] if entry['id'] == 'en-en')
        reference = dict(dictionary, download=dictionary['url'], filename='en-en-v1.2.0.zip')
        dictionary_path = cached_book(reference, args.dependency_cache / 'screenshot-dictionaries', args.offline, 256 * 1024 * 1024)
    themes = ('light', 'dark') if args.theme == 'both' else (args.theme,)
    manifest = dict(complete=False, created=datetime.now(timezone.utc).isoformat(), groups=groups, themes=themes,
                    width=args.width, height=args.height, densities=densities, clock='09:00', battery_percent=100, screens=[],
                    books={name: references[name] for name in sorted(needed)}, exclusions=EXCLUSIONS,
                    binary_sha256=digest(binary), reference_sha256=digest(BOOKS))
    if dictionary: manifest['dictionaries'] = {'en-en': dictionary}
    lock = output / '.capture.lock'
    try:
        lock_fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        raise ValueError(f'Another capture run owns {lock}. If it was interrupted, remove this lock before retrying.')
    os.close(lock_fd)
    staging = None
    try:
        previous = json.loads((output / 'manifest.json').read_text()) if (output / 'manifest.json').exists() else None
        if previous and any(previous[key] != manifest[key] for key in ('width', 'height')):
            if groups != full_groups or themes != ('light', 'dark') or args.screen:
                raise ValueError('Viewport differs from the current gallery. Run a full sweep to replace it.')
            previous = None
        staging = Path(tempfile.mkdtemp(prefix='kobalt-screenshot-capture-'))
        capture_started = time.monotonic()
        def capture(ppi, theme, group):
            task_started = time.monotonic()
            folder = staging / capture_folder(ppi, theme, group)
            folder.mkdir(parents=True)
            command = [str(binary), '--book', str(books[GROUPS[group][0]]), '--output', str(folder), '--group', group,
                       '--theme', theme, '--ppi', str(ppi), '--width', str(args.width), '--height', str(args.height)]
            selected = [screen.split('/', 1)[1] for screen in args.screen if screen.startswith(group + '/')]
            if selected: command += ['--only', ','.join(selected)]
            environment = dict(os.environ, QT_QPA_PLATFORM='minimal:enable_fonts:freetype' if sys.platform == 'darwin' else 'offscreen',
                               QT_SCALE_FACTOR='1', QT_FONT_DPI='96', KOBALT_TEST_DEVICE_FIXTURE='libra-colour')
            if group == 'dictionary': environment['KOBALT_SCREENSHOT_DICTIONARY'] = str(dictionary_path)
            try:
                with (folder / 'run.log').open('w') as log:
                    subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
            except subprocess.SubprocessError as error:
                raise ValueError(f'{theme}/{group} failed. See {folder / "run.log"}: {error}') from error
            captured = []
            for metadata in sorted(folder.glob('*.json')):
                shot = json.loads(metadata.read_text())
                if shot.get('ppi') != ppi or shot.get('reading_ppi', ppi) != ppi:
                    raise ValueError('Capture density differs from requested density; rebuild the screenshot executable')
                path = metadata.with_suffix('.png')
                shot.update(name=f'{theme}/{group}/{metadata.stem}', file=path.relative_to(staging).as_posix(), sha256=digest(path),
                            theme=theme, group=group, captured_at=manifest['created'], binary_sha256=manifest['binary_sha256'])
                captured.append(shot)
            if not captured or set(selected) - {Path(shot['file']).stem for shot in captured}:
                raise ValueError('Requested screenshots were not captured: ' + group + ' ' + ', '.join(selected))
            elapsed = time.monotonic() - task_started
            return captured, dict(ppi=ppi, theme=theme, group=group, seconds=round(elapsed, 2))
        timings = []
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(capture, ppi, theme, group) for ppi in densities for theme in themes for group in groups]
            for future in as_completed(futures):
                captured, timing = future.result()
                manifest['screens'].extend(captured)
                timings.append(timing)
                print(f"CAPTURED {timing['ppi']} ppi {timing['theme']}/{timing['group']}: {len(captured)} in {timing['seconds']:.1f}s", flush=True)
        if args.android_serial:
            from android_screenshots import capture as capture_android
            manifest['screens'].extend(capture_android(args.android_serial, args.android_apk, books['alice'], staging))
            manifest['groups'].append('android-device')
        manifest['screens'].sort(key=lambda shot: shot['file'])
        manifest['complete'] = True
        manifest['timing'] = dict(jobs=args.jobs, capture_seconds=round(time.monotonic() - capture_started, 2),
                                  groups=sorted(timings, key=lambda item: (item['theme'], item['group'])))
        if groups == full_groups and themes == ('light', 'dark') and not args.screen and set(densities) >= {shot.get('ppi', 300) for shot in (previous or {}).get('screens', []) if shot['group'] != 'android-device'}:
            merged = manifest
        else:
            merged = merge_captures(previous, manifest, args.screen)
        if previous and not args.android_serial:
            retained_android = [shot for shot in previous['screens'] if shot['group'] == 'android-device']
            merged['screens'] = [shot for shot in merged['screens'] if shot['group'] != 'android-device'] + retained_android
            if retained_android: merged['groups'] = list(dict.fromkeys(merged['groups'] + ['android-device']))
        if (staging / 'android').exists():
            shutil.copytree(staging / 'android', output / 'android', dirs_exist_ok=True)
        # Build the single-file gallery once, after every capture succeeds.
        for folder in staging.iterdir():
            if folder.is_dir() and folder.name != 'android':
                shutil.copytree(folder, output / folder.name, dirs_exist_ok=True)
        merged['timing']['total_seconds'] = round(time.monotonic() - started, 2)
        report(output, merged)
        prune_captures(output, merged)
        (output / 'capture-error.log').unlink(missing_ok=True)
        elapsed = time.monotonic() - started
        print(f"Captured {len(manifest['screens'])} screenshots in {elapsed:.1f}s; capture phase {manifest['timing']['capture_seconds']:.1f}s", flush=True)
    except Exception:
        if staging is not None:
            logs = '\n'.join(str(path.relative_to(staging)) + '\n' + path.read_text(errors='replace') for path in sorted(staging.rglob('run.log')))
            (output / 'capture-error.log').write_text(logs)
            print(f'Previous gallery preserved. Capture log: {output / "capture-error.log"}', file=sys.stderr)
        raise
    finally:
        if staging is not None: shutil.rmtree(staging)
        lock.unlink()
    print(output / 'index.html')


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
