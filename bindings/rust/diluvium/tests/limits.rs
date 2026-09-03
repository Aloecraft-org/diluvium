//! Budgets, snapshots and endpoints from the Rust side: the rest of `dv.h`,
//! transcribed in 5.5.1_build12's binding pass. Same posture as `host.rs` --
//! dv_check.c proves the C surface, this proves it is usable from a host with
//! its own types and its own opinions about ownership.

use diluvium::{Accepted, Config, Error, Instance, Step};

// --- budgets ----------------------------------------------------------------

#[test]
fn a_runaway_loop_is_stopped_by_its_instruction_budget() {
    let mut inst = Config::new()
        .budget(50_000, 0)
        .load_source("while true do end", "runaway")
        .unwrap();
    match inst.run() {
        Err(Error::Program(_)) => {}
        other => panic!("expected the budget to abort the program, got {other:?}"),
    }
    assert!(
        inst.exceeded(),
        "the stop must be attributable to the budget"
    );
    let used = inst.usage();
    assert!(
        used.instructions >= 50_000,
        "usage reports what was spent: {used:?}"
    );
}

#[test]
fn a_failing_program_is_not_reported_as_over_budget() {
    // The two arrive as the same Error::Program; `exceeded` is what tells a
    // supervisor to grow a budget rather than restart a bug.
    let mut inst = Config::new()
        .budget(1_000_000, 0)
        .load_source("error('a real bug', 0)", "buggy")
        .unwrap();
    assert!(inst.run().is_err());
    assert!(!inst.exceeded());
}

#[test]
fn a_budget_cannot_change_mid_flight() {
    let mut inst =
        Instance::from_source("local q = queue.lookup('inbox') queue.wait({q})", "started")
            .unwrap();
    assert!(inst.set_budget(1000, 0).is_ok(), "not started yet");
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    match inst.set_budget(2000, 0) {
        Err(Error::Busy(_)) => {}
        other => panic!("a started instance must refuse a budget, got {other:?}"),
    }
}

#[test]
fn memory_reports_bytes_now_and_the_peak() {
    let mut inst = Instance::from_source(
        "local big = ('x'):rep(200000) big = nil collectgarbage() return 1",
        "peaky",
    )
    .unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Done));
    let mem = inst.memory();
    assert!(
        mem.bytes_peak >= 200_000,
        "the peak saw the allocation: {mem:?}"
    );
    assert!(mem.bytes_now > 0, "an instance is never free: {mem:?}");
    assert!(mem.bytes_now <= mem.bytes_peak);
    let used = inst.usage();
    assert_eq!(
        used.memory_kb_peak,
        mem.bytes_peak / 1024,
        "one high-water mark behind both readers"
    );
}

// --- snapshots --------------------------------------------------------------

/// A program that counts what it is fed, so state visibly survives the
/// crossing: messages before the snapshot must still be in the sum after it.
const COUNTER: &str = r#"
    local inbox = queue.lookup("inbox")
    local outbox = queue.lookup("outbox")
    local total = 0
    while true do
        local _, n = queue.wait({inbox})
        if n == "report" then
            queue.push(outbox, total)
            return
        end
        total = total + n
    end
"#;

#[test]
fn a_parked_program_crosses_a_snapshot_and_continues() {
    let mut inst = Instance::from_source(COUNTER, "counter").unwrap();
    let inbox = inst.queue("inbox").unwrap();

    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst.push(inbox, &7u32).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Parked(_)));

    let bytes = inst.snapshot(None).unwrap();
    assert!(!bytes.is_empty());
    drop(inst);

    let mut woken = Instance::restore(&bytes, None).unwrap();
    // The park was never returned by run or resume, so this is the one way to
    // learn it -- and it must be the same park the original was in.
    let wait = woken.current_wait().expect("a restored instance is parked");
    let inbox = woken.queue("inbox").unwrap();
    assert_eq!(wait.ids(), &[inbox]);

    woken.push(inbox, &5u32).unwrap();
    assert!(matches!(woken.resume(inbox).unwrap(), Step::Parked(_)));
    woken.push(inbox, &"report").unwrap();
    assert!(matches!(woken.resume(inbox).unwrap(), Step::Done));

    let outbox = woken.queue("outbox").unwrap();
    assert_eq!(
        woken.pop::<u32>(outbox).unwrap(),
        Some(12),
        "the 7 fed before the snapshot survived it"
    );
}

