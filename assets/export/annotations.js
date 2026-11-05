(() => {
  'use strict';
  const formatter = new Intl.DateTimeFormat(undefined, {
    year: 'numeric', month: 'short', day: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit', timeZoneName: 'short'
  });
  for (const time of document.querySelectorAll('time[data-local-time]')) {
    const instant = new Date(time.getAttribute('datetime'));
    if (Number.isFinite(instant.getTime())) time.textContent = formatter.format(instant);
  }
  const input = document.getElementById('query');
  const markup = document.getElementById('show-markup');
  const root = document.getElementById('top');
  const kind = 'annotations';
  if (markup) {
    markup.checked = true;
    root.dataset.markup = 'on';
    markup.addEventListener('change', () => { root.dataset.markup = markup.checked ? 'on' : 'off'; });
  }
  const clear = document.getElementById('clear-search');
  const status = document.getElementById('search-status');
  const empty = document.getElementById('no-results');
  const contents = document.getElementById('chapter-summary');
  const navigation = document.getElementById('chapter-navigation');
  const sort = document.getElementById('sort-order');
  sort.value = 'position';
  const chronological = document.getElementById('chronological');
  const normalize = value => value.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
  let position = 0;
  const chapters = Array.from(document.querySelectorAll('section.chapter')).map(section => ({
    section,
    link: document.querySelector('nav a[href="#' + section.id + '"]'),
    rows: Array.from(section.querySelectorAll('article')).map(article => ({
      article,
      position: position++,
      created: Date.parse(article.dataset.created),
      text: normalize(section.querySelector('h2').textContent + ' ' + article.textContent)
    }))
  }));
  const rows = chapters.flatMap(chapter => chapter.rows);
  function update() {
    const terms = normalize(input.value).split(' ').filter(Boolean);
    let count = 0;
    let matchingChapters = 0;
    for (const chapter of chapters) {
      let matches = 0;
      for (const row of chapter.rows) {
        const visible = terms.every(term => row.text.includes(term));
        row.article.hidden = !visible;
        if (visible) matches++;
      }
      chapter.section.hidden = sort.value !== 'position' || matches === 0;
      chapter.link.parentElement.hidden = matches === 0;
      count += matches;
      if (matches) matchingChapters++;
    }
    navigation.hidden = sort.value !== 'position';
    chronological.hidden = sort.value === 'position';
    status.textContent = terms.length ? count + ' of ' + rows.length + ' ' + kind : rows.length + ' ' + kind;
    contents.textContent = terms.length ? 'Browse ' + matchingChapters + ' matching chapters' : 'Browse ' + chapters.length + ' chapters';
    empty.hidden = count !== 0;
    clear.disabled = input.value.length === 0;
  }
  sort.addEventListener('change', () => {
    if (sort.value === 'position') {
      for (const chapter of chapters)
        for (const row of chapter.rows) chapter.section.appendChild(row.article);
    } else {
      const ordered = rows.slice().sort((a, b) => {
        if (Number.isFinite(a.created) !== Number.isFinite(b.created)) return Number.isFinite(a.created) ? -1 : 1;
        const difference = sort.value === 'newest' ? b.created - a.created : a.created - b.created;
        return difference || a.position - b.position;
      });
      for (const row of ordered) chronological.appendChild(row.article);
    }
    update();
  });
  input.addEventListener('input', update);
  clear.addEventListener('click', () => { input.value = ''; update(); input.focus(); });
  input.addEventListener('keydown', event => { if (event.key === 'Escape') { event.preventDefault(); input.value = ''; update(); } });
  let printQuery = null;
  window.addEventListener('beforeprint', () => {
    printQuery = input.value;
    input.value = '';
    update();
  });
  window.addEventListener('afterprint', () => {
    if (printQuery !== null) { input.value = printQuery; printQuery = null; update(); }
  });
  document.getElementById('search-toolbar').hidden = false;
  update();
})();
