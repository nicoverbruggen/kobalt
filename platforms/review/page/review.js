import { hasAnnotations, reviewVerdict, completeFeedback, sameAnnotations, wasSeen } from './feedback.mjs';
import { bezel, bezelRadius, zoomSteps, zoomIndex, hasBaseline, differsFromBaseline } from './view.mjs';
// Screen review page. Shows one screen at a time next to its reference
// capture and saves the verdict, the general note and pinned notes.

const $ = id => document.getElementById(id);
const deviceNames = {
    'phone': 'Phone',
    'phone-landscape': 'Phone landscape',
    'six-inch': '6-inch e-reader',
    'six-inch-landscape': '6-inch e-reader landscape',
    'seven-inch': '7-inch e-reader',
};
const deviceName = device => deviceNames[device] || device;
const filters = ['group', 'orientation', 'status'];
const state = { round: null, feedback: { items: {} }, earlier: null, latest: false, current: null, target: ['before', 'previous'].includes(recall('compare')) ? recall('compare') : 'none', baseline: null, queue: {}, peek: false, diff: false, side: recall('side') === 'on', activePin: -1, zoom: null, fit: 1, steps: [], newest: 0, bezel: recall('bezel') === 'on',
    showReference: recall('hide-reference') !== 'yes', showSubject: recall('hide-subject') !== 'yes' };
let saving = Promise.resolve();
let pending = null;

function remember(key, value) {
    try { localStorage.setItem(`kobalt-review-${key}`, value); } catch {}
}
function recall(key) {
    try { return localStorage.getItem(`kobalt-review-${key}`) || ''; } catch { return ''; }
}
async function api(url, options) {
    const response = await fetch(url, options);
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || response.statusText);
    return body;
}

function entry(id) {
    const saved = state.feedback.items[id];
    return { seen: wasSeen(saved), verdict: reviewVerdict(saved), note: saved?.note || '', pins: saved?.pins ? saved.pins.map(pin => ({ ...pin })) : [], carried: saved?.carried, resolved: saved?.resolved };
}
function matches(item) {
    const { verdict, note, pins } = entry(item.id);
    if ($('group').value && item.group !== $('group').value) return false;
    if ($('orientation').value && item.orientation !== $('orientation').value) return false;
    switch ($('status').value) {
    case 'queued': return Boolean(state.queue[item.id]);
    case 'open': return !wasSeen(state.feedback.items[item.id]);
    case 'seen': return wasSeen(state.feedback.items[item.id]);
    case 'needs-changes': return verdict === 'needs-changes';
    case 'approved': return verdict === 'approved';
    case 'changed': return item.status === 'changed';
    case 'noted': return Boolean(note || pins.length);
    case 'differs': return differsFromBaseline(item, state.target);
    default: return true;
    }
}
const visible = () => state.round.items.filter(matches);
const currentItem = () => state.round?.items.find(item => item.id === state.current) || null;

// The current screen stays put when a verdict takes it out of the filter.
// Moving goes to the nearest matching screen in round order.
function step(direction) {
    markSeen();
    const items = state.round.items;
    const at = items.findIndex(item => item.id === state.current);
    for (let i = at + direction; i >= 0 && i < items.length; i += direction)
        if (matches(items[i])) return show(items[i].id);
}

function show(id) {
    if (state.current !== id) markSeen();
    flush();
    state.current = id;
    state.peek = false;
    state.activePin = -1;
    $('stage').scrollTo(0, 0);
    history.replaceState(null, '', `#round=${state.round.number}&screen=${encodeURIComponent(id)}`);
    render();
    const items = visible();
    const next = items[items.findIndex(item => item.id === id) + 1];
    for (const image of [next?.image, next?.reference]) if (image) new Image().src = `images/${image.sha}.png`;
}

async function loadRound(number, screen) {
    markSeen();
    flush();
    state.current = null;
    const data = await api(`api/rounds/${number}`);
    state.round = data.round;
    state.feedback = data.feedback;
    state.earlier = data.previousFeedback;
    state.baseline = data.baseline;
    state.latest = data.latest;
    state.queue = data.queue || {};
    // A round with a queue opens on it once. Afterwards the reviewer's own
    // filter choice stays.
    const queued = Object.keys(state.queue).length > 0;
    $('status').querySelector('option[value="queued"]').hidden = !queued;
    if (queued && state.latest && recall('queue-round') !== String(number)) {
        $('status').value = 'queued';
        remember('status', 'queued');
        remember('queue-round', String(number));
    }
    if (!queued && $('status').value === 'queued') $('status').value = '';
    const group = $('group');
    const chosen = group.value || recall('group');
    group.length = 1;
    for (const name of [...new Set(state.round.items.map(item => item.group))]) group.add(new Option(name, name));
    group.value = [...group.options].some(option => option.value === chosen) ? chosen : '';
    showSuperseded();
    const items = visible();
    const start = state.round.items.find(item => item.id === screen) || items[0] || null;
    if (start) show(start.id);
    else { state.current = null; render(); }
}

function render() {
    const item = currentItem();
    const items = visible();
    renderList(items);
    renderTally(items);
    $('screen').hidden = !item;
    $('empty').hidden = Boolean(item);
    $('empty').textContent = state.round.items.length ? 'No screens match these filters.' : 'This round has no screens.';
    if (!item) return;
    const saved = entry(item.id);
    $('where').textContent = `${item.group} · ${deviceName(item.image.device)} · Page ${item.page} of ${item.pages}`;
    $('title').textContent = item.title;
    renderStatus(item, saved);
    $('note').readOnly = !state.latest;
    if (document.activeElement !== $('note')) $('note').value = saved.note;
    renderCompare(item);
    renderImages(item);
    renderPins(item, saved);
    renderEarlier(item);
    renderQuestion(item);
    renderAnswer(item);
}

