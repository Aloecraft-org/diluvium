//! Handing a host-owned column to a guest.
//!
//! Two things are checked here that a single build cannot check alone: the
//! `numeric` feature has to be reachable *through this crate* (an embedder
//! depends on `diluvium`, not on `diluvium-sys`), and `adopt` has to do the
//! documented thing in both configurations -- take the buffer where the
//! feature is on, copy it into a string where it is off. The test reads
//! which build it is in rather than assuming, so `cargo test -p diluvium`
//! and `cargo test -p diluvium --features numeric` both mean something.

use diluvium::{Adopted, Config, Instance, Step};

fn parked() -> Instance {
    let mut inst = Config::new()
        .load_source(
            r#"
            local inbox = queue.lookup("inbox")
            local _, m = queue.wait({inbox})
            return m
            "#,
            "adopter",
        )
        .unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst
}

fn has_numeric() -> bool {
    diluvium::library_features().contains(&"numeric")
}

/// The feature reaches the core through this crate, or it reaches nothing.
///
/// Without the passthrough in `Cargo.toml` an embedder takes `diluvium-sys`
/// with defaults, and a host that asked for arrays gets a core reporting no
/// `numeric` and having none.
#[test]
fn the_numeric_feature_is_reachable_from_this_crate() {
    let features = diluvium::library_features();
    assert!(
        features.contains(&"regex") && features.contains(&"snapshot"),
        "the core always reports its always-on features: {features:?}"
    );
    assert_eq!(
        features.contains(&"numeric"),
        cfg!(feature = "numeric"),
        "the crate feature and what the core reports have to agree; \
         {features:?} under cfg numeric = {}",
        cfg!(feature = "numeric")
    );
}

/// What the guest gets is the documented pair, and which one it is follows
/// the build rather than the call.
#[test]
fn a_column_is_adopted_or_copied_and_says_which() {
    let mut inst = parked();
    let values: Vec<f64> = (0..1024).map(|i| i as f64 * 0.5).collect();
    let got = inst
        .adopt(&values)
        .expect("a parked instance takes a column");
    if has_numeric() {
        assert_eq!(got, Adopted::Array, "the feature is on, so it is an array");
    } else {
        assert_eq!(
            got,
            Adopted::StringCopy,
            "no feature, so the documented fallback"
        );
    }
}

/// A column the instance holds is on the instance's counter, whichever
/// handover it went through.
///
/// This used to assert the opposite for the adopt path -- that the counter
/// barely moved -- and passed, because the adopted bytes were never charged:
/// the buffer does not pass through the instance's allocator, so nothing
/// counted it. That made `memory()` report a header where a megabyte was, and
/// a budget no bound at all on what a host hands over. The counter is not
/// where zero-copy shows any more, and it never should have been: the
/// instance holds the same number of bytes either way, and the difference
/// between the two paths is the copy that is not made, not the memory that is
/// not held.
#[test]
fn an_adopted_column_is_charged_like_a_copied_one() {
    let mut inst = parked();
    let before = inst.memory().bytes_now;
    let values: Vec<f64> = vec![1.5; 131072]; // 1 MB
    inst.adopt(&values).unwrap();
    let grew = inst.memory().bytes_now.saturating_sub(before);
    assert!(
        grew >= 1024 * 1024,
        "the instance holds the column, so its counter says so -- grew {grew} \
         under numeric = {}",
        has_numeric()
    );
}

/// A column bigger than the budget comes back, rather than taking the process
/// with it.
///
/// Every way of putting the bytes in front of the guest allocates, and
/// `dv_array_adopt` is called from host code with nothing protected above it:
/// an allocation failure there used to reach the panic function and abort. A
/// test for that is a test that the process is still alive to run the next
/// line, so that is what this asserts. Which answer comes back depends on the
/// build -- with the feature the array's header fits and the column is taken,
/// leaving the instance over its limit; without it the copy is the whole
/// column and the handover is refused -- and both are the documented answer.
#[test]
fn a_column_bigger_than_the_budget_returns() {
    let mut inst = Config::new()
        .budget(0, 256)
        .load_source(
            r#"
            local inbox = queue.lookup("inbox")
            local _, m = queue.wait({inbox})
            return m
            "#,
            "adopter",
        )
        .unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));

    let values: Vec<f64> = vec![1.5; 131072]; // 1 MB against 256 KB
    match inst.adopt(&values) {
        Ok(Adopted::Array) => assert!(
            has_numeric() && inst.memory().bytes_now > 256 * 1024,
            "the column was taken, so the instance is over its limit and \
             says so"
        ),
        Ok(Adopted::StringCopy) => panic!("1 MB cannot be copied into 256 KB"),
        Err(e) => assert!(
            !has_numeric(),
            "the copy path refuses; the adopt path had room for a header: {e}"
        ),
    }
}

/// All three dtypes `dv.h` documents, and the empty slice, which needs no
/// allocation and is not an error.
#[test]
fn every_dtype_and_the_empty_column() {
    let mut inst = parked();
    inst.adopt(&[1.0f64, 2.0]).unwrap();
    inst.adopt(&[1i64, -2, 3]).unwrap();
    inst.adopt(&[0u8, 255]).unwrap();
    inst.adopt::<f64>(&[]).unwrap();
    inst.adopt::<u8>(&[]).unwrap();
}
