// Build the amalgamation for the target being built, and link it in.
//
// §12.2 wants a -sys crate carrying prebuilt static libraries per triple with a
// build-from-source fallback. This is the fallback, and it is what the
// repository itself uses: fetching an archive during a repo test would make the
// test depend on a release having happened.
//
// The published crate adds the prebuilt path in front of this; nothing here
// changes when it does.
//
// **A build script's `cfg!` describes the HOST, not the target.** The first
// version of this file used `cfg!(target_os = ...)` to pick `-DLUA_USE_LINUX`
// and `-ldl`, and invoked a bare `cc` with no `--target`, so
// `cargo build --target wasm32-unknown-unknown` compiled an x86-64 object,
// emitted `-lm`/`-ldl`, and reported success -- because `cargo build` on a
// library never links, and nothing looked at what came out. Forcing the link
// found it: "archive member 'onelua.o' is neither Wasm object file nor LLVM
// bitcode". Everything here reads `TARGET` instead, `tests/link.rs` forces a
// link on every target so the silent version cannot come back, and a target
// this script cannot honestly compile for is a hard error rather than a
// wrong object.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let target = std::env::var("TARGET").expect("cargo sets TARGET");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    // src/ is four levels up: bindings/rust/diluvium-sys -> repo root.
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .canonicalize()
        .expect("cannot locate the repository root");
    let src = root.join("src");

    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=AR");
    println!("cargo:rerun-if-env-changed=WASI_SDK_PATH");

    let plat = Platform::of(&target);
    let toolchain = Toolchain::resolve(&plat, &target);

    // The amalgamation is one translation unit, so one invocation builds
    // everything: core Lua, the guest libraries and the ABI.
    let obj = out.join("onelua.o");
    let mut cc = toolchain.cc();
    cc.arg("-O2")
        .arg("-std=c99")
        .arg("-DMAKE_LIB") // no standalone main(); we are a library here
        .arg("-c")
        .arg(src.join("onelua.c"))
        .arg("-I")
        .arg(&src)
        .arg("-o")
        .arg(&obj);
    for flag in plat.cflags(&src, &toolchain) {
        cc.arg(flag);
    }
    run(cc, "compiling onelua.c");
    plat.assert_object_format(&obj);

    let mut members = vec![obj];

    // wasm only: definitions rustc's bundled wasi-libc is too old to carry.
    // Weak, so a newer libc that has them wins and nothing collides.
    if plat.is_wasm() {
        let compat_src = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("wasm_compat.c");
        println!("cargo:rerun-if-changed={}", compat_src.display());
        let compat_obj = out.join("wasm_compat.o");
        let mut cc = toolchain.cc();
        cc.arg("-O2")
            .arg("-std=c99")
            .arg("-c")
            .arg(&compat_src)
            .arg("-o")
            .arg(&compat_obj);
        for flag in plat.cflags(&src, &toolchain) {
            cc.arg(flag);
        }
        run(cc, "compiling wasm_compat.c");
        plat.assert_object_format(&compat_obj);
        members.push(compat_obj);
    }

    let lib = out.join("libdiluvium.a");
    let _ = std::fs::remove_file(&lib); // ar rcs appends; stale members confuse it
    let mut ar = toolchain.ar();
    ar.arg("rcs").arg(&lib).args(&members);
    run(ar, "archiving libdiluvium.a");

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=diluvium");

    match &plat {
        Platform::Native { os } => {
            println!("cargo:rustc-link-lib=dylib=m");
            if os == "linux" {
                println!("cargo:rustc-link-lib=dylib=dl");
            }
        }
        // No libm or libdl anywhere in wasm: wasi-libc carries the math, and
        // there is no dynamic loader to ask for.
        Platform::Wasi { p2 } => {
            // The wasi-sdk archives the sjlj lowering needs, COPIED into
            // OUT_DIR rather than reached through the sysroot's own lib
            // directory. Putting that directory on the link search path
            // shadows rustc's self-contained crt1 with wasi-sdk's, whose
            // newer wasi-libc weak-imports `__wasi_init_tp` -- an `env::`
            // import no host defines, so the module instantiates nowhere.
            // (bindings/rust/WASM-SPIKE.md, wrinkle 2.)
            let mut archives = vec!["libsetjmp.a", "libwasi-emulated-signal.a"];
            if !p2 {
                // Preview1-only: `libwasi-emulated-process-clocks.a` carries a
                // `__wasi_clock_time_get` reference that p2 componentization
                // cannot resolve. On p2 `clock()` comes from wasm_compat.c.
                archives.push("libwasi-emulated-process-clocks.a");
            }
            for archive in archives {
                let from = toolchain.sysroot_lib().join(archive);
                let to = out.join(archive);
                std::fs::copy(&from, &to).unwrap_or_else(|e| {
                    panic!("cannot copy {} from the wasi sysroot: {e}", from.display())
                });
                let name = archive
                    .trim_start_matches("lib")
                    .trim_end_matches(".a");
                println!("cargo:rustc-link-lib=static={name}");
            }
        }
        Platform::Browser => {
            let from = toolchain.sysroot_lib().join("libsetjmp.a");
            let to = out.join("libsetjmp.a");
            std::fs::copy(&from, &to).unwrap_or_else(|e| {
                panic!("cannot copy {} from the wasi sysroot: {e}", from.display())
            });
            println!("cargo:rustc-link-lib=static=setjmp");
            // The browser build takes its libc from the embedder at
            // instantiation -- see the "browser contract" section of
            // WASM-SPIKE.md for the symbol list -- so the references stay
            // undefined here and become `env::` imports. This covers this
            // crate's own linked targets (its tests); a downstream binary
            // must pass the same flag itself.
            println!("cargo:rustc-link-arg=--allow-undefined");
        }
    }

    println!("cargo:include={}", src.display());
}

