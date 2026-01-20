BUILD_MNT:=-v $(CURDIR)/.data:/data
WASI_IMG:=ghcr.io/webassembly/wasi-sdk
WASI_CLANG:=cd /data && /opt/wasi-sdk/bin/clang -O3
WASM_LLVM_OPT:=-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
BUILD_WASM_OPT:=-lsetjmp -lwasi-emulated-signal -lwasi-emulated-process-clocks -Wl,--export-all, -Wl,--export=malloc -Wl,--export=free
PODMAN_RUN_WASM:=podman run --rm $(BUILD_MNT) $(WASI_IMG)
PODMAN_BUILD_WASM:=$(PODMAN_RUN_WASM) bash -c
PODMAN_RUN_ALPINE := podman run --rm $(BUILD_MNT) -w /data alpine:latest

UNAME_S := $(shell uname -s)
UNAME_Sl := $(shell uname -s | tr 'A-Z' 'a-z')
ARCHl := $(shell uname -m | tr 'A-Z' 'a-z')

# TODO: Test `BUILD_WASM_OPT` without `-Wl,--export-all` to reduce binary size
# TODO: Add this later: -include script/platform.mk

# Special flags required by the test suite
TEST_CFLAGS = -DLUA_USER_H='"ltests.h"' -O0 -g -DLUA_USE_LINUX -Wl,-E -ldl -lreadline
TEST_BIN:=$(CURDIR)/dist/diluvium_debug

