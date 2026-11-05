// Run against the generated gallery with an installed Playwright module.
// Checks the capture inventory, PNG bytes, cards, filters, viewer controls, frames, keyboard navigation and focus.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const path = resolve(process.argv[2] || 'build/screenshots/mac/index.html');
const manifest = JSON.parse(await readFile(resolve(dirname(path), 'manifest.json'), 'utf8'));
const scenes = new Map();
for (const shot of manifest.images) {
    assert.equal(shot.capture, 'screen');
    assert.equal(shot.ppi, 300);
    if (shot.scene.startsWith('vertical-')) {
        assert.ok(shot.book.title.includes("Trevelyan's Type Tester"));
        assert.equal(shot.book.sha256, manifest.book_sha256);
        assert.equal(shot.book.fixture, undefined);
    }
    const expected = { 'six-inch': [1072, 1448], 'seven-inch': [1264, 1680], phone: [720, 1600], 'six-inch-landscape': [1072, 1448], 'phone-landscape': [720, 1480] }[shot.device];
    assert.ok(expected);
    // Scenes that turn the phone capture it letterboxed, as the phone landscape profile does.
    const turnedPhone = shot.device === 'phone' && shot.width === 1480 && shot.height === 720;
    assert.deepEqual([shot.width, shot.height].sort((a, b) => a - b), turnedPhone ? [720, 1480] : expected);
    const variants = scenes.get(shot.screen) || new Map();
    const key = `${shot.device}/${shot.theme}`;
    const pages = variants.get(key) || new Set();
    assert.ok(!pages.has(shot.capture_page));
    pages.add(shot.capture_page);
    variants.set(key, pages);
    scenes.set(shot.screen, variants);
    const png = await readFile(resolve(dirname(path), shot.file));
    assert.equal(createHash('sha256').update(png).digest('hex'), shot.sha256);
    assert.equal(png.readUInt32BE(16), shot.width);
    assert.equal(png.readUInt32BE(20), shot.height);
    let density;
    for (let offset = 8; offset + 12 <= png.length;) {
        const length = png.readUInt32BE(offset);
        if (png.toString('ascii', offset + 4, offset + 8) === 'pHYs') {
            assert.equal(png[offset + 16], 1);
            density = [png.readUInt32BE(offset + 8), png.readUInt32BE(offset + 12)];
        }
        offset += length + 12;
    }
    assert.deepEqual(density, [11811, 11811]);
}
for (const variants of scenes.values()) {
    assert.deepEqual([...variants.keys()].sort(), ['six-inch', 'seven-inch', 'phone', 'six-inch-landscape', 'phone-landscape'].flatMap(device => (device.startsWith('phone') ? ['dark', 'light'] : ['dark', 'large', 'light']).map(theme => `${device}/${theme}`)).sort());
    for (const device of ['six-inch', 'seven-inch', 'phone', 'six-inch-landscape', 'phone-landscape']) {
        const pages = [...variants.get(device + '/light')].sort((a, b) => a - b);
        assert.deepEqual([...variants.get(device + '/dark')].sort((a, b) => a - b), pages);
        assert.deepEqual(pages, Array.from({ length: pages.length }, (_, index) => index + 1));
        // Larger text can add pages; they must still be complete.
        if (device.startsWith('phone')) continue;
        const large = [...variants.get(device + '/large')].sort((a, b) => a - b);
        assert.deepEqual(large, Array.from({ length: large.length }, (_, index) => index + 1));
    }
}
const browser = await chromium.launch({ headless: true, ...(process.env.KOBALT_CHROMIUM ? { executablePath: process.env.KOBALT_CHROMIUM } : {}) });
const deadline = setTimeout(() => browser.close(), 60000);
try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const requests = [];
    page.on('request', request => { if (/^https?:/.test(request.url())) requests.push(request.url()); });
    await page.goto(pathToFileURL(path).href);
    assert.equal(await page.locator('figure').count(), scenes.size);
    assert.equal(await page.locator('figure:not([hidden])').count(), scenes.size);
    assert.equal(await page.locator('#device').inputValue(), 'six-inch');
    const checkLayout = async () => {
        await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
        const layout = await page.evaluate(() => {
            const rect = selector => {
                const b = document.querySelector(selector).getBoundingClientRect();
                return { x: b.x, y: b.y, width: b.width, height: b.height };
            };
            const frame = document.querySelector('.viewer-body');
            return { top: rect('#viewer-top'), bottom: rect('#viewer-bottom'), stage: rect('#viewer-stage'),
                body: rect('.viewer-body'), image: rect('#image'), width: innerWidth, height: innerHeight,
                device: frame.dataset.device, landscape: frame.dataset.orientation === 'landscape', framed: frame.classList.contains('framed') };
        });
        const near = (actual, expected) => assert.ok(Math.abs(actual - expected) < 1.5, `${actual} should equal ${expected}`);
        near(layout.top.y, 0);
        near(layout.bottom.y + layout.bottom.height, layout.height);
        near(layout.body.x + layout.body.width / 2, layout.stage.x + layout.stage.width / 2);
        near(layout.body.y + layout.body.height / 2, layout.stage.y + layout.stage.height / 2);
        assert.ok(layout.body.x >= 0 && layout.body.x + layout.body.width <= layout.width);
        assert.ok(layout.body.y >= layout.stage.y && layout.body.y + layout.body.height <= layout.stage.y + layout.stage.height + 1);
        const models = {
            'phone': { w: 800, h: 1680, x: 40, y: 40, sw: 720, sh: 1600 },
            'phone-landscape': { w: 800, h: 1680, x: 40, y: 100, sw: 720, sh: 1480 },
            'seven-inch': { w: 1708, h: 1902, x: 90, y: 111, sw: 1264, sh: 1680 },
        };
        const device = layout.device === 'phone' && layout.landscape ? 'phone-landscape' : layout.device;
        let model = models[device] || { w: 1323, h: 1890, x: 126, y: 150, sw: 1072, sh: 1448 };
        if (layout.landscape) model = { w: model.h, h: model.w, x: model.h - model.y - model.sh, y: model.x, sw: model.sh, sh: model.sw };
        if (!layout.framed) model = { w: model.sw, h: model.sh, x: 0, y: 0, sw: model.sw, sh: model.sh };
        const scale = layout.body.width / model.w;
        near(layout.body.height, model.h * scale);
        near(layout.image.x, layout.body.x + model.x * scale);
        near(layout.image.y, layout.body.y + model.y * scale);
        near(layout.image.width, model.sw * scale);
        near(layout.image.height, model.sh * scale);
        assert.equal(await page.locator('#viewer-device').inputValue(), await page.locator('#viewer-device-bottom').inputValue());
        assert.equal(await page.locator('#viewer-theme').getAttribute('aria-pressed'), await page.locator('#viewer-theme-bottom').getAttribute('aria-pressed'));
        assert.equal(await page.locator('#viewer-frame').isChecked(), await page.locator('#viewer-frame-bottom').isChecked());
    };
    // The sidebar follows the filters, jumps to a card and repeats in the viewer.
    assert.equal(await page.locator('#list').isVisible(), false);
    await page.locator('#toggle-list').click();
    assert.equal(await page.locator('#toggle-list').getAttribute('aria-expanded'), 'true');
    assert.equal(await page.locator('#list > a').count(), scenes.size);
    await page.locator('#group').selectOption('Library');
    const libraryScreens = new Set(manifest.images.filter(image => image.group === 'Library').map(image => image.screen));
    assert.equal(await page.locator('#list > a').count(), libraryScreens.size);
    const entry = page.locator('#list > a').last();
    const target = await entry.textContent();
    await entry.click();
    assert.equal(await page.locator('#list > a[aria-current]').textContent(), target);
    assert.equal(await page.evaluate(() => document.activeElement.closest('figure')?.dataset.title), target);
    // A group heading marks the first screen of its group.
    await page.locator('#list h3 a').first().click();
    assert.equal(await page.locator('#list > a[aria-current]').textContent(), await page.locator('#list > a').first().textContent());
    await page.locator('figure:not([hidden]) .capture').first().click();
    assert.equal(await page.locator('#viewer-screens').isVisible(), true);
    assert.equal(await page.locator('#viewer-list').getAttribute('aria-expanded'), 'true');
    await checkLayout();
    const second = await page.locator('#viewer-screens > a').nth(1).textContent();
    await page.locator('#viewer-screens > a').nth(1).click();
    assert.equal(await page.locator('#viewer-title').textContent(), second);
    assert.equal(await page.locator('#viewer-screens > a[aria-current]').textContent(), second);
    await page.locator('#viewer-list').click();
    assert.equal(await page.locator('#viewer-screens').isVisible(), false);
    assert.equal(await page.locator('#toggle-list').getAttribute('aria-expanded'), 'false');
    await checkLayout();
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#list').isVisible(), false);
    await page.locator('#reset').click();
    const labels = await page.locator('figcaption').allTextContents();
    assert.ok(labels.includes('Font picker'));
    assert.ok(labels.every(label => !label.includes('.png') && !label.includes('six-inch-')));
    await page.locator('#group').selectOption('International books');
    const literary = manifest.images.filter(image => image.group === 'International books');
    assert.equal(new Set(literary.map(image => image.book.language)).size, 16);
    const literaryScreens = new Set(literary.map(image => image.screen));
    const literaryVariants = new Set(literary.map(image => image.device + '/' + image.theme));
    assert.equal(literary.length, literaryScreens.size * literaryVariants.size);
    assert.equal(await page.locator('figure:not([hidden])').count(), literaryScreens.size);
    await page.locator('#query').fill('Japanese vertical reading');
    assert.equal(await page.locator('figure:not([hidden])').count(), 1);
    const card = page.locator('figure:not([hidden])');
    await card.locator('.capture').click();
    assert.equal(await page.locator('#viewer').evaluate(element => element.open), true);
    await page.locator('#image').evaluate(image => image.decode());
    assert.deepEqual(await page.locator('#image').evaluate(image => [image.naturalWidth, image.naturalHeight]), [1072, 1448]);
    assert.equal(await page.locator('#viewer-frame').isChecked(), true);
    assert.equal(await page.locator('#viewer-frame-bottom').isChecked(), true);
    assert.equal(await page.locator('#viewer-model').textContent(), 'Kobo Clara · 6-inch');
    assert.equal(await page.locator('#frame-buttons').isVisible(), false);
    await checkLayout();
    const light = await page.locator('#image').getAttribute('src');
    await page.keyboard.press('Space');
    assert.equal(await page.locator('#viewer-theme').getAttribute('aria-pressed'), 'true');
    assert.notEqual(await page.locator('#image').getAttribute('src'), light);
    await page.keyboard.press('Space');
    assert.equal(await page.locator('#image').getAttribute('src'), light);
    await page.locator('#viewer-device').selectOption('seven-inch');
    await page.locator('#image').evaluate(image => image.decode());
    assert.deepEqual(await page.locator('#image').evaluate(image => [image.naturalWidth, image.naturalHeight]), [1264, 1680]);
    assert.equal(await page.locator('#viewer-model').textContent(), 'Kobo Libra · 7-inch');
    assert.equal(await page.locator('#frame-buttons').isVisible(), true);
    await checkLayout();
    for (const [device, dimensions] of [['phone', [720, 1600]], ['six-inch-landscape', [1448, 1072]], ['phone-landscape', [1480, 720]]]) {
        await page.locator('#viewer-device').selectOption(device);
        await page.locator('#image').evaluate(image => image.decode());
        assert.deepEqual(await page.locator('#image').evaluate(image => [image.naturalWidth, image.naturalHeight]), dimensions);
        assert.equal(await page.locator('#text-size').isDisabled(), device.startsWith('phone'));
        await checkLayout();
    }
    await page.locator('#viewer-device-bottom').selectOption('six-inch');
    assert.equal(await page.locator('#viewer-device').inputValue(), 'six-inch');
    await checkLayout();
    await page.locator('#viewer-device-bottom').selectOption('seven-inch');
    await page.locator('#viewer-theme-bottom').click();
    assert.equal(await page.locator('#viewer-theme').getAttribute('aria-pressed'), 'true');
    await page.locator('#viewer-theme-bottom').click();
    const raw = await page.locator('#image').getAttribute('src');
    await page.locator('#viewer-frame').uncheck();
    await checkLayout();
    assert.equal(await page.locator('#viewer-frame-bottom').isChecked(), false);
    await page.locator('#viewer-frame-bottom').check();
    await checkLayout();
    assert.equal(await page.locator('.viewer-body').evaluate(element => element.classList.contains('framed')), true);
    assert.equal(await page.locator('#image').getAttribute('src'), raw);
    assert.equal(await page.locator('#raw-image').getAttribute('href'), raw);
    assert.equal(await page.locator('#raw-image').getAttribute('download'), 'seven-inch-light-international-ja-vertical-reading.png');
    assert.equal(await page.locator('#capture-file').isVisible(), false);
    const summary = page.locator('.viewer-meta summary');
    await summary.focus();
    await page.keyboard.press('Space');
    assert.equal(await page.locator('.viewer-meta').evaluate(element => element.open), true);
    assert.equal(await page.locator('#viewer-theme').getAttribute('aria-pressed'), 'false');
    await checkLayout();
    assert.equal(await page.locator('#capture-file').textContent(), 'seven-inch-light-international-ja-vertical-reading.png');
    await page.locator('#close').focus();
    await page.keyboard.press('Space');
    assert.equal(await page.locator('#viewer').evaluate(element => element.open), false);
    assert.equal(await page.locator('#theme').getAttribute('aria-pressed'), 'false');
    assert.equal(await card.locator('.capture').evaluate(element => element === document.activeElement), true);
    assert.equal(await page.locator('figure').count(), scenes.size);
    // 150% text is a light variant; the theme waits until it is off again.
    const regular = await card.locator('img').getAttribute('src');
    await page.locator('#text-size').click();
    assert.equal(await page.locator('#text-size').getAttribute('aria-pressed'), 'true');
    assert.equal(await page.locator('#theme').isDisabled(), true);
    const largeCapture = manifest.images.find(image => image.screen === 'international-ja-vertical-reading' &&
        image.device === 'seven-inch' && image.theme === 'large' && image.capture_page === 1);
    assert.ok(largeCapture);
    const largeSource = await card.locator('img').getAttribute('src');
    assert.equal(createHash('sha256').update(Buffer.from(largeSource.split(',')[1], 'base64')).digest('hex'), largeCapture.sha256);
    assert.match(await card.locator('.tag').textContent(), /150%/);
    await page.locator('#text-size').click();
    assert.equal(await page.locator('#theme').isDisabled(), false);
    assert.equal(await card.locator('img').getAttribute('src'), regular);
    await page.locator('#query').fill('no matching screenshot xyzzy');
    assert.equal(await page.locator('figure:not([hidden])').count(), 0);
    assert.equal(await page.locator('#empty').isVisible(), true);
    await page.locator('#reset').click();
    assert.equal(await page.locator('#device').inputValue(), 'six-inch');
    await page.locator('#query').fill('dictionary definition');
    assert.ok(await page.locator('figure:not([hidden])').count() > 1);
    await page.locator('figure:not([hidden]) .capture').first().click();
    const before = await page.locator('#viewer-title').textContent();
    await page.keyboard.press('ArrowRight');
    assert.notEqual(await page.locator('#viewer-title').textContent(), before);
    await page.keyboard.press('ArrowLeft');
    assert.equal(await page.locator('#viewer-title').textContent(), before);
    await page.locator('#close-bottom').focus();
    await page.keyboard.press('Space');
    assert.equal(await page.locator('#viewer').evaluate(element => element.open), false);
    await page.locator('#reset').click();
    await page.locator('#query').fill('equations');
    assert.equal(await page.locator('figure:not([hidden])').count(), 1);
    await page.locator('figure:not([hidden]) .capture').click();
    assert.equal(await page.locator('#viewer-page option').count(), 4);
    await page.locator('#viewer-page-bottom').selectOption('3');
    assert.equal(await page.locator('#viewer-page').inputValue(), '3');
    assert.ok((await page.locator('#capture-file').textContent()).endsWith('maths-equations-3.png'));
    await page.locator('#viewer-theme').click();
    assert.equal(await page.locator('#viewer-page').inputValue(), '3');
    await page.locator('#viewer-device').selectOption('seven-inch');
    assert.equal(await page.locator('#viewer-page').inputValue(), '1');
    await page.keyboard.press('Escape');
    // Decode every embedded raw image without activating hidden lazy thumbnails.
    const bad = await page.locator('#captures').evaluate(async template => {
        let failures = 0;
        for (const image of template.content.querySelectorAll('img')) {
            const probe = new Image();
            probe.src = image.src;
            try {
                await probe.decode();
                if (!image.src.startsWith('data:image/png;base64,') || probe.naturalWidth !== Number(image.getAttribute('width')) || probe.naturalHeight !== Number(image.getAttribute('height'))) failures++;
            } catch { failures++; }
        }
        return failures;
    });
    assert.equal(bad, 0);
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    await page.locator('figure:not([hidden]) .capture').first().click();
    await checkLayout();
    await page.setViewportSize({ width: 844, height: 390 });
    await checkLayout();
    await page.keyboard.press('Escape');
    await page.locator('#reset').click();
    await page.locator('#query').fill('expanded columns');
    await page.locator('figure:not([hidden]) .capture').click();
    assert.equal(await page.locator('.viewer-body').getAttribute('data-orientation'), 'landscape');
    await checkLayout();
    await page.locator('#viewer-device-bottom').selectOption('seven-inch');
    await checkLayout();
    await page.setViewportSize({ width: 390, height: 844 });
    await checkLayout();
    const bounds = await page.locator('#viewer').boundingBox();
    assert.ok(bounds.x >= 0 && bounds.x + bounds.width <= 390 && bounds.y >= 0 && bounds.y + bounds.height <= 844);
    assert.deepEqual(requests, []);
    assert.deepEqual(errors, []);
    console.log(`PASS: ${scenes.size} cards, ${manifest.images.length} frameless 300 PPI PNGs, four variants per screen, page selection, labels, filters, sidebar, viewport controls, default Clara/Libra frames, native Space activation, raw downloads, checksums and responsive centering`);
} finally {
    clearTimeout(deadline);
    await browser.close();
}