#[test]
fn a_snapshot_carries_queue_contents() {
    let mut inst = Instance::from_source(COUNTER, "loaded-inbox").unwrap();
    let inbox = inst.queue("inbox").unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    // Parked, with unconsumed messages sitting in the inbox.
    assert_eq!(inst.push(inbox, &3u32).unwrap(), Accepted::Yes);
    let bytes = inst.snapshot(None).unwrap();
    drop(inst);

    let mut woken = Instance::restore(&bytes, None).unwrap();
    let inbox = woken.queue("inbox").unwrap();
    assert_eq!(
        woken.queue_info(inbox).unwrap().len,
        1,
        "the queued message crossed inside the snapshot"
    );
    assert!(matches!(woken.resume(inbox).unwrap(), Step::Parked(_)));
    woken.push(inbox, &"report").unwrap();
    assert!(matches!(woken.resume(inbox).unwrap(), Step::Done));
    let outbox = woken.queue("outbox").unwrap();
    assert_eq!(woken.pop::<u32>(outbox).unwrap(), Some(3));
}

#[test]
fn a_stamped_snapshot_restores_only_under_its_own_stamp() {
    let mut inst = Instance::from_source(COUNTER, "stamped").unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    let bytes = inst.snapshot(Some("host-a")).unwrap();

    match Instance::restore(&bytes, Some("host-b")) {
        Err(Error::SnapshotMismatch(_)) => {}
        other => panic!("a foreign stamp must be refused, got {other:?}"),
    }
    match Instance::restore(&bytes, None) {
        Err(Error::SnapshotMismatch(_)) => {}
        other => panic!("dropping the stamp must not launder it, got {other:?}"),
    }
    assert!(Instance::restore(&bytes, Some("host-a")).is_ok());
}

#[test]
fn an_unstamped_snapshot_is_refused_by_a_host_with_an_identity() {
    // The asymmetry that makes stamping mean something: a host that supplies
    // an identity refuses a snapshot without one.
    let mut inst = Instance::from_source(COUNTER, "unstamped").unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    let bytes = inst.snapshot(None).unwrap();
    match Instance::restore(&bytes, Some("host-a")) {
        Err(Error::SnapshotMismatch(_)) => {}
        other => panic!("advisory stamping is no stamping, got {other:?}"),
    }
}

#[test]
fn garbage_is_refused_not_raised() {
    match Instance::restore(&[0xde, 0xad, 0xbe, 0xef], None) {
        Err(_) => {}
        Ok(_) => panic!("four bytes of garbage are not an instance"),
    }
}

#[test]
fn a_budget_spans_residencies() {
    // The snapshot carries the instructions consumed so far and restore
    // continues the count, so a woken instance is exactly as budgeted as when
    // it parked. The budget is set through Config, the only order that exists.
    //
    // The guest does real work per message because the count advances at the
    // hook's granularity (1000 instructions): a program that parks in fewer
    // has honestly spent zero, and this test needs a nonzero figure to watch
    // cross the snapshot.
    let mut inst = Config::new()
        .budget(1_000_000, 0)
        .load_source(
            r#"
            local inbox = queue.lookup("inbox")
            while true do
                local _, n = queue.wait({inbox})
                if n == "stop" then return end
                local burn = 0
                for i = 1, 5000 do burn = burn + i end
            end
            "#,
            "budgeted",
        )
        .unwrap();
    let inbox = inst.queue("inbox").unwrap();
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst.push(inbox, &1u32).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Parked(_)));
    let spent_before = inst.usage().instructions;
    assert!(spent_before > 0);
    let bytes = inst.snapshot(None).unwrap();
    drop(inst);

    let woken = Config::new()
        .budget(1_000_000, 0)
        .restore(&bytes, None)
        .unwrap();
    assert!(
        woken.usage().instructions >= spent_before,
        "the count continued from where the snapshot left it: {} then {}",
        spent_before,
        woken.usage().instructions
    );
}

// --- endpoints --------------------------------------------------------------

/// A guest that waits for a message carrying an endpoint reference, binds it,
/// and forwards annotated payloads to it.
const FORWARDER: &str = r#"
    local inbox = queue.lookup("inbox")
    while true do
        local _, msg = queue.wait({inbox})
        if msg == "stop" then return end
        local peer = endpoint.bind(msg.ref, "peer")
        queue.push(peer, { via = "forwarder", body = msg.body })
    end
