#!/usr/bin/env node
// Screen review. Turns gallery captures into numbered review rounds and
// serves a page where each screen is approved or rejected next to its
// reference capture, with general and pinned notes. See doc/screen-review.md.
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { StringDecoder } from 'node:string_decoder';
import { parseArgs } from 'node:util';
import { reviewVerdict, wasSeen } from './page/feedback.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const usage = `Usage:
  node platforms/review/review.mjs round [--source DIR] [--theme light] [--pair DEVICE[=REFERENCE]]... [--fresh]
  node platforms/review/review.mjs serve [--port 8765]
  node platforms/review/review.mjs notes [--round N]
All commands accept --data DIR (default build/review).`;

// Each phone capture is reviewed next to the 6-inch capture with the same orientation.
const defaultPairs = ['phone=six-inch', 'phone-landscape=six-inch-landscape'];
const maxNote = 20000, maxPins = 100;

const { values: options, positionals } = parseArgs({
    allowPositionals: true,
    options: {
        data: { type: 'string', default: path.join(root, 'build/review') },
        source: { type: 'string', default: path.join(root, 'build/screenshots/mac') },
        theme: { type: 'string', default: 'light' },
        pair: { type: 'string', multiple: true },
        fresh: { type: 'boolean', default: false },
        port: { type: 'string', default: '8765' },
        round: { type: 'string' },
        help: { type: 'boolean', short: 'h', default: false },
    },
});
const data = path.resolve(options.data);
const imagesDir = path.join(data, 'images');
const roundsDir = path.join(data, 'rounds');

function fail(message) {
    console.error(message);
    process.exit(1);
}
function readJson(file, fallback) {
    try {
        return JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
        if (fallback !== undefined && error.code === 'ENOENT') return fallback;
        throw error;
    }
}
function writeJson(file, value) {
    const temporary = `${file}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify(value, null, 1) + '\n');
    fs.renameSync(temporary, file);
}
function roundDir(number) {
    return path.join(roundsDir, String(number).padStart(3, '0'));
}
function roundNumbers() {
    if (!fs.existsSync(roundsDir)) return [];
    return fs.readdirSync(roundsDir).filter(name => /^\d+$/.test(name) &&
        fs.existsSync(path.join(roundsDir, name, 'round.json'))).map(Number).sort((a, b) => a - b);
}
function loadRound(number) {
    const dir = roundDir(number);
    return { round: readJson(path.join(dir, 'round.json')), feedback: readJson(path.join(dir, 'feedback.json'), { items: {} }) };
}

