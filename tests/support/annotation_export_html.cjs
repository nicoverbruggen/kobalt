// Run against HTML extracted from the production exporter. No browser or network is used.
// Checks search, date sorting, annotation numbers, the styling toggle, printing every annotation and local times in two zones.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const html = fs.readFileSync(process.argv[2], 'utf8');
const markup = html.replace(/<script>[\s\S]*?<\/script>/g, '');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const decode = text => text.replace(/&(amp|lt|gt|quot);/g, (_, name) => ({amp:'&',lt:'<',gt:'>',quot:'"'}[name]));
const text = value => decode(value.replace(/<[^>]*>/g, ''));
assert.ok(!/\b(fetch|XMLHttpRequest|import|localStorage|sessionStorage)\b/.test(script));
assert.ok(!/<script\b[^>]*src=|<link\b[^>]*href=/i.test(html));
assert.ok(markup.includes('>#1</a>') && !markup.includes('>Annotation 001</a>') && !markup.includes('Page '));
const allIds = [...markup.matchAll(/\bid="([^"]+)"/g)].map(([,id]) => id);
assert.equal(new Set(allIds).size, allIds.length);
for (const [,id] of markup.matchAll(/href="#([^"]+)"/g)) assert.ok(allIds.includes(id), id);
class Element {
  constructor(textContent = '') {
    Object.assign(this, {textContent, hidden:false, value:'', events:{}, children:[], dataset:{}});
  }
  addEventListener(type, listener) { this.events[type] = listener; }
  focus() { this.focused = true; }
  appendChild(child) {
    if (child.parentElement) child.parentElement.children = child.parentElement.children.filter(item => item !== child);
    child.parentElement = this;
    this.children.push(child);
  }
}
function check(timeZone) {
  const ids = new Map();
  for (const id of ['query', 'clear-search', 'search-status', 'no-results', 'search-toolbar', 'chapter-summary', 'chapter-navigation', 'sort-order', 'chronological', 'show-markup', 'top']) ids.set(id, new Element());
  ids.get('sort-order').value = 'newest';
  const links = new Map([...markup.matchAll(/<nav[\s\S]*?<\/nav>/g)][0][0].matchAll(/<a href="#([^"]+)">/g).map(([,id]) => {
    const link = new Element(); link.parentElement = new Element(); return [id, link];
  }));
  const articles = [];
  const sections = [...markup.matchAll(/<section\b([^>]*)>([\s\S]*?)<\/section>/g)].map(([,attrs,body]) => {
    const section = new Element(); section.id = attrs.match(/\bid="([^"]+)"/)[1];
    const heading = new Element(text(body.match(/<h2[^>]*>([\s\S]*?)<\/h2>/)[1]));
    for (const [,id,created,content] of body.matchAll(/<article\b[^>]*id="annotation-(\d+)" data-created="([^"]+)"[^>]*>([\s\S]*?)<\/article>/g)) {
      const article = new Element(text(content));
      article.number = Number(id); article.dataset.created = created;
      section.appendChild(article); articles.push(article);
    }
    section.querySelector = selector => { assert.equal(selector, 'h2'); return heading; };
    section.querySelectorAll = selector => { assert.equal(selector, 'article'); return section.children.slice(); };
    return section;
  });
  const times = [...markup.matchAll(/<time data-local-time datetime="([^"]+)">([^<]*)<\/time>/g)].map(([,instant,fallback]) => {
    const time = new Element(fallback); time.getAttribute = name => { assert.equal(name,'datetime'); return instant; }; return time;
  });
  const events = {};
  const document = {
    getElementById: id => { assert.ok(ids.has(id), id); return ids.get(id); },
    querySelectorAll: selector => selector === 'time[data-local-time]' ? times : (assert.equal(selector, 'section.chapter'), sections),
    querySelector: selector => links.get(selector.match(/href="#([^"]*)"/)[1]) || null
  };
  const DateTimeFormat = class extends Intl.DateTimeFormat {
    constructor(locale, options) { super('en-GB', {...options, timeZone}); }
  };
  vm.runInNewContext(script, {document, window:{addEventListener:(name, action) => { events[name] = action; }}, Intl:{DateTimeFormat}});
  assert.equal(articles.length, 500);
  assert.equal(times.length, 501);
  const format = new DateTimeFormat(undefined, {year:'numeric',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit',second:'2-digit',timeZoneName:'short'});
  assert.equal(times[0].textContent, format.format(new Date('2026-10-24T22:30:07.123Z')));
  assert.ok(times[0].textContent.includes(timeZone === 'Europe/Brussels' ? '25 Oct' : '24 Oct'));
  assert.equal(ids.get('search-toolbar').hidden, false);
  const markupControl = ids.get('show-markup');
  if (markupControl) {
    assert.equal(markupControl.checked, true);
    assert.equal(ids.get('top').dataset.markup, 'on');
    markupControl.checked = false;
    markupControl.events.change();
    assert.equal(ids.get('top').dataset.markup, 'off');
  } else {
    assert.ok(!markup.includes('id="show-markup"'));
  }
  const query = ids.get('query');
  const sort = ids.get('sort-order');
  const timeline = ids.get('chronological');
  const visible = () => (sort.value === 'position' ? sections.flatMap(section => section.children) : timeline.children).filter(article => !article.hidden);
  const numbers = () => visible().map(article => article.number);
  function search(value, count) {
    query.value = value; query.events.input();
    assert.equal(visible().length, count, value);
    assert.equal(ids.get('no-results').hidden, count !== 0);
    if (sort.value === 'position') for (const section of sections) {
      const any = section.children.some(article => !article.hidden);
      assert.equal(section.hidden, !any);
      assert.equal(links.get(section.id).parentElement.hidden, !any);
    }
  }
  assert.deepEqual(numbers(), Array.from({length:500}, (_,i) => i+1));
  for (const value of ['Natasha', 'NATÁSHA', '  natasha  ']) search(value, 250);
  search('prince pierre', 250);
  search('note-only', 125);
  for (const value of ['not-a-real-passage', '<script>alert(1)</script>', '[.*+?^${}()|\\]']) search(value, 0);
  search('   ', 500);
  function order(value) {
    sort.value = value; sort.events.change();
    const expected = articles.filter(article => !article.hidden).slice().sort((a,b) => {
      const difference = value === 'position' ? 0 : value === 'newest' ? Date.parse(b.dataset.created)-Date.parse(a.dataset.created) : Date.parse(a.dataset.created)-Date.parse(b.dataset.created);
      return difference || a.number-b.number;
    });
    assert.deepEqual(numbers(), expected.map(article => article.number));
    assert.equal(ids.get('chapter-navigation').hidden, value !== 'position');
    assert.equal(timeline.hidden, value === 'position');
  }
  for (const value of ['newest', 'oldest', 'position', 'newest']) order(value);
  search('note-only', 125);
  order('oldest');
  events.beforeprint(); assert.equal(visible().length, 500); order('oldest');
  const filteredCount = 125;
  events.afterprint(); assert.equal(visible().length, filteredCount); assert.equal(query.value, 'note-only');
  if (markupControl) {
    assert.equal(ids.get('top').dataset.markup, 'off');
    markupControl.checked = true;
    markupControl.events.change();
    assert.equal(ids.get('top').dataset.markup, 'on');
  }
  assert.equal(visible().length, filteredCount);
  ids.get('clear-search').events.click();
  assert.equal(query.value, ''); assert.equal(visible().length, 500); assert.ok(query.focused);
  search('Natasha', 250);
  let prevented = false;
  query.events.keydown({key:'Escape', preventDefault(){ prevented = true; }});
  assert.ok(prevented); assert.equal(query.value, ''); assert.equal(visible().length, 500);
  assert.equal(ids.get('search-status').textContent, '500 annotations');
  order('position');
  assert.equal(timeline.children.length, 0);
  console.log(`PASS: 500 annotations; search, sorting, markup toggle, fixed numbering, printing and ${timeZone} timestamps`);
}
check('Europe/Brussels');
check('America/Los_Angeles');
