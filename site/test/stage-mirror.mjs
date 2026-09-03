// Copy just enough of the deployed release mirrors into _out/ that the smoke
// test exercises the same-origin path production actually uses, without the
// test itself depending on the network mid-run.
//
//   node test/stage-mirror.mjs
//   MIRROR=http://localhost:9000/release DRT_MIRROR=http://localhost:9000/drt node test/stage-mirror.mjs
//
// Shells out to curl rather than using fetch: node's fetch ignores HTTP_PROXY,
// which matters on any machine behind one.

import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const BASE = process.env.MIRROR || 'https://diluvium.aloecraft.org/release';
const DRT_BASE = process.env.DRT_MIRROR || 'https://diluvium.aloecraft.org/drt';
const OUT = new URL('../_out/', import.meta.url).pathname;

const grab = (base, path, dest) => {
  mkdirSync(join(dest, '..'), { recursive: true });
  execFileSync('curl', ['-fsSL', '--retry', '2', '-o', dest, `${base}/${path}`],
               { stdio: ['ignore', 'ignore', 'inherit'] });
};

// The Diluvium mirror: the index, the kernel and its sums, under the tag and
// under latest/ (a symlink on the server, a copy here).
const release = join(OUT, 'release');
grab(BASE, 'releases.json', join(release, 'releases.json'));
const { latest } = JSON.parse(readFileSync(join(release, 'releases.json'), 'utf8'));
for (const f of ['libdiluvium_wasi.wasm', 'SHA256SUMS.txt']) {
  grab(BASE, `${latest}/${f}`, join(release, latest, f));
}
grab(BASE, `${latest}/libdiluvium_wasi.wasm`, join(release, 'latest', 'libdiluvium_wasi.wasm'));

// DRT's mirror: the index and BUILDINFO.txt, which is what the page reads.
const drt = join(OUT, 'drt');
grab(DRT_BASE, 'releases.json', join(drt, 'releases.json'));
grab(DRT_BASE, 'latest/BUILDINFO.txt', join(drt, 'latest', 'BUILDINFO.txt'));

console.log(`staged diluvium ${latest} and DRT into ${OUT}`);
