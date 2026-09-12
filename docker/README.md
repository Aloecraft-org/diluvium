# Docker images

Two images, both built from this repository's own source:

| image | Dockerfile | what it is |
|---|---|---|
| `diluvium` | `Dockerfile.cli` | the `dlua` interpreter/REPL and the bytecode compiler, as static musl binaries |
| `diluvium-demo` | `Dockerfile.browser` | the browser wasm build served as a static demo page |

Everything below works the same with `podman` in place of `docker` (the files
are named `Dockerfile.*` because that name is recognised by both;
`podman build -f docker/Dockerfile.cli .` is the equivalent).

## CLI

```sh
docker build -f docker/Dockerfile.cli -t diluvium .

docker run --rm -it diluvium                        # the REPL
docker run --rm -i  diluvium - < script.dl          # a script piped on stdin
docker run --rm -v "$PWD:/work" -w /work diluvium script.dl   # a file on disk
```

The image ships two binaries:

- `diluvium` — the interpreter and REPL (the entrypoint).
- `diluvium-compiler` — the bytecode compiler (`luac`).

```sh
docker run --rm -v "$PWD:/work" -w /work \
  --entrypoint diluvium-compiler diluvium -o out.luac script.dl
```

The version baked into the binary comes from `VERSION`, so it matches the
release the image was cut from:

```sh
docker run --rm diluvium -e 'print(_DILUVIUM.version)'
```

The recipe is `build_linux_static`'s (see the `Makefile`), run natively in the
build stage rather than through podman. The runtime is a bare `alpine` with a
non-root user; the binaries are statically linked, so they depend on nothing in
it.

## Browser demo

```sh
docker build -f docker/Dockerfile.browser -t diluvium-demo .
docker run --rm -p 8080:80 diluvium-demo            # open http://localhost:8080
```

The build stage runs `make build_browser` — the same recipe CI runs, the
distro's clang against a `wasm32-wasi` sysroot, no podman — and nginx serves the
result: `demo.html` as the index, next to `diluvium.js` (the loader) and
`diluvium_browser.wasm`. There is no server code; the page fetches the wasm and
runs everything in the browser. See `web/README.md` for what the module is.

The build stage is pinned to `$BUILDPLATFORM` because the wasm is `wasm32` and
identical on every host arch, so a multi-arch image compiles it once and varies
only the nginx runtime.

## Publishing

`.github/workflows/docker.yml` builds both images for `linux/amd64` and
`linux/arm64` and pushes them to GHCR:

- **`ghcr.io/aloecraft-org/diluvium`**
- **`ghcr.io/aloecraft-org/diluvium-demo`**

It follows `release.yml`'s idiom. Pushing a `v*` tag builds and publishes that
tag (and moves `latest`). A manual run (**Actions → Docker images → Run
workflow**) is a build-only rehearsal by default; set **publish = true** to
push. Pull requests that touch the build are validated without pushing. Auth is
the run's own `GITHUB_TOKEN` — no secret to configure.
