// Reading the release mirrors.
//
// Every Aloecraft release mirror lives under software.aloecraft.org/releases/,
// one directory per entry in lk2's manifest/mirrors.json, generated on the
// server by script/release_mirror.py from changelog.json in each repository's
// tree: releases.json is the index, latest/ a symlink to whichever tag the
// changelog marks, and every tag directory carries the release's own
// SHA256SUMS.txt and BUILDINFO.txt. The page reads the mirrors rather than the
// GitHub API so it costs no rate limit and keeps working when GitHub does not.
//
// Surface:
//   RELEASE_BASE, DRT_BASE, KERNEL_URL     where things are
//   TARGETS, DRT_TARGETS                   the downloadable binaries, per platform
//   guessTarget()                          which one to show first
//   loadIndex(base), loadBuildinfo(base)   the two files the page reads
//   assetUrl(base, tag, asset), notesUrl(base, rel)

const LOCALHOST = /^(localhost|127\.0\.0\.1|\[::1\])$/;

// The mirrors are cross-origin, and the /releases/ tree sends
// Access-Control-Allow-Origin: * so this page can read them. For
// development, a page served on localhost may be pointed at a copy of them
// -- the smoke test stages one same-origin and opens
//
//   http://127.0.0.1:8099/?release=/releases/diluvium&drt=/releases/diluvium-drt
//
// Honoured on localhost only: a link to the real site must not be able to
// point the kernel loader, or the download links, anywhere else.
function mirrorBase(path, param) {
  if (LOCALHOST.test(location.hostname)) {
    const v = new URLSearchParams(location.search).get(param);
    if (v) return v.replace(/\/+$/, '');
  }
  return path;
}

export const RELEASE_BASE = mirrorBase('https://software.aloecraft.org/releases/diluvium', 'release');
export const DRT_BASE = mirrorBase('https://software.aloecraft.org/releases/diluvium-drt', 'drt');

// It must be libdiluvium_wasi.wasm: diluvium_wasi.wasm is a command module
// whose _start runs the interpreter against stdin, and
// diluvium_compiler_wasi.wasm is luac. See repl/runtime.js.
export const KERNEL_URL = `${RELEASE_BASE}/latest/libdiluvium_wasi.wasm`;

// The downloadable binaries, per platform. Names come from each release
// workflow; the mirrors carry them verbatim. `id` is what guessTarget()
// answers with, so a platform DRT does not build for simply has no DRT row.
export const TARGETS = [
  { id: 'linux-x86_64',  label: 'Linux · x86-64',        asset: 'diluvium_linux_static_x86_64' },
  { id: 'linux-aarch64', label: 'Linux · ARM64',         asset: 'diluvium_linux_static_aarch64' },
  { id: 'linux-armv7l',  label: 'Linux · ARMv7',         asset: 'diluvium_linux_static_armv7l' },
  { id: 'darwin-arm64',  label: 'macOS · Apple silicon', asset: 'diluvium_darwin_arm64' },
  { id: 'darwin-x86_64', label: 'macOS · Intel',         asset: 'diluvium_darwin_x86_64' },
  { id: 'windows-x86_64', label: 'Windows · x86-64',     asset: 'diluvium_windows_x86_64.exe' },
];
export const DRT_TARGETS = [
  { id: 'linux-x86_64',  label: 'Linux · x86-64',        asset: 'drt_linux_static_x86_64' },
  { id: 'darwin-arm64',  label: 'macOS · Apple silicon', asset: 'drt_darwin_arm64' },
  { id: 'darwin-x86_64', label: 'macOS · Intel',         asset: 'drt_darwin_x86_64' },
];

// Best-effort, and only used to decide which download to show first. Every
// target stays listed whatever this guesses, and the install scripts do the
// real detection on the machine that will run the binary -- which is the only
// place it can be done correctly.
export function guessTarget() {
  const ua = navigator.userAgent;
  const platform = navigator.userAgentData?.platform || navigator.platform || '';
  const arm = /arm|aarch64/i.test(ua);

  if (/Win/i.test(platform)) return 'windows-x86_64';
  if (/Mac/i.test(platform) || /Mac/i.test(ua)) {
    // Safari reports Intel on Apple silicon and there is no reliable way
    // around it from script. Apple silicon is the likelier machine now, so
    // that is the guess; the Intel build is one click away in the list.
    return 'darwin-arm64';
  }
  if (/Linux|X11|Android/i.test(platform) || /Linux/i.test(ua)) {
    return arm ? 'linux-aarch64' : 'linux-x86_64';
  }
  return 'linux-x86_64';
}

export async function loadIndex(base) {
  const r = await fetch(`${base}/releases.json`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`${r.status} fetching ${base}/releases.json`);
  const doc = await r.json();
  const latest = doc.releases?.find((x) => x.tag === doc.latest) || doc.releases?.[0];
  return { doc, latest };
}

// BUILDINFO.txt is `key: value` lines, written by the release workflow and
// read back by `drt buildinfo`. The page shows the same facts, off the same
// artifact, so what it says a release carries is never inferred from a tag.
export async function loadBuildinfo(base, tag = 'latest') {
  const r = await fetch(`${base}/${encodeURIComponent(tag)}/BUILDINFO.txt`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`${r.status} fetching BUILDINFO.txt`);
  const info = {};
  for (const line of (await r.text()).split('\n')) {
    const i = line.indexOf(':');
    if (i > 0) info[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  }
  return info;
}

export function assetUrl(base, tag, asset) {
  return `${base}/${encodeURIComponent(tag)}/${asset}`;
}

export function notesUrl(base, rel) {
  return rel?.notes ? `${base}/${encodeURIComponent(rel.tag)}/${rel.notes}` : null;
}