// The gallery page lists cards in reading order with their titles. The
// manifest has neither, so read both from the page generated next to it.
// The cards come first; the page then embeds every image, which can be
// larger than a string, so stop reading where the images start.
function readCards(source) {
    const cards = new Map();
    let html = '';
    try {
        const file = fs.openSync(path.join(source, 'index.html'), 'r');
        const decoder = new StringDecoder('utf8');
        const buffer = Buffer.alloc(1 << 20);
        for (let read; !html.includes('<img data-scene') && (read = fs.readSync(file, buffer)) > 0;)
            html += decoder.write(buffer.subarray(0, read));
        fs.closeSync(file);
    } catch {
        return cards;
    }
    const decode = text => text.replace(/&(amp|quot|lt|gt|#39);/g, (_, name) =>
        ({ amp: '&', quot: '"', lt: '<', gt: '>', '#39': "'" })[name]);
    for (const match of html.matchAll(/<figure data-group="([^"]*)" data-scene="([^"]*)" data-title="([^"]*)"/g))
        if (!cards.has(decode(match[2]))) cards.set(decode(match[2]), { title: decode(match[3]), order: cards.size });
    return cards;
}

function storeImage(file) {
    const bytes = fs.readFileSync(file);
    const sha = createHash('sha256').update(bytes).digest('hex');
    const target = path.join(imagesDir, `${sha}.png`);
    if (!fs.existsSync(target)) fs.writeFileSync(target, bytes);
    return sha;
}

function createRound() {
    const source = path.resolve(options.source);
    const manifestFile = path.join(source, 'manifest.json');
    if (!fs.existsSync(manifestFile)) fail(`No gallery manifest in ${source}. Run a mac-screenshots build first.`);
    const manifest = readJson(manifestFile);
    const cards = readCards(source);
    const devices = new Set(manifest.images.map(shot => shot.device));
    let pairs = (options.pair?.length ? options.pair : defaultPairs).map(text => {
        const [device, reference] = text.split('=');
        return { device, reference: reference || null };
    });
    // Without phone captures, review the 6-inch screens on their own.
    if (!options.pair?.length && !pairs.some(pair => devices.has(pair.device))) pairs = [{ device: 'six-inch', reference: null }];
    const shots = new Map();
    for (const shot of manifest.images) {
        if (shot.theme !== options.theme) continue;
        const key = `${shot.device}|${shot.screen}`;
        if (!shots.has(key)) shots.set(key, []);
        shots.get(key).push(shot);
    }
    for (const list of shots.values()) list.sort((a, b) => a.capture_page - b.capture_page);
    const captured = new Set([...shots.keys()].map(key => key.split('|')[1]));
    const numbers = roundNumbers();
    const previous = numbers.length ? loadRound(numbers.at(-1)) : null;
    // A scene-filtered capture has its own small gallery page. Its screens
    // keep the place and title they had in the previous round.
    const partial = Boolean(previous && !options.fresh && previous.round.items.some(item => !captured.has(item.screen)));
    const earlierCards = new Map(previous?.round.items.map(item => [item.screen, { title: item.title, order: item.order[0] }]) || []);

    fs.mkdirSync(imagesDir, { recursive: true });
    const image = shot => {
        const sha = storeImage(path.join(source, shot.file));
        if (sha !== shot.sha256) fail(`${shot.file} does not match its manifest checksum. Wait for the gallery run to finish.`);
        return { sha, width: shot.width, height: shot.height, device: shot.device, file: shot.file };
    };
    const items = [];
    for (const [pairIndex, pair] of pairs.entries()) {
        for (const [key, list] of shots) {
            const [device, screen] = key.split('|');
            if (device !== pair.device) continue;
            const references = pair.reference ? shots.get(`${pair.reference}|${screen}`) || [] : [];
            for (const shot of list) {
                // A phone often needs more pages than the e-reader. Pair a
                // later page with the reference's last page.
                const reference = references.filter(r => r.capture_page <= shot.capture_page).at(-1) || references[0];
                // A screen new to a partial capture goes last.
                const card = cards.get(screen), place = partial ? earlierCards.get(screen) : card;
                items.push({
                    id: `${device}/${screen}/${shot.capture_page}`,
                    screen, scene: shot.scene, group: shot.group, title: place?.title || card?.title || screen,
                    page: shot.capture_page, pages: list.length,
                    orientation: shot.width > shot.height ? 'landscape' : 'portrait',
                    image: image(shot), reference: reference ? image(reference) : null,
                    order: [place?.order ?? Number.MAX_SAFE_INTEGER, pairIndex, shot.capture_page],
                });
            }
        }
    }
    if (!items.length) fail(`The ${options.theme} captures in ${source} have no ${pairs.map(p => p.device).join(' or ')} screens.`);

    const previousItems = new Map(previous?.round.items.map(item => [item.id, item]) || []);
    const feedback = { items: {} };
    for (const item of items) {
        const before = previousItems.get(item.id);
        item.status = !before ? 'new' : before.image.sha === item.image.sha &&
            before.reference?.sha === item.reference?.sha ? 'unchanged' : 'changed';
        if (before) item.previous = { round: previous.round.number, sha: before.image.sha, width: before.image.width, height: before.image.height };
        const entry = previous?.feedback.items[item.id];
        if (item.status === 'unchanged' && entry) feedback.items[item.id] = { ...entry, carried: entry.carried || previous.round.number };
    }
    // A scene-filtered capture holds only some screens. Keep the others from
    // the previous round so that every round is complete.
    let kept = 0;
    if (partial) {
        for (const before of previous.round.items) {
            if (captured.has(before.screen) || !pairs.some(pair => before.id.startsWith(`${pair.device}/`))) continue;
            items.push({ ...before, status: 'unchanged', previous: { round: previous.round.number, sha: before.image.sha, width: before.image.width, height: before.image.height } });
            const entry = previous.feedback.items[before.id];
            if (entry) feedback.items[before.id] = { ...entry, carried: entry.carried || previous.round.number };
            kept++;
        }
    }
    items.sort((a, b) => a.order[0] - b.order[0] || a.screen.localeCompare(b.screen) || a.order[1] - b.order[1] || a.order[2] - b.order[2]);

    const number = (numbers.at(-1) || 0) + 1;
    const round = {
        schema: 1, number, created: new Date().toISOString(), source, theme: options.theme,
        commit: manifest.commit || null, pairs, items,
    };
    fs.mkdirSync(roundDir(number), { recursive: true });
    writeJson(path.join(roundDir(number), 'feedback.json'), feedback);
    writeJson(path.join(roundDir(number), 'round.json'), round);
    const count = status => items.filter(item => item.status === status).length;
    console.log(`Round ${number}: ${items.length} screens, ${count('changed')} changed, ${count('new')} new, ${count('unchanged')} unchanged` +
        (kept ? ` (${kept} kept from round ${previous.round.number})` : '') + '.');
    console.log(`Verdicts carried over: ${Object.keys(feedback.items).length}.`);
}

function printNotes() {
    const numbers = roundNumbers();
    if (!numbers.length) fail('There are no review rounds yet.');
    const number = options.round ? Number(options.round) : numbers.at(-1);
    if (!numbers.includes(number)) fail(`There is no round ${options.round}.`);
    const { round, feedback } = loadRound(number);
    const verdicts = round.items.map(item => reviewVerdict(feedback.items[item.id]));
    const tally = verdict => verdicts.filter(v => v === verdict).length;
    console.log(`Round ${number}: ${tally('needs-changes')} need changes, ${round.items.filter(item => wasSeen(feedback.items[item.id])).length} seen, ${round.items.filter(item => !wasSeen(feedback.items[item.id])).length} unseen.`);
    for (const item of round.items) {
        const entry = feedback.items[item.id];
        if (!entry || (reviewVerdict(entry) !== 'needs-changes' && !entry.note && !entry.pins?.length)) continue;
        const mark = reviewVerdict(entry) === 'needs-changes' ? 'NEEDS CHANGES' : reviewVerdict(entry) || 'no verdict';
        console.log(`\n${mark}: ${item.id} · ${item.group} · ${item.title}, ${item.orientation}, page ${item.page} of ${item.pages}` +
            (entry.carried ? ` (unchanged since round ${entry.carried})` : ''));
        console.log(`  image: ${path.join(imagesDir, `${item.image.sha}.png`)} (${item.image.width}x${item.image.height})`);
        if (item.reference) console.log(`  reference: ${path.join(imagesDir, `${item.reference.sha}.png`)} (${item.reference.device})`);
        if (entry.note) console.log(entry.note.split('\n').map(line => `  note: ${line}`).join('\n'));
        for (const [index, pin] of (entry.pins || []).entries()) {
            const image = pin.target === 'reference' ? item.reference : item.image;
            console.log(`  ${pin.target === 'reference' ? 'reference' : 'phone'} pin ${index + 1} at ${Math.round(pin.x * image.width)},${Math.round(pin.y * image.height)}: ${pin.note.replace(/\n/g, ' ')}`);
        }
    }
}

function cleanEntry(body) {
    if (typeof body !== 'object' || body === null) return null;
    const verdict = body.verdict ?? null;
    if (![null, 'approved', 'rejected', 'needs-changes'].includes(verdict)) return null;
    if (body.seen !== undefined && typeof body.seen !== 'boolean') return null;
    let seen = wasSeen(body);
    const note = typeof body.note === 'string' ? body.note.slice(0, maxNote) : '';
    if (!Array.isArray(body.pins ?? []) || (body.pins?.length || 0) > maxPins) return null;
    const pins = [];
    for (const pin of body.pins || []) {
        const x = Number(pin?.x), y = Number(pin?.y);
        if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return null;
        const target = pin.target ?? 'subject';
        if (!['subject', 'reference'].includes(target)) return null;
        pins.push({ target, x, y, note: typeof pin.note === 'string' ? pin.note.slice(0, maxNote) : '' });
    }
    seen ||= Boolean(note.trim() || pins.length);
    return { seen, verdict: reviewVerdict({ seen, verdict, note, pins }), note, pins, updated: new Date().toISOString() };
}

function serve() {
    const port = Number(options.port);
    const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8' };
    const pageFiles = new Set(['/index.html', '/review.css', '/review.js', '/feedback.mjs']);
    let writes = Promise.resolve();
    const send = (response, status, body, type = 'application/json; charset=utf-8') => {
        response.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store' });
        response.end(type.startsWith('application/json') ? JSON.stringify(body) : body);
    };
    const server = createServer(async (request, response) => {
        try {
            const url = new URL(request.url, 'http://localhost');
            const route = url.pathname === '/' ? '/index.html' : url.pathname;
            let match;
            if (request.method === 'GET' && pageFiles.has(route)) {
                send(response, 200, fs.readFileSync(path.join(here, 'page', route.slice(1))), types[path.extname(route)]);
            } else if (request.method === 'GET' && (match = route.match(/^\/images\/([0-9a-f]{64})\.png$/))) {
                const file = path.join(imagesDir, `${match[1]}.png`);
                if (!fs.existsSync(file)) return send(response, 404, { error: 'No such image' });
                response.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'max-age=31536000, immutable' });
                fs.createReadStream(file).pipe(response);
            } else if (request.method === 'GET' && route === '/api/rounds') {
                send(response, 200, roundNumbers().map(number => {
                    const { round } = loadRound(number);
                    return { number, created: round.created, commit: round.commit, count: round.items.length };
                }));
            } else if (request.method === 'GET' && (match = route.match(/^\/api\/rounds\/(\d+)$/))) {
                const number = Number(match[1]);
                const numbers = roundNumbers();
                if (!numbers.includes(number)) return send(response, 404, { error: 'No such round' });
                const current = loadRound(number);
                const before = numbers.filter(n => n < number).at(-1);
                send(response, 200, { ...current, latest: number === numbers.at(-1),
                    previousFeedback: before ? loadRound(before).feedback : null });
            } else if (request.method === 'PUT' && (match = route.match(/^\/api\/rounds\/(\d+)\/items\/(.+)$/))) {
                const number = Number(match[1]), id = decodeURIComponent(match[2]);
                if (number !== roundNumbers().at(-1)) return send(response, 409, { error: 'Only the latest round takes feedback' });
                let text = '';
                for await (const chunk of request) {
                    text += chunk;
                    if (text.length > 4 * maxNote * maxPins) return send(response, 413, { error: 'Feedback is too large' });
                }
                let entry = null;
                try {
                    entry = cleanEntry(JSON.parse(text));
                } catch {}
                if (!entry) return send(response, 400, { error: 'Invalid feedback' });
                const file = path.join(roundDir(number), 'feedback.json');
                // Saves arrive one at a time per screen, but several screens
                // can save at once. Write the file in turn.
                const job = writes.then(() => {
                    const { round } = loadRound(number);
                    const item = round.items.find(item => item.id === id);
                    if (!item) return 404;
                    if (!item.reference && entry.pins.some(pin => pin.target === 'reference')) return 400;
                    const feedback = readJson(file, { items: {} });
                    if (!entry.seen && !entry.verdict && !entry.note && !entry.pins.length) delete feedback.items[id];
                    else feedback.items[id] = entry;
                    writeJson(file, feedback);
                    return 200;
                });
                writes = job.catch(() => {});
                const status = await job;
                send(response, status, status === 200 ? { saved: entry.updated } : { error: 'No such screen' });
            } else {
                send(response, 404, { error: 'Not found' });
            }
        } catch (error) {
            console.error(error);
            if (!response.headersSent) send(response, 500, { error: String(error.message || error) });
        }
    });
    server.on('error', error => fail(error.code === 'EADDRINUSE' ? `Port ${port} is in use. Pass --port.` : String(error)));
    server.listen(port, '127.0.0.1', () => {
        if (!roundNumbers().length) console.log('There are no review rounds yet. Run the round command first.');
        console.log(`Screen review: http://localhost:${server.address().port}/`);
    });
}

const command = positionals[0];
if (options.help || !command) console.log(usage);
else if (command === 'round') createRound();
else if (command === 'serve') serve();
else if (command === 'notes') printNotes();
else fail(usage);