// The screen's status as chips: the verdict first, then how the screen
// compares with the previous round. Seen needs no chip of its own.
function renderStatus(item, saved) {
    const chip = (text, kind, title = '') => {
        const element = document.createElement('span');
        element.className = `chip ${kind}`;
        element.textContent = text;
        if (title) element.title = title;
        return element;
    };
    const since = item.previous ? ` since round ${item.previous.round}` : '';
    const chips = [];
    if (state.queue[item.id]) chips.push(chip('Queued for you', 'queued', 'The developer asks you to look at this screen'));
    if (saved.verdict === 'needs-changes') chips.push(chip('Needs changes', 'needs-changes'));
    else if (!saved.seen) chips.push(chip('Unseen', 'unseen'));
    chips.push(chip({ new: 'New', changed: `Changed${since}`, unchanged: `Unchanged${since}` }[item.status], item.status,
        saved.carried ? `Its verdict and notes come from round ${saved.carried}` : ''));
    if (!state.latest) chips.push(chip('Read only', 'old', 'Only the newest round takes notes'));
    $('state').replaceChildren(...chips);
}

// The old image would show stretched in the new frame until the new one
// loads, so hide it in the meantime.
function setImage(element, sha) {
    const source = `images/${sha}.png`;
    if (element.getAttribute('src') === source) return;
    element.style.visibility = 'hidden';
    element.onload = () => { element.style.visibility = ''; };
    element.src = source;
}

// Compare with names a baseline: the original screen before the feature
// or the previous round. The choice stays from screen to screen; a screen
// without that baseline shows it greyed out. Holding C, or the Hold to
// compare button, shows the baseline in place of the latest images.
const originalCommit = item => (item.before || item.referenceBefore)?.commit;
function renderCompare(item) {
    const commit = originalCommit(item);
    const choices = [
        ['none', 'None'],
        ['before', `Original${commit ? ` · ${commit.slice(0, 8)}` : ''}`],
        ['previous', item.previous ? `Round ${item.previous.round}` : 'Previous round'],
    ];
    const focused = document.activeElement?.closest?.('#compare') ? state.target : null;
    $('compare').replaceChildren(...choices.map(([value, text]) => {
        const label = document.createElement('label');
        const input = document.createElement('input');
        input.type = 'radio';
        input.name = 'compare';
        input.value = value;
        input.checked = state.target === value;
        input.disabled = value !== 'none' && !hasBaseline(item, value);
        input.addEventListener('change', () => {
            state.target = value;
            remember('compare', value === 'none' ? '' : value);
            render();
        });
        label.append(input, text);
        return label;
    }));
    if (focused) $('compare').querySelector(`input[value="${focused}"]`)?.focus();
    // A rebase can move the branch's base after the originals were captured.
    const stale = state.baseline?.stale;
    $('stale').hidden = !stale;
    $('stale').textContent = stale ? `Original is from ${state.baseline.commit.slice(0, 8)}, but the branch now leaves ${state.baseline.baseRef} at ${state.baseline.current.slice(0, 8)}. Capture the base again.` : '';
    const available = hasBaseline(item, state.target);
    const chosen = choices.find(([value]) => value === state.target)[1];
    $('compare-button').classList.toggle('warn', Boolean(stale));
    $('compare-summary').textContent = state.target === 'none' ? 'Compare' :
        `Compare: ${chosen}${available ? '' : ' (not on this screen)'}${state.side ? ' · side by side' : ''}${state.diff ? ' · differences' : ''}`;
    $('compare-button').title = $('compare-summary').textContent;
    if (!available) state.peek = false;
    // Side by side already shows the baseline, so there is nothing to hold.
    $('peek').disabled = !available || state.side;
    $('show-diff').disabled = !available;
    $('show-side').disabled = !available;
    $('show-side').checked = state.side;
    $('show-diff').checked = state.diff;
    $('peek').setAttribute('aria-pressed', state.peek);
    $('stage').classList.toggle('peeking', state.peek);
}
function setPeek(on) {
    const item = currentItem();
    if (!item || state.peek === on || (on && (state.side || !hasBaseline(item, state.target)))) return;
    state.peek = on;
    renderCompare(item);
    renderImages(item);
    renderPins(item, entry(item.id));
}

// The images of one version of the screen, for the subject and the
// reference device. A version can lack one of them.
function version(item, mode = state.peek ? state.target : 'now') {
    if (mode === 'before') {
        const commit = (item.before || item.referenceBefore).commit;
        return { subject: item.before, reference: item.referenceBefore, label: `Original${commit ? ` · ${commit.slice(0, 8)}` : ''}` };
    }
    if (mode === 'previous')
        return { subject: { ...item.previous, device: item.image.device }, reference: item.previousReference, label: `Round ${item.previous.round}` };
    return { subject: item.image, reference: item.reference, label: '' };
}

