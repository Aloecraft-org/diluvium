"""The Python side of the host contract.

Overlaps dv_check.c and the Rust tests on purpose. Those prove the C surface and
that a statically-typed host can use it; this proves a dynamically-typed one can,
which is a different claim -- Python has no struct field order to couple to, but
it does have `None` meaning two things and dict keys that are not strings.

Run with: cd bindings/python && python3 build_ffi.py && python3 -m pytest tests
"""

import threading

import pytest

import diluvium
from diluvium import Accepted, Busy, Instance, ProgramError, UnknownQueue


def test_version_is_checked_before_anything_else():
    assert diluvium.abi_version() == diluvium.library_abi_version()


def test_a_program_runs_and_finishes():
    with Instance.from_source("return 1", name="trivial") as inst:
        assert inst.run().done


def test_an_error_carries_its_traceback_across():
    inst = Instance.from_source(
        "local function inner() error('boom', 0) end inner()", name="bad"
    )
    with pytest.raises(ProgramError) as e:
        inst.run()
    msg = str(e.value)
    assert "boom" in msg
    assert "stack traceback" in msg
    assert "inner" in msg, "the traceback names the failing frame, not the ABI"


def test_a_syntax_error_is_reported_at_load():
    with pytest.raises(ProgramError):
        Instance.from_source("this is not lua", name="bad")


def test_dicts_and_lists_round_trip_through_a_parked_program():
    inst = Instance.from_source(
        """
        local inbox = queue.lookup("inbox")
        local outbox = queue.lookup("outbox")
        while true do
            local _, order = queue.wait({inbox})
            if order == "done" then return end
            queue.push(outbox, { id = order.id, total = order.qty * 3,
                                 items = order.items })
        end
        """,
        name="orders",
    )
    inbox = inst.queue("inbox")
    outbox = inst.queue("outbox")
    assert inbox is not None and outbox is not None

    step = inst.run()
    replies = []
    for n in (1, 2, 3):
        assert step.parked
        assert step.wait.ids == (inbox,)
        assert step.wait.timeout is None
        assert not step.wait.for_write
        assert inst.push(inbox, {"id": n, "qty": n * 2, "items": ["a", "b"]}) == "ok"
        step = inst.resume(inbox)
        replies.append(inst.pop(outbox))

    assert replies == [
        {"id": 1, "total": 6, "items": ["a", "b"]},
        {"id": 2, "total": 12, "items": ["a", "b"]},
        {"id": 3, "total": 18, "items": ["a", "b"]},
    ]

    # A resume that parks again reports the right wait-set, which dv_resume's own
    # signature cannot -- the wrapper reads it back.
    assert step.parked
    inst.push(inbox, "done")
    assert inst.resume(inbox).done
    inst.close()


def test_a_full_queue_is_an_answer_not_an_error():
    inst = Instance.from_source(
        "queue.declare('small', {capacity = 1, exported = true}) return 0",
        name="small",
    )
    inst.run()
    q = inst.queue("small")
    assert inst.push(q, 1) == Accepted.YES
    assert inst.push(q, 2) == Accepted.FULL
    assert Accepted.is_accepted(Accepted.YES)
    assert not Accepted.is_accepted(Accepted.FULL)
    info = inst.queue_info(q)
    assert (info.len, info.capacity) == (1, 1)
    assert info.exported and info.enabled
    assert info.on_full == "reject"
    assert info.direction == "both"
    inst.close()


def test_drop_oldest_reports_what_it_did():
    inst = Instance.from_source(
        "queue.declare('ring', {capacity = 1, on_full = 'drop_oldest', "
        "exported = true}) return 0",
        name="ring",
    )
    inst.run()
    q = inst.queue("ring")
    inst.push(q, "first")
    assert inst.push(q, "second") == Accepted.DROPPED_OLDEST
    assert inst.pop(q) == "second"
    assert inst.queue_info(q).on_full == "drop_oldest"
    inst.close()


