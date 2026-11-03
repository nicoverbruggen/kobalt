import { hasAnnotations, reviewVerdict, completeFeedback, wasSeen } from './feedback.mjs';
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
const state = { round: null, feedback: { items: {} }, earlier: null, latest: false, current: null, showPrevious: false, activePin: -1 };
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
    return { seen: wasSeen(saved), verdict: reviewVerdict(saved), note: saved?.note || '', pins: saved?.pins ? saved.pins.map(pin => ({ ...pin })) : [], carried: saved?.carried };
}
function matches(item) {
    const { verdict, note, pins } = entry(item.id);
    if ($('group').value && item.group !== $('group').value) return false;
    if ($('orientation').value && item.orientation !== $('orientation').value) return false;
    switch ($('status').value) {
    case 'open': return !wasSeen(state.feedback.items[item.id]);
    case 'seen': return wasSeen(state.feedback.items[item.id]);
    case 'needs-changes': return verdict === 'needs-changes';
    case 'approved': return verdict === 'approved';
    case 'changed': return item.status === 'changed';
    case 'noted': return Boolean(note || pins.length);
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
    state.showPrevious = false;
    state.activePin = -1;
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
    state.latest = data.latest;
    const group = $('group');
    const chosen = group.value || recall('group');
    group.length = 1;
    for (const name of [...new Set(state.round.items.map(item => item.group))]) group.add(new Option(name, name));
    group.value = [...group.options].some(option => option.value === chosen) ? chosen : '';
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
    $('where').textContent = `${item.group} · ${deviceName(item.image.device)} · page ${item.page} of ${item.pages}`;
    $('title').textContent = item.title;
    renderStatus(item, saved);
    $('add-note').disabled = !state.latest;
    $('note').readOnly = !state.latest;
    $('note-box').hidden = state.noteOpen !== item.id && !saved.note;
    if (document.activeElement !== $('note')) $('note').value = saved.note;
    $('subject-label').textContent = deviceName(item.image.device);
    $('switch').hidden = !item.previous;
    $('show-reference').setAttribute('aria-pressed', !state.showPrevious);
    $('show-previous').setAttribute('aria-pressed', state.showPrevious);
    renderImages(item);
    renderPins(item, saved);
    renderEarlier(item);
}

function renderStatus(item, saved) {
    const since = item.previous ? ` since round ${item.previous.round}` : '';
    $('state').textContent = { new: 'New in this round', changed: `Changed${since}`, unchanged: `Unchanged${since}` }[item.status] +
        (saved.seen ? ' · seen' : ' · unseen') +
        (saved.verdict === 'needs-changes' ? ' · needs changes' : '') +
        (saved.carried ? ` · verdict from round ${saved.carried}` : '') + (state.latest ? '' : ' · read only, not the latest round');
    $('state').className = `label${item.status === 'changed' ? ' state-changed' : ''}`;
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

function renderImages(item) {
    const left = state.showPrevious && item.previous ? { ...item.previous, device: item.image.device } : item.reference;
    $('reference-label').textContent = state.showPrevious ? `Round ${item.previous.round}` :
        item.reference ? deviceName(item.reference.device) : 'No reference';
    $('reference-frame').parentElement.hidden = !left;
    $('reference-frame').classList.toggle('pinnable', Boolean(item.reference && !state.showPrevious && state.latest));
    setImage($('subject'), item.image.sha);
    if (left) setImage($('reference'), left.sha);
    // Both captures are 300 PPI, so one scale keeps their physical sizes comparable.
    const stage = $('stage');
    const top = stage.getBoundingClientRect().top + window.scrollY;
    // Leave room for the buttons and the note box, so that rejecting a
    // screen does not move the images or scroll the page.
    const height = Math.max(240, window.innerHeight - top - 220);
    const width = stage.clientWidth - (left ? 28 : 0) - 4;
    const tallest = Math.max(item.image.height, left?.height || 0);
    const scale = Math.min(1, height / tallest, width / (item.image.width + (left?.width || 0)));
    const size = (image, frame) => {
        frame.style.width = `${Math.round(image.width * scale)}px`;
        frame.style.height = `${Math.round(image.height * scale)}px`;
        frame.firstElementChild.style.width = '100%';
    };
    size(item.image, $('subject-frame'));
    if (left) size(left, $('reference-frame'));
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
        if (pin.target !== 'reference' || !state.showPrevious)
            $(pin.target === 'reference' ? 'reference-frame' : 'subject-frame').append(element);
    });
    if (state.showPrevious) {
        const before = state.earlier?.items[item.id];
        (before?.pins || []).forEach((pin, index) => {
            if (pin.target !== 'reference') $('reference-frame').append(marker(index + 1, pin.x, pin.y, true));
        });
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
        remove.textContent = 'Remove';
        remove.disabled = !state.latest;
        remove.addEventListener('click', () => update(current => { current.pins.splice(index, 1); }, true, true));
        row.append(marker(index + 1, 0, 0), text, remove);
        return row;
    }));
    if (focused !== undefined) list.querySelector(`li[data-index="${focused}"] textarea`)?.focus({ preventScroll: true });
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