function renderImages(item) {
    const shown = version(item);
    const subjectImage = shown.subject, left = shown.reference;
    const caption = device => `${deviceName(device)}${shown.label ? ` · ${shown.label}` : ''}`;
    $('subject-label').textContent = caption(item.image.device);
    $('reference-label').textContent = item.reference ? caption(item.reference.device) : 'No reference';
    // The checkboxes choose the devices, also when the chosen version lacks
    // an image for one of them. Then the stage says what is missing.
    const wantLeft = Boolean(item.reference) && state.showReference;
    const wantSubject = state.showSubject || !wantLeft;
    const showLeft = wantLeft && Boolean(left);
    const showSubject = wantSubject && Boolean(subjectImage);
    // Side by side puts the baseline of each shown device before its latest image.
    const baseline = hasBaseline(item, state.target) ? version(item, state.target) : {};
    const side = state.side && hasBaseline(item, state.target) && !state.peek;
    const baseLeft = side && wantLeft ? baseline.reference : null;
    const baseSubject = side && wantSubject ? baseline.subject : null;
    const missingIn = (label, list) => list.length ? `${label || 'This round'} has no ${list.map(deviceName).join(' or ')} capture of this screen.` : '';
    const notes = [
        missingIn(shown.label, [wantLeft && !left && item.reference.device, wantSubject && !subjectImage && item.image.device].filter(Boolean)),
        side ? missingIn(baseline.label, [wantLeft && !baseLeft && item.reference.device, wantSubject && !baseSubject && item.image.device].filter(Boolean)) : '',
    ].filter(Boolean);
    $('stage-empty').hidden = !notes.length;
    $('stage-empty').textContent = notes.join(' ');
    $('reference-frame').closest('figure').hidden = !showLeft;
    $('subject-frame').closest('figure').hidden = !showSubject;
    $('reference-base-frame').closest('figure').hidden = !baseLeft;
    $('subject-base-frame').closest('figure').hidden = !baseSubject;
    if (baseLeft) {
        $('reference-base-label').textContent = `${deviceName(item.reference.device)} · ${baseline.label}`;
        setImage($('reference-base'), baseLeft.sha);
    }
    if (baseSubject) {
        $('subject-base-label').textContent = `${deviceName(item.image.device)} · ${baseline.label}`;
        setImage($('subject-base'), baseSubject.sha);
    }
    $('show-reference-name').textContent = item.reference ? deviceName(item.reference.device) : 'No reference';
    $('show-reference-image').checked = Boolean(item.reference) && state.showReference;
    $('show-reference-image').disabled = !item.reference;
    $('show-subject-name').textContent = deviceName(item.image.device);
    $('show-subject-image').checked = wantSubject;
    $('devices-summary').textContent = [wantLeft && item.reference.device, wantSubject && item.image.device].filter(Boolean).map(deviceName).join(' · ') +
        (state.bezel ? ' · bezel' : '');
    $('devices-button').title = $('devices-summary').textContent;
    // Notes go on the current images only.
    const now = !state.peek && state.latest;
    $('reference-frame').classList.toggle('pinnable', Boolean(item.reference) && now);
    $('subject-frame').classList.toggle('pinnable', now);
    document.querySelector('.hint').hidden = !now;
    if (subjectImage) setImage($('subject'), subjectImage.sha);
    if (left) setImage($('reference'), left.sha);
    // Both captures are 300 PPI, so one scale keeps their physical sizes comparable.
    const stage = $('stage');
    // Keep a spot in place while zooming: the one under the pointer for a
    // scroll zoom, otherwise the middle of the view. From Fit, a scroll zoom
    // starts at the pointer's place in the images and other zooms at the top.
    const anchor = state.zoomAnchor ?? { x: stage.clientWidth / 2, y: stage.clientHeight / 2 };
    state.zoomAnchor = null;
    const centre = stage.classList.contains('zoomed') ? [
        (stage.scrollLeft + anchor.x) / stage.scrollWidth,
        (stage.scrollTop + anchor.y) / stage.scrollHeight,
    ] : [anchor.x / stage.clientWidth, anchor.pointer ? anchor.y / stage.clientHeight : 0];
    stage.classList.remove('zoomed');
    stage.style.height = '';
    const top = stage.getBoundingClientRect().top + window.scrollY;
    // The captions take 36 pixels above the frames. Beside the notes sidebar
    // the images fill the window above the status bar. When the notes sit
    // below, leave them room, so that a note does not move the images.
    const beside = getComputedStyle(document.querySelector('.side')).position === 'fixed';
    const status = beside ? $('statusbar').offsetHeight : 0;
    const room = Math.max(240, window.innerHeight - top - status - (beside ? 16 : 220));
    const height = room - 36;
    // With the bezel on, the device outlines fit the window, not only the captures.
    const outline = image => (state.bezel && bezel(image)) || image;
    const sizes = [showSubject && subjectImage, showLeft && left, baseLeft, baseSubject].filter(Boolean).map(outline);
    const width = stage.clientWidth - (sizes.length - 1) * 32 - 4;
    if (!sizes.length) return;
    const tallest = Math.max(...sizes.map(image => image.height));
    const fit = Math.min(1, height / tallest, width / sizes.reduce((total, image) => total + image.width, 0));
    $('show-bezel').checked = state.bezel;
    // Zoom is a scale in image pixels per CSS pixel. It moves in fixed steps,
    // with Fit in its place among them. Without a chosen zoom the images fit
    // the window. Above Fit the stage scrolls.
    state.fit = fit;
    state.steps = zoomSteps(fit);
    const scale = state.zoom ?? fit;
    const zoomed = scale > fit;
    stage.classList.toggle('zoomed', zoomed);
    if (zoomed) stage.style.height = `${room}px`;
    renderZoom(scale);
    const size = (image, frame) => {
        frame.style.width = `${Math.round(image.width * scale)}px`;
        frame.style.height = `${Math.round(image.height * scale)}px`;
        frame.firstElementChild.style.width = '100%';
        const outer = state.bezel && bezel(image), wrap = frame.parentElement;
        wrap.classList.toggle('on', Boolean(outer));
        if (!outer) {
            wrap.style.padding = wrap.style.borderRadius = '';
            return;
        }
        const { x, y, width, height } = outer.screen;
        const px = value => `${Math.round(value * scale)}px`;
        wrap.style.padding = `${px(y)} ${px(outer.width - x - width)} ${px(outer.height - y - height)} ${px(x)}`;
        wrap.style.borderRadius = px(bezelRadius);
    };
    if (subjectImage) size(subjectImage, $('subject-frame'));
    if (left) size(left, $('reference-frame'));
    if (baseLeft) size(baseLeft, $('reference-base-frame'));
    if (baseSubject) size(baseSubject, $('subject-base-frame'));
    if (zoomed) stage.scrollTo(centre[0] * stage.scrollWidth - anchor.x,
                               centre[1] * stage.scrollHeight - anchor.y);
    // Differences: tint the latest pixels that differ from the baseline of
    // the same device and size.
    const diffs = state.diff && !state.peek;
    renderDiff($('subject-frame'), diffs && showSubject && subjectImage, baseline.subject);
    renderDiff($('reference-frame'), diffs && showLeft && left, baseline.reference);
}

