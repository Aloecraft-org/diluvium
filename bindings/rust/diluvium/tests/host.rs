//! The Rust side of M4's acceptance: a program exchanges messages with a host
//! across the boundary, including the zero-copy path.
//!
//! These overlap dv_check.c on purpose. That one proves the C surface works;
//! this one proves the surface is usable from a host that has its own types and
//! its own opinions about ownership, which is a different claim.

use diluvium::{Accepted, Error, Instance, Step};
use serde::{Deserialize, Serialize};

#[test]
fn version_is_checked_before_anything_else() {
    assert_eq!(diluvium::abi_version(), diluvium::library_abi_version());
}

#[test]
fn a_program_runs_and_finishes() {
    let mut inst = Instance::from_source("return 1", "trivial").unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Done));
}

#[test]
fn an_error_carries_its_traceback_across() {
    let mut inst =
        Instance::from_source("local function inner() error('boom', 0) end inner()", "bad")
            .unwrap();
    match inst.run() {
        Err(Error::Program(msg)) => {
            assert!(msg.contains("boom"), "message crossed: {msg}");
            assert!(msg.contains("stack traceback"), "traceback: {msg}");
            assert!(msg.contains("inner"), "names the failing frame: {msg}");
        }
        other => panic!("expected a program error, got {other:?}"),
    }
}

#[test]
fn a_syntax_error_is_reported_at_load() {
    match Instance::from_source("this is not lua", "bad") {
        Err(Error::Program(msg)) => assert!(!msg.is_empty()),
        other => panic!("expected a load error, got {other:?}"),
    }
}

/// Host and guest exchanging typed messages, which is the point of the crate:
/// the guest sees Lua tables, the host sees a struct, and msgpack is the only
/// thing in between.
#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct Order {
    id: u32,
    item: String,
    qty: u32,
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct Confirmation {
    id: u32,
    total: u32,
}

#[test]
fn typed_messages_round_trip_through_a_parked_program() {
    let mut inst = Instance::from_source(
        r#"
        local inbox = queue.lookup("inbox")
        local outbox = queue.lookup("outbox")
        while true do
            local _, order = queue.wait({inbox})
            if order == "done" then return end
            queue.push(outbox, { id = order.id, total = order.qty * 3 })
        end
        "#,
        "orders",
    )
    .unwrap();

    let inbox = inst.queue("inbox").expect("inbox exists from the start");
    let outbox = inst.queue("outbox").expect("outbox too");

    let mut step = inst.run().unwrap();
    let mut confirmations = Vec::new();

    for n in 1..=3u32 {
        let wait = match &step {
            Step::Parked(w) => w.clone(),
            Step::Done => panic!("finished too early"),
        };
        assert_eq!(wait.ids(), &[inbox], "it is waiting on its inbox");
        assert!(!wait.is_waiting_for_space());
        assert_eq!(wait.timeout(), None, "the program set no timeout");

        let order = Order {
            id: n,
            item: format!("widget-{n}"),
            qty: n * 2,
        };
        assert_eq!(inst.push(inbox, &order).unwrap(), Accepted::Yes);
        step = inst.resume(inbox).unwrap();

        let c: Confirmation = inst.pop(outbox).unwrap().expect("a confirmation came back");
        confirmations.push(c);
    }

    assert_eq!(
        confirmations,
        vec![
            Confirmation { id: 1, total: 6 },
            Confirmation { id: 2, total: 12 },
            Confirmation { id: 3, total: 18 },
        ]
    );

    // And the wait-set is correct on a resume that parks again, which is the
    // case dv_resume's own signature cannot report -- the wrapper reads it back.
    assert!(matches!(step, Step::Parked(_)));
    inst.push(inbox, &"done").unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Done));
}

#[test]
fn a_full_queue_is_an_answer_not_an_error() {
    let mut inst = Instance::from_source(
        "queue.declare('small', {capacity = 1, exported = true}) return 0",
        "small",
    )
    .unwrap();
    inst.run().unwrap();
    let q = inst.queue("small").unwrap();
    assert_eq!(inst.push(q, &1u32).unwrap(), Accepted::Yes);
    assert_eq!(inst.push(q, &2u32).unwrap(), Accepted::Full);
    assert!(!Accepted::Full.is_accepted());
    let info = inst.queue_info(q).unwrap();
    assert_eq!((info.len, info.capacity), (1, 1));
    assert!(info.exported);
}

#[test]
fn drop_oldest_reports_what_it_did() {
    let mut inst = Instance::from_source(
        "queue.declare('ring', {capacity = 1, on_full = 'drop_oldest', exported = true}) return 0",
        "ring",
    )
    .unwrap();
    inst.run().unwrap();
    let q = inst.queue("ring").unwrap();
    inst.push(q, &"first").unwrap();
    assert_eq!(inst.push(q, &"second").unwrap(), Accepted::DroppedOldest);
    assert_eq!(
        inst.pop::<String>(q).unwrap().as_deref(),
        Some("second"),
        "the older message was the one evicted"
    );
}

