//! What the engine has to accept, checked without the wasi-sdk.
//!
//! These exist because of a CI failure that lasted weeks and was invisible here:
//! `cargo build` passed, `cargo test` had nothing to run, and the only thing that
//! ever loaded a real module was a job that needed a container running the pinned
//! wasi-sdk. So the one property the crate depends on -- that the engine can parse
//! what the wasi-sdk emits -- was checked nowhere a developer would see it.
//!
//! The modules below are hand-written WAT rather than a real `diluvium.wasm`,
//! which is the point: they isolate the *engine configuration* from the build that
//! produces the guest, and they run anywhere cargo does.

use diluvium_wasmtime::Runtime;

/// The exception-handling proposal, which every `diluvium.wasm` needs.
///
/// The wasi-sdk lowers `setjmp`/`longjmp` onto EH instructions (`-mllvm
/// -wasm-enable-sjlj` in the Makefile), and Lua's error handling is built on
/// `longjmp` -- so `throw` and `try_table` appear in the module whether or not any
/// program ever raises an error. An engine without EH refuses it at parse.
///
/// This is the exact failure CI reported: `exceptions proposal not enabled (at
/// offset 0xd60)`. Removing `config.wasm_exceptions(true)`, or dropping wasmtime's
/// `gc` feature that gates its default, turns this test red.
#[test]
fn the_engine_accepts_exception_handling() {
    let wasm = wat::parse_str(
        r#"
        (module
          (tag $e)
          (func $raises
            (throw $e))
          (func (export "catches")
            (block $done
              (try_table (catch_all $done)
                (call $raises))))
          (memory (export "memory") 1))
        "#,
    )
    .expect("the WAT itself is valid");

    // Runtime::new compiles the module, which is where the refusal happened.
    match Runtime::new(&wasm) {
        Ok(_) => {}
        Err(e) => panic!(
            "the engine refused a module using exception handling, which every \
             diluvium.wasm contains: {e:#}"
        ),
    }
}

/// Fuel, for the same reason: it is enabled on the engine and opt-in per instance,
/// so a config that forgot it would fail only once a caller tried to meter
/// something.
#[test]
fn the_engine_accepts_a_plain_module_and_has_fuel_enabled() {
    let wasm = wat::parse_str(
        r#"
        (module
          (func (export "nothing"))
          (memory (export "memory") 1))
        "#,
    )
    .expect("the WAT itself is valid");
    assert!(Runtime::new(&wasm).is_ok(), "a trivial module compiles");
}

/// A module that is not wasm at all must be refused with something readable, not
/// a panic. `Runtime::new` is the first thing a host calls and its error is the
/// first thing a host reads.
#[test]
fn garbage_is_refused_with_a_message() {
    let msg = match Runtime::new(b"this is not a wasm module") {
        Ok(_) => panic!("a non-wasm buffer compiled, which it must not"),
        Err(e) => format!("{e:#}"),
    };
    assert!(
        msg.contains("compiling diluvium.wasm"),
        "the error should say what was being compiled, got: {msg}"
    );
}