// Changed pixels in red over the latest image. Images are same-origin, so
// their pixels can be read. Masks are kept for the last few image pairs.
const masks = new Map();
function loadImage(sha) {
    return new Promise(resolve => {
        const image = new Image();
        image.onload = () => resolve(image);
        image.onerror = () => resolve(null);
        image.src = `images/${sha}.png`;
    });
}
function pixels(image) {
    const canvas = document.createElement('canvas');
    canvas.width = image.naturalWidth;
    canvas.height = image.naturalHeight;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    context.drawImage(image, 0, 0);
    return context.getImageData(0, 0, canvas.width, canvas.height);
}
async function diffMask(latest, base) {
    const key = `${latest.sha}:${base.sha}`;
    if (!masks.has(key)) {
        masks.set(key, (async () => {
            const [a, b] = await Promise.all([loadImage(latest.sha), loadImage(base.sha)]);
            if (!a || !b || a.naturalWidth !== b.naturalWidth || a.naturalHeight !== b.naturalHeight) return null;
            const one = pixels(a), two = pixels(b);
            const mask = new ImageData(one.width, one.height);
            for (let i = 0; i < one.data.length; i += 4) {
                const delta = Math.abs(one.data[i] - two.data[i]) + Math.abs(one.data[i + 1] - two.data[i + 1]) + Math.abs(one.data[i + 2] - two.data[i + 2]);
                if (delta > 24) mask.data.set([224, 40, 32, 170], i);
            }
            return mask;
        })());
        if (masks.size > 12) masks.delete(masks.keys().next().value);
    }
    return masks.get(key);
}
function renderDiff(frame, latest, base) {
    const canvas = frame.querySelector('canvas.diff');
    const token = latest && base && latest.width === base.width && latest.height === base.height ? `${latest.sha}:${base.sha}` : '';
    canvas.dataset.token = token;
    if (!token) {
        canvas.hidden = true;
        return;
    }
    diffMask(latest, base).then(mask => {
        if (canvas.dataset.token !== token) return;
        canvas.hidden = !mask;
        if (!mask) return;
        canvas.width = mask.width;
        canvas.height = mask.height;
        canvas.getContext('2d').putImageData(mask, 0, 0);
    });
}

function marker(number, x, y, old) {
    const element = document.createElement('span');
    element.className = `marker${old ? ' old' : ''}`;
    element.textContent = number;
    element.style.left = `${x * 100}%`;
    element.style.top = `${y * 100}%`;
    return element;
}

function renderPins(item, saved) {
    for (const old of document.querySelectorAll('.frame .marker')) old.remove();
    saved.pins.forEach((pin, index) => {
        const element = marker(index + 1, pin.x, pin.y);
        element.classList.toggle('active', index === state.activePin);
        element.addEventListener('click', event => {
            event.stopPropagation();
            focusPin(index);
        });
        // This round's pins belong to this round's images.
        if (!state.peek)
            $(pin.target === 'reference' ? 'reference-frame' : 'subject-frame').append(element);
    });
    // The previous round shows its own pins, grey, on its own images: in
    // place while peeking, or on the baseline images side by side.
    const sideBySide = state.side && hasBaseline(item, state.target) && !state.peek;
    if (state.target === 'previous' && (state.peek || sideBySide)) {
        const before = state.earlier?.items[item.id];
        const frames = sideBySide ? ['reference-base-frame', 'subject-base-frame'] : ['reference-frame', 'subject-frame'];
        (before?.pins || []).forEach((pin, index) =>
            $(frames[pin.target === 'reference' ? 0 : 1]).append(marker(index + 1, pin.x, pin.y, true)));
    }
    const list = $('pins');
    const focused = document.activeElement?.closest?.('#pins li')?.dataset.index;
    list.replaceChildren(...saved.pins.map((pin, index) => {
        const row = document.createElement('li');
        row.dataset.index = index;
        row.classList.toggle('active', index === state.activePin);
        const text = document.createElement('textarea');
        text.rows = 2;
        text.value = pin.note;
        text.readOnly = !state.latest;
        text.placeholder = 'Note for this spot';
        text.setAttribute('aria-label', `Pin ${index + 1} on ${deviceName(pin.target === 'reference' ? item.reference?.device : item.image.device)}`);
        text.addEventListener('focus', () => setActivePin(index));
        text.addEventListener('input', () => update(current => { current.pins[index].note = text.value; }, false));
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'remove';
        remove.title = `Remove pin ${index + 1}`;
        remove.setAttribute('aria-label', remove.title);
        remove.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M18 6 6 18M6 6l12 12"/></svg>';
        remove.disabled = !state.latest;
        remove.addEventListener('click', () => update(current => { current.pins.splice(index, 1); }, true, true));
        row.append(marker(index + 1, 0, 0), text, remove);
        return row;
    }));
    if (focused !== undefined) list.querySelector(`li[data-index="${focused}"] textarea`)?.focus({ preventScroll: true });
    $('pin-count').textContent = saved.pins.length ? String(saved.pins.length) : '';
    $('pins-empty').hidden = saved.pins.length > 0 || !state.latest;
}

function setActivePin(index) {
    state.activePin = index;
    renderPinHighlights();
    document.querySelectorAll('#pins li').forEach((row, i) => row.classList.toggle('active', i === index));
}
function renderPinHighlights() {
    document.querySelectorAll('.frame .marker:not(.old)').forEach(element =>
        element.classList.toggle('active', Number(element.textContent) - 1 === state.activePin));
}
function focusPin(index) {
    setActivePin(index);
    $('pins').querySelector(`li[data-index="${index}"] textarea`)?.focus({ preventScroll: true });
}

