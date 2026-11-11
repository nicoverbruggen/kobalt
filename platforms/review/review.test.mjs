import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { hasAnnotations, wasSeen, reviewVerdict, completeFeedback } from './page/feedback.mjs';
import { bezel, zoomSteps, zoomIndex, hasBaseline, differsFromBaseline } from './page/view.mjs';

const root = fileURLToPath(new URL('../..', import.meta.url));

test('seen and annotations remain separate, including older feedback', () => {
    assert.equal(wasSeen({}), false);
    assert.equal(reviewVerdict({}), null);
    assert.equal(reviewVerdict(completeFeedback({ pins: [] })), 'approved');
    for (const feedback of [{ note: 'Fix spacing' }, { pins: [{ target: 'reference', note: 'Too small' }] }]) {
        assert.equal(hasAnnotations(feedback), true);
        assert.equal(reviewVerdict(completeFeedback(feedback)), 'needs-changes');
    }
    assert.equal(reviewVerdict({ verdict: 'approved', pins: [{ note: 'Old pin' }] }), 'needs-changes');
    assert.equal(wasSeen({ verdict: 'rejected' }), true);
    assert.equal(reviewVerdict({ verdict: 'rejected' }), 'needs-changes');
    assert.equal(reviewVerdict({ seen: true, note: '', pins: [], verdict: 'needs-changes' }), 'approved');
    assert.equal(reviewVerdict({ note: '   ', pins: [] }), null);
});

test('bezels turn with landscape captures and keep the screen opening', () => {
    assert.equal(bezel({ device: 'unknown', width: 10, height: 20 }), null);
    const portrait = bezel({ device: 'six-inch', width: 1072, height: 1448 });
    assert.deepEqual(portrait.screen, { x: 126, y: 150, width: 1072, height: 1448 });
    const landscape = bezel({ device: 'six-inch-landscape', width: 1448, height: 1072 });
    assert.deepEqual([landscape.width, landscape.height], [1890, 1323]);
    assert.deepEqual(landscape.screen, { x: 1890 - 150 - 1448, y: 126, width: 1448, height: 1072 });
    assert.deepEqual([bezel({ device: 'phone-landscape', width: 1480, height: 720 }).screen.width], [1480]);
});

test('zoom stops place Fit among the steps and merge a step next to it', () => {
    const stops = zoomSteps(.35);
    assert.deepEqual(stops.map(stop => stop.scale), [.35, .5, .67, .75, .9, 1, 1.25, 1.5, 2]);
    assert.equal(stops[0].fit, true);
    assert.equal(zoomSteps(1.005).filter(stop => stop.scale === 1).length, 0);
    assert.equal(zoomIndex(stops, .35, true), 0);
    assert.equal(zoomIndex(stops, 1.3, false), stops.findIndex(stop => stop.scale === 1.25));
});

test('a screen differs from its baseline when a device image changed or is missing', () => {
    const image = { sha: 'a' }, reference = { sha: 'b' };
    const item = { image, reference, previous: { sha: 'a' }, previousReference: { sha: 'b' }, before: { sha: 'x' } };
    assert.equal(hasBaseline(item, 'none'), false);
    assert.equal(differsFromBaseline(item, 'previous'), false);
    assert.equal(differsFromBaseline({ ...item, previousReference: { sha: 'c' } }, 'previous'), true);
    assert.equal(differsFromBaseline({ ...item, previousReference: null }, 'previous'), true);
    assert.equal(differsFromBaseline(item, 'before'), true);
    assert.equal(differsFromBaseline({ image, reference: null, previous: null }, 'previous'), false);
});