fn run(mut cmd: Command, what: &str) {
    let status = cmd
        .status()
        .unwrap_or_else(|e| panic!("{what}: cannot run {:?}: {e}", cmd.get_program()));
    assert!(status.success(), "{what} failed: {cmd:?}");
}

/// What the *target* is, as opposed to whatever machine is doing the building.
enum Platform {
    Native { os: String },
    /// wasm32-wasip1 / wasm32-wasip2 (`p2` distinguishes them: see the clocks
    /// archive above).
    Wasi { p2: bool },
    /// wasm32-unknown-unknown: no WASI, libc from the embedder.
    Browser,
}

impl Platform {
    fn of(target: &str) -> Self {
        if !target.starts_with("wasm32") {
            let os = if target.contains("linux") {
                "linux"
            } else if target.contains("darwin") || target.contains("apple") {
                "macos"
            } else if target.contains("windows") {
                "windows"
            } else {
                "other"
            };
            return Platform::Native { os: os.into() };
        }
        match target {
            "wasm32-wasip2" => Platform::Wasi { p2: true },
            "wasm32-wasip1" | "wasm32-wasi" | "wasm32-wasip1-threads" => {
                Platform::Wasi { p2: false }
            }
            "wasm32-unknown-unknown" => Platform::Browser,
            other => panic!(
                "diluvium-sys does not know how to build the C core for {other}. \
                 Supported: native targets, wasm32-wasip1, wasm32-wasip2, \
                 wasm32-unknown-unknown."
            ),
        }
    }

    fn is_wasm(&self) -> bool {
        !matches!(self, Platform::Native { .. })
    }

    /// Look at what the compiler actually produced.
    ///
    /// This is the guard that would have caught the original bug on its own,
    /// and it is deliberately the *artifact* being checked rather than a
    /// downstream symptom. Forcing a link (tests/link.rs) is the second
    /// layer, but it is not sufficient by itself on wasm32-unknown-unknown:
    /// that target links with `--allow-undefined`, so a symbol the archive
    /// failed to provide becomes an `env::` import instead of an error, and
    /// dead-code elimination can drop the reference entirely. Four bytes of
    /// magic cannot be argued with.
    fn assert_object_format(&self, obj: &Path) {
        let bytes = std::fs::read(obj).unwrap_or_else(|e| {
            panic!("the compiler reported success but {} is unreadable: {e}", obj.display())
        });
        let magic = bytes.get(..4).unwrap_or_default();
        let looks_wasm = magic == b"\0asm";
        // A wasm object may also be LLVM bitcode under LTO ('BC\xc0\xde').
        let looks_bitcode = magic == b"BC\xc0\xde";
        if self.is_wasm() && !(looks_wasm || looks_bitcode) {
            let what = if magic.starts_with(b"\x7fELF") {
                "an ELF object (a native build)"
            } else if magic == b"\xcf\xfa\xed\xfe" || magic == b"\xce\xfa\xed\xfe" {
                "a Mach-O object (a native build)"
            } else {
                "not a wasm object"
            };
            panic!(
                "\n\
                 {} is {what}, but the target is wasm.\n\
                 \n\
                 The C compiler in use is building for the host instead of the\n\
                 target. Set WASI_SDK_PATH (or CC) to a wasm-capable toolchain --\n\
                 see bindings/rust/WASM-SPIKE.md.\n\
                 \n\
                 Checked here because a wrong-architecture object is invisible to\n\
                 `cargo build`: a library crate is never linked, so the build\n\
                 reports success and the failure surfaces much later, in whatever\n\
                 downstream binary first tries to link it.\n",
                obj.display()
            );
        }
    }

