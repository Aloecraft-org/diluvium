#!/bin/sh
#
# Build the generic host as a fully static musl binary in a local Alpine
# container, and drop it at dist/diluvium-host-musl. Works with docker or
# podman. Run from anywhere; the repo root is found relative to this script.
#
#   host/build-musl.sh
#
# The result is one self-contained binary: scp it to the Alpine box and run
# it. It links no shared library and no libc, so it does not care that the
# target Alpine is older than the builder.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

if command -v docker >/dev/null 2>&1; then
  ENGINE=docker
elif command -v podman >/dev/null 2>&1; then
  ENGINE=podman
else
  echo "build-musl: need docker or podman on the build machine" >&2
  exit 1
fi

TAG=diluvium-host-musl-build
mkdir -p dist

echo "=== building the static musl host with $ENGINE (Alpine container) ==="
$ENGINE build -f host/Dockerfile.musl --target build -t "$TAG" .

# Copy the artifact out of a throwaway container.
CID=$($ENGINE create "$TAG")
trap '$ENGINE rm "$CID" >/dev/null 2>&1 || true' EXIT
$ENGINE cp "$CID:/src/dist/diluvium-host-musl" dist/diluvium-host-musl
# The rest plugin rides along: it is built in the same image, by the same
# musl toolchain, and a host without a plugin to run through its plugin
# channel is half of what build 8 shipped.
$ENGINE cp "$CID:/src/dist/diluvium-rest-plugin-musl" dist/diluvium-rest-plugin-musl || true

echo
echo "=== dist/diluvium-host-musl ==="
ls -la dist/diluvium-host-musl
[ -f dist/diluvium-rest-plugin-musl ] && ls -la dist/diluvium-rest-plugin-musl
# Prove it is static and identify the arch, without needing 'file'. On Alpine
# 'ldd' is ld-musl, which answers "Not a valid dynamic program" for a static
# binary -- that is the success signal, not an error, so interpret it rather
# than print it raw.
if command -v "$ENGINE" >/dev/null 2>&1; then
  $ENGINE run --rm -v "$REPO_ROOT/dist:/d" "$TAG" sh -c '
    out=$(ldd /d/diluvium-host-musl 2>&1 || true)
    case "$out" in
      *"Not a valid dynamic program"*|*"not a dynamic executable"*)
        echo "static: confirmed -- no shared-library dependencies" ;;
      *) echo "dynamic dependencies (NOT fully static): $out" ;;
    esac
    echo "arch: $(uname -m)  (must match the target box)"' || true
fi
echo
echo "scp dist/diluvium-host-musl to the Alpine box, alongside a *.host.lua"
echo "config and its supervisor program, and run: ./diluvium-host deploy.host.lua"