"#;

/// The reference a host mints: msgpack ext 0x02 around bytes only the host
/// interprets. Ext 8 header, then the payload.
fn reference(payload: &[u8]) -> Vec<u8> {
    let mut r = vec![0xc7, payload.len() as u8, 0x02];
    r.extend_from_slice(payload);
    r
}

/// The message `{ref = <ext>, body = <n>}`, encoded by hand because the ext is
/// not something serde can be asked for.
fn ref_message(payload: &[u8], body: u8) -> Vec<u8> {
    let mut m = vec![0x82]; // fixmap, 2 entries
    m.extend_from_slice(&[0xa3]); // fixstr "ref"
    m.extend_from_slice(b"ref");
    m.extend_from_slice(&reference(payload));
    m.extend_from_slice(&[0xa4]); // fixstr "body"
    m.extend_from_slice(b"body");
    m.push(body); // positive fixint
    m
}

#[test]
fn an_allowed_reference_binds_and_its_queue_drains() {
    #[derive(serde::Deserialize, Debug, PartialEq)]
    struct Forwarded {
        via: String,
        body: u32,
    }

    let mut inst = Instance::from_source(FORWARDER, "forwarder").unwrap();
    let inbox = inst.queue("inbox").unwrap();
    inst.endpoint_allow(b"peer-1", 42);

    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    assert!(
        inst.endpoint_queue(42).is_none(),
        "no queue until the program binds"
    );

    inst.push_raw(inbox, &ref_message(b"peer-1", 7)).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Parked(_)));

    let q = inst.endpoint_queue(42).expect("bound now");
    assert_eq!(
        inst.pop::<Forwarded>(q).unwrap(),
        Some(Forwarded {
            via: "forwarder".into(),
            body: 7
        }),
        "what the guest pushed to its peer is the host's to drain"
    );
}

#[test]
fn a_reference_nobody_authorised_refuses_to_bind() {
    let mut inst = Instance::from_source(FORWARDER, "unauthorised").unwrap();
    let inbox = inst.queue("inbox").unwrap();
    // No endpoint_allow, no handler: the bind must raise in the guest, because
    // it asked for one specific destination and did not get it.
    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst.push_raw(inbox, &ref_message(b"nowhere", 1)).unwrap();
    match inst.resume(inbox) {
        Err(Error::Program(msg)) => {
            assert!(!msg.is_empty());
        }
        other => panic!("expected the guest to raise, got {other:?}"),
    }
}

#[test]
fn the_bind_callback_answers_what_allow_did_not() {
    let mut inst = Instance::from_source(FORWARDER, "handled").unwrap();
    let inbox = inst.queue("inbox").unwrap();
    inst.on_endpoint_bind(|reference| (reference == b"dynamic").then_some(9));

    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst.push_raw(inbox, &ref_message(b"dynamic", 3)).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Parked(_)));
    let q = inst.endpoint_queue(9).expect("the callback's token");
    assert!(inst.pop_raw(q).unwrap().is_some());
}

#[test]
fn a_closed_endpoint_is_gone_and_stays_gone() {
    let mut inst = Instance::from_source(
        r#"
        local inbox = queue.lookup("inbox")
        local outbox = queue.lookup("outbox")
        local _, msg = queue.wait({inbox})
        local peer = endpoint.bind(msg.ref, "peer")
        queue.push(outbox, endpoint.status(peer))
        queue.wait({inbox})
        queue.push(outbox, endpoint.status(peer))
        "#,
        "mortality",
    )
    .unwrap();
    let inbox = inst.queue("inbox").unwrap();
    let outbox = inst.queue("outbox").unwrap();
    inst.endpoint_allow(b"peer-1", 1);

    assert!(matches!(inst.run().unwrap(), Step::Parked(_)));
    inst.push_raw(inbox, &ref_message(b"peer-1", 0)).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Parked(_)));
    assert_eq!(inst.pop::<String>(outbox).unwrap().as_deref(), Some("live"));

    let q = inst.endpoint_queue(1).unwrap();
    inst.endpoint_close(q).unwrap();
    inst.push(inbox, &0u32).unwrap();
    assert!(matches!(inst.resume(inbox).unwrap(), Step::Done));
    assert_eq!(inst.pop::<String>(outbox).unwrap().as_deref(), Some("gone"));
}
