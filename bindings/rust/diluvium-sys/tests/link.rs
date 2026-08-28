//! Force a link, on whatever target is being built.
//!
//! `cargo build` on a library crate compiles an rlib and never links it, so a
//! build script that produced an object for the *wrong architecture* reported
//! success: `cargo build --target wasm32-unknown-unknown` emitted an x86-64
//! `onelua.o`, `-DLUA_USE_LINUX` and `-ldl`, and finished green. Only forcing
//! the link showed it ("archive member 'onelua.o' is neither Wasm object file
//! nor LLVM bitcode", plus `unable to find library -lm`/`-ldl`).
//!
//! An integration test is a linked artifact, so building this file is the
//! check. Keep it dependency-free and trivial: what is being tested is that
//! the archive links at all, and that the symbols in it are real.

#[test]
fn the_c_core_links_and_answers() {
    // Calls across the FFI, so the symbol must resolve at link time rather
    // than merely be declared.
    assert_eq!(
        diluvium_sys::DV_ABI_VERSION,
        unsafe { diluvium_sys::dv_abi_version() },
        "the linked library and this crate disagree about the ABI version"
    );
}

/// A second symbol from a different part of the amalgamation, so the test
/// covers more than one translation-unit-worth of the archive.
#[test]
fn a_status_name_crosses_back() {
    let p = unsafe { diluvium_sys::dv_status_name(diluvium_sys::DV_OK) };
    assert!(!p.is_null());
    let name = unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy();
    assert_eq!(name, "DV_OK", "dv_status_name(DV_OK) = {name}");
}
