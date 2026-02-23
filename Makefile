BUILD_MNT:=-v $(CURDIR)/.data:/data
WASI_IMG:=ghcr.io/webassembly/wasi-sdk
WASI_CLANG:=cd /data && /opt/wasi-sdk/bin/clang -O3
WASM_LLVM_OPT:=-mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false
BUILD_WASM_OPT:=-lsetjmp -lwasi-emulated-signal -lwasi-emulated-process-clocks -Wl,--export-all, -Wl,--export=malloc -Wl,--export=free
PODMAN_RUN_WASM:=podman run --rm $(BUILD_MNT) $(WASI_IMG)
PODMAN_BUILD_WASM:=$(PODMAN_RUN_WASM) bash -c
PODMAN_RUN_ALPINE := podman run --rm $(BUILD_MNT) -w /data alpine:latest
WASI_AR:=/opt/wasi-sdk/bin/llvm-ar
WASI_SYSROOT:=/opt/wasi-sdk/share/wasi-sysroot
WASI_SYSROOT_LIBS:=/opt/wasi-sdk/share/wasi-sysroot/lib/wasm32-wasi
WASI_INCLUDES:=-I$(WASI_SYSROOT)/include -I$(WASI_SYSROOT)/include/wasm32-wasi

UNAME_S := $(shell uname -s)
UNAME_Sl := $(shell uname -s | tr 'A-Z' 'a-z')
ARCHl := $(shell uname -m | tr 'A-Z' 'a-z')

PLAT_CFLAGS  := -std=c99 -DLUA_USE_LINUX -DLUA_USE_READLINE
PLAT_LDFLAGS := -Wl,-E
PLAT_LIBS    := -ldl -lreadline

ifeq ($(UNAME_S),Darwin)
    PLAT_CFLAGS  := -std=c99 -DLUA_USE_MACOSX -DLUA_USE_READLINE
    PLAT_LDFLAGS := 
    PLAT_LIBS    := -lreadline
endif

ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLAT_CFLAGS  := -std=c99 -DLUA_USE_WINDOWS
    PLAT_LDFLAGS := 
    PLAT_LIBS    := 
    UNAME_Sl     := windows
endif

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

_native_static_lib: _build_step0
	@echo '=== Building Native Static Archive ==='
	rm -f $(CURDIR)/dist/libdiluvium_$(UNAME_Sl)_$(ARCHl).a
	# We use -DLUA_LIB and -UMAKE_LUA to ensure the standalone 'main' is NOT compiled
	cd .data && gcc -O3 -c onelua.c -o onelua.o -fPIC $(PLAT_CFLAGS) -DDILUVIUM_AS_LIBRARY
	cd .data && gcc -O3 -c wasm_stubs.c -o wasm_stubs.o -fPIC
	cd .data && gcc -O3 -c analyze.c -o analyze.o -fPIC -std=c99 -DLUA_USE_LINUX -DDILUVIUM_AS_LIBRARY
	cd .data && gcc -O3 -c diluvium_api.c -o diluvium_api.o -fPIC -std=c99 -DLUA_USE_LINUX -DDILUVIUM_AS_LIBRARY
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
		ar rcs /data/libdiluvium_musl_$(ARCHl).a onelua.o wasm_stubs.o analyze.o"
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
	$(PODMAN_RUN_WASM) sh -c "cp $(WASI_SYSROOT_LIBS)/libwasi-emulated-signal.a /data/ && \
								cp $(WASI_SYSROOT_LIBS)/libwasi-emulated-process-clocks.a /data/ && \
								cp $(WASI_SYSROOT_LIBS)/libsetjmp.a /data/ && \
								cp $(WASI_SYSROOT_LIBS)/libc.a /data/libwasic.a"
	@cp .data/libwasi-emulated-signal.a dist/
	@cp .data/libwasi-emulated-process-clocks.a dist/
	@cp .data/libsetjmp.a dist/
	@cp .data/libwasic.a dist/

_wasm_unknown_build: _build_step0
	@echo '=== Building for wasm32-unknown-unknown (browser, no WASI) ==='
	$(PODMAN_BUILD_WASM) "$(WASI_CLANG) --target=wasm32-unknown-unknown \
		-c onelua.c -o onelua_wasm_unknown.o -O3 -fPIC \
		-I/data \
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
		apk add --no-cache gcc make musl-dev ncurses-static readline-static readline-dev && \
		sed -i 's/-march=native//g' makefile && \
		make clean && \
		make all \
			CC=gcc \
			MYCFLAGS='-static -Os -std=c99 -DLUA_USE_LINUX -DLUA_USE_READLINE -DMAKE_LUAC' \
			MYLDFLAGS='-static' \
			MYLIBS='-lreadline -lncurses' && \
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
	@echo "Running Test: secure_function.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) secure_function.lua)
	@echo "Running Test: sort.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) sort.lua         )
	@echo "Running Test: strings.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) strings.lua      )
	@echo "Running Test: test_analysis.lua"
	@echo "============================================="
	(cd $(CURDIR)/test && $(TEST_BIN) test_analysis.lua)
	@echo "Running Test: test_fstrings.lua"
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