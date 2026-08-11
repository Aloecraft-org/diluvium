# Build and run the toy without Docker, straight out of the checkout.
#
#   make -C examples/discofetch run
#
# The two libraries come from the repository's own targets rather than being
# rebuilt here, so this example cannot drift into compiling the runtime a second
# way. 'build_swarm_lib' also checks the layer boundary at the linker.

ROOT := $(CURDIR)/../..

# The native archive is named for the platform, exactly as the root Makefile names
# it. Derived rather than wildcarded: after a cross build dist/ holds several, and
# picking one by glob would link the wrong architecture with a confusing error.
UNAME_Sl := $(shell uname -s | tr 'A-Z' 'a-z')
ARCHl    := $(shell uname -m | tr 'A-Z' 'a-z')
LIBS     := $(ROOT)/dist/libdiluvium-swarm.a \
            $(ROOT)/dist/libdiluvium_$(UNAME_Sl)_$(ARCHl).a

.PHONY: run build clean

build:
	$(MAKE) -C $(ROOT) build_swarm_lib
	$(MAKE) -C $(ROOT) _native_static_lib
	gcc -Wall -Wextra -O2 -std=c99 -I$(ROOT)/.data \
	  -o $(CURDIR)/swarmd $(CURDIR)/swarmd.c \
	  $(LIBS) -lm

run: build
	@cd $(CURDIR) && ./swarmd $(STEPS)

clean:
	rm -f $(CURDIR)/swarmd
