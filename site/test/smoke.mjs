// Smoke test: serve the built site, and drive the real REPL in a real
// browser.
//
//   cd site && npm test                      against a locally staged mirror
//   SITE=https://diluvium.aloecraft.org npm test    against the deployed site
//
// Needs `../site/build.sh` to have run (it serves _out/) and, for the local
// case, `npm run stage-mirror` to have copied the kernel into _out/release/.
// It asserts on behaviour that has actually broken: doubled output (the
// runtime's onOutput callback firing twice), continuation prompts not opening
// on unfinished input, an error killing the session instead of being caught
// -- and, since the page grew a second mirror, that both indexes are read.
// A screenshot is written to test/.out/site.png either way.

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';

const SITE_DIR = new URL('..', import.meta.url).pathname;
const OUT_DIR = join(SITE_DIR, '_out');
const REPORT = join(SITE_DIR, 'test', '.out');
const PORT = 8099;

const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.wasm': 'application/wasm',
  '.ico': 'image/x-icon', '.txt': 'text/plain', '.md': 'text/markdown',
};

// A static server that, like nginx, ignores the ?v= query render.py stamps
// onto every asset URL.
function serve() {
  const server = createServer(async (req, res) => {
    let p = normalize(decodeURIComponent(req.url.split('?')[0]));
    if (p.endsWith('/')) p += 'index.html';
    const file = join(OUT_DIR, p);
    if (!file.startsWith(OUT_DIR) || !existsSync(file)) {
      res.writeHead(404).end('not found');
      return;
    }
    res.writeHead(200, { 'content-type': TYPES[extname(file)] || 'application/octet-stream' });
    res.end(await readFile(file));
  });
  return new Promise((ok) => server.listen(PORT, '127.0.0.1', () => ok(server)));
}

const failures = [];
const check = (name, cond, detail = '') => {
  if (cond) console.log(`  ok   ${name}`);
  else { console.log(`  FAIL ${name}${detail ? ` — ${detail}` : ''}`); failures.push(name); }
};

const base = process.env.SITE || `http://127.0.0.1:${PORT}`;
const server = process.env.SITE ? null : await serve();
await mkdir(REPORT, { recursive: true });

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 1400 } });
const consoleErrors = [];
page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));

await page.goto(base, { waitUntil: 'domcontentloaded' });
await page.waitForFunction(
  () => /ready|unavailable/.test(document.getElementById('repl-status')?.textContent || ''),
  { timeout: 60000 },
);
// The mirror reads race the kernel load; give them a moment to land.
await page.waitForFunction(
  () => document.getElementById('footer-version')?.textContent.trim() !== '',
  { timeout: 15000 },
).catch(() => {});

const status = await page.textContent('#repl-status');
check('kernel loads', status.startsWith('ready'), `status was "${status}"`);
check('diluvium release index read', (await page.textContent('#version-badge')).trim() !== '');
check('diluvium downloads listed', (await page.$$('#downloads li')).length >= 6);
check('drt release index read', (await page.textContent('#drt-version-badge')).trim() !== '');
check('drt downloads listed', (await page.$$('#drt-downloads li')).length >= 3);
check('drt buildinfo read', /full/.test(await page.textContent('#drt-buildinfo')));
check('code samples highlighted', (await page.$$('pre code .token')).length > 0);

// The theme toggle: dark by default, light after a click, remembered.
check('dark by default', (await page.getAttribute('html', 'data-theme')) === 'dark');
await page.click('#theme');
check('toggle switches to light', (await page.getAttribute('html', 'data-theme')) === 'light');
check('choice is stored', (await page.evaluate(() => localStorage.getItem('aloecraft-theme'))) === 'light');
await page.click('#theme');

const type = async (s) => { await page.keyboard.type(s); await page.keyboard.press('Enter'); await page.waitForTimeout(400); };
const screen = () => page.evaluate(() => document.querySelector('.xterm-rows')?.innerText || '');

if (status.startsWith('ready')) {
  await page.click('#term');

  await type('print($"hi {1+1}")');
  const once = await screen();
  check('output is not doubled', (once.match(/hi 2/g) || []).length === 1,
        `saw ${(once.match(/hi 2/g) || []).length} copies`);

  await type('function sq(x)');
  check('unfinished input opens a continuation prompt', (await screen()).includes('>>'));
  await type('return x*x');
  await type('end');
  await type('print(sq(7), nil ?? "coalesced")');
  const after = await screen();
  check('multi-line definition runs', after.includes('49'));
  check('null coalescing works', after.includes('coalesced'));

  await type('error("boom")');
  await type('print("still alive")');
  const recovered = await screen();
  check('an error is caught, not fatal', recovered.includes('boom') && recovered.includes('still alive'));

  await page.click('#repl-reset');
  await page.waitForTimeout(300);
  check('restart works', (await screen()).includes('kernel restarted'));
}

check('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));

await page.screenshot({ path: join(REPORT, 'site.png'), fullPage: true });
await writeFile(join(REPORT, 'terminal.txt'), await screen());
await browser.close();
server?.close();

console.log(failures.length ? `\n${failures.length} failure(s)` : '\nall checks passed');
process.exit(failures.length ? 1 : 0);
