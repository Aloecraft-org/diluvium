// Copy just enough of the release mirrors into _out/releases/ that the smoke
// test can serve them same-origin and point the page at the copies, without
// the test itself depending on the network mid-run. The layout under
// _out/releases/ mirrors software.aloecraft.org/releases/.
//
//   node test/stage-mirror.mjs
//   MIRROR=file:///tmp/m/diluvium DRT_MIRROR=file:///tmp/m/diluvium-drt node test/stage-mirror.mjs
//
// Shells out to curl rather than using fetch: node's fetch ignores HTTP_PROXY,
// which matters on any machine behind one.

import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const BASE = process.env.MIRROR || 'https://software.aloecraft.org/releases/diluvium';
const DRT_BASE = process.env.DRT_MIRROR || 'https://software.aloecraft.org/releases/diluvium-drt';
const OUT = new URL('../_out/releases/', import.meta.url).pathname;

const grab = (base, path, dest) => {
  mkdirSync(join(dest, '..'), { recursive: true });
  execFileSync('curl', ['-fsSL', '--retry', '2', '-o', dest, `${base}/${path}`],
               { stdio: ['ignore', 'ignore', 'inherit'] });
};

// The Diluvium mirror: the index, the kernel and its sums, under the tag and
// under latest/ (a symlink on the server, a copy here).
const release = join(OUT, 'diluvium');
grab(BASE, 'releases.json', join(release, 'releases.json'));
const { latest } = JSON.parse(readFileSync(join(release, 'releases.json'), 'utf8'));
for (const f of ['libdiluvium_wasi.wasm', 'SHA256SUMS.txt']) {
  grab(BASE, `${latest}/${f}`, join(release, latest, f));
}
grab(BASE, `${latest}/libdiluvium_wasi.wasm`, join(release, 'latest', 'libdiluvium_wasi.wasm'));

// DRT's mirror: the index and BUILDINFO.txt, which is what the page reads.
const drt = join(OUT, 'diluvium-drt');
grab(DRT_BASE, 'releases.json', join(drt, 'releases.json'));
grab(DRT_BASE, 'latest/BUILDINFO.txt', join(drt, 'latest', 'BUILDINFO.txt'));

console.log(`staged diluvium ${latest} and DRT into ${OUT}`);
