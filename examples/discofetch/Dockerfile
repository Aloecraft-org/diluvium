# A toy Discofetch swarm, built from this checkout.
#
# Build context is the repository root, not this directory:
#
#   docker build -f examples/discofetch/Dockerfile -t discofetch-toy .
#   docker run --rm discofetch-toy
#
# Or use examples/discofetch/run.sh, which does both from anywhere.
#
# Two stages, so the image that ships is a static binary and three Lua files with
# no compiler and no source in it. The result is a few megabytes and needs nothing
# at runtime -- which is the packaging story Diluvium is built for, and the reason
# this example is worth having as a container rather than a shell script.

FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# 'build_swarm_lib' is the swarm layer compiled on its own, which is also where
# 4.1's layer boundary is enforced: the target fails if dvs.o references any core
# Lua symbol. Building it here rather than compiling dvs.c into the toy keeps that
# check on the path.
#
# '_build_step0' gathers src/ into .data/ and produces the amalgamation the runtime
# is built from; the two static libraries come from _native_static_lib.
RUN make build_swarm_lib && make _native_static_lib

# The host program. It sees dv.h, dvs.h and the msgpack cursor -- no Lua header,
# which is the claim dv.h makes about itself and worth exercising in an example.
RUN gcc -Wall -Wextra -O2 -std=c99 -I/src/.data \
      -o /src/dist/swarmd examples/discofetch/swarmd.c \
      /src/dist/libdiluvium-swarm.a \
      /src/dist/libdiluvium_$(uname -s | tr 'A-Z' 'a-z')_$(uname -m).a -lm


FROM debian:bookworm-slim

# A swarm of interpreters should not run as root.
RUN useradd --create-home --shell /usr/sbin/nologin swarm
WORKDIR /app

COPY --from=build /src/dist/swarmd /usr/local/bin/swarmd
COPY examples/discofetch/supervisor.lua  /app/
COPY examples/discofetch/coordinator.lua /app/
COPY examples/discofetch/handler.lua     /app/

USER swarm

# The argument is a step budget. The host has no clock of its own -- 8.3 gives the
# clock to the host, and this host's clock is a for-loop, which makes a run
# reproducible.
ENTRYPOINT ["swarmd"]
CMD ["40"]
