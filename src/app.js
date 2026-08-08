import './styles.css';
import '@xterm/xterm/css/xterm.css';

import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';

import { Prism, installDiluviumGrammar } from './prism-diluvium.js';
import { Repl } from './repl/terminal.js';
import { loadRuntime } from './repl/runtime.js';
import {
  KERNEL_URL, loadIndex, targets, guessTarget, assetUrl, notesUrl,
} from './release.js';

const $ = (id) => document.getElementById(id);

/* ------------------------------------------------------------------ *
 * Code samples
 * ------------------------------------------------------------------ */
installDiluviumGrammar();
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
wireCopy('copy-install', 'install-cmd');
wireCopy('copy-install-2', 'install-cmd-2');

/* ------------------------------------------------------------------ *
 * The release mirror
 * ------------------------------------------------------------------ */
async function wireRelease() {
  const list = $('downloads');
  const badge = $('version-badge');
  const note = $('release-note');
  const footer = $('footer-version');

  let latest = null;
  try {
    ({ latest } = await loadIndex());
  } catch (err) {
    // The page is still fully usable without the mirror: the REPL has its
    // own failure path, and the downloads fall back to the tag directory
    // through `latest/`, which is a symlink the mirror maintains.
    if (note) note.textContent = 'Could not reach the release mirror; links point at /release/latest/.';
  }

  const tag = latest?.tag || 'latest';
  if (latest) {
    const bits = [latest.version || latest.tag];
    if (latest.lua_base) bits.push(`Lua ${latest.lua_base}`);
    if (badge) {
      badge.textContent = latest.version || latest.tag;
      badge.title = bits.join(' · ');
    }
    if (footer) footer.textContent = `${latest.tag}${latest.date ? ` · ${latest.date}` : ''}`;
    if (note) {
      const notes = notesUrl(latest);
      note.innerHTML = `Showing <code>${latest.tag}</code>${latest.date ? ` (${latest.date})` : ''}. `
        + (notes ? `<a href="${notes}">Release notes</a> · ` : '')
        + `<a href="/release/">all releases</a>`;
    }
  }

  if (!list) return;
  const preferred = guessTarget();
  const ordered = [...targets()].sort(
    (a, b) => (b.id === preferred) - (a.id === preferred),
  );
  list.innerHTML = '';
  for (const t of ordered) {
    const li = document.createElement('li');
    if (t.id === preferred) li.className = 'preferred';
    const a = document.createElement('a');
    a.href = assetUrl(tag, t.asset);
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
  sums.innerHTML = `<a href="/release/${encodeURIComponent(tag)}/SHA256SUMS.txt">SHA256SUMS.txt</a>`;
  list.appendChild(sums);
}

/* ------------------------------------------------------------------ *
 * The interpreter
 * ------------------------------------------------------------------ */
const EXAMPLES = [
  { label: 'interpolation', code: 'local who = "world"\nprint($"Hello, {who}! pi is {math.pi::%.3f}")' },
  { label: 'switch', code: 'local code = 302\nswitch code do\ncase 200, 204 then print("ok")\ncase 301, 302 then print("redirected")\ndefault print("unexpected")\nend' },
  { label: 'safe navigation', code: 'local cfg = { server = {} }\nprint(cfg?.server?.port ?? 8080)\nprint(cfg?.missing?.deep?.value)' },
  { label: 'defer', code: 'do\n  defer print("cleaned up")\n  print("working")\nend' },
];

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
    theme: {
      background: '#12141a',
      foreground: '#d7dae0',
      cursor: '#7fd1b9',
      selectionBackground: '#2f3a44',
    },
  });
  const fit = new FitAddon();
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
          // feed() splits on newlines and submits each line, so a multi-line
          // example opens continuation prompts exactly as typing it would.
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

wireRelease();
wireRepl();
