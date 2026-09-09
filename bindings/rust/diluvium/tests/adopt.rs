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

/// Adoption takes the buffer; the copy path copies it. The memory counter is
/// where the difference shows, and it is the whole reason the lane exists.
#[test]
fn adoption_does_not_copy_and_the_copy_path_does() {
    let mut inst = parked();
    let before = inst.memory().bytes_now;
    let values: Vec<f64> = vec![1.5; 8192]; // 64 KB
    inst.adopt(&values).unwrap();
    let grew = inst.memory().bytes_now.saturating_sub(before);
    if has_numeric() {
        assert!(
            grew < 4096,
            "an adopted buffer is taken, not copied, so the guest heap grows \
             by a header and not by 64 KB -- grew {grew}"
        );
    } else {
        assert!(
            grew >= 65536,
            "the copy path copies: the string is in the guest heap -- grew {grew}"
        );
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
