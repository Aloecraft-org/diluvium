BUILD_MNT:=-v $(CURDIR)/.data:/data
# Pinned by digest: an unpinned :latest tracks wasi-sdk's main branch and
# broke the 2026-08 builds when the sysroot moved from lib/wasm32-wasi to
# lib/wasm32-wasip1. The numbered tags (<= wasi-sdk-23) are too old for the
# -mllvm -wasm-use-legacy-eh flag below, so this pins the :latest that the
# 2026-08 builds compile cleanly against. Bump deliberately, not by surprise.
WASI_IMG:=ghcr.io/webassembly/wasi-sdk@sha256:46e14a8323321ca68b92ead633fc3fb004e5fa4205dd4b77b8aa1197bfe1f07b
WASI_CLANG:=cd /data && /opt/wasi-sdk/bin/clang -O3
WASM_LLVM_OPT:=-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
BUILD_WASM_OPT:=-lsetjmp -lwasi-emulated-signal -lwasi-emulated-process-clocks -Wl,--export-all, -Wl,--export=malloc -Wl,--export=free
PODMAN_RUN_WASM:=podman run --rm $(BUILD_MNT) $(WASI_IMG)
PODMAN_BUILD_WASM:=$(PODMAN_RUN_WASM) bash -c
PODMAN_RUN_ALPINE := podman run --rm $(BUILD_MNT) -w /data alpine:latest
WASI_AR:=/opt/wasi-sdk/bin/llvm-ar
WASI_SYSROOT:=/opt/wasi-sdk/share/wasi-sysroot
# wasi-sdk <=22 lays the sysroot out as wasm32-wasi, newer as wasm32-wasip1.
# Lib paths are resolved inside the container (see _wasi_static_lib); for
# includes, passing a -I that does not exist is harmless, so list both.
WASI_INCLUDES:=-I$(WASI_SYSROOT)/include -I$(WASI_SYSROOT)/include/wasm32-wasi -I$(WASI_SYSROOT)/include/wasm32-wasip1

UNAME_S := $(shell uname -s)
UNAME_Sl := $(shell uname -s | tr 'A-Z' 'a-z')
ARCHl := $(shell uname -m | tr 'A-Z' 'a-z')

PLAT_CFLAGS  := -std=c99 -DLUA_USE_LINUX
PLAT_LDFLAGS := -Wl,-E
PLAT_LIBS    := -ldl

ifeq ($(UNAME_S),Darwin)
    PLAT_CFLAGS  := -std=c99 -DLUA_USE_MACOSX
    PLAT_LDFLAGS := 
    PLAT_LIBS    :=
endif

ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLAT_CFLAGS  := -std=c99 -DLUA_USE_WINDOWS
    PLAT_LDFLAGS := 
    PLAT_LIBS    := 
    UNAME_Sl     := windows
endif

# TODO: Test `BUILD_WASM_OPT` without `-Wl,--export-all` to reduce binary size
# TODO: Add this later: -include script/platform.mk

# Special flags required by the test suite.
# Line editing is dline.c, which needs nothing but termios, so the test
# build and the release build now use the same editor and neither links a
# third-party library for it.
TEST_CFLAGS = -DLUA_USER_H='"ltests.h"' -O0 -g -DLUA_USE_LINUX -Wl,-E -ldl

ifeq ($(UNAME_S),Darwin)
    TEST_CFLAGS = -DLUA_USER_H='"ltests.h"' -O0 -g -DLUA_USE_POSIX
endif

TEST_BIN:=$(CURDIR)/dist/diluvium_debug
TEST_RUNNER:=$(CURDIR)/test/run_tests.sh

