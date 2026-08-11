#!/bin/sh
# Build the image and run the toy swarm. Works from anywhere in the checkout.
#
#   examples/discofetch/run.sh            # 40 steps
#   examples/discofetch/run.sh 80         # longer
#
# The build context has to be the repository root, because the image builds the
# runtime from source -- that is the only thing about the invocation that is not
# obvious, and the reason this script exists.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
steps=${1:-40}

docker build -f "$here/Dockerfile" -t discofetch-toy "$root"
exec docker run --rm discofetch-toy "$steps"