#[test]
fn raw_bytes_skip_the_codec_in_both_directions() {
    let mut inst = Instance::from_source(
        "local q = queue.declare('raw', {exported = true}) queue.push(q, 7) return 0",
        "raw",
    )
    .unwrap();
    inst.run().unwrap();
    let q = inst.queue("raw").unwrap();
    // 7 is a msgpack positive fixint, so exactly one byte.
    assert_eq!(inst.pop_raw(q).unwrap().as_deref(), Some(&[0x07u8][..]));
    inst.push_raw(q, &[0x08]).unwrap();
    assert_eq!(inst.pop::<u32>(q).unwrap(), Some(8));
}

#[test]
fn a_guest_queue_is_invisible_until_the_program_declares_it() {
    let mut inst = Instance::from_source(
        "queue.declare('later', {exported = true}) return 0",
        "later",
    )
    .unwrap();
    assert!(
        inst.queue("later").is_none(),
        "queues are guest-declared, so nothing exists before it runs"
    );
    inst.run().unwrap();
    assert!(inst.queue("later").is_some());
    assert!(inst.queue("never").is_none());
}

#[test]
fn the_host_owns_the_clock() {
    // The program asks for five seconds. The test does not wait five seconds:
    // the host decides when time is up, which is the whole of 8.3.
    let mut inst = Instance::from_source(
        r#"
        local q = queue.declare('t')
        local _, _, status = queue.wait({q}, 5000)
        local out = queue.declare('res', {exported = true})
        queue.push(out, status)
        "#,
        "clock",
    )
    .unwrap();
    let step = inst.run().unwrap();
    match step {
        Step::Parked(w) => assert_eq!(w.timeout(), Some(std::time::Duration::from_millis(5000))),
        Step::Done => panic!("expected a park"),
    }
    let started = std::time::Instant::now();
    assert!(matches!(inst.resume_timeout().unwrap(), Step::Done));
    assert!(
        started.elapsed() < std::time::Duration::from_secs(1),
        "answering a timeout must not mean waiting for it"
    );
    let res = inst.queue("res").unwrap();
    assert_eq!(inst.pop::<String>(res).unwrap().as_deref(), Some("timeout"));
}

#[test]
fn a_park_for_space_asks_to_be_drained() {
    let mut inst = Instance::from_source(
        r#"
        local q = queue.declare('b', {capacity = 1, on_full = 'block', exported = true})
        queue.push(q, 1)
        queue.push(q, 2)
        "#,
        "backpressure",
    )
    .unwrap();
    let step = inst.run().unwrap();
    let wait = match step {
        Step::Parked(w) => w,
        Step::Done => panic!("expected a park"),
    };
    assert!(
        wait.is_waiting_for_space(),
        "the host must be able to tell 'drain me' from 'feed me'"
    );
    let q = wait.ids()[0];
    assert_eq!(inst.pop::<u32>(q).unwrap(), Some(1));
    assert!(matches!(inst.resume(q).unwrap(), Step::Done));
}

#[test]
fn running_a_parked_program_is_refused() {
    let mut inst = Instance::from_source(
        "local q = queue.lookup('inbox') queue.wait({q})",
        "parked",
    )
    .unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    match inst.run() {
        Err(Error::Busy(_)) => {}
        other => panic!("expected Busy, got {other:?}"),
    }
}

#[test]
fn export_notifications_reach_the_host() {
    use std::sync::{Arc, Mutex};
    let seen = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&seen);
    let mut inst = Instance::from_source(
        r#"
        local pub = queue.declare('pub', {exported = true})
        local priv = queue.declare('priv')
        queue.push(priv, 1)
        queue.push(pub, 2)
        "#,
        "notify",
    )
    .unwrap();
    inst.on_export(move |id| sink.lock().unwrap().push(id));
    inst.run().unwrap();
    let seen = seen.lock().unwrap();
    assert_eq!(
        seen.len(),
        1,
        "only exported queues are the host's business"
    );
    assert_eq!(seen[0], inst.queue("pub").unwrap());
}

#[test]
fn an_instance_moves_between_threads() {
    // Send but not Sync: moving is fine, sharing is not. The compiler enforces
    // the second half; this exercises the first.
    let inst = Instance::from_source("return 1", "moved").unwrap();
    let handle = std::thread::spawn(move || {
        let mut inst = inst;
        matches!(inst.run().unwrap(), Step::Done)
    });
    assert!(handle.join().unwrap());
}