_build_step0:
	@echo '=== Step 0: Clean & Gather ==='
	rm -rf $(CURDIR)/.data/*
	mkdir -p $(CURDIR)/.data
	mkdir -p $(CURDIR)/dist
	cp -r $(CURDIR)/src/* $(CURDIR)/.data

_native_static_lib: _build_step0
	@echo '=== Building Native Static Archive ==='
	rm -f $(CURDIR)/dist/libdiluvium_$(UNAME_Sl)_$(ARCHl).a
	# We use -DLUA_LIB and -UMAKE_LUA to ensure the standalone 'main' is NOT compiled
	cd .data && gcc -O3 -c onelua.c -o onelua.o -fPIC $(PLAT_CFLAGS) -DDILUVIUM_AS_LIBRARY
	cd .data && gcc -O3 -c wasm_stubs.c -o wasm_stubs.o -fPIC
	# analyze.c / diluvium_api.c set their own _POSIX_C_SOURCE and need no
	# Lua platform config. Do NOT pass -DLUA_USE_LINUX here: on Windows,
	# 5.5's luaconf auto-defines LUA_USE_C89, and LUA_USE_LINUX would then
	# force LUA_USE_POSIX, tripping luaconf's "POSIX not compatible with C89".
	cd .data && gcc -O3 -c analyze.c -o analyze.o -fPIC -std=gnu99 -DDILUVIUM_AS_LIBRARY
	cd .data && gcc -O3 -c diluvium_api.c -o diluvium_api.o -fPIC -std=gnu99 -DDILUVIUM_AS_LIBRARY
	ar rcs dist/libdiluvium_$(UNAME_Sl)_$(ARCHl).a .data/onelua.o .data/wasm_stubs.o .data/diluvium_api.o .data/analyze.o
	@echo 'Native library built: dist/libdiluvium_$(UNAME_Sl)_$(ARCHl).a'

_portable_static_lib: _build_step0
	@echo '=== Building Portable (musl) Static Archive ==='
	$(PODMAN_RUN_ALPINE) sh -c "\
		apk add --no-cache gcc musl-dev && \
		gcc -O3 -c onelua.c -o onelua.o -fPIC -std=c99 -DLUA_USE_LINUX -DDILUVIUM_AS_LIBRARY && \
		gcc -O3 -c wasm_stubs.c -o wasm_stubs.o -fPIC && \
		gcc -O3 -c analyze.c -o analyze.o -fPIC -std=c99 -DLUA_USE_LINUX -DDILUVIUM_AS_LIBRARY  && \
		gcc -O3 -c diluvium_api.c -o diluvium_api.o -fPIC -std=c99 -DLUA_USE_LINUX -DDILUVIUM_AS_LIBRARY  && \
		ar rcs /data/libdiluvium_musl_$(ARCHl).a onelua.o wasm_stubs.o diluvium_api.o analyze.o"
	@cp .data/libdiluvium_musl_$(ARCHl).a dist/libdiluvium_musl_$(ARCHl).a

_wasm_build_step0: _build_step0

_wasm_build_step1:
	@echo '=== Step 1: Compile Lua (PIC for library use) ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c onelua.c -o onelua_wasi.o $(WASM_LLVM_OPT) -fPIC \
	-DL_tmpnam=32 \
	-D_WASI_EMULATED_SIGNAL \
	-D_WASI_EMULATED_PROCESS_CLOCKS \
	-Wno-deprecated-declarations"

_wasm_build_step2:
	@echo '=== Step 2: Compile WASM Stubs ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c wasm_stubs.c -o wasm_stubs_wasi.o $(WASM_LLVM_OPT)"
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c analyze.c -o analyze_wasi.o $(WASM_LLVM_OPT) \
	-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-deprecated-declarations"
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c diluvium_api.c -o diluvium_api_wasi.o $(WASM_LLVM_OPT) \
		-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Wno-deprecated-declarations"

_wasi_static_lib: _build_step0 _wasm_build_step1 _wasm_build_step2
	@echo '=== Creating Static Archive and Extracting WASI Libs ==='
	$(PODMAN_BUILD_WASM) "/opt/wasi-sdk/bin/llvm-ar rcs /data/libdiluvium_wasi.a /data/onelua_wasi.o /data/wasm_stubs_wasi.o /data/diluvium_api_wasi.o /data/analyze_wasi.o"
	@cp .data/libdiluvium_wasi.a dist/libdiluvium_wasi.a

	@echo '=== Pulling WASI/C libs from container ==='
	$(PODMAN_RUN_WASM) sh -c 'libs="$(WASI_SYSROOT)/lib/wasm32-wasip1"; \
		[ -d "$$libs" ] || libs="$(WASI_SYSROOT)/lib/wasm32-wasi"; \
		echo "using sysroot libs: $$libs"; \
		cp "$$libs/libwasi-emulated-signal.a" /data/ && \
		cp "$$libs/libwasi-emulated-process-clocks.a" /data/ && \
		cp "$$libs/libsetjmp.a" /data/ && \
		cp "$$libs/libc.a" /data/libwasic.a'
	@cp .data/libwasi-emulated-signal.a dist/
	@cp .data/libwasi-emulated-process-clocks.a dist/
	@cp .data/libsetjmp.a dist/
	@cp .data/libwasic.a dist/

_wasm_unknown_build: _build_step0
	@echo '=== Building for wasm32-unknown-unknown (browser, no WASI) ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) --target=wasm32-unknown-unknown \
		-c onelua.c -o onelua_wasm_unknown.o -O3 -fPIC \
		-I/data \
		-I/data/wasm-shim \
		$(WASI_INCLUDES) \
		-DDILUVIUM_AS_LIBRARY \
		-DLUA_USE_C89 \
		-DL_tmpnam=32 \
		-Dloadlib_c \
		-Dloslib_c \
		-Dliolib_c \
		-D__wasi__ \
		-D_WASI_EMULATED_SIGNAL \
		-D_WASI_EMULATED_PROCESS_CLOCKS \
		-Wno-deprecated-declarations"

	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) --target=wasm32-unknown-unknown \
		-c analyze.c -o analyze_wasm_unknown.o -O3 \
		$(WASI_INCLUDES) \
		-DDILUVIUM_AS_LIBRARY \
		-DLUA_USE_C89 \
		-DL_tmpnam=32 \
		-Dloadlib_c \
		-Dloslib_c \
		-Dliolib_c \
		-D__wasi__ \
		-D_WASI_EMULATED_SIGNAL \
		-D_WASI_EMULATED_PROCESS_CLOCKS \
		-Wno-deprecated-declarations"

	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) --target=wasm32-unknown-unknown \
		-c diluvium_api.c -o diluvium_api_wasm_unknown.o -O3 \
		$(WASI_INCLUDES) \
		-DDILUVIUM_AS_LIBRARY \
		-DLUA_USE_C89 \
		-DL_tmpnam=32 \
		-Dloadlib_c \
		-Dloslib_c \
		-Dliolib_c \
		-D__wasi__ \
		-D_WASI_EMULATED_SIGNAL \
		-D_WASI_EMULATED_PROCESS_CLOCKS \
		-Wno-deprecated-declarations"

	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) --target=wasm32-unknown-unknown \
		-c wasm_stubs_unknown.c -o wasm_stubs_wasm_unknown.o -O3 \
		$(WASI_INCLUDES) -D__wasi__"
	$(PODMAN_BUILD_WASM) "/opt/wasi-sdk/bin/llvm-ar rcs /data/libdiluvium_wasm_unknown.a \
		/data/onelua_wasm_unknown.o /data/analyze_wasm_unknown.o /data/wasm_stubs_wasm_unknown.o"
	@cp .data/libdiluvium_wasm_unknown.a dist/libdiluvium_wasm_unknown.a

_wasm_build_compiler_obj:
	@echo '=== Building Compiler Object (oneluac_wasi.o) ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c onelua.c -o oneluac_wasi.o $(WASM_LLVM_OPT) \
		-DMAKE_LUAC \
		-D_WASI_EMULATED_SIGNAL \
		-D_WASI_EMULATED_PROCESS_CLOCKS \
		-Wno-deprecated-declarations"

_wasm_build_step3: _wasm_build_compiler_obj
	@echo '=== Step 3: Link with C Driver ==='
	
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) onelua_wasi.o analyze_wasi.o diluvium_api_wasi.o wasm_stubs_wasi.o -o diluvium_wasi.wasm $(BUILD_WASM_OPT)"
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) onelua_wasi.o analyze_wasi.o diluvium_api_wasi.o wasm_stubs_wasi.o -o libdiluvium_wasi.wasm $(BUILD_WASM_OPT) -Wl,--no-entry -Wl,--allow-undefined"
	
	@echo '=== Building Compiler (luac.wasm) - No stubs needed ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) oneluac_wasi.o analyze_wasi.o -o luac_wasi.wasm -lsetjmp -lwasi-emulated-signal -lwasi-emulated-process-clocks -Wl,--export=malloc -Wl,--export=free"

	@cp .data/diluvium_wasi.wasm dist/diluvium_wasi.wasm
	@cp .data/libdiluvium_wasi.wasm dist/libdiluvium_wasi.wasm
	@cp .data/luac_wasi.wasm dist/diluvium_compiler_wasi.wasm

_wasm_verify_step1:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-objdump -d /data/onelua.o | grep -E "longjmp|setjmp" | head -n 5

_wasm_verify_step2:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/wasm_stubs.o | grep system

_wasm_verify_step3:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/diluvium.wasm | grep -E "luaL_newstate|lua_close" | head -n 5
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/libdiluvium.wasm | grep -E "luaL_newstate|lua_close" | head -n 5

build_wasm: _wasm_build_step0 _wasm_build_step1 _wasm_build_step2 _wasi_static_lib _wasm_build_step3

build_platform: _build_step0 _native_static_lib
	@echo "Building for $(UNAME_S)..."
	cd src && make clean && make all \
		MYCFLAGS='$(PLAT_CFLAGS)' \
		MYLDFLAGS='$(PLAT_LDFLAGS)' \
		MYLIBS='$(PLAT_LIBS)'

	@echo '=== Building Compiler (luac) ==='
	gcc -o .data/luac_$(UNAME_Sl)_$(ARCHl) .data/onelua.c .data/analyze.c .data/diluvium_api.c \
		-std=c99 -DMAKE_LUAC -lm
	
	cp src/lua dist/diluvium_$(UNAME_Sl)_$(ARCHl) 2>/dev/null || \
	cp src/lua.exe dist/diluvium_$(UNAME_Sl)_$(ARCHl).exe
	
	cp .data/luac_$(UNAME_Sl)_$(ARCHl) dist/diluvium_compiler_$(UNAME_Sl)_$(ARCHl) 2>/dev/null || \
	cp .data/luac_$(UNAME_Sl)_$(ARCHl).exe dist/diluvium_compiler_$(UNAME_Sl)_$(ARCHl).exe
	
	cd src && make clean

build_linux_static: _build_step0 _portable_static_lib
	@echo '=== Building Static Alpine Binary ==='
	$(PODMAN_RUN_ALPINE) sh -c "\
		apk add --no-cache gcc make musl-dev && \
		sed -i 's/-march=native//g' makefile && \
		make clean && \
		make all \
			CC=gcc \
			MYCFLAGS='-static -Os -std=c99 -DLUA_USE_LINUX -DMAKE_LUAC' \
			MYLDFLAGS='-static' \
			MYLIBS='' && \
		echo '--- Building Compiler (luac) ---' && \
		gcc -o /data/luac onelua.c analyze.c diluvium_api.c -static -Os -std=c99 -DMAKE_LUAC -lm"

	cp .data/luac dist/diluvium_compiler_linux_static_$(ARCHl)
	cp .data/lua dist/diluvium_linux_static_$(ARCHl)

build_static_libs: _wasi_static_lib _native_static_lib _portable_static_lib _wasm_unknown_build

# build_static_libs: _wasm_unknown_build _wasi_static_lib _native_static_lib _portable_static_lib

verify_wasm: _wasm_verify_step1 _wasm_verify_step2 _wasm_verify_step3

test_build: _build_step0
	gcc $(TEST_CFLAGS) -o $(TEST_BIN) $(CURDIR)/.data/onelua.c -lm

failing_test_cases:
	@echo 'Tests excluded from the default run (see test/run_tests.sh):'
	@$(TEST_RUNNER) --list-skipped

# Contract tests for the coroutine-hosted call driver. C rather than Lua
# because the driver has no guest binding by design -- its callers are the
# host ABI and, later, an opt-in task mode -- so there is nothing for the
# .lua suite to call yet. Built with the same debug flags as the suite, since
# 'api_check' is where a stack-arithmetic mistake surfaces as an abort
# instead of silent corruption.
# Ctrl-C must interrupt a runaway loop in both execution modes. Shell rather
# than Lua because it needs a subprocess and a signal, and nothing else in the
# suite presses Ctrl-C -- see the header of test/interrupt_check.sh for the
# hazard this exists to catch.
interrupt_check: test_build
	@$(CURDIR)/test/interrupt_check.sh --bin $(TEST_BIN)

# Contract tests for the instance ABI, written against dv.h alone -- which is
# also a check that the header is sufficient on its own for a host.
dv_check: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/dv_check \
	  $(CURDIR)/test/dv_check.c $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/dist/dv_check

dtask_check: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/dtask_check \
	  $(CURDIR)/test/dtask_check.c $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/dist/dtask_check

# No Lua at all: dhash.c is self-contained, and compiling it alone is part of
# what is being checked -- the compiler links it too and must not pull the
# runtime in.
dhash_check:
	@mkdir -p $(CURDIR)/dist
	gcc -Wall -Wextra -O2 -std=c99 -I$(CURDIR)/src \
	  -o $(CURDIR)/dist/dhash_check \
	  $(CURDIR)/test/dhash_check.c $(CURDIR)/src/dhash.c
	@$(CURDIR)/dist/dhash_check

dsnap_check: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/dsnap_check \
	  $(CURDIR)/test/dsnap_check.c $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/dist/dsnap_check

# The snapshot fuzzer's target: a snapshot in, a verdict out, as a subprocess --
# because the thing being checked is that a malformed snapshot does not crash,
# and a crash cannot be asserted from inside the process it happens in.
# The swarm layer (11.5) is a separate library, so it is compiled separately --
# which also enforces 4.1's boundary: dvs.c sees dv.h and dmsgpack.h and nothing
# else, and a stray include of lua.h would fail here rather than be absorbed by
# the amalgamation.
dvs_check: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/dvs_check \
	  $(CURDIR)/test/dvs_check.c $(CURDIR)/.data/dvs.c \
	  $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/dist/dvs_check

# Every contract test under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# These were outside the sanitizer sweep entirely, and that was a real hole rather
# than an oversight worth shrugging at: the ASan job builds 'onelua.c' and runs the
# *Lua* suite, so it covers the runtime a program reaches but not the C ABI a host
# reaches -- and dvs.c is not in the amalgamation at all, so the newest code, with
# the most raw malloc/free in the tree, had never met a sanitizer. Adding this found
# undefined behaviour in the SHA-256 update on its first run.
#
# Not the ltests.h build: that installs its own allocator and would fight ASan for
# the same job. TEST_CFLAGS is therefore not reused here.
SAN_CFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g
SAN_ENV = ASAN_OPTIONS=detect_leaks=1 \
	  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

sanitize_checks: _build_step0
	@set -e; \
	for t in dv_check dtask_check dshim_check dsnap_check; do \
	  echo "=== $$t (asan+ubsan)"; \
	  gcc $(SAN_CFLAGS) -DLUA_USE_LINUX -Wl,-E -ldl -DMAKE_LIB \
	    -I$(CURDIR)/.data -o $(CURDIR)/dist/$${t}_asan \
	    $(CURDIR)/test/$$t.c $(CURDIR)/.data/onelua.c -lm; \
	  $(SAN_ENV) $(CURDIR)/dist/$${t}_asan >/dev/null; \
	done; \
	echo "=== dvs_check (asan+ubsan)"; \
	gcc $(SAN_CFLAGS) -DLUA_USE_LINUX -Wl,-E -ldl -DMAKE_LIB \
	  -I$(CURDIR)/.data -o $(CURDIR)/dist/dvs_check_asan \
	  $(CURDIR)/test/dvs_check.c $(CURDIR)/.data/dvs.c \
	  $(CURDIR)/.data/onelua.c -lm; \
	$(SAN_ENV) $(CURDIR)/dist/dvs_check_asan >/dev/null; \
	echo "=== dhash_check (asan+ubsan)"; \
	gcc $(SAN_CFLAGS) -I$(CURDIR)/.data -o $(CURDIR)/dist/dhash_check_asan \
	  $(CURDIR)/test/dhash_check.c $(CURDIR)/.data/dhash.c; \
	$(SAN_ENV) $(CURDIR)/dist/dhash_check_asan >/dev/null; \
	echo "all contract tests clean under asan+ubsan"

snap_fuzz: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/snap_harness \
	  $(CURDIR)/test/snap_harness.c $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/script/fuzz_snapshot.py --bin $(CURDIR)/dist/snap_harness

dshim_check: _build_step0
	gcc $(TEST_CFLAGS) -DMAKE_LIB -I$(CURDIR)/.data \
	  -o $(CURDIR)/dist/dshim_check \
	  $(CURDIR)/test/dshim_check.c $(CURDIR)/.data/onelua.c -lm
	@$(CURDIR)/dist/dshim_check

# Run the suite. Keeps going after a failure and prints a summary, so one
# broken test does not mask the state of the rest. The list of tests and the
# skip reasons live in test/run_tests.sh -- add new tests there, not here.
test_cases: test_build
	@$(TEST_RUNNER) --bin $(TEST_BIN)

# Same suite, for CI. Separate target so the workflow has a stable entry point
# even if the local convenience target changes.
test_ci: test_build
	@$(TEST_RUNNER) --bin $(TEST_BIN) --timeout 300

# Run one or more named tests, e.g. `make test_one T="strings gc"`.
test_one: test_build
	@$(TEST_RUNNER) --bin $(TEST_BIN) $(T)

.PHONY: test_build test_cases test_ci test_one failing_test_cases \
        dv_check dtask_check dhash_check dsnap_check dshim_check dvs_check \
        snap_fuzz sanitize_checks

# wasmtime --wasm exceptions .data/lua.wasm
# wasmtime --wasm exceptions --dir=.::/workspace .data/lua.wasm /workspace/benchmark/benchmark.lua
# build_step4:
# 	@echo '=== Step 4: Optimize with wasm-opt ==='
# 	wasm-opt .data/lua.wasm -O4  --all-features  -o .data/lua-optimized.wasm
# web_start:
# 	cp .data/libdiluvium.wasm ./www/
# 	python3 -m http.server -d ./www/	