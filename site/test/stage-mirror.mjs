// Copy just enough of the deployed release mirror into dist/release/ that the
// smoke test exercises the same-origin path production actually uses, without
// the test itself depending on the network mid-run.
//
//   node test/stage-mirror.mjs
//   MIRROR=http://localhost:9000/release node test/stage-mirror.mjs
//
// Shells out to curl rather than using fetch: node's fetch ignores HTTP_PROXY,
// which matters on any machine behind one.

import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const BASE = process.env.MIRROR || 'https://diluvium.aloecraft.org/release';
const DIST = new URL('../dist/release/', import.meta.url).pathname;

const grab = (path, dest) => {
  mkdirSync(join(dest, '..'), { recursive: true });
  execFileSync('curl', ['-fsSL', '--retry', '2', '-o', dest, `${BASE}/${path}`],
               { stdio: ['ignore', 'ignore', 'inherit'] });
};

mkdirSync(join(DIST, 'latest'), { recursive: true });
grab('releases.json', join(DIST, 'releases.json'));

const { latest } = JSON.parse(readFileSync(join(DIST, 'releases.json'), 'utf8'));
mkdirSync(join(DIST, latest), { recursive: true });
for (const f of ['libdiluvium_wasi.wasm', 'SHA256SUMS.txt']) {
  grab(`${latest}/${f}`, join(DIST, latest, f));
}
grab(`${latest}/libdiluvium_wasi.wasm`, join(DIST, 'latest', 'libdiluvium_wasi.wasm'));

console.log(`staged ${latest} into dist/release/`);