test('server preserves reference pins and seen flags without modifying older rounds', async () => {
    await fs.mkdir(path.join(root, 'build'), { recursive: true });
    const data = await fs.mkdtemp(path.join(root, 'build/review-test-'));
    const roundDir = path.join(data, 'rounds/001');
    const item = { id: 'phone/test/1', screen: 'test', page: 1, image: { device: 'phone', width: 720, height: 1600 }, reference: { width: 1072, height: 1448 } };
    await fs.mkdir(roundDir, { recursive: true });
    await fs.writeFile(path.join(roundDir, 'round.json'), JSON.stringify({ number: 1, items: [item, { ...item, id: 'no-reference', reference: null }] }));
    const server = spawn(process.execPath, ['platforms/review/review.mjs', 'serve', '--port', '0', '--data', data], { cwd: root });
    let output = '';
    const ready = new Promise((resolve, reject) => {
        server.once('error', reject);
        server.once('exit', code => reject(new Error(`Server exited: ${code}`)));
        server.stdout.on('data', chunk => {
            output += chunk;
            const match = output.match(/http:\/\/localhost:(\d+)\//);
            if (match) resolve(`http://127.0.0.1:${match[1]}`);
        });
    });
    try {
        const base = await ready;
        const put = async (body, id = item.id, round = 1) => fetch(`${base}/api/rounds/${round}/items/${encodeURIComponent(id)}`, {
            method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
        });
        assert.equal((await fetch(`${base}/feedback.mjs`)).status, 200);
        assert.equal((await put({ seen: true, pins: [] })).status, 200);
        let saved = JSON.parse(await fs.readFile(path.join(roundDir, 'feedback.json'), 'utf8')).items[item.id];
        assert.equal(saved.seen, true);
        assert.equal(saved.verdict, 'approved');
        const pin = { target: 'reference', x: 0.25, y: 0.75, note: 'Kobo spacing' };
        assert.equal((await put({ seen: true, verdict: 'approved', pins: [pin] })).status, 200);
        saved = JSON.parse(await fs.readFile(path.join(roundDir, 'feedback.json'), 'utf8')).items[item.id];
        assert.equal(saved.verdict, 'needs-changes');
        assert.deepEqual(saved.pins, [pin]);
        assert.equal((await put({ pins: [{ ...pin, target: 'invalid' }] })).status, 400);
        assert.equal((await put({ seen: 'yes', pins: [] })).status, 400);
        assert.equal((await put({ pins: [pin] }, 'no-reference')).status, 400);
        const next = path.join(data, 'rounds/002');
        await fs.mkdir(next);
        await fs.writeFile(path.join(next, 'round.json'), JSON.stringify({ number: 2, items: [item] }));
        assert.equal((await put({ seen: true, pins: [] })).status, 409);
        assert.deepEqual(await (await fetch(`${base}/api/latest`)).json(), { number: 2 });
        // A Before set finds a screen's base image through its match.
        const shot = { sha: 'a'.repeat(64), width: 720, height: 1600, device: 'phone', page: 1 };
        await fs.writeFile(path.join(data, 'baseline.json'), JSON.stringify({
            commit: 'abc', matches: { test: 'base-test' }, shots: { 'phone|base-test': [shot] },
        }));
        const loaded = await (await fetch(`${base}/api/rounds/2`)).json();
        assert.deepEqual(loaded.round.items[0].before, { ...shot, screen: 'base-test', commit: 'abc' });
        // A Before set from a base ref is stale once the branch leaves that ref elsewhere.
        const head = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
        const baselineFile = path.join(data, 'baseline.json');
        const stored = JSON.parse(await fs.readFile(baselineFile, 'utf8'));
        await fs.writeFile(baselineFile, JSON.stringify({ ...stored, commit: head, baseRef: 'HEAD' }));
        assert.equal((await (await fetch(`${base}/api/rounds/2`)).json()).baseline.stale, false);
        await fs.writeFile(baselineFile, JSON.stringify({ ...stored, commit: 'abc', baseRef: 'HEAD' }));
        assert.equal((await (await fetch(`${base}/api/rounds/2`)).json()).baseline.stale, true);
        // Finish review records the end of the round; a later change reopens it.
        const finished = await fetch(`${base}/api/rounds/2/finish`, { method: 'PUT' });
        assert.equal(finished.status, 200);
        const feedbackFile = path.join(next, 'feedback.json');
        assert.ok(JSON.parse(await fs.readFile(feedbackFile, 'utf8')).finished);
        assert.equal((await fetch(`${base}/api/rounds/1/finish`, { method: 'PUT' })).status, 409);
        assert.equal((await put({ seen: true, pins: [] }, item.id, 2)).status, 200);
        assert.equal(JSON.parse(await fs.readFile(feedbackFile, 'utf8')).finished, undefined);
        // Saves from the page keep the answer, and keep the carried round
        // while the notes stay the same.
        const resolved = { note: '', pins: [pin], reply: 'Answered', round: 1 };
        await fs.writeFile(feedbackFile, JSON.stringify({ items: { [item.id]: { seen: true, note: '', pins: [pin], carried: 1, resolved } } }));
        assert.equal((await put({ seen: true, pins: [pin] }, item.id, 2)).status, 200);
        saved = JSON.parse(await fs.readFile(feedbackFile, 'utf8')).items[item.id];
        assert.equal(saved.carried, 1);
        assert.deepEqual(saved.resolved, resolved);
        assert.equal((await put({ seen: true, pins: [] }, item.id, 2)).status, 200);
        saved = JSON.parse(await fs.readFile(feedbackFile, 'utf8')).items[item.id];
        assert.equal(saved.carried, undefined);
        assert.deepEqual(saved.resolved, resolved);
    } finally {
        const stopped = once(server, 'exit');
        server.kill('SIGTERM');
        await stopped;
        await fs.rm(data, { recursive: true, force: true });
    }
});

test('a reply answers notes in the next round until the reviewer has seen it', async () => {
    await fs.mkdir(path.join(root, 'build'), { recursive: true });
    const data = await fs.mkdtemp(path.join(root, 'build/review-test-'));
    const source = path.join(data, 'source');
    await fs.mkdir(source);
    const images = [];
    for (const [device, width, height] of [['phone', 720, 1600], ['six-inch', 1072, 1448]]) {
        const file = `${device}.png`, bytes = Buffer.from(`image ${device}`);
        await fs.writeFile(path.join(source, file), bytes);
        const sha256 = (await import('node:crypto')).createHash('sha256').update(bytes).digest('hex');
        images.push({ file, sha256, device, width, height, screen: 'test', scene: 'test', group: 'Test', theme: 'light', capture_page: 1 });
    }
    await fs.writeFile(path.join(source, 'manifest.json'), JSON.stringify({ images }));
    const review = (...args) => execFileSync(process.execPath, ['platforms/review/review.mjs', ...args, '--data', data], { cwd: root, encoding: 'utf8', stdio: 'pipe' });
    const feedback = async number => JSON.parse(await fs.readFile(path.join(data, `rounds/00${number}/feedback.json`), 'utf8'));
    const id = 'phone/test/1';
    const pin = { target: 'reference', x: 0.5, y: 0.5, note: 'Does this cover text?' };
    try {
        review('round', '--source', source);
        assert.throws(() => review('reply', id, 'No'), /has no notes/);
        await fs.writeFile(path.join(data, 'rounds/001/feedback.json'), JSON.stringify({ items: { [id]: { seen: true, verdict: 'needs-changes', note: '', pins: [pin] } } }));
        assert.throws(() => review('reply', 'phone/missing/1', 'No'), /has no screen/);
        review('reply', id, 'No, it covers only the footer.');
        assert.match(review('notes'), /Replies for the next round: phone\/test\/1/);
        // The unchanged screen starts unseen, with the notes set apart.
        assert.match(review('round', '--source', source), /Answered: 1/);
        let entry = (await feedback(2)).items[id];
        assert.equal(wasSeen(entry), false);
        assert.equal(hasAnnotations(entry), false);
        assert.deepEqual(entry.resolved, { note: '', pins: [pin], reply: 'No, it covers only the footer.', round: 1 });
        await assert.rejects(fs.access(path.join(data, 'replies.json')));
        assert.match(review('notes'), /ANSWERED: phone\/test\/1 .*reply not seen yet/);
        // An unseen answer carries over; a seen one does not.
        review('round', '--source', source);
        assert.ok((await feedback(3)).items[id].resolved);
        await fs.writeFile(path.join(data, 'rounds/003/feedback.json'), JSON.stringify({ items: { [id]: { ...entry, seen: true, verdict: 'approved' } } }));
        review('round', '--source', source);
        entry = (await feedback(4)).items[id];
        assert.equal(entry.resolved, undefined);
        assert.equal(entry.carried, 3);
    } finally {
        await fs.rm(data, { recursive: true, force: true });
    }
});