// Notes that were answered without a change to the screen: the reviewer's
// notes, quiet, and the reply below them.
// The developer's question for a queued screen. The reviewer answers in
// the notes.
function renderQuestion(item) {
    const question = state.queue[item.id]?.question || '';
    $('question').hidden = !question;
    $('question-text').textContent = question;
}

function renderAnswer(item) {
    const resolved = state.feedback.items[item.id]?.resolved;
    $('answer').hidden = !resolved;
    if (!resolved) return;
    $('answer-title').textContent = `Answered · notes from round ${resolved.round}`;
    const asked = [];
    if (resolved.note) asked.push(resolved.note);
    resolved.pins.forEach((pin, index) => asked.push(`${index + 1}. ${pin.target === 'reference' ? 'Reference: ' : ''}${pin.note}`));
    $('answer-asked').replaceChildren(...asked.map(text => Object.assign(document.createElement('p'), { textContent: text })));
    $('answer-reply').textContent = resolved.reply;
}

function renderEarlier(item) {
    const box = $('earlier');
    // An answer already shows the previous round's notes.
    const answered = state.feedback.items[item.id]?.resolved;
    const before = item.status === 'changed' && !answered ? state.earlier?.items[item.id] : null;
    box.hidden = !before;
    if (!before) return;
    const notes = (before.note ? 1 : 0) + (before.pins?.length || 0);
    const verdict = reviewVerdict(before) === 'needs-changes' ? 'needs changes' : reviewVerdict(before) ? 'seen' : 'no verdict';
    $('earlier-summary').textContent = `Round ${item.previous.round} · ${verdict}${notes ? ` · ${notes} note${notes === 1 ? '' : 's'}` : ''}`;
    const parts = [];
    if (before.note) {
        const note = document.createElement('p');
        note.textContent = before.note;
        parts.push(note);
    }
    (before.pins || []).forEach((pin, index) => {
        const line = document.createElement('p');
        line.textContent = `${index + 1}. ${pin.target === 'reference' ? 'Reference: ' : ''}${pin.note}`;
        parts.push(line);
    });
    $('earlier-body').replaceChildren(...parts);
}

// The folded screen list keeps a map of the filtered screens: one tick per
// screen with its verdict and a gap between groups. The ticks move closer
// together so that a long review still fits.
function renderMinimap(items) {
    const map = $('minimap');
    if (!$('list').hidden) return map.replaceChildren();
    const groups = new Set(items.map(item => item.group)).size;
    const room = map.clientHeight - 16;
    // Each tick takes one pitch; each space between groups one and a half.
    const pitch = Math.max(3, Math.min(14, (room - 32) / Math.max(1, items.length + 1.5 * (groups - 1))));
    map.style.setProperty('--pitch', `${pitch}px`);
    map.style.setProperty('--tick', pitch < 5 ? '1px' : '2px');
    const children = [];
    let group = null;
    for (const item of items) {
        if (group !== null && item.group !== group) {
            const gap = document.createElement('span');
            gap.className = 'group-gap';
            children.push(gap);
        }
        group = item.group;
        const mark = document.createElement('button');
        mark.type = 'button';
        mark.className = entry(item.id).verdict || '';
        const label = `${item.title}${item.pages > 1 ? ` · ${item.page}` : ''}${item.orientation === 'landscape' ? ' · landscape' : ''}`;
        mark.title = label;
        mark.setAttribute('aria-label', label);
        if (item.id === state.current) mark.setAttribute('aria-current', 'true');
        mark.addEventListener('click', () => show(item.id));
        children.push(mark);
    }
    map.replaceChildren(...children);
}

function renderList(items) {
    const list = $('list');
    renderMinimap(items);
    if (list.hidden) return;
    const children = [];
    let group = null;
    let count;
    for (const item of items) {
        if (item.group !== group) {
            group = item.group;
            const heading = document.createElement('h3');
            heading.className = 'label';
            count = document.createElement('span');
            count.textContent = '0';
            heading.append(group, count);
            children.push(heading);
        }
        count.textContent = String(Number(count.textContent) + 1);
        const link = document.createElement('a');
        link.href = `#round=${state.round.number}&screen=${encodeURIComponent(item.id)}`;
        if (item.id === state.current) link.setAttribute('aria-current', 'true');
        const dot = document.createElement('span');
        dot.className = `dot ${entry(item.id).verdict || ''}`;
        const text = document.createElement('span');
        text.textContent = `${item.title}${item.pages > 1 ? ` · ${item.page}` : ''}${item.orientation === 'landscape' ? ' · landscape' : ''}`;
        link.append(dot, text);
        // Screens whose latest images differ from the chosen baseline.
        if (differsFromBaseline(item, state.target)) {
            const mark = document.createElement('span');
            mark.className = 'differs';
            mark.textContent = '≠';
            mark.title = 'Differs from the baseline';
            link.append(mark);
        }
        link.addEventListener('click', event => {
            event.preventDefault();
            show(item.id);
        });
        children.push(link);
    }
    list.replaceChildren(...children);
    list.querySelector('[aria-current]')?.scrollIntoView({ block: 'nearest' });
}