    fn cflags(&self, src: &Path, toolchain: &Toolchain) -> Vec<String> {
        let mut flags: Vec<String> = Vec::new();
        match self {
            Platform::Native { os } => {
                flags.push("-fPIC".into());
                match os.as_str() {
                    "linux" => flags.push("-DLUA_USE_LINUX".into()),
                    "macos" => flags.push("-DLUA_USE_MACOSX".into()),
                    _ => {}
                }
            }
            Platform::Wasi { .. } => {
                flags.push("-fPIC".into());
                flags.extend(WASM_EH.iter().map(|s| s.to_string()));
                flags.extend(
                    [
                        "-DL_tmpnam=32",
                        "-D_WASI_EMULATED_SIGNAL",
                        "-D_WASI_EMULATED_PROCESS_CLOCKS",
                        "-Wno-deprecated-declarations",
                    ]
                    .iter()
                    .map(|s| s.to_string()),
                );
            }
            Platform::Browser => {
                // The Makefile's browser configuration: no OS-facing
                // libraries (a sealed instance has none anyway), the repo's
                // own setjmp shim on the include path, and the wasi headers
                // for the rest of libc.
                flags.push("-fPIC".into());
                flags.extend(WASM_EH.iter().map(|s| s.to_string()));
                flags.push("-I".into());
                flags.push(src.join("wasm-shim").display().to_string());
                flags.extend(
                    [
                        "-DDILUVIUM_AS_LIBRARY",
                        "-DLUA_USE_C89",
                        "-DL_tmpnam=32",
                        "-Dloadlib_c",
                        "-Dloslib_c",
                        "-Dliolib_c",
                        "-D__wasi__",
                        "-D_WASI_EMULATED_SIGNAL",
                        "-D_WASI_EMULATED_PROCESS_CLOCKS",
                        "-Wno-deprecated-declarations",
                        "-Wno-macro-redefined",
                    ]
                    .iter()
                    .map(|s| s.to_string()),
                );
            }
        }
        flags.extend(toolchain.target_flags.clone());
        flags
    }
}

/// Lua's error handling is setjmp/longjmp; on wasm the wasi-sdk lowers it onto
/// the standard exception-handling proposal. Both halves are required: without
/// the lowering a longjmp traps, and with the *legacy* encoding the module is
/// refused by engines implementing the standardised proposal. Needs LLVM 20 or
/// newer (wasi-sdk 24+); older clangs reject the second flag by name, which is
/// what `Toolchain::probe_eh` turns into an actionable message.
const WASM_EH: [&str; 4] = [
    "-mllvm",
    "-wasm-enable-sjlj",
    "-mllvm",
    "-wasm-use-legacy-eh=false",
];

struct Toolchain {
    cc: String,
    ar: String,
    /// `--target=...` and `--sysroot=...`, when the compiler needs telling.
    target_flags: Vec<String>,
    sysroot: Option<PathBuf>,
    wasi_lib_subdir: &'static str,
}

