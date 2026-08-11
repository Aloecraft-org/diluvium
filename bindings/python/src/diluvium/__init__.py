"""Embed a Diluvium program.

A Diluvium program is an isolated instance with its own heap and its own bounded
message queues. It runs until it needs something, then parks and tells you what
it is waiting for; you decide what happens next. There is no scheduler and no
clock inside the runtime, which is what lets the same program run under asyncio,
a thread, or the plain loop below.

    from diluvium import Instance

    inst = Instance.from_source('''
        local inbox = queue.lookup("inbox")
        local outbox = queue.lookup("outbox")
        while true do
            local _, msg = queue.wait({inbox})
            if msg == nil then return end
            queue.push(outbox, msg:upper())
        end
    ''', name="echo")

    inbox, outbox = inst.queue("inbox"), inst.queue("outbox")
    step = inst.run()
    while step.parked:
        inst.push(inbox, "hello")
        step = inst.resume(inbox)
        print(inst.pop(outbox))

Threading
---------
One instance, one thread. Nothing here is protected by a lock, and adding one
would hide the contract rather than honour it: two threads inside one instance is
undefined, not slow. Move an instance between threads freely; do not share it.
Python's GIL does not help, because a call can release it.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Any, Callable, Optional, Sequence

import msgpack

from ._dv import ffi, lib

__all__ = [
    "Instance",
    "Step",
    "Wait",
    "Accepted",
    "QueueInfo",
    "DiluviumError",
    "AbiMismatch",
    "ProgramError",
    "UnknownQueue",
    "Busy",
    "abi_version",
    "library_abi_version",
]

ABI_VERSION = 1

_DV_OK = 0
_DV_QUEUE_FULL = 1
_DV_QUEUE_DISABLED = 2
_DV_QUEUE_UNKNOWN = 3
_DV_QUEUE_EMPTY = 4
_DV_IDLE = 6
_DV_DONE = 7
_DV_ERROR = 8
_DV_BUSY = 11
_DV_BUFFER_TOO_SMALL = 12
_DV_QUEUE_DROPPED = 13


class DiluviumError(Exception):
    """Base class for everything this module raises."""


class AbiMismatch(DiluviumError):
    """The library and this wrapper disagree about the ABI.

    Refusing to start is the point: a wrapper that guessed could misread a
    message instead of failing.
    """


class ProgramError(DiluviumError):
    """The program did not compile, or raised. Carries the traceback as text."""


class UnknownQueue(DiluviumError):
    """A handle this instance never issued, or has destroyed."""


class Busy(DiluviumError):
    """Called in the wrong order: running a parked program, or resuming one that
    is not parked."""


class Accepted:
    """What happened to a push.

    A full or disabled queue is an ordinary answer, not a failure: the runtime's
    one delivery guarantee is that a push tells you whether the message was
    accepted.
    """

    YES = "ok"
    DROPPED_OLDEST = "dropped_oldest"
    FULL = "full"
    DISABLED = "disabled"

    _ACCEPTING = frozenset({YES, DROPPED_OLDEST})

    @classmethod
    def is_accepted(cls, value: str) -> bool:
        """Whether the message was queued at all."""
        return value in cls._ACCEPTING


@dataclass(frozen=True)
class QueueInfo:
    """How a queue is configured, and how full it is."""

    capacity: int
    len: int
    enabled: bool
    exported: bool
    direction: str
    on_full: str


@dataclass(frozen=True)
class Wait:
    """What a parked program is waiting for."""

    ids: Sequence[int]
    #: How long the program asked to wait, in seconds, or None for no limit.
    #: You own the clock: honour it, ignore it, or answer at once.
    timeout: Optional[float]
    #: True when it wants *space* in a full queue rather than a message. Drain
    #: the queue rather than pushing to it.
    for_write: bool


@dataclass(frozen=True)
class Step:
    """Where a program got to."""

    #: What it is waiting for, or None when it finished.
    wait: Optional[Wait] = None

    @property
    def parked(self) -> bool:
        return self.wait is not None

    @property
    def done(self) -> bool:
        return self.wait is None


_DIRECTIONS = ("both", "guest_write", "guest_read")
_ON_FULL = ("reject", "drop_oldest", "drop_newest", "block")


def abi_version() -> int:
    """The ABI version this wrapper was built against."""
    return ABI_VERSION


def library_abi_version() -> int:
    """The ABI version the linked library reports."""
    return int(lib.dv_abi_version())


class Instance:
    """One Diluvium program, with its own heap, queues and fate."""

    __slots__ = ("_raw", "_owner", "_notify_handle", "_notify_cb")

    def __init__(self, *, _raw: Any) -> None:
        self._raw = _raw
        # Recorded so a misuse from another thread can be reported rather than
        # corrupting the instance silently. This is a diagnostic, not a lock.
        self._owner = threading.get_ident()
        self._notify_handle: Any = None
        self._notify_cb: Any = None

    # -- construction ------------------------------------------------------

    @classmethod
    def from_source(
        cls,
        source: str,
        name: str = "=(program)",
        *,
        sealed: bool = False,
        unsafe_debug: bool = False,
    ) -> "Instance":
        """Compile Lua source into a fresh instance.

        `name` appears in error messages and tracebacks; give it something a
        human can act on.

        `sealed` leaves `io`, `os` and `package` out of the instance, along with
        `dofile` and `loadfile`. **Set it whenever the program is not one you
        wrote**: without it a program can call `os.execute`. What is left is the
        language, the queues, coroutines and the codec, so a program that needs
        a clock or a file should be handed it through a queue.

        `unsafe_debug` opens the whole `debug` library rather than the narrowed
        one, which restores the endpoint-forgery route and lets the program
        switch off its own instruction budget. Only for programs you wrote.

        Note that `sealed` defaults to False, which is not a sandbox. Whether
        that default should invert is an open decision; see section 18.3 of
        `doc/Messaging.md`.
        """
        return cls._load(
            source.encode("utf-8"),
            name,
            text_only=True,
            sealed=sealed,
            unsafe_debug=unsafe_debug,
        )

    @classmethod
    def from_bytecode(
        cls,
        code: bytes,
        name: str = "=(program)",
        *,
        sealed: bool = False,
        unsafe_debug: bool = False,
    ) -> "Instance":
        """Load a program that may be precompiled bytecode.

        Prefer :meth:`from_source` for anything you did not compile yourself.
        The loader refuses malformed bytecode rather than crashing, but that is a
        smaller claim than "bytecode is safe".
        """
        return cls._load(
            bytes(code),
            name,
            text_only=False,
            sealed=sealed,
            unsafe_debug=unsafe_debug,
        )

    # dv.h's flags. Kept together so the mapping is one place.
    _FLAG_TEXT_ONLY = 0x1
    _FLAG_UNSAFE_DEBUG = 0x2
    _FLAG_SEALED = 0x4

    @classmethod
    def _load(
        cls,
        code: bytes,
        name: str,
        *,
        text_only: bool,
        sealed: bool = False,
        unsafe_debug: bool = False,
    ) -> "Instance":
        library = library_abi_version()
        if library != ABI_VERSION:
            raise AbiMismatch(
                f"the library is ABI version {library}, this wrapper expects "
                f"{ABI_VERSION}"
            )
        cfg = ffi.new("dv_config *")
        cfg.abi_version = ABI_VERSION
        cfg.flags = (
            (cls._FLAG_TEXT_ONLY if text_only else 0)
            | (cls._FLAG_SEALED if sealed else 0)
            | (cls._FLAG_UNSAFE_DEBUG if unsafe_debug else 0)
        )
        raw = lib.dv_new(cfg)
        if raw == ffi.NULL:
            raise DiluviumError("could not create a Diluvium instance")
        inst = cls(_raw=raw)
        st = lib.dv_load(raw, code, len(code), name.encode("utf-8"))
        if st != _DV_OK:
            raise ProgramError(inst._last_error())
        return inst

    def close(self) -> None:
        """Free the instance. Idempotent; also called on garbage collection."""
        if self._raw is not None and self._raw != ffi.NULL:
            lib.dv_free(self._raw)
            self._raw = None

    def __del__(self) -> None:  # pragma: no cover - timing dependent
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Instance":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __repr__(self) -> str:
        state = "closed" if self._raw in (None, ffi.NULL) else "open"
        return f"<diluvium.Instance {state}>"

    # -- internals ---------------------------------------------------------

    def _check(self) -> Any:
        if self._raw in (None, ffi.NULL):
            raise DiluviumError("this instance has been closed")
        if threading.get_ident() != self._owner:
            # One instance, one thread. Reporting it beats corrupting the
            # instance and finding out somewhere else.
            raise DiluviumError(
                "this instance belongs to another thread; one instance, one "
                "thread is the ABI's contract, not a performance hint"
            )
        return self._raw

    def _last_error(self) -> str:
        p = lib.dv_last_error(self._raw)
        if p == ffi.NULL:
            return "(no message)"
        return ffi.string(p).decode("utf-8", "replace")

    # -- queues ------------------------------------------------------------

    def queue(self, name: str) -> Optional[int]:
        """Find a queue the program declared, or one of the ``inbox``/``outbox``
        pair every instance starts with.

        Queues belong to the program, not to you: this finds one and never
        creates one. ``None`` is also how you learn that the program you loaded
        is not the program you expected.
        """
        raw = self._check()
        qid = lib.dv_queue_lookup(raw, name.encode("utf-8"))
        return int(qid) if qid != 0 else None

    def queue_info(self, qid: int) -> QueueInfo:
        """Capacity, current length, and how the queue is configured."""
        raw = self._check()
        out = ffi.new("dv_queue_info *")
        if lib.dv_queue_state(raw, qid, out) != _DV_OK:
            raise UnknownQueue(f"no such queue: {qid}")
        return QueueInfo(
            capacity=int(out.capacity),
            len=int(out.len),
            enabled=bool(out.enabled),
            exported=bool(out.exported),
            direction=_DIRECTIONS[int(out.direction) % len(_DIRECTIONS)],
            on_full=_ON_FULL[int(out.on_full) % len(_ON_FULL)],
        )

    def push(self, qid: int, value: Any) -> str:
        """Send a message, encoding it to msgpack.

        Returns one of the :class:`Accepted` strings. Accepting never waits, so
        a program's ``on_full = "block"`` reads as ``full`` from here; watch
        :attr:`QueueInfo.len` against :attr:`QueueInfo.capacity` for
        backpressure.
        """
        return self.push_raw(qid, msgpack.packb(value, use_bin_type=True))

    def push_raw(self, qid: int, payload: bytes) -> str:
        """Send msgpack bytes you already have, with no re-encoding."""
        raw = self._check()
        st = lib.dv_queue_push(raw, qid, payload, len(payload))
        if st == _DV_OK:
            return Accepted.YES
        if st == _DV_QUEUE_DROPPED:
            return Accepted.DROPPED_OLDEST
        if st == _DV_QUEUE_FULL:
            return Accepted.FULL
        if st == _DV_QUEUE_DISABLED:
            return Accepted.DISABLED
        raise UnknownQueue(f"no such queue: {qid}")

    def pop(self, qid: int) -> Any:
        """Take the oldest message and decode it. ``None`` when empty.

        A queue can legitimately hold a nil, which decodes to ``None`` too --
        use :meth:`pop_raw` if you need to tell those apart.
        """
        payload = self.pop_raw(qid)
        if payload is None:
            return None
        return msgpack.unpackb(payload, raw=False, strict_map_key=False)

    def pop_raw(self, qid: int) -> Optional[bytes]:
        """Take the oldest message as raw msgpack. ``None`` when empty.

        Uses the borrow-and-release path, so the only copy is into the returned
        bytes object.
        """
        raw = self._check()
        ptr = ffi.new("const uint8_t **")
        out_len = ffi.new("size_t *")
        st = lib.dv_queue_peek(raw, qid, ptr, out_len)
        if st == _DV_QUEUE_EMPTY:
            return None
        if st != _DV_OK:
            raise UnknownQueue(f"no such queue: {qid}")
        payload = bytes(ffi.buffer(ptr[0], out_len[0]))
        lib.dv_queue_release(raw, qid)
        return payload

    # -- scheduling --------------------------------------------------------

    def run(self) -> Step:
        """Start the program, or continue it after a completed step."""
        raw = self._check()
        ws = ffi.new("dv_waitset *")
        return self._settle(lib.dv_run(raw, ws), ws)

    def resume(self, fired: int) -> Step:
        """Answer a park: say which queue fired.

        What a fired handle means -- a message, or a queue that has gone away --
        is worked out from the queue, so you never have to model the difference.
        """
        raw = self._check()
        st = lib.dv_resume(raw, fired)
        return self._settle_after_resume(st)

    def resume_timeout(self) -> Step:
        """Answer a park by saying its timeout elapsed."""
        raw = self._check()
        st = lib.dv_resume(raw, 0)
        return self._settle_after_resume(st)

    def _settle_after_resume(self, st: int) -> Step:
        # dv_resume has nowhere to put a wait-set, and a program looping on
        # queue.wait parks again immediately, so read the current park back
        # rather than reporting an empty one.
        ws = ffi.new("dv_waitset *")
        if st == _DV_IDLE:
            lib.dv_waitset_get(self._raw, ws)
        return self._settle(st, ws)

    def _settle(self, st: int, ws: Any) -> Step:
        if st == _DV_DONE:
            return Step(wait=None)
        if st == _DV_IDLE:
            n = int(ws.n)
            return Step(
                wait=Wait(
                    ids=tuple(int(ws.ids[i]) for i in range(n)),
                    timeout=(float(ws.timeout_ms) / 1000.0
                             if ws.timeout_ms >= 0 else None),
                    for_write=bool(ws.for_write),
                )
            )
        if st == _DV_BUSY:
            raise Busy(self._last_error())
        raise ProgramError(self._last_error())

    # -- notification ------------------------------------------------------

    def on_export(self, callback: Callable[[int], None]) -> None:
        """Call ``callback(queue_id)`` when the program pushes to a queue it
        exported, so you need not poll.

        The callback fires synchronously inside :meth:`run` or :meth:`resume`
        and **must not touch this instance** -- it is mid-step when your code
        runs. Record the id and act on it after the step returns.
        """
        raw = self._check()

        @ffi.callback("void(void *, dv_queue_id)")
        def trampoline(_ud: Any, qid: int) -> None:
            callback(int(qid))

        # Held on the instance: CFFI callbacks are collected when the last
        # Python reference goes, and the runtime keeps the pointer for the life
        # of the instance. Dropping it here would leave a dangling call.
        self._notify_cb = trampoline
        lib.dv_set_notify(raw, trampoline, ffi.NULL)
