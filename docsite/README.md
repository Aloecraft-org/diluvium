# docsite/

`diluvium.aloecraft.org/docs/`: the reference, built with
[Docusaurus](https://docusaurus.io/) from the markdown in `doc/`.

```sh
cd docsite
npm ci
npm start                # http://localhost:3000/docs/ with live reload
npm run build            # -> docsite/build
npm run serve            # serve that build
```

Deploying is a verb on the kit, not something this directory does:

```sh
./deploy/cloud1/docs/deploy.sh build     # npm ci && docusaurus build
./deploy/cloud1/docs/deploy.sh diff      # what sync would change on cloud1
./deploy/cloud1/docs/deploy.sh sync      # ship it
```

```
docusaurus.config.js   what this site is, and PUBLISHED -- the allowlist
sidebars.js            the order it is read in
src/css/custom.css     the landing page's palette, as Infima tokens
static/                favicon and logo, copied from site/ and doc/
```

## The content is `doc/`, read in place

`docs.path` is `../doc`. Nothing is copied here, so an edit to
`doc/Guide.md` *is* the docs site changing, and a review sees one diff
rather than a file and its copy. The cost is that the two directories are
one letter apart; `doc/` is the markdown and `docsite/` is the machinery
that publishes it.

**`PUBLISHED` in `docusaurus.config.js` is an allowlist, not a deny
list.** `doc/` also carries the roadmap, `Plan-2026-09.md`, the alignment
audit, the `BUILD*.md` notes and `audit/` — working documents that are
fine in the repo and are not a public reference. A deny list publishes
the next one somebody adds. `doc/README.md` is both the index of that
directory on GitHub and the front page of this site, which is why
Docusaurus needs no landing page of its own.

`sidebars.js` decides the order and `PUBLISHED` decides what exists. An id
in the sidebar that is not published fails the build — the two disagreeing
is the bug, not something to find as a missing entry on the live site.

## `format: 'detect'`

`doc/` is CommonMark written long before this site existed. Docusaurus 3
reads `.md` as MDX by default, where a `{name}` in prose is a JSX
expression and a bare `<` is a tag. `markdown.format: 'detect'` reads
`.md` as CommonMark and only `.mdx` as MDX, so publishing the existing
documentation did not mean editing it for the renderer —
`$"hello {name}"` in the Guide renders as written.

## Why this is not on the site contract

`site/` is built by `./site/build.sh`: offline, hermetic, idempotent, no
npm. `lk_web` in lk2 runs it with no credentials and no network, and
`test.yml`'s `site` job fails a build that grew a fetch — which is why
the webpack build was removed from this repo. Docusaurus is an `npm ci`,
so it cannot go there. `/` keeps the contract; `/docs/` is built and
shipped by `deploy/cloud1/docs/deploy.sh`. One vhost, two builds, and
neither calls the other.

`deploy/README.md` carries the rest, including the row lk2's
`manifest/sites.json` needs before the first sync — without it,
`lk_web`'s next landing-page deploy deletes `/docs/`.