_build_step0:
	@echo '=== Step 0: Clean & Gather ==='
	rm -rf $(CURDIR)/.data/*
	mkdir -p $(CURDIR)/.data
	mkdir -p $(CURDIR)/dist
	cp -r $(CURDIR)/src/* $(CURDIR)/.data

_wasm_build_step0: _build_step0

_wasm_build_step1:
	@echo '=== Step 1: Compile Lua ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c onelua.c -o onelua.o $(WASM_LLVM_OPT) \
	-DL_tmpnam=32 \
	-D_WASI_EMULATED_SIGNAL \
	-D_WASI_EMULATED_PROCESS_CLOCKS \
	-Wno-deprecated-declarations"

_wasm_build_step2:
	@echo '=== Step 2: Compile WASM Stubs ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) -c wasm_stubs.c -o wasm_stubs.o $(WASM_LLVM_OPT)"

_wasm_build_step3:
	@echo '=== Step 3: Link with C Driver ==='
	
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) onelua.o wasm_stubs.o -o diluvium.wasm $(BUILD_WASM_OPT)"
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) onelua.o wasm_stubs.o -o libdiluvium.wasm $(BUILD_WASM_OPT) \
		-Wl,--no-entry"
	@cp .data/diluvium.wasm dist/diluvium.wasm
	@cp .data/libdiluvium.wasm dist/libdiluvium.wasm
	
_wasm_verify_step1:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-objdump -d /data/onelua.o | grep -E "longjmp|setjmp" | head -n 5

_wasm_verify_step2:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/wasm_stubs.o | grep system

_wasm_verify_step3:
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/diluvium.wasm | grep -E "luaL_newstate|lua_close" | head -n 5
	$(PODMAN_RUN_WASM) /opt/wasi-sdk/bin/llvm-nm /data/libdiluvium.wasm | grep -E "luaL_newstate|lua_close" | head -n 5

build_wasm: _wasm_build_step0 _wasm_build_step1 _wasm_build_step2 _wasm_build_step3

build_platform:
	mkdir -p dist
	(cd src && make all && \
     cp lua ../dist/diluvium_$(UNAME_Sl)_$(ARCHl) 2>/dev/null || \
     cp lua.exe ../dist/diluvium_$(UNAME_Sl)_$(ARCHl).exe && make clean)

build_linux_static: _build_step0
	@echo '=== Building Static Alpine Binary ==='
	$(PODMAN_RUN_ALPINE) sh -c "\
		apk add --no-cache gcc make musl-dev ncurses-static readline-static readline-dev && \
		sed -i 's/-march=native//g' makefile && \
		make clean && \
		make all \
			CC=gcc \
			MYCFLAGS='-static -Os -std=c99 -DLUA_USE_LINUX -DLUA_USE_READLINE' \
			MYLDFLAGS='-static' \
			MYLIBS='-lreadline -lncurses'"
	cp .data/lua dist/diluvium_linux_static_$(ARCHl)

verify_wasm: _wasm_verify_step1 _wasm_verify_step2 _wasm_verify_step3

test_build: _build_step0
	gcc $(TEST_CFLAGS) -o $(TEST_BIN) $(CURDIR)/.data/onelua.c -lm

failing_test_cases:
	echo "skipping: main.lua (fails on static binary)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) main.lua         )
	echo "skipping: literals.lua (musl vs glibc conversion)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) literals.lua     )
	echo "skipping: heavy.lua (designed to consume 2GB–16GB of RAM)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) heavy.lua        )
	echo "skipping: big.lua (designed to consume 2GB–16GB of RAM)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) big.lua          )
	echo "skipping: attrib.lua (c-api/ltests.c missing)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) attrib.lua       )
	echo "skipping: api.lua (c-api/ltests.c missing)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) api.lua          )

test_cases: test_build
	@echo "Running Test: api.lua"
	@echo "============================================="
# 	(cd $(CURDIR)/test && $(TEST_BIN) api.lua          )
	@echo "(skipping)"
	@echo "Running Test: attrib.lua"
	@echo "============================================="
# 	(cd $(CURDIR)/test && $(TEST_BIN) attrib.lua       )
	@echo "(skipping)"
	@echo "Running Test: benchmark.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) benchmark.lua    )
	@echo "Running Test: big.lua"
	@echo "============================================="
# 	(cd $(CURDIR)/test && $(TEST_BIN) big.lua          )
	@echo "(skipping)"
	@echo "Running Test: bitwise.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) bitwise.lua      )
	@echo "Running Test: bwcoercion.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) bwcoercion.lua   )
	@echo "Running Test: calls.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) calls.lua        )
	@echo "Running Test: closure.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) closure.lua      )
	@echo "Running Test: code.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) code.lua         )
	@echo "Running Test: constructs.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) constructs.lua   )
	@echo "Running Test: coroutine.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) coroutine.lua    )
	@echo "Running Test: cstack.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) cstack.lua       )
	@echo "Running Test: db.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) db.lua           )
	@echo "Running Test: errors.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) errors.lua       )
	@echo "Running Test: events.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) events.lua       )
	@echo "Running Test: files.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) files.lua        )
	@echo "Running Test: gc.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) gc.lua           )
	@echo "Running Test: gengc.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) gengc.lua        )
	@echo "Running Test: goto.lua"
	@echo "============================================="
	@echo "(skipping)"
	(cd $(CURDIR)/test && $(TEST_BIN) goto.lua         )
	@echo "Running Test: heavy.lua"
	@echo "============================================="
	@echo "(skipping)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) heavy.lua        )
	@echo "Running Test: literals.lua"
	@echo "============================================="
	@echo "(skipping)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) literals.lua     )
	@echo "Running Test: locals.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) locals.lua       )
	@echo "Running Test: main.lua"
	@echo "============================================="
	@echo "(skipping)"
# 	(cd $(CURDIR)/test && $(TEST_BIN) main.lua         )
	@echo "Running Test: math.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) math.lua         )
	@echo "Running Test: nextvar.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) nextvar.lua      )
	@echo "Running Test: pm.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) pm.lua           )
	@echo "Running Test: sort.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) sort.lua         )
	@echo "Running Test: strings.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) strings.lua      )
	@echo "Running Test: test_fstrings.lu"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) test_fstrings.lua)
	@echo "Running Test: tpack.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) tpack.lua        )
	@echo "Running Test: tracegc.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) tracegc.lua      )
	@echo "Running Test: utf8.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) utf8.lua         )
	@echo "Running Test: vararg.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) vararg.lua       )
	@echo "Running Test: verybig.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) verybig.lua      )

# wasmtime --wasm exceptions .data/lua.wasm
# wasmtime --wasm exceptions --dir=.::/workspace .data/lua.wasm /workspace/benchmark/benchmark.lua
# build_step4:
# 	@echo '=== Step 4: Optimize with wasm-opt ==='
# 	wasm-opt .data/lua.wasm -O4  --all-features  -o .data/lua-optimized.wasm
# web_start:
# 	cp .data/libdiluvium.wasm ./www/
# 	python3 -m http.server -d ./www/	