// Build the amalgamation and link it in.
//
// §12.2 wants a -sys crate carrying prebuilt static libraries per triple with a
// build-from-source fallback. This is the fallback, and it is what the
// repository itself uses: fetching an archive during a repo test would make the
// test depend on a release having happened.
//
// The published crate adds the prebuilt path in front of this; nothing here
// changes when it does.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    // src/ is four levels up: bindings/rust/diluvium-sys -> repo root.
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .canonicalize()
        .expect("cannot locate the repository root");
    let src = root.join("src");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    println!("cargo:rerun-if-changed={}", src.display());

    // The amalgamation is one translation unit, so one invocation builds
    // everything: core Lua, the guest libraries and the ABI.
    let obj = out.join("onelua.o");
    let mut cc = Command::new(std::env::var("CC").unwrap_or_else(|_| "cc".into()));
    cc.arg("-O2")
        .arg("-std=c99")
        .arg("-fPIC")
        .arg("-DMAKE_LIB") // no standalone main(); we are a library here
        .arg("-c")
        .arg(src.join("onelua.c"))
        .arg("-I")
        .arg(&src)
        .arg("-o")
        .arg(&obj);
    if cfg!(target_os = "linux") {
        cc.arg("-DLUA_USE_LINUX");
    } else if cfg!(target_os = "macos") {
        cc.arg("-DLUA_USE_MACOSX");
    }
    let status = cc.status().expect("failed to run the C compiler");
    assert!(status.success(), "compiling onelua.c failed");

    let lib = out.join("libdiluvium.a");
    let status = Command::new(std::env::var("AR").unwrap_or_else(|_| "ar".into()))
        .arg("rcs")
        .arg(&lib)
        .arg(&obj)
        .status()
        .expect("failed to run ar");
    assert!(status.success(), "archiving failed");

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=diluvium");
    println!("cargo:rustc-link-lib=dylib=m");
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=dylib=dl");
    }
    println!("cargo:include={}", src.display());
}
