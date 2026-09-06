#!/bin/sh
# Diluvium installer.
#
#   curl -fsSL https://diluvium.aloecraft.org/start | sh
#   curl -fsSL https://diluvium.aloecraft.org/start | sh -s -- --prefix ~/.local
#
# Downloads the interpreter for this machine from the release mirror,
# verifies it against the release's published SHA256SUMS.txt, and installs it
# as `diluvium`.
#
# POSIX sh on purpose: this runs on whatever /bin/sh the machine has, which on
# Alpine is busybox ash and on macOS is an old bash pretending. No arrays, no
# `local` outside functions, no bashisms.
#
# Env:
#   DILUVIUM_BASE     mirror root      (default https://software.aloecraft.org/releases/diluvium)
#   DILUVIUM_VERSION  tag to install   (default the mirror's `latest`)
#   DILUVIUM_PREFIX   install prefix   (default /usr/local, or ~/.local unwritable)
#   DILUVIUM_COMPILER set to 1 to also install the bytecode compiler (luac)

set -eu

BASE="${DILUVIUM_BASE:-https://software.aloecraft.org/releases/diluvium}"
VERSION="${DILUVIUM_VERSION:-}"
PREFIX="${DILUVIUM_PREFIX:-}"
WANT_COMPILER="${DILUVIUM_COMPILER:-0}"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)   PREFIX="${2:?--prefix needs a path}"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --version)  VERSION="${2:?--version needs a tag}"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --compiler) WANT_COMPILER=1; shift ;;
        -h|--help)
            sed -n '2,20p' "$0" 2>/dev/null || echo "see https://diluvium.aloecraft.org"
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------- fetch
# curl on most machines, wget on the minimal ones (a Debian container often
# has neither, which is worth saying plainly rather than failing obscurely).
if have curl; then
    fetch()   { curl -fsSL --retry 3 -o "$2" "$1"; }
    fetchout(){ curl -fsSL --retry 3 "$1"; }
elif have wget; then
    fetch()   { wget -q -O "$2" "$1"; }
    fetchout(){ wget -q -O - "$1"; }
else
    die "neither curl nor wget is available"
fi

# ---------------------------------------------------------------- target
os=$(uname -s 2>/dev/null || echo unknown)
arch=$(uname -m 2>/dev/null || echo unknown)

case "$os" in
    Linux)  os_part=linux_static ;;
    Darwin) os_part=darwin ;;
    MINGW*|MSYS*|CYGWIN*)
        die "Windows: download diluvium_windows_x86_64.exe from $BASE/latest/" ;;
    *) die "unsupported operating system: $os" ;;
esac

case "$os:$arch" in
    Linux:x86_64|Linux:amd64)    asset=diluvium_linux_static_x86_64 ;;
    Linux:aarch64|Linux:arm64)   asset=diluvium_linux_static_aarch64 ;;
    Linux:armv7l|Linux:armv6l)   asset=diluvium_linux_static_armv7l ;;
    Darwin:arm64)                asset=diluvium_darwin_arm64 ;;
    Darwin:x86_64)               asset=diluvium_darwin_x86_64 ;;
    *) die "no build for $os $arch — see $BASE/ for what exists" ;;
esac
compiler_asset=$(echo "$asset" | sed 's/^diluvium_/diluvium_compiler_/')

# ---------------------------------------------------------------- version
# `latest` is a symlink the mirror maintains from the changelog, so the
# default path needs no lookup at all. An explicit --version addresses the
# tag directory directly.
if [ -n "$VERSION" ]; then
    dir="$BASE/$VERSION"
    label="$VERSION"
else
    dir="$BASE/latest"
    label=$(fetchout "$BASE/releases.json" 2>/dev/null \
        | sed -n 's/.*"latest"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -1)
    [ -n "$label" ] || label="latest"
fi

# ---------------------------------------------------------------- prefix
if [ -z "$PREFIX" ]; then
    if [ -w /usr/local/bin ] 2>/dev/null; then
        PREFIX=/usr/local
    elif [ "$(id -u)" = 0 ]; then
        PREFIX=/usr/local
    else
        PREFIX="$HOME/.local"
    fi
fi
bindir="$PREFIX/bin"

say "Diluvium $label"
say "  target  $os $arch  ($asset)"
say "  install $bindir/diluvium"
say ""

mkdir -p "$bindir" || die "cannot create $bindir"
[ -w "$bindir" ] || die "$bindir is not writable — re-run with --prefix \$HOME/.local, or as root"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/diluvium.XXXXXX") || die "cannot create a temp directory"
trap 'rm -rf "$tmp"' EXIT INT TERM

say "downloading $asset ..."
fetch "$dir/$asset" "$tmp/$asset" || die "download failed: $dir/$asset"

# ---------------------------------------------------------------- verify
# The mirror carries the SHA256SUMS.txt produced by the release workflow, so
# this checks the binary against what CI published rather than against
# anything the mirror computed for itself.
if fetch "$dir/SHA256SUMS.txt" "$tmp/SHA256SUMS.txt" 2>/dev/null; then
    if have sha256sum;   then actual=$(sha256sum "$tmp/$asset" | cut -d' ' -f1)
    elif have shasum;    then actual=$(shasum -a 256 "$tmp/$asset" | cut -d' ' -f1)
    elif have openssl;   then actual=$(openssl dgst -sha256 "$tmp/$asset" | sed 's/.*= *//')
    else actual=""; say "warning: no sha256 tool found; skipping verification"
    fi
    if [ -n "$actual" ]; then
        expected=$(grep " $asset\$" "$tmp/SHA256SUMS.txt" | cut -d' ' -f1)
        [ -n "$expected" ] || die "$asset is not listed in SHA256SUMS.txt"
        [ "$expected" = "$actual" ] || die "checksum mismatch for $asset
  expected $expected
  actual   $actual"
        say "checksum ok"
    fi
else
    say "warning: no SHA256SUMS.txt at $dir — installing unverified"
fi

# ---------------------------------------------------------------- install
chmod +x "$tmp/$asset"
# Move into place via a temp name in the destination directory: replacing a
# running binary in place fails with ETXTBSY on Linux, and rename does not.
mv -f "$tmp/$asset" "$bindir/.diluvium.new"
mv -f "$bindir/.diluvium.new" "$bindir/diluvium"
say "installed $bindir/diluvium"

if [ "$WANT_COMPILER" = 1 ]; then
    say "downloading $compiler_asset ..."
    if fetch "$dir/$compiler_asset" "$tmp/$compiler_asset" 2>/dev/null; then
        chmod +x "$tmp/$compiler_asset"
        mv -f "$tmp/$compiler_asset" "$bindir/.diluvium-compiler.new"
        mv -f "$bindir/.diluvium-compiler.new" "$bindir/diluvium-compiler"
        say "installed $bindir/diluvium-compiler"
    else
        say "warning: $compiler_asset not available for this release"
    fi
fi

# ---------------------------------------------------------------- report
case ":$PATH:" in
    *":$bindir:"*) ;;
    *) say ""
       say "$bindir is not on your PATH. Add it:"
       say "  export PATH=\"$bindir:\$PATH\"" ;;
esac

say ""
if "$bindir/diluvium" -v 2>/dev/null; then
    :
else
    say "installed, but '$bindir/diluvium -v' did not run — check the architecture"
fi
say ""
say "Run 'diluvium' for the interactive interpreter, or 'help()' once inside."