impl Toolchain {
    fn resolve(plat: &Platform, target: &str) -> Self {
        let env_key = |prefix: &str| format!("{prefix}_{}", target.replace('-', "_"));
        let cc_env = std::env::var(env_key("CC")).or_else(|_| std::env::var("CC"));
        let ar_env = std::env::var(env_key("AR")).or_else(|_| std::env::var("AR"));

        if !plat.is_wasm() {
            return Toolchain {
                cc: cc_env.unwrap_or_else(|_| "cc".into()),
                ar: ar_env.unwrap_or_else(|_| "ar".into()),
                target_flags: Vec::new(),
                sysroot: None,
                wasi_lib_subdir: "",
            };
        }

        // A wasm target needs a compiler that can produce wasm objects and a
        // wasi sysroot for the headers. Explicit CC first, then WASI_SDK_PATH.
        let sdk = std::env::var("WASI_SDK_PATH").ok().map(PathBuf::from);
        let (cc, ar, sysroot) = match (cc_env, &sdk) {
            (Ok(cc), sdk) => {
                let sysroot = sdk.clone().map(|s| s.join("share/wasi-sysroot"));
                (cc, ar_env.unwrap_or_else(|_| "llvm-ar".into()), sysroot)
            }
            (Err(_), Some(sdk)) => (
                sdk.join("bin/clang").display().to_string(),
                ar_env.unwrap_or_else(|_| sdk.join("bin/llvm-ar").display().to_string()),
                Some(sdk.join("share/wasi-sysroot")),
            ),
            (Err(_), None) => panic!(
                "\n\
                 diluvium-sys cannot build the C core for {target}: no wasm C toolchain.\n\
                 \n\
                 Building the Diluvium core for wasm needs a wasi-sdk (>= 24, for LLVM\n\
                 20's exception-handling flags). Install one and point at it:\n\
                 \n\
                     export WASI_SDK_PATH=/path/to/wasi-sdk-27.0-<platform>\n\
                 \n\
                 or set CC (and AR) to a clang that can target wasm32 with a wasi\n\
                 sysroot. See bindings/rust/WASM-SPIKE.md for the whole recipe.\n\
                 \n\
                 Refusing rather than compiling a host object: a native .o in a wasm\n\
                 archive builds green and only fails at the link, which is how this\n\
                 was missed before.\n"
            ),
        };

        let sysroot = sysroot.expect(
            "a wasi sysroot is required for wasm targets; set WASI_SDK_PATH \
             (its share/wasi-sysroot is used for headers and libsetjmp)",
        );
        assert!(
            sysroot.join("include").exists(),
            "no wasi sysroot at {} (expected share/wasi-sysroot under WASI_SDK_PATH)",
            sysroot.display()
        );

        // wasi-sdk <= 22 lays the sysroot out as wasm32-wasi, newer as
        // wasm32-wasip1; a -I that does not exist is harmless, so pass both.
        let wasi_lib_subdir = if sysroot.join("lib/wasm32-wasip1").exists() {
            "lib/wasm32-wasip1"
        } else {
            "lib/wasm32-wasi"
        };

        let mut target_flags = vec![
            format!("--target={}", clang_triple(target)),
            format!("--sysroot={}", sysroot.display()),
            format!("-I{}", sysroot.join("include").display()),
            format!("-I{}", sysroot.join("include/wasm32-wasip1").display()),
            format!("-I{}", sysroot.join("include/wasm32-wasi").display()),
        ];
        // The browser target has no wasi sysroot of its own; it borrows the
        // headers above and takes libc from the embedder, so --sysroot would
        // point the linker at libraries that do not apply.
        if matches!(plat, Platform::Browser) {
            target_flags.retain(|f| !f.starts_with("--sysroot="));
        }

        let tc = Toolchain {
            cc,
            ar,
            target_flags,
            sysroot: Some(sysroot),
            wasi_lib_subdir,
        };
        tc.probe_eh();
        tc
    }

    fn cc(&self) -> Command {
        Command::new(&self.cc)
    }

    fn ar(&self) -> Command {
        Command::new(&self.ar)
    }

    fn sysroot_lib(&self) -> PathBuf {
        self.sysroot
            .as_ref()
            .expect("wasm targets always have a sysroot")
            .join(self.wasi_lib_subdir)
    }

    /// Check the EH flags before compiling 30k lines with them, so an old
    /// clang produces one clear sentence instead of a wall of unknown-argument
    /// noise.
    fn probe_eh(&self) {
        let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
        let probe_c = out.join("eh_probe.c");
        std::fs::write(&probe_c, "int probe(void) { return 0; }\n").expect("cannot write probe");
        let mut cc = self.cc();
        cc.args(&self.target_flags)
            .args(WASM_EH)
            .arg("-c")
            .arg(&probe_c)
            .arg("-o")
            .arg(out.join("eh_probe.o"));
        let ok = cc
            .output()
            .map(|o| o.status.success())
            .unwrap_or_else(|e| panic!("cannot run the C compiler {:?}: {e}", self.cc));
        assert!(
            ok,
            "\n\
             the C compiler ({}) rejected the wasm exception-handling flags.\n\
             \n\
             Lua's setjmp/longjmp is lowered onto the EH proposal on wasm, and\n\
             '-mllvm -wasm-use-legacy-eh=false' needs LLVM 20 or newer -- wasi-sdk\n\
             25 (clang 19) fails here, wasi-sdk 27 (clang 20.1) works.\n\
             \n\
             Point WASI_SDK_PATH at a newer wasi-sdk. See\n\
             bindings/rust/WASM-SPIKE.md, wrinkle 1.\n",
            self.cc
        );
    }
}

/// rustc's triples are not always clang's.
fn clang_triple(target: &str) -> &str {
    match target {
        // clang names preview1 "wasi"; "wasm32-wasip1" is understood by
        // recent clangs but "wasm32-wasi" is understood by all of them.
        "wasm32-wasip1" | "wasm32-wasi" => "wasm32-wasi",
        // p2's *object code* is ordinary wasip1 core wasm; rustc's target
        // componentizes it afterwards.
        "wasm32-wasip2" => "wasm32-wasi",
        other => other,
    }
}