function renderEarlier(item) {
    const box = $('earlier');
    const before = item.status === 'changed' ? state.earlier?.items[item.id] : null;
    box.hidden = !before;
    if (!before) return;
    const parts = [];
    const heading = document.createElement('p');
    heading.className = 'label';
    heading.textContent = `Round ${item.previous.round}: ${reviewVerdict(before) || 'no verdict'}`;
    parts.push(heading);
    if (before.note) {
        const note = document.createElement('p');
        note.textContent = before.note;
        parts.push(note);
    }
    (before.pins || []).forEach((pin, index) => {
        const line = document.createElement('p');
        line.textContent = `${index + 1}. ${pin.target === 'reference' ? 'Kobo reference: ' : ''}${pin.note}`;
        parts.push(line);
    });
    if (before.pins?.length) {
        const hint = document.createElement('p');
        hint.className = 'label';
        hint.textContent = 'Press P to see these pins on the previous image';
        parts.push(hint);
    }
    box.replaceChildren(...parts);
}

function renderList(items) {
    const list = $('list');
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
    $('tally').textContent = `${state.round.items.filter(item => entry(item.id).seen).length} seen · ${count('needs-changes')} need changes · ${state.round.items.filter(item => !entry(item.id).seen).length} unseen`;
}

// Changes apply to the page at once. Typing saves after a pause; verdicts
// and pins save immediately. Saves run in order so the last one wins.
function update(change, now, rerender = now) {
    const item = currentItem();
    if (!item || !state.latest) return;
    const current = entry(item.id);
    change(current);
    if (hasAnnotations(current)) current.seen = true;
    current.verdict = reviewVerdict({ ...current, verdict: null });
    const { carried, ...body } = current;
    if (!body.seen && !body.verdict && !body.note && !body.pins.length) delete state.feedback.items[item.id];
    else state.feedback.items[item.id] = body;
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
function requestChanges() {
    if (!state.latest || !currentItem()) return;
    state.noteOpen = state.current;
    render();
    $('note').focus({ preventScroll: true });
}
function completeAndNext() {
    if (!state.latest || !currentItem()) return;
    step(1);
}

$('add-note').addEventListener('click', requestChanges);
$('previous').addEventListener('click', () => step(-1));
$('next').addEventListener('click', () => step(1));
$('note').addEventListener('input', () => update(current => {
    current.note = $('note').value;
    current.verdict = hasAnnotations(current) ? 'needs-changes' : null;
}, false));
$('show-reference').addEventListener('click', () => { state.showPrevious = false; render(); });
$('show-previous').addEventListener('click', () => { state.showPrevious = true; render(); });
for (const target of ['subject', 'reference']) $(target + '-frame').addEventListener('click', event => {
    if (!state.latest || !currentItem() || event.target.classList.contains('marker') ||
        (target === 'reference' && (state.showPrevious || !currentItem().reference))) return;
    const box = $(target + '-frame').getBoundingClientRect();
    const x = Math.min(1, Math.max(0, (event.clientX - box.left) / box.width));
    const y = Math.min(1, Math.max(0, (event.clientY - box.top) / box.height));
    const index = entry(state.current).pins.length;
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
    if ((event.key === 'Escape' || (event.key === 'Tab' && !event.shiftKey)) && target.matches('textarea')) {
        event.preventDefault();
        $('main').focus({ preventScroll: true });
        return;
    }
    if (event.metaKey || event.ctrlKey || event.altKey || target.closest('textarea, input, [contenteditable="true"]')) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
        if (!state.round) return;
        event.preventDefault();
        step(event.key === 'ArrowLeft' ? -1 : 1);
        return;
    }
    if (target.matches('select')) return;
    const actions = {
        r: requestChanges,
        ' ': completeAndNext,
        p: () => { if (currentItem()?.previous) { state.showPrevious = !state.showPrevious; render(); } },
    };
    const action = actions[event.key.length === 1 ? event.key.toLowerCase() : event.key];
    if (!action || !state.round) return;
    event.preventDefault();
    if (event.key === ' ' && event.repeat) return;
    action();
});
window.addEventListener('resize', () => { if (currentItem()) renderImages(currentItem()); });
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

function fail(error) {
    $('empty').hidden = false;
    $('screen').hidden = true;
    $('empty').textContent = `Could not load the review: ${error.message}`;
}

// The screen list sticks below the header, whose height depends on wrapping.
new ResizeObserver(() => document.documentElement.style.setProperty('--bar', `${$('bar').offsetHeight}px`)).observe($('bar'));

(async () => {
    if (recall('list')) {
        $('list').hidden = false;
        $('toggle-list').setAttribute('aria-expanded', 'true');
    }
    const rounds = await api('api/rounds');
    if (!rounds.length) throw new Error('there are no rounds yet. Run the round command first.');
    for (const round of rounds)
        $('round').add(new Option(`Round ${round.number} · ${new Date(round.created).toLocaleDateString()}`, round.number));
    const hash = new URLSearchParams(location.hash.slice(1));
    const number = rounds.at(-1).number;
    $('round').value = number;
    await loadRound(number, hash.get('screen'));
})().catch(fail);
