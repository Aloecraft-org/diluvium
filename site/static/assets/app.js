// diluvium.aloecraft.org -- the page's wiring.
//
// Runs as a module after the classic scripts index.html loads first, which
// define the globals it reaches for: Terminal and FitAddon (xterm.js) and
// Prism (core + lua). The imports below are stamped with content hashes by
// site/render.py at build time, so nothing here needs a bundler.
//
// Surface:
//   wireCopy(buttonId, sourceId)   a copy button
//   wireMirror(spec)               version badge, downloads and release note for one mirror
//   wireBuildinfo()                what the DRT binary carries, off BUILDINFO.txt
//   wireRepl()                     the interpreter panel
//
// Configurable values: COPY_BUTTONS, MIRRORS, EXAMPLES, TERM_THEME.

import { installDiluviumGrammar, installShellGrammar } from './prism-diluvium.js';
import { Repl } from './repl/terminal.js';
import { loadRuntime } from './repl/runtime.js';
import {
  RELEASE_BASE, DRT_BASE, KERNEL_URL, TARGETS, DRT_TARGETS,
  guessTarget, loadIndex, loadBuildinfo, assetUrl, notesUrl,
} from './release.js';

const $ = (id) => document.getElementById(id);

// (button, the element whose text it copies)
const COPY_BUTTONS = [
  ['copy-install', 'install-cmd'],
  ['copy-install-2', 'install-cmd-2'],
  ['copy-install-drt', 'install-drt'],
  ['copy-install-dollup', 'install-dollup'],
];

// One entry per mirror the page reads. `name` prefixes the badge; the ids
// name the elements in index.html that site/check.py insists on.
const MIRRORS = [
  { name: 'diluvium', base: RELEASE_BASE, targets: TARGETS,
    badge: 'version-badge', list: 'downloads', note: 'release-note' },
  { name: 'drt', base: DRT_BASE, targets: DRT_TARGETS,
    badge: 'drt-version-badge', list: 'drt-downloads', note: 'drt-release-note' },
];

// The chips under the terminal. feed() splits on newlines and submits each
// line, so a multi-line example opens continuation prompts exactly as typing
// it would.
const EXAMPLES = [
  { label: 'interpolation', code: 'local who = "world"\nprint($"Hello, {who}! pi is {math.pi::%.3f}")' },
  { label: 'switch', code: 'local code = 302\nswitch code do\ncase 200, 204 then print("ok")\ncase 301, 302 then print("redirected")\ndefault print("unexpected")\nend' },
  { label: 'safe navigation', code: 'local cfg = { server = {} }\nprint(cfg?.server?.port ?? 8080)\nprint(cfg?.missing?.deep?.value)' },
  { label: 'defer', code: 'do\n  defer print("cleaned up")\n  print("working")\nend' },
];

// The terminal is dark in both page themes, the way an editor's terminal
// pane is. `background` matches .term in styles.css so the padding around
// the canvas is the same colour.
const TERM_THEME = {
  background: '#12141a',
  foreground: '#d7dae0',
  cursor: '#7fd1b9',
  selectionBackground: '#2f3a44',
};

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

/* ------------------------------------------------------------------ *
 * Code samples
 * ------------------------------------------------------------------ */
installDiluviumGrammar();
installShellGrammar();
Prism.highlightAll();

/* ------------------------------------------------------------------ *
 * Copy buttons
 * ------------------------------------------------------------------ */
function wireCopy(buttonId, sourceId) {
  const button = $(buttonId);
  const source = $(sourceId);
  if (!button || !source) return;
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(source.textContent.trim());
      button.textContent = 'Copied';
    } catch {
      // Clipboard is origin- and permission-gated; selecting the text is
      // the honest fallback rather than pretending it worked.
      const range = document.createRange();
      range.selectNodeContents(source);
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
      button.textContent = 'Select + copy';
    }
    setTimeout(() => { button.textContent = 'Copy'; }, 1800);
  });
}

/* ------------------------------------------------------------------ *
 * The release mirrors
 * ------------------------------------------------------------------ */
