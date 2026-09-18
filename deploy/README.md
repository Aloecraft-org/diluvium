# deploy/

One box, one directory. One thing it serves, one directory inside it. One
file you run.

    deploy/<node>/<service>/deploy.sh <verb>

| node | the box | what this repo ships to it |
|---|---|---|
| `cloud1` | the web box | `docs` — `diluvium.aloecraft.org/docs/` |

The shape is discofetch's `deploy/`, which took it from lk2's
`lk_deploy/`. This repo has one kit and needs no shared driver: that
driver stages a kit under `/opt` and runs an `install.sh` on the box, and
a static tree behind an nginx that is already running has neither —
the same reason `discofetch/deploy/cloud1/www/deploy.sh` stands alone.

## What is NOT here

**The landing page at `/`.** That is `site/`, built by the Aloecraft site
contract — `./site/build.sh`, offline, hermetic, idempotent, and it never
deploys — and shipped by `lk_web` in lk2, which clones this repo and runs
that script with no credentials and no network. Nothing in this directory
touches it. See `site/README.md`.

## Why the docs are not on that contract

Docusaurus is an `npm ci`. The contract forbids a network fetch at build
time, `test.yml`'s `site` job enforces it by installing nothing, and the
webpack build was removed from this repo for exactly that reason. So the
two builds are separate: `/` keeps the contract, `/docs/` is built and
shipped from here, and neither calls the other.

## The one thing lk2 has to say

`lk_web/deploy.py` rsyncs `diluvium-www` with `--delete` over
`/var/www/html/diluvium/`, and `lk_web/sites.py` derives the subtrees to
spare from the **other rows in `manifest/sites.json`** — a path is safe
because a sibling entry owns it, not because someone remembered an
`--exclude`. Until `manifest/sites.json` in lk2 carries a row at
`/docs/`, the next landing-page deploy removes everything
`cloud1/docs/deploy.sh` ships.

That row is the same device `diluvium-release` (`/release/`) and
`diluvium-drt-release` (`/drt/`) exist to provide, and their notes say so
in as many words. The row to add:

```json
{
    "name": "diluvium-docs",
    "vhost": "diluvium.aloecraft.org",
    "path": "/docs/",
    "serves": "server",
    "generator": "deploy/cloud1/docs/deploy.sh in Aloecraft-org/diluvium",
    "schedule": "none -- pushed from the laptop",
    "note": "Docusaurus, built from doc/ by that kit and rsynced to cloud1. serves:server because nothing stages it through lk_web: the build is an `npm ci`, which the site contract forbids, so it cannot ride diluvium-www's build. The row's job is the one /release/ and /drt/ have -- it is what keeps /docs/ off diluvium-www's --delete. If it is ever moved onto lk_web, it becomes serves:static with build.kind cmd and output docsite/build, and this note goes away."
}
```

With `serves: "server"`, `deploy.py` skips it (nothing to stage) and every
other sync at that vhost excludes it. That is the behaviour this needs.
