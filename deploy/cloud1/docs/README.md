## Deploy

    ./deploy.sh build     npm ci && docusaurus build -> docsite/build
    ./deploy.sh diff      what sync would change on cloud1
    ./deploy.sh sync      rsync that build to cloud1, under /docs/

Runs on the laptop. `build` is the only verb that needs the network, and
`diff` and `sync` both refuse a build that is missing or older than
`doc/` — syncing the build from before the edit is the failure this
guards, and it is the one `deploy/fetch1/api` in discofetch hit for real.

The source is `docsite/` at the repo root; the content is `doc/`, read in
place, with `docsite/docusaurus.config.js` carrying the allowlist of what
is public. `../../README.md` is why this is not `site/build.sh`, and what
lk2 still has to declare before the first sync.