// depth: one mirror's badge, downloads and note; returns its latest entry
async function wireMirror(m) {
  const list = $(m.list);
  const badge = $(m.badge);
  const note = $(m.note);

  let latest = null;
  try {
    ({ latest } = await loadIndex(m.base));
  } catch (err) {
    // The page is still fully usable without the mirror: the REPL has its
    // own failure path, and the downloads fall back to the tag directory
    // through `latest/`, which is a symlink the mirror maintains.
    if (note) note.textContent = `Could not reach the ${m.name} mirror; links point at ${m.base}/latest/.`;
  }

  const tag = latest?.tag || 'latest';
  if (latest) {
    const facts = latest.facts || {};
    const bits = [latest.version || latest.tag];
    if (latest.lua_base) bits.push(`Lua ${latest.lua_base}`);
    if (facts.dv_abi != null) bits.push(`dv ABI ${facts.dv_abi}`);
    if (facts.diluvium) bits.push(`diluvium ${String(facts.diluvium).slice(0, 12)}`);
    if (badge) {
      badge.textContent = `${m.name} ${latest.version || latest.tag}`;
      badge.title = bits.join(' · ');
    }
    if (note) {
      const notes = notesUrl(m.base, latest);
      note.innerHTML = `Showing <code>${esc(latest.tag)}</code>${latest.date ? ` (${esc(latest.date)})` : ''}. `
        + (notes ? `<a href="${esc(notes)}">Release notes</a> · ` : '')
        + `<a href="${esc(m.base)}/">all releases</a>`;
    }
  }

  if (!list) return latest;
  const preferred = guessTarget();
  const ordered = [...m.targets].sort(
    (a, b) => (b.id === preferred) - (a.id === preferred),
  );
  list.innerHTML = '';
  for (const t of ordered) {
    const li = document.createElement('li');
    if (t.id === preferred) li.className = 'preferred';
    const a = document.createElement('a');
    a.href = assetUrl(m.base, tag, t.asset);
    a.textContent = t.label;
    a.setAttribute('download', '');
    li.appendChild(a);
    if (t.id === preferred) {
      const tick = document.createElement('span');
      tick.className = 'tag';
      tick.textContent = 'your platform';
      li.appendChild(tick);
    }
    list.appendChild(li);
  }
  const sums = document.createElement('li');
  sums.className = 'sums';
  sums.innerHTML = `<a href="${esc(assetUrl(m.base, tag, 'SHA256SUMS.txt'))}">SHA256SUMS.txt</a>`;
  list.appendChild(sums);
  return latest;
}

// depth: the DRT connector list, read off the artifact rather than written here
async function wireBuildinfo() {
  const el = $('drt-buildinfo');
  if (!el) return;
  let info;
  try {
    info = await loadBuildinfo(DRT_BASE);
  } catch {
    return;   // the template's own sentence stands
  }
  const profiles = Object.keys(info)
    .filter((k) => /^profile\.\w+\.connectors$/.test(k))
    .map((k) => `<code>${esc(k.split('.')[1])}</code>: ${esc(info[k].split(',').join(', '))}`);
  if (!profiles.length) return;
  const facts = [];
  if (info.tag) facts.push(`<code>${esc(info.tag)}</code>`);
  if (info.diluvium) facts.push(`embeds diluvium <code>${esc(info.diluvium.slice(0, 12))}</code>`);
  if (info.dv_abi) facts.push(`dv ABI ${esc(info.dv_abi)}`);
  el.innerHTML = `${facts.join(', ')} — ${profiles.join('; ')}.`;
}

/* ------------------------------------------------------------------ *
 * The interpreter
 * ------------------------------------------------------------------ */
// depth: xterm, the kernel, the line editor, the example chips
async function wireRepl() {
  const status = $('repl-status');
  const host = $('term');
  if (!host) return;

  const term = new Terminal({
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
    fontSize: 13,
    cursorBlink: true,
    convertEol: false,
    scrollback: 2000,
    theme: TERM_THEME,
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(host);
  fit.fit();

  let resizeTimer;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => { try { fit.fit(); } catch { /* detached */ } }, 100);
  });

  let repl;
  try {
    // Not `repl?.output(t) ?? term.write(t)`: output() returns undefined, so
    // the ?? branch fires on every successful write and everything prints
    // twice. The runtime is loaded before the Repl exists, hence the guard.
    const onOutput = (text) => {
      if (repl) repl.output(text);
      else term.write(text.replace(/\n/g, '\r\n'));
    };
    const rt = await loadRuntime(KERNEL_URL, onOutput);
    repl = new Repl(term, rt);

    status.textContent = rt.hasRepl ? 'ready' : 'ready (older build: no completion)';
    status.classList.add('ok');

    term.write('Diluvium — the real runtime, compiled to WebAssembly.\r\n');
    term.write('Try  print($"hi {1+1}")  or press Tab after  string.\r\n\r\n');
    repl.redraw();

    term.onData((d) => repl.feed(d));

    $('repl-reset')?.addEventListener('click', () => {
      repl.reset();
      term.focus();
    });

    const examples = $('examples');
    if (examples) {
      for (const ex of EXAMPLES) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'chip';
        b.textContent = ex.label;
        b.addEventListener('click', () => {
          term.focus();
          repl.feed(ex.code + '\r');
        });
        examples.appendChild(b);
      }
    }

    term.focus();
  } catch (err) {
    status.textContent = 'unavailable';
    status.classList.add('bad');
    term.write('\r\nCould not load the interpreter.\r\n');
    term.write(String(err) + '\r\n\r\n');
    term.write(`Tried: ${KERNEL_URL}\r\n`);
    term.write('The downloads below still work.\r\n');
  }
}

/* ------------------------------------------------------------------ */
for (const [button, source] of COPY_BUTTONS) wireCopy(button, source);

Promise.all(MIRRORS.map(wireMirror)).then(([diluvium, drt]) => {
  const footer = $('footer-version');
  if (!footer) return;
  const parts = [];
  if (diluvium) parts.push(`${diluvium.tag}${diluvium.date ? ` · ${diluvium.date}` : ''}`);
  if (drt) parts.push(`DRT ${drt.tag}`);
  footer.textContent = parts.join(' · ');
});
wireBuildinfo();
wireRepl();