def test_pop_distinguishes_empty_from_a_nil_message():
    # `pop` returns None for both, which is a real ambiguity in a language where
    # None is a value. `pop_raw` is how you tell them apart, and saying so in a
    # test is better than saying so only in a docstring.
    inst = Instance.from_source(
        "local q = queue.declare('n', {exported = true}) queue.push(q, nil)",
        name="nil",
    )
    inst.run()
    q = inst.queue("n")
    assert inst.queue_info(q).len == 1, "a nil is a message"
    assert inst.pop_raw(q) == b"\xc0", "and it is msgpack nil on the wire"
    assert inst.pop_raw(q) is None, "whereas an empty queue is no bytes at all"
    inst.close()


def test_raw_bytes_skip_the_codec_in_both_directions():
    inst = Instance.from_source(
        "local q = queue.declare('raw', {exported = true}) queue.push(q, 7)",
        name="raw",
    )
    inst.run()
    q = inst.queue("raw")
    assert inst.pop_raw(q) == b"\x07", "7 is a msgpack positive fixint"
    inst.push_raw(q, b"\x08")
    assert inst.pop(q) == 8
    inst.close()


def test_a_guest_queue_is_invisible_until_the_program_declares_it():
    inst = Instance.from_source(
        "queue.declare('later', {exported = true}) return 0", name="later"
    )
    assert inst.queue("later") is None, "queues are guest-declared"
    inst.run()
    assert inst.queue("later") is not None
    assert inst.queue("never") is None
    inst.close()


def test_the_host_owns_the_clock():
    import time

    inst = Instance.from_source(
        """
        local q = queue.declare('t')
        local _, _, status = queue.wait({q}, 5000)
        local out = queue.declare('res', {exported = true})
        queue.push(out, status)
        """,
        name="clock",
    )
    step = inst.run()
    assert step.parked
    assert step.wait.timeout == pytest.approx(5.0)
    started = time.monotonic()
    assert inst.resume_timeout().done
    assert time.monotonic() - started < 1.0, (
        "answering a timeout must not mean waiting for it"
    )
    assert inst.pop(inst.queue("res")) == "timeout"
    inst.close()


def test_a_park_for_space_asks_to_be_drained():
    inst = Instance.from_source(
        """
        local q = queue.declare('b', {capacity = 1, on_full = 'block',
                                      exported = true})
        queue.push(q, 1)
        queue.push(q, 2)
        """,
        name="backpressure",
    )
    step = inst.run()
    assert step.parked
    assert step.wait.for_write, "the host must tell 'drain me' from 'feed me'"
    q = step.wait.ids[0]
    assert inst.pop(q) == 1
    assert inst.resume(q).done
    inst.close()


def test_running_a_parked_program_is_refused():
    inst = Instance.from_source(
        "local q = queue.lookup('inbox') queue.wait({q})", name="parked"
    )
    assert inst.run().parked
    with pytest.raises(Busy):
        inst.run()
    inst.close()


def test_an_unknown_handle_is_reported():
    inst = Instance.from_source("return 1", name="x")
    with pytest.raises(UnknownQueue):
        inst.push(9999, 1)
    with pytest.raises(UnknownQueue):
        inst.queue_info(9999)
    inst.close()


def test_export_notifications_reach_the_host():
    seen = []
    inst = Instance.from_source(
        """
        local pub = queue.declare('pub', {exported = true})
        local priv = queue.declare('priv')
        queue.push(priv, 1)
        queue.push(pub, 2)
        """,
        name="notify",
    )
    inst.on_export(seen.append)
    inst.run()
    assert len(seen) == 1, "only exported queues are the host's business"
    assert seen[0] == inst.queue("pub")
    inst.close()


def test_using_an_instance_from_another_thread_is_reported():
    # One instance, one thread. The GIL does not make this safe, because a call
    # can release it. Reporting beats corrupting and finding out elsewhere.
    inst = Instance.from_source("return 1", name="threaded")
    box = {}

    def other():
        try:
            inst.run()
        except Exception as e:  # noqa: BLE001 - the type is what we assert
            box["err"] = e

    t = threading.Thread(target=other)
    t.start()
    t.join()
    assert isinstance(box.get("err"), diluvium.DiluviumError)
    assert "one thread" in str(box["err"])
    inst.close()


def test_a_closed_instance_refuses_rather_than_crashing():
    inst = Instance.from_source("return 1", name="closed")
    inst.close()
    inst.close()  # idempotent
    with pytest.raises(diluvium.DiluviumError):
        inst.run()
