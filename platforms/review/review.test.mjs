import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { hasAnnotations, wasSeen, reviewVerdict, completeFeedback } from './page/feedback.mjs';

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

test('server preserves reference pins and seen flags without modifying older rounds', async () => {
    await fs.mkdir(path.join(root, 'build'), { recursive: true });
    const data = await fs.mkdtemp(path.join(root, 'build/review-test-'));
    const roundDir = path.join(data, 'rounds/001');
    const item = { id: 'phone/test/1', image: { width: 720, height: 1600 }, reference: { width: 1072, height: 1448 } };
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
    } finally {
        const stopped = once(server, 'exit');
        server.kill('SIGTERM');
        await stopped;
        await fs.rm(data, { recursive: true, force: true });
    }
});