function renderTally(items) {
    const all = state.round.items.map(item => entry(item.id).verdict);
    const count = verdict => all.filter(v => v === verdict).length;
    const position = items.findIndex(item => item.id === state.current) + 1;
    $('position').textContent = position ? `${position} of ${items.length}` : '';
    $('previous').disabled = position <= 1;
    $('next').disabled = !position || position === items.length;
    const unseen = state.round.items.filter(item => !entry(item.id).seen).length;
    const total = state.round.items.length, changes = count('needs-changes');
    $('tally').textContent = `${total - unseen} of ${total} seen` + (changes ? ` · ${changes} need${changes === 1 ? 's' : ''} changes` : '');
    $('progress-changes').style.width = `${100 * changes / total}%`;
    $('progress-seen').style.width = `${100 * (total - unseen - changes) / total}%`;
    // Finish review tells the developer that the round is reviewed. It needs
    // every screen seen; a later change reopens the round.
    const finished = state.feedback.finished;
    $('finish').hidden = Boolean(finished) || !state.latest;
    $('finish').disabled = unseen > 0;
    $('finish').title = unseen ? `${unseen} screens are still unseen` : 'Tell the developer that this round is reviewed';
    $('finished').textContent = finished ? `Review finished · ${new Date(finished).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : '';
}
async function finishReview() {
    if (!state.latest || !state.round) return;
    markSeen();
    flush();
    await saving;
    try {
        const { finished } = await api(`api/rounds/${state.round.number}/finish`, { method: 'PUT' });
        state.feedback.finished = finished;
    } catch (error) {
        $('saved').textContent = `Not saved: ${error.message}`;
        $('saved').classList.add('error');
    }
    renderTally(visible());
}

// Changes apply to the page at once. Typing saves after a pause; verdicts
// and pins save immediately. Saves run in order so the last one wins.
function update(change, now, rerender = now) {
    const item = currentItem();
    if (!item || !state.latest) return;
    const current = entry(item.id);
    // The server reopens a finished round on any change; match it here.
    state.feedback.finished = null;
    change(current);
    if (hasAnnotations(current)) current.seen = true;
    current.verdict = reviewVerdict({ ...current, verdict: null });
    // Only the tool writes where unchanged notes came from and answers. The
    // server keeps both; a changed note is no longer the carried one.
    const { carried, resolved, ...body } = current;
    const saved = state.feedback.items[item.id];
    const kept = { ...body, resolved };
    if (carried && sameAnnotations(saved, body)) kept.carried = carried;
    if (!body.seen && !body.verdict && !body.note && !body.pins.length && !resolved) delete state.feedback.items[item.id];
    else state.feedback.items[item.id] = kept;
    if (rerender) render();
    else {
        renderTally(visible());
        renderList(visible());
        renderStatus(item, current);
    }
    clearTimeout(pending?.timer);
    pending = { id: item.id, number: state.round.number, body, timer: setTimeout(flush, now ? 0 : 500) };
}
function flush() {
    if (!pending) return;
    clearTimeout(pending.timer);
    const { id, number, body } = pending;
    pending = null;
    $('saved').textContent = 'Saving';
    $('saved').classList.remove('error');
    saving = saving.then(() => api(`api/rounds/${number}/items/${encodeURIComponent(id)}`, {
        method: 'PUT', keepalive: true, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    })).then(() => {
        if (!pending) $('saved').textContent = 'Saved';
    }, error => {
        $('saved').textContent = `Not saved: ${error.message}`;
        $('saved').classList.add('error');
    });
}

function markSeen() {
    if (!currentItem() || !state.latest || entry(state.current).seen) return;
    update(current => Object.assign(current, completeFeedback(current)), true, false);
}
function setNotesShown(shown) {
    if (document.body.classList.contains('notes-hidden') !== shown) return;
    document.body.classList.toggle('notes-hidden', !shown);
    $('toggle-notes').setAttribute('aria-expanded', shown);
    remember('notes', shown ? '' : 'hidden');
    if (currentItem()) renderImages(currentItem());
}
function toggleBezel() {
    state.bezel = !state.bezel;
    remember('bezel', state.bezel ? 'on' : '');
    if (currentItem()) renderImages(currentItem());
}
const toggleNotes = () => setNotesShown(document.body.classList.contains('notes-hidden'));
// Writing a note needs its box, so it opens a folded sidebar.
function requestChanges() {
    if (!state.latest || !currentItem()) return;
    setNotesShown(true);
    $('note').focus({ preventScroll: true });
}
function renderZoom(scale) {
    const slider = $('zoom');
    const index = zoomIndex(state.steps, scale, state.zoom === null);
    slider.max = state.steps.length - 1;
    slider.value = index;
    $('zoom-ticks').replaceChildren(...state.steps.map((stop, i) => new Option('', i)));
    $('zoom-value').textContent = state.zoom === null ? `Fit · ${Math.round(scale * 100)}%` : `${Math.round(scale * 100)}%`;
    $('zoom-out').disabled = index <= 0;
    $('zoom-in').disabled = index >= state.steps.length - 1;
}
function setZoom(scale) {
    if (!currentItem()) return;
    state.zoom = scale;
    renderImages(currentItem());
}
function zoomToStop(index) {
    const stop = state.steps[Math.max(0, Math.min(state.steps.length - 1, index))];
    setZoom(stop.fit ? null : stop.scale);
}
const zoomBy = direction => zoomToStop(zoomIndex(state.steps, state.zoom ?? state.fit, state.zoom === null) + direction);
const toggleZoom = () => setZoom(state.zoom === null ? 1 : null);
// Alt (Option) and scrolling zooms by one step around the pointer. A mouse
// wheel notch is one step; a trackpad's small movements add up to one.
let wheelTravel = 0;
$('stage').addEventListener('wheel', event => {
    if (!event.altKey || !currentItem() || !event.deltaY) return;
    event.preventDefault();
    wheelTravel += event.deltaMode ? event.deltaY * 40 : event.deltaY;
    if (Math.abs(wheelTravel) < 40) return;
    const direction = wheelTravel < 0 ? 1 : -1;
    wheelTravel = 0;
    const rect = $('stage').getBoundingClientRect();
    state.zoomAnchor = { x: event.clientX - rect.left, y: event.clientY - rect.top, pointer: true };
    zoomBy(direction);
}, { passive: false });
function completeAndNext() {
    if (!state.latest || !currentItem()) return;
    step(1);
}

$('toggle-notes').addEventListener('click', toggleNotes);

// Both sidebars resize by dragging their inner edge. The widths are
// remembered; a double-click restores the default.
function resizer(handle, property, fallback, min, max, direction) {
    const root = document.documentElement;
    const saved = Number(recall(property));
    if (saved) root.style.setProperty(`--${property}`, `${Math.min(max, Math.max(min, saved))}px`);
    let start = null;
    const redraw = () => {
        if (state.round) renderMinimap(visible());
        if (currentItem()) renderImages(currentItem());
    };
    handle.addEventListener('pointerdown', event => {
        event.preventDefault();
        handle.setPointerCapture(event.pointerId);
        start = { x: event.clientX, width: handle.parentElement.getBoundingClientRect().width };
        handle.classList.add('dragging');
        document.body.classList.add('resizing');
    });
    handle.addEventListener('pointermove', event => {
        if (!start) return;
        const width = Math.round(Math.min(max, Math.max(min, start.width + direction * (event.clientX - start.x))));
        root.style.setProperty(`--${property}`, `${width}px`);
        redraw();
    });
    const end = () => {
        if (!start) return;
        start = null;
        handle.classList.remove('dragging');
        document.body.classList.remove('resizing');
        remember(property, parseInt(getComputedStyle(root).getPropertyValue(`--${property}`)) || '');
    };
    handle.addEventListener('pointerup', end);
    handle.addEventListener('pointercancel', end);
    handle.addEventListener('dblclick', () => {
        root.style.setProperty(`--${property}`, `${fallback}px`);
        remember(property, '');
        redraw();
    });
}
resizer($('list-resizer'), 'list-open', 260, 180, 520, 1);
resizer($('side-resizer'), 'side', 320, 260, 640, -1);
$('finish').addEventListener('click', finishReview);
const toggleShortcuts = () => $('shortcuts').open ? $('shortcuts').close() : $('shortcuts').showModal();
$('shortcuts-open').addEventListener('click', toggleShortcuts);
$('show-bezel').addEventListener('change', toggleBezel);
for (const [box, key, name] of [['show-reference-image', 'showReference', 'hide-reference'], ['show-subject-image', 'showSubject', 'hide-subject']])
    $(box).addEventListener('change', () => {
        // Hiding the last visible screenshot is refused.
        if (!$('show-reference-image').checked && !$('show-subject-image').checked) {
            $(box).checked = true;
            return;
        }
        state[key] = $(box).checked;
        remember(name, state[key] ? '' : 'yes');
        if (currentItem()) renderImages(currentItem());
    });
$('zoom').addEventListener('input', () => zoomToStop(Number($('zoom').value)));
$('zoom-out').addEventListener('click', () => zoomBy(-1));
$('zoom-in').addEventListener('click', () => zoomBy(1));
$('previous').addEventListener('click', () => step(-1));
$('next').addEventListener('click', () => step(1));
$('note').addEventListener('input', () => update(current => {
    current.note = $('note').value;
    current.verdict = hasAnnotations(current) ? 'needs-changes' : null;
}, false));
// The status bar's pop-up menus. One is open at a time; Esc or a click
// elsewhere closes it.
function closeMenus(except) {
    for (const button of document.querySelectorAll('.menu-button')) {
        if (button === except) continue;
        button.setAttribute('aria-expanded', 'false');
        const panel = $(button.getAttribute('aria-controls'));
        // A closed menu gives the keyboard back to the page.
        if (!panel.hidden && panel.contains(document.activeElement)) $('main').focus({ preventScroll: true });
        panel.hidden = true;
    }
}
for (const button of document.querySelectorAll('.menu-button'))
    button.addEventListener('click', () => {
        const panel = $(button.getAttribute('aria-controls'));
        closeMenus(button);
        panel.hidden = !panel.hidden;
        button.setAttribute('aria-expanded', !panel.hidden);
    });
document.addEventListener('pointerdown', event => { if (!event.target.closest('.menu')) closeMenus(); });
for (const name of ['pointerdown']) $('peek').addEventListener(name, event => { event.preventDefault(); setPeek(true); });
for (const name of ['pointerup', 'pointerleave', 'pointercancel']) $('peek').addEventListener(name, () => setPeek(false));
$('show-diff').addEventListener('change', () => { state.diff = $('show-diff').checked; render(); });
const toggleSide = () => { state.side = !state.side; remember('side', state.side ? 'on' : ''); render(); };
$('show-side').addEventListener('change', toggleSide);
// Holding C peeks at the baseline; releasing it, or leaving the window, ends the peek.
document.addEventListener('keyup', event => { if (event.key.toLowerCase() === 'c') setPeek(false); });
window.addEventListener('blur', () => setPeek(false));
for (const target of ['subject', 'reference']) $(target + '-frame').addEventListener('click', event => {
    if (!state.latest || !currentItem() || event.target.classList.contains('marker') ||
        state.peek || (target === 'reference' && !currentItem().reference)) return;
    const box = $(target + '-frame').getBoundingClientRect();
    const x = Math.min(1, Math.max(0, (event.clientX - box.left) / box.width));
    const y = Math.min(1, Math.max(0, (event.clientY - box.top) / box.height));
    const index = entry(state.current).pins.length;
    setNotesShown(true);
    update(current => { current.pins.push({ target, x, y, note: '' }); }, true);
    focusPin(index);
});
for (const name of filters) {
    const element = $(name);
    if (name !== 'group') element.value = ({ rejected: 'needs-changes', approved: 'seen' }[recall(name)] || recall(name));
    element.addEventListener('change', () => {
        remember(name, element.value);
        const items = visible();
        if (!items.some(item => item.id === state.current) && items.length) show(items[0].id);
        else render();
    });
}
$('round').addEventListener('change', () => loadRound(Number($('round').value)).catch(fail));
$('toggle-list').addEventListener('click', () => {
    const list = $('list');
    list.hidden = !list.hidden;
    $('toggle-list').setAttribute('aria-expanded', !list.hidden);
    remember('list', list.hidden ? '' : 'open');
    render();
});
document.addEventListener('keydown', event => {
    const target = event.target instanceof Element ? event.target : document.body;
    if (event.key === 'Escape' && target.closest('.menu')) {
        const button = target.closest('.menu').querySelector('.menu-button');
        closeMenus();
        button.focus();
        return;
    }
    if (event.key === 'Escape' && document.querySelector('.menu-panel:not([hidden])')) {
        closeMenus();
        return;
    }
    if ((event.key === 'Escape' || (event.key === 'Tab' && !event.shiftKey)) && target.matches('textarea')) {
        event.preventDefault();
        $('main').focus({ preventScroll: true });
        return;
    }
    // The shortcuts overlay is modal: only ? and Esc act while it is open.
    if ($('shortcuts').open && event.key !== '?') return;
    if (event.metaKey || event.ctrlKey || event.altKey || target.closest('textarea, input:not([type="checkbox"], [type="radio"], [type="range"]), [contenteditable="true"]')) return;
    // A checkbox, radio button or slider keeps the keys it uses itself;
    // letter shortcuts still reach the page.
    if (target.matches('input') && ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', ' ', 'Enter'].includes(event.key)) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
        if (!state.round) return;
        event.preventDefault();
        step(event.key === 'ArrowLeft' ? -1 : 1);
        return;
    }
    if (target.matches('select')) return;
    const actions = {
        r: requestChanges,
        '?': toggleShortcuts,
        z: toggleZoom,
        '-': () => zoomBy(-1),
        '=': () => zoomBy(1),
        '+': () => zoomBy(1),
        n: toggleNotes,
        b: toggleBezel,
        ' ': completeAndNext,
        c: () => { if (!event.repeat) setPeek(true); },
        d: () => { if (!$('show-diff').disabled) { state.diff = !state.diff; render(); } },
        s: () => { if (!$('show-side').disabled) toggleSide(); },
    };
    const action = actions[event.key.length === 1 ? event.key.toLowerCase() : event.key];
    if (!action || !state.round) return;
    event.preventDefault();
    if (event.key === ' ' && event.repeat) return;
    action();
});
window.addEventListener('resize', () => {
    if (state.round) renderMinimap(visible());
    if (currentItem()) renderImages(currentItem());
});
window.addEventListener('beforeunload', () => { markSeen(); flush(); });
window.addEventListener('hashchange', () => {
    const hash = new URLSearchParams(location.hash.slice(1));
    const number = Number(hash.get('round')), screen = hash.get('screen');
    if (number && number !== state.round?.number) {
        $('round').value = number;
        loadRound(number, screen).catch(fail);
    } else if (state.round?.items.some(item => item.id === screen) && screen !== state.current) {
        show(screen);
    }
});

async function listRounds() {
    const rounds = await api('api/rounds');
    const chosen = $('round').value;
    $('round').length = 0;
    for (const round of rounds)
        $('round').add(new Option(`Round ${round.number} · ${new Date(round.created).toLocaleDateString()}`, round.number));
    if (chosen) $('round').value = chosen;
    return rounds;
}

// Any round older than the newest one shows a banner: the server refuses
// its notes, so it is read only.
function showSuperseded() {
    const superseded = Boolean(state.round) && state.round.number < state.newest;
    $('new-round').hidden = !superseded;
    if (!superseded) return;
    $('new-round-text').textContent = `Round ${state.newest} is the newest round. This round is read only.`;
    $('open-new-round').textContent = `Open round ${state.newest}`;
}

// A server from before api/latest answers with the round list instead.
async function newestRound() {
    try {
        return (await api('api/latest')).number;
    } catch {
        return (await api('api/rounds')).at(-1)?.number;
    }
}

// Notice a round created while the page is open, instead of letting notes
// on the superseded round fail to save.
async function checkForNewRound() {
    if (!state.round || document.hidden) return;
    let number;
    try { number = await newestRound(); } catch { return; }
    if (!number || number <= state.newest) return;
    state.newest = number;
    await listRounds().catch(() => {});
    if (state.latest) {
        flush();
        state.latest = false;
        render();
    }
    showSuperseded();
    if (currentItem()) renderImages(currentItem());
}
$('open-new-round').addEventListener('click', () => {
    $('round').value = state.newest;
    loadRound(state.newest, state.current).catch(fail);
});
setInterval(checkForNewRound, 5000);
document.addEventListener('visibilitychange', checkForNewRound);

function fail(error) {
    $('empty').hidden = false;
    $('screen').hidden = true;
    $('empty').textContent = `Could not load the review: ${error.message}`;
}

// The screen list sticks below the header, whose height depends on wrapping.
// The status bar spans the space between the two sidebars.
new ResizeObserver(() => document.documentElement.style.setProperty('--bar', `${$('bar').offsetHeight}px`)).observe($('bar'));
new ResizeObserver(() => document.documentElement.style.setProperty('--list', `${document.querySelector('.sidebar').offsetWidth}px`)).observe(document.querySelector('.sidebar'));

(async () => {
    if (recall('notes') === 'hidden') setNotesShown(false);
    if (recall('list')) {
        $('list').hidden = false;
        $('toggle-list').setAttribute('aria-expanded', 'true');
    }
    const rounds = await listRounds();
    if (!rounds.length) throw new Error('there are no rounds yet. Run the round command first.');
    state.newest = rounds.at(-1).number;
    const hash = new URLSearchParams(location.hash.slice(1));
    const number = rounds.at(-1).number;
    $('round').value = number;
    await loadRound(number, hash.get('screen'));
})().catch(fail);
