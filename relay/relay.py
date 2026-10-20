#!/usr/bin/env python3
"""AI PASSPORT relay server.

Stdlib-only internet relay for a pair of ESP32-C3 "AI PASSPORT" handhelds.
Speaks the binary "HL" frame protocol over HTTP; see README.md for the
operator summary and the wire layout.

Run:  python3 relay.py --host 0.0.0.0 --port 8787 --data-file relay-state.json
"""

import argparse
import hashlib
import hmac
import json
import logging
import os
import secrets
import signal
import socket
import struct
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVER_VERSION = "AIPASSPORT-Relay/1.0"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8787
DEFAULT_DATA_FILE = "relay-state.json"

# ---------------------------------------------------------------- protocol

MAGIC = b"HL"
VERSION = 1
HEADER_LEN = 14
HMAC_LEN = 16
MAX_PAYLOAD = 512
MAX_EVENTS_PER_SYNC = 12
MAX_HTTP_BODY = 4096
MAX_NAME_BYTES = 64
INBOX_CAP = 64

PAIR_TTL_SEC = 300.0
# Held open on purpose: a sync that finds nothing sleeps here instead of
# returning instantly, so an event reaches the partner in milliseconds.  The
# device-side socket timeout must exceed this plus the round trip, and the
# relay answers Cloudflare's tunnel ~1.9 s late, so 25 s left only ~3 s of a
# 30 s budget -- and a response lost on the way back loses the events for good,
# because _collect_sync_response pops the inbox before the bytes are written.
# 15 s kept the interaction latency identical but left only ~5 s of headroom
# against the device's 20 s socket timeout.  On-device logs from two paired
# boards showed that timeout being hit repeatedly, the retry backoff climbing
# to 18 s, and a hug landing 19.7 s after it was sent.  8 s restores ~12 s of
# headroom and caps one failed retry cycle at roughly 10 s instead of 17 s.
LONG_POLL_SEC = 8.0
PRESENCE_STALE_SEC = 45.0
HEART_WINDOW_SEC = 10.0
PERSIST_DEBOUNCE_SEC = 1.0
# A pair is only real once both devices have proved they hold the pair key (see
# Relay._heal_half_pair).  A freshly created pair is left alone for this long
# before that proof is required, so a pair whose initiator is still waiting on
# its long poll is never torn down underneath it.
PAIR_CONFIRM_GRACE_SEC = 60.0

# The pairing screen on the device is a digit wheel ('0' + digit per column),
# so it cannot enter letters; the relay is the only thing that decides what a
# code may contain.  Digits leave just 10^6 codes, so a pending code is burned
# after MAX_CODE_ATTEMPTS wrong claims (see Relay.handle_claim).
CODE_ALPHABET = "0123456789"
CODE_LEN = 6
MAX_CODE_ATTEMPTS = 8

T_PAIR_BEGIN_REQ = 0x01
T_PAIR_BEGIN_RSP = 0x02
T_PAIR_CLAIM_REQ = 0x03
T_PAIR_CLAIM_RSP = 0x04
T_PAIR_AWAIT_REQ = 0x05
T_PAIR_AWAIT_RSP = 0x06
T_SYNC_REQ = 0x07
T_SYNC_RSP = 0x08
T_UNPAIR_REQ = 0x09
T_UNPAIR_RSP = 0x0A

KNOWN_TYPES = frozenset(
    (
        T_PAIR_BEGIN_REQ,
        T_PAIR_BEGIN_RSP,
        T_PAIR_CLAIM_REQ,
        T_PAIR_CLAIM_RSP,
        T_PAIR_AWAIT_REQ,
        T_PAIR_AWAIT_RSP,
        T_SYNC_REQ,
        T_SYNC_RSP,
        T_UNPAIR_REQ,
        T_UNPAIR_RSP,
    )
)
SIGNED_TYPES = frozenset((T_SYNC_REQ, T_SYNC_RSP, T_UNPAIR_REQ, T_UNPAIR_RSP))

ST_OK = 0
ST_BAD_REQUEST = 1
ST_CODE_NOT_FOUND = 2
ST_CODE_EXPIRED = 3
ST_UNAUTHORIZED = 4
ST_ALREADY_PAIRED = 5
ST_SERVER_BUSY = 6
ST_PAIR_NOT_FOUND = 7

STATUS_NAMES = {
    ST_OK: "OK",
    ST_BAD_REQUEST: "BAD_REQUEST",
    ST_CODE_NOT_FOUND: "CODE_NOT_FOUND",
    ST_CODE_EXPIRED: "CODE_EXPIRED",
    ST_UNAUTHORIZED: "UNAUTHORIZED",
    ST_ALREADY_PAIRED: "ALREADY_PAIRED",
    ST_SERVER_BUSY: "SERVER_BUSY",
    ST_PAIR_NOT_FOUND: "PAIR_NOT_FOUND",
}

EV_MISS = 0x01
EV_HUG_START = 0x02
EV_HUG_END = 0x03
EV_HUG_SYNC = 0x04
EV_HEART_SYNC = 0x05
EV_UNPAIR = 0x06
EV_PAIRED = 0x07
EV_OFFLINE_SUMMARY = 0x08

#: event type -> exact value length
EVENT_LEN = {
    EV_MISS: 0,
    EV_HUG_START: 0,
    EV_HUG_END: 2,
    EV_HUG_SYNC: 0,
    EV_HEART_SYNC: 0,
    EV_UNPAIR: 0,
    EV_PAIRED: 0,
    EV_OFFLINE_SUMMARY: 1,
}

#: event types only the server may generate
SERVER_EVENTS = frozenset((EV_HUG_SYNC, EV_HEART_SYNC, EV_UNPAIR, EV_OFFLINE_SUMMARY))

#: bit 0 of the sync request's local_flags: "holding OK right now"
LOCAL_FLAG_HOLDING = 0x01

PAIR_STATE_NOT_PAIRED = 0
PAIR_STATE_PAIRED = 1
PAIR_STATE_PEER_LEFT = 2

AWAIT_WAITING = 0
AWAIT_PAIRED = 1
AWAIT_EXPIRED = 2

ZERO_KEY = b"\x00" * 32

# ------------------------------------------------------------ love energy

#: The pair's Love Energy ledger.
#:
#: Both handhelds keep a private copy of this ledger so the home screen never
#: has to wait for the network, but only the relay's copy is authoritative:
#: every sync answer carries the pair total and the device overwrites its
#: local value with it.  That is what keeps the two screens showing the same
#: number -- with per-device arithmetic they drift apart for good the first
#: time an event fails to arrive.
#:
#: The rules are copied from the device-side ledger, main/couple_state.c ::
#: couple_stats_apply_interaction(), and the amounts from main/couple_config.h.
#: Keep the two in step.
ENERGY_MISS = 1
ENERGY_HEART_SYNC = 5
ENERGY_HUG_SYNC = 5
ENERGY_DAILY_STREAK = 3

#: A total that lands exactly on one of these pays its own value as a one-off
#: bonus.  The position in this tuple is the bit kept in ``milestones``.
MILESTONE_VALUES = (9, 18, 99, 100, 365, 520, 999, 1314)

#: A HUG_END shorter than this was a mis-touch: worth nothing at all, not even
#: a place in the "today" counters (COUPLE_HUG_TIER_SMALL_MS).
HUG_TIER_SMALL_MS = 700

#: The "today" boundary is the device's local day, not UTC
#: (COUPLE_UTC_OFFSET_MINUTES = 480).
DAY_SECONDS = 86400
UTC_OFFSET_MINUTES = 480


def _day_index(now):
    """Local day number used by the streak bookkeeping."""
    day = (int(now) + UTC_OFFSET_MINUTES * 60) // DAY_SECONDS
    return day if day > 0 else 0


def _new_score(energy=0):
    """A fresh ledger.  ``energy`` seeds a pair migrated from an older relay."""
    return {
        "energy": max(0, int(energy)) & 0xFFFFFFFF,
        "total": 0,
        "milestones": 0,
        "day": 0,
        "day_first": 0,
        "streak": 0,
        "last_day": 0,
    }


def _parse_score(raw):
    """Decode a persisted ledger as forgivingly as possible.

    The caller sits inside ``Relay._load``'s single try block, where any
    exception discards the entire state file -- every pairing and every queued
    event with it.  A malformed ledger is therefore downgraded to a fresh one
    rather than raised.
    """
    if not isinstance(raw, dict):
        return _new_score()
    try:
        return {
            "energy": max(0, int(raw.get("energy", 0))) & 0xFFFFFFFF,
            "total": max(0, int(raw.get("total", 0))),
            "milestones": max(0, int(raw.get("milestones", 0))),
            "day": max(0, int(raw.get("day", 0))),
            "day_first": 1 if int(raw.get("day_first", 0)) else 0,
            "streak": max(0, int(raw.get("streak", 0))),
            "last_day": max(0, int(raw.get("last_day", 0))),
        }
    except (TypeError, ValueError):
        return _new_score()


class RequestError(Exception):
    """A request that must be answered with a status byte instead of a result."""

    def __init__(self, status, detail=""):
        super().__init__(detail or STATUS_NAMES.get(status, str(status)))
        self.status = status
        self.detail = detail


# ---------------------------------------------------------------- framing


def encode_frame(ftype, seq, ack, payload=b"", key=None):
    """Serialize one protocol frame. Signed types require ``key``."""
    if ftype not in KNOWN_TYPES:
        raise ValueError("unknown frame type 0x%02x" % ftype)
    payload = bytes(payload)
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload of %d bytes exceeds %d" % (len(payload), MAX_PAYLOAD))
    frame = struct.pack(
        ">2sBBHII",
        MAGIC,
        VERSION,
        ftype,
        len(payload),
        int(seq) & 0xFFFFFFFF,
        int(ack) & 0xFFFFFFFF,
    ) + payload
    if ftype in SIGNED_TYPES:
        if not key:
            raise ValueError("frame type 0x%02x is signed but no key was given" % ftype)
        frame += hmac.new(bytes(key), frame, hashlib.sha256).digest()[:HMAC_LEN]
    return frame


def split_frame(data):
    """Parse a frame. Returns (ftype, seq, ack, payload, mac); raises RequestError."""
    data = bytes(data)
    if len(data) < HEADER_LEN:
        raise RequestError(ST_BAD_REQUEST, "frame shorter than header")
    if data[0:2] != MAGIC:
        raise RequestError(ST_BAD_REQUEST, "bad magic")
    if data[2] != VERSION:
        raise RequestError(ST_BAD_REQUEST, "unsupported protocol version %d" % data[2])
    ftype = data[3]
    if ftype not in KNOWN_TYPES:
        raise RequestError(ST_BAD_REQUEST, "unknown frame type 0x%02x" % ftype)
    (plen,) = struct.unpack_from(">H", data, 4)
    if plen > MAX_PAYLOAD:
        raise RequestError(ST_BAD_REQUEST, "payload_len %d exceeds %d" % (plen, MAX_PAYLOAD))
    seq, ack = struct.unpack_from(">II", data, 6)
    want = HEADER_LEN + plen + (HMAC_LEN if ftype in SIGNED_TYPES else 0)
    if len(data) != want:
        raise RequestError(ST_BAD_REQUEST, "frame is %d bytes, expected %d" % (len(data), want))
    payload = data[HEADER_LEN : HEADER_LEN + plen]
    mac = data[HEADER_LEN + plen :] if ftype in SIGNED_TYPES else b""
    return ftype, seq, ack, payload, mac


def verify_frame_mac(raw, key):
    """Verify the trailing HMAC of a signed frame; raises RequestError(UNAUTHORIZED)."""
    if not key:
        raise RequestError(ST_UNAUTHORIZED, "signed frame without a key")
    expected = hmac.new(bytes(key), raw[:-HMAC_LEN], hashlib.sha256).digest()[:HMAC_LEN]
    if not hmac.compare_digest(expected, raw[-HMAC_LEN:]):
        raise RequestError(ST_UNAUTHORIZED, "hmac mismatch")


def decode_unsigned_request(raw, expect_type):
    ftype, seq, ack, payload, _mac = split_frame(raw)
    if ftype != expect_type:
        raise RequestError(ST_BAD_REQUEST, "frame type 0x%02x on an unsigned endpoint" % ftype)
    return seq, ack, payload


def decode_signed_request(raw, key, expect_type):
    ftype, seq, ack, payload, _mac = split_frame(raw)
    if ftype not in SIGNED_TYPES or ftype != expect_type:
        raise RequestError(ST_BAD_REQUEST, "frame type 0x%02x on a signed endpoint" % ftype)
    verify_frame_mac(raw, key)
    return seq, ack, payload


# ---------------------------------------------------------------- payloads


def _parse_name(payload, offset):
    """Parse `u8 name_len, name[utf8]` at ``offset``; returns (name, end_offset)."""
    if offset >= len(payload):
        raise RequestError(ST_BAD_REQUEST, "missing name")
    name_len = payload[offset]
    if name_len > MAX_NAME_BYTES:
        raise RequestError(ST_BAD_REQUEST, "name_len %d exceeds %d" % (name_len, MAX_NAME_BYTES))
    if offset + 1 + name_len > len(payload):
        raise RequestError(ST_BAD_REQUEST, "truncated name")
    raw = payload[offset + 1 : offset + 1 + name_len]
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise RequestError(ST_BAD_REQUEST, "name is not valid utf-8")
    return name, offset + 1 + name_len


def parse_begin_request(payload):
    name, end = _parse_name(payload, 0)
    if end != len(payload):
        raise RequestError(ST_BAD_REQUEST, "trailing bytes in pair begin body")
    return name


def parse_claim_request(payload):
    if len(payload) < CODE_LEN:
        raise RequestError(ST_BAD_REQUEST, "missing pair code")
    code_raw = payload[:CODE_LEN]
    try:
        code = code_raw.decode("ascii")
    except UnicodeDecodeError:
        raise RequestError(ST_BAD_REQUEST, "pair code is not ascii")
    name, end = _parse_name(payload, CODE_LEN)
    if end != len(payload):
        raise RequestError(ST_BAD_REQUEST, "trailing bytes in pair claim body")
    return code.upper(), name


def parse_sync_request(payload):
    """Returns (events, local_flags)."""
    if not payload:
        raise RequestError(ST_BAD_REQUEST, "empty sync body")
    count = payload[0]
    if count > MAX_EVENTS_PER_SYNC:
        raise RequestError(ST_BAD_REQUEST, "event count %d exceeds %d" % (count, MAX_EVENTS_PER_SYNC))
    offset = 1
    events = []
    for _ in range(count):
        if offset + 2 > len(payload):
            raise RequestError(ST_BAD_REQUEST, "truncated event header")
        etype = payload[offset]
        elen = payload[offset + 1]
        offset += 2
        if offset + elen > len(payload):
            raise RequestError(ST_BAD_REQUEST, "truncated event value")
        value = payload[offset : offset + elen]
        offset += elen
        if etype not in EVENT_LEN:
            raise RequestError(ST_BAD_REQUEST, "unknown event type 0x%02x" % etype)
        if etype in SERVER_EVENTS:
            raise RequestError(ST_BAD_REQUEST, "event type 0x%02x is server generated" % etype)
        if EVENT_LEN[etype] != elen:
            raise RequestError(ST_BAD_REQUEST, "event type 0x%02x takes %d value bytes" % (etype, EVENT_LEN[etype]))
        events.append((etype, bytes(value)))
    if offset + 1 != len(payload):
        raise RequestError(ST_BAD_REQUEST, "missing local_flags byte")
    return events, payload[offset]


def encode_event(etype, value=b""):
    value = bytes(value)
    return bytes((etype, len(value))) + value


def encode_events(events):
    body = bytearray((len(events),))
    for etype, value in events:
        body += encode_event(etype, value)
    return bytes(body)


def begin_rsp_payload(status, code="", ttl_secs=0, token=b""):
    code_bytes = code.encode("ascii") if code else b""
    code_bytes = (code_bytes + b"------")[:CODE_LEN]
    token = bytes(token)
    token = (token + ZERO_KEY)[:32]
    return bytes((status,)) + code_bytes + struct.pack(">H", int(ttl_secs) & 0xFFFF) + token


def claim_rsp_payload(status, pair_id=b"", key=b"", partner_id=b"", name=""):
    if status != ST_OK:
        return bytes((status,))
    name_bytes = name.encode("utf-8")
    return (
        bytes((status,))
        + bytes(pair_id)
        + bytes(key)
        + bytes(partner_id)
        + bytes((len(name_bytes),))
        + name_bytes
    )


def await_rsp_payload(status, state=AWAIT_WAITING, pair_id=b"", key=b"", partner_id=b"", name=""):
    if status != ST_OK:
        return bytes((status,))
    body = bytearray((status, state))
    if state == AWAIT_PAIRED:
        name_bytes = name.encode("utf-8")
        body += bytes(pair_id) + bytes(key) + bytes(partner_id) + bytes((len(name_bytes),)) + name_bytes
    return bytes(body)


def error_rsp_payload(rsp_type, status, server_seq=0, now=None):
    """Well formed status-only response body for any endpoint."""
    if rsp_type == T_SYNC_RSP:
        ts = int(time.time() if now is None else now) & 0xFFFFFFFF
        # The trailing u32 is the pair's Love Energy.  A status-only answer has
        # no pair to score against, so it reports zero -- but the shape stays
        # identical to a real answer, whatever produced it.
        return (
            bytes((status, 0))
            + struct.pack(">II", int(server_seq) & 0xFFFFFFFF, ts)
            + bytes((0, 0, 0))
            + struct.pack(">I", 0)
        )
    if rsp_type == T_PAIR_BEGIN_RSP:
        return begin_rsp_payload(status)
    return bytes((status,))


# ---------------------------------------------------------------- state


class Relay:
    """All relay state plus the request handlers.

    Every instance attribute around protocol state is guarded by ``self.lock``
    (a re-entrant lock exposed through ``self.cond``).  Handlers return the
    bytes to write; they never touch a socket.
    """

    def __init__(
        self,
        data_file=None,
        logger=None,
        pair_ttl_sec=PAIR_TTL_SEC,
        long_poll_sec=LONG_POLL_SEC,
        presence_stale_sec=PRESENCE_STALE_SEC,
        heart_window_sec=HEART_WINDOW_SEC,
        inbox_cap=INBOX_CAP,
        persist_debounce_sec=PERSIST_DEBOUNCE_SEC,
        pair_confirm_grace_sec=PAIR_CONFIRM_GRACE_SEC,
        autosave=True,
    ):
        self.log = logger if logger is not None else logging.getLogger("relay")
        self.data_file = data_file
        self.pair_ttl_sec = float(pair_ttl_sec)
        self.long_poll_sec = float(long_poll_sec)
        self.presence_stale_sec = float(presence_stale_sec)
        self.heart_window_sec = float(heart_window_sec)
        self.inbox_cap = int(inbox_cap)
        self.persist_debounce_sec = float(persist_debounce_sec)
        self.pair_confirm_grace_sec = float(pair_confirm_grace_sec)
        self.instance = uuid.uuid4().hex

        self.lock = threading.RLock()
        self.cond = threading.Condition(self.lock)

        self.server_seq = 0
        self.devices = {}
        self.pairs = {}
        self.dev_pair = {}
        self.pending = {}
        self.codes = {}

        self._shutdown = False
        self._dirty = False
        self._last_write = 0.0
        self._stop_flusher = threading.Event()
        self._flusher = None

        self._load()
        if autosave and self.data_file:
            if os.path.dirname(os.path.abspath(self.data_file)):
                try:
                    os.makedirs(os.path.dirname(os.path.abspath(self.data_file)), exist_ok=True)
                except OSError as exc:
                    self.log.warning("cannot create data file directory: %s", exc)
            self._flusher = threading.Thread(target=self._flush_loop, name="relay-persist", daemon=True)
            self._flusher.start()

    # ------------------------------------------------------------ helpers

    def _device(self, devhex):
        dev = self.devices.get(devhex)
        if dev is None:
            dev = {"last_seen": 0.0, "last_accepted": 0, "holding": False, "last_miss": None, "inbox": []}
            self.devices[devhex] = dev
        return dev

    def _touch(self, devhex, now):
        self._device(devhex)["last_seen"] = now
        self._dirty = True

    def _live_pair_of(self, devhex):
        ph = self.dev_pair.get(devhex)
        if ph is None:
            return None
        pair = self.pairs.get(ph)
        if pair is None or pair["dissolved"]:
            return None
        return pair

    def _resolve_pair(self, devhex, pairhex):
        pair = self.pairs.get(pairhex)
        if pair is None:
            raise RequestError(ST_PAIR_NOT_FOUND, "unknown pair")
        if devhex not in pair["members"]:
            if pair["dissolved"]:
                raise RequestError(ST_PAIR_NOT_FOUND, "pair was dissolved")
            raise RequestError(ST_UNAUTHORIZED, "device is not a member of this pair")
        return pair

    @staticmethod
    def _partner_of(pair, devhex):
        for member in pair["members"]:
            if member != devhex:
                return member
        return None

    def _wake(self):
        self.cond.notify_all()

    def _new_code(self):
        for _ in range(64):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.codes:
                return code
        raise RequestError(ST_SERVER_BUSY, "no pairing code available")

    def _drop_pending(self, devhex):
        old = self.pending.pop(devhex, None)
        if old is not None:
            if self.codes.get(old["code"]) == devhex:
                self.codes.pop(old["code"], None)
            self._dirty = True

    def _count_wrong_claim(self, now):
        """Charge one wrong pairing code to every live pending session.

        A claim carries only the code, so a guess that matches no pending
        session cannot be attributed to the session it was aimed at; every
        waiting session pays instead.  A counter that reaches
        MAX_CODE_ATTEMPTS burns the code, so the 10^6 code space cannot be
        walked by a remote caller.
        """
        for owner, entry in list(self.pending.items()):
            if entry["state"] != "waiting" or now >= entry["expires"]:
                continue
            entry["attempts"] = entry.get("attempts", 0) + 1
            self._dirty = True
            if entry["attempts"] < MAX_CODE_ATTEMPTS:
                continue
            self.log.warning(
                "pairing code of device %s burned after %d wrong claims",
                owner[:8],
                entry["attempts"],
            )
            self._drop_pending(owner)

    def _drop_dead_pair_of(self, devhex):
        """Forget a dissolved pair once its last member starts a new pairing."""
        ph = self.dev_pair.get(devhex)
        if ph is None:
            return
        pair = self.pairs.get(ph)
        if pair is not None and pair["dissolved"] and devhex in pair["members"]:
            self.pairs.pop(ph, None)
            self._dirty = True
        self.dev_pair.pop(devhex, None)

    def _heal_half_pair(self, devhex, now):
        """Dissolve a pair this device never confirmed; True when one was dropped.

        ``handle_claim`` creates the pair as soon as the claimant presents the
        code, but the initiator learns its identity from a single long-poll
        answer.  If the initiator is cut off in between -- it reboots, crashes,
        or loses the socket right then -- the server keeps a pair whose
        initiator holds no key at all.  That state is a dead end: the initiator's
        next "begin" is answered ALREADY_PAIRED, and it cannot unpair either,
        because it no longer knows the pair id.  Only the partner could free it.

        The one hard proof that a device holds the pair key is a signed request
        it produced itself, and ``handle_sync`` records exactly that in the
        pair's ``seen`` map.  So a pair older than the grace period in which the
        caller has never signed anything never reached the caller, and is
        dissolved through the ordinary unpair path -- which also tells the
        partner, whose device clears itself on the EV_UNPAIR it receives.

        The caller's own evidence is what counts, never the partner's: a device
        that signed recently is paired, whatever the other side is doing, and a
        pair whose partner is merely offline must not be dropped just because
        someone re-opened the pairing screen.
        """
        pair = self._live_pair_of(devhex)
        if pair is None:
            return False
        age = now - pair.get("created", now)
        if age < self.pair_confirm_grace_sec:
            return False
        if devhex in (pair.get("seen") or {}):
            return False
        self.log.warning(
            "pair %s never reached device %s (age %.0fs); dissolving it so the device can pair again",
            pair["id"].hex(),
            devhex[:8],
            age,
        )
        self._dissolve(pair, devhex, now)
        self._dirty = True
        self._wake()
        return True

    def _reset_for_new_pair(self, devhex):
        """Start a fresh pairing with a fresh sequence watermark.

        ``couple_store_clear_pair()`` zeroes the device's sequence counter, so
        a device enters a new pair at seq 1 (plus a small boot margin).  The
        dedup watermark ``last_accepted`` is therefore only meaningful inside
        one pairing: carried across a re-pair it sits far above the device's
        new sequences, and ``handle_sync`` judges every batch a retransmission
        -- silently dropping the events while still answering OK, so the sender
        prunes its outbox and the partner never receives anything.

        Only the watermark and the per-pair transient state are reset;
        ``last_seen`` is presence, not sequence, and the device is talking to
        us right now.
        """
        dev = self._device(devhex)
        dev["last_accepted"] = 0
        dev["holding"] = False
        dev["last_miss"] = None
        dev["inbox"] = []
        self._dirty = True

    def _push(self, devhex, etype, value=b"", now=None, offline=None):
        """Append a server-visible event to a device inbox. Caller holds the lock."""
        now = time.time() if now is None else now
        dev = self._device(devhex)
        if offline is None:
            offline = (now - dev["last_seen"]) > self.presence_stale_sec
        self.server_seq = (self.server_seq + 1) & 0xFFFFFFFF
        dev["inbox"].append({"t": int(etype), "v": bytes(value), "off": bool(offline), "seq": self.server_seq})
        while len(dev["inbox"]) > self.inbox_cap:
            dev["inbox"].pop(0)
            self.log.warning("inbox of device %s overflowed, dropped oldest event", devhex[:8])
        self._dirty = True
        self._wake()

    def _broadcast(self, pair, etype, value, now):
        for member in list(pair["members"]):
            self._push(member, etype, value, now)

    # ------------------------------------------------------------ pairing

    def handle_begin(self, devhex, raw, now=None):
        now = time.time() if now is None else now
        with self.cond:
            self._touch(devhex, now)
            if self._live_pair_of(devhex) is not None and not self._heal_half_pair(devhex, now):
                return ST_ALREADY_PAIRED, begin_rsp_payload(ST_ALREADY_PAIRED), None, 0, 0
            _seq, _ack, payload = decode_unsigned_request(raw, T_PAIR_BEGIN_REQ)
            name = parse_begin_request(payload)
            code = self._new_code()
            token = secrets.token_bytes(32)
            self._drop_pending(devhex)
            self.pending[devhex] = {
                "code": code,
                "token": token,
                "name": name,
                "created": now,
                "expires": now + self.pair_ttl_sec,
                "state": "waiting",
                "attempts": 0,
                "identity": None,
            }
            self.codes[code] = devhex
            self._dirty = True
            self._wake()
            ttl = int(self.pair_ttl_sec)
            return ST_OK, begin_rsp_payload(ST_OK, code, ttl, token), None, 0, 0

    def handle_claim(self, devhex, raw, now=None):
        now = time.time() if now is None else now
        with self.cond:
            self._touch(devhex, now)
            if self._live_pair_of(devhex) is not None and not self._heal_half_pair(devhex, now):
                return ST_ALREADY_PAIRED, claim_rsp_payload(ST_ALREADY_PAIRED), None, 0, 0
            _seq, _ack, payload = decode_unsigned_request(raw, T_PAIR_CLAIM_REQ)
            code, name = parse_claim_request(payload)

            initiator = self.codes.get(code)
            pending = self.pending.get(initiator) if initiator else None
            if initiator is None or pending is None or pending["code"] != code:
                # Only a code that matches nothing is charged here: expired,
                # used and malformed claims are decided elsewhere and must not
                # be able to burn someone else's code.
                self._count_wrong_claim(now)
                return ST_CODE_NOT_FOUND, claim_rsp_payload(ST_CODE_NOT_FOUND), None, 0, 0
            if now >= pending["expires"]:
                return ST_CODE_EXPIRED, claim_rsp_payload(ST_CODE_EXPIRED), None, 0, 0
            if initiator == devhex:
                return ST_BAD_REQUEST, claim_rsp_payload(ST_BAD_REQUEST), None, 0, 0
            if pending["state"] != "waiting":
                return ST_ALREADY_PAIRED, claim_rsp_payload(ST_ALREADY_PAIRED), None, 0, 0
            if self._live_pair_of(initiator) is not None:
                return ST_ALREADY_PAIRED, claim_rsp_payload(ST_ALREADY_PAIRED), None, 0, 0

            pair_id = secrets.token_bytes(8)
            key = secrets.token_bytes(32)
            pairhex = pair_id.hex()
            self._drop_dead_pair_of(initiator)
            self._drop_dead_pair_of(devhex)
            self.pairs[pairhex] = {
                "id": pair_id,
                "key": key,
                "members": [initiator, devhex],
                "names": {initiator: pending["name"], devhex: name},
                "state": {initiator: PAIR_STATE_PAIRED, devhex: PAIR_STATE_PAIRED},
                "dissolved": False,
                "hug_latch": False,
                "created": now,
                "score": _new_score(),
            }
            self.dev_pair[initiator] = pairhex
            self.dev_pair[devhex] = pairhex
            self._reset_for_new_pair(initiator)
            self._reset_for_new_pair(devhex)
            pending["state"] = "paired"
            # From the awaiting device's point of view the claimant is its
            # partner, so the identity blob carries the claimant's id and name
            # (mirroring PAIR_CLAIM, which carries the awaiting device's).
            pending["identity"] = {
                "pair_id": pair_id,
                "key": key,
                "partner_id": devhex,
                "name": name,
            }
            self._dirty = True
            self._wake()
            return (
                ST_OK,
                claim_rsp_payload(ST_OK, pair_id, key, bytes.fromhex(initiator), pending["name"]),
                None,
                0,
                0,
            )

    def handle_await(self, devhex, token, now=None):
        with self.cond:
            pending = self.pending.get(devhex)
            if pending is None or not hmac.compare_digest(pending["token"], token):
                return ST_UNAUTHORIZED, await_rsp_payload(ST_UNAUTHORIZED), None, 0, 0
            deadline = time.time() + self.long_poll_sec
            while True:
                if self.pending.get(devhex) is not pending:
                    return ST_UNAUTHORIZED, await_rsp_payload(ST_UNAUTHORIZED), None, 0, 0
                if pending["state"] == "paired" and pending["identity"] is not None:
                    ident = pending["identity"]
                    payload = await_rsp_payload(
                        ST_OK,
                        AWAIT_PAIRED,
                        ident["pair_id"],
                        ident["key"],
                        bytes.fromhex(ident["partner_id"]),
                        ident["name"],
                    )
                    self._touch(devhex, time.time())
                    return ST_OK, payload, None, 0, 0
                now = time.time()
                if now >= pending["expires"]:
                    self._touch(devhex, now)
                    return ST_OK, await_rsp_payload(ST_OK, AWAIT_EXPIRED), None, 0, 0
                if self._shutdown:
                    return ST_OK, await_rsp_payload(ST_OK, AWAIT_WAITING), None, 0, 0
                remaining = deadline - now
                if remaining <= 0:
                    self._touch(devhex, now)
                    return ST_OK, await_rsp_payload(ST_OK, AWAIT_WAITING), None, 0, 0
                self.cond.wait(timeout=min(remaining, 0.25))

    # ------------------------------------------------------------ sync

    def handle_sync(self, devhex, pairhex, raw, now=None):
        now = time.time() if now is None else now
        with self.cond:
            pair = self._resolve_pair(devhex, pairhex)
            seq, _ack, payload = decode_signed_request(raw, pair["key"], T_SYNC_REQ)
            if seq == 0:
                raise RequestError(ST_BAD_REQUEST, "seq must start at 1")
            # The MAC above verified, so this device really does hold the pair
            # key: that is the evidence _heal_half_pair reads.  A retransmission
            # proves it just as well as a fresh sequence number.
            pair.setdefault("seen", {})[devhex] = now
            self._dirty = True
            events, local_flags = parse_sync_request(payload)

            dev = self._device(devhex)
            self._touch(devhex, now)
            retransmission = seq <= dev["last_accepted"]
            if not retransmission:
                dev["last_accepted"] = seq
                self._ingest(devhex, pair, events, local_flags, now)

            deadline = now + self.long_poll_sec
            while not dev["inbox"] and not self._shutdown:
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                self.cond.wait(timeout=min(remaining, 0.25))

            payload_out, delivered = self._collect_sync_response(devhex, pair, time.time())
            self._dirty = True
            self._wake()
            return ST_OK, payload_out, pair["key"], dev["last_accepted"], len(delivered)

    def _ingest(self, devhex, pair, events, local_flags, now):
        partner = self._partner_of(pair, devhex)
        dev = self._device(devhex)
        was_holding = dev["holding"]
        batch_hug_start = False

        for etype, value in events:
            heart_sync = False
            if etype == EV_HUG_START:
                dev["holding"] = True
                batch_hug_start = True
            elif etype == EV_HUG_END:
                dev["holding"] = False
            elif etype == EV_MISS:
                dev["last_miss"] = now
                if partner is not None:
                    peer = self._device(partner)
                    if peer["last_miss"] is not None and abs(now - peer["last_miss"]) <= self.heart_window_sec:
                        peer["last_miss"] = None
                        dev["last_miss"] = None
                        self._broadcast(pair, EV_HEART_SYNC, b"", now)
                        heart_sync = True
            # Scoring mirrors the device-side ledger, and the order matters: a
            # MISS that closes a heart sync scores first as a MISS and then as
            # the HEART_SYNC bonus, exactly as the two callbacks fire on the
            # handhelds.  Only the events that actually move the ledger are
            # passed in; _score_event sorts out the rest.
            if etype in (EV_MISS, EV_HUG_END):
                self._score_event(pair, etype, value, now)
            if heart_sync:
                self._score_event(pair, EV_HEART_SYNC, b"", now)
            if partner is not None:
                self._push(partner, etype, value, now)

        # local_flags bit 0 is the live "holding right now" level and wins over
        # the event-derived state, except that a HUG_START in this same batch
        # always means holding.
        if local_flags & 0x01:
            dev["holding"] = True
        elif not batch_hug_start:
            dev["holding"] = False

        if partner is not None:
            peer = self._device(partner)
            if dev["holding"] and not was_holding and peer["holding"] and not pair["hug_latch"]:
                pair["hug_latch"] = True
                self._broadcast(pair, EV_HUG_SYNC, b"", now)
                self._score_event(pair, EV_HUG_SYNC, b"", now)
            elif not dev["holding"] or not peer["holding"]:
                pair["hug_latch"] = False

    def _score_event(self, pair, etype, value, now):
        """Apply one scored event to the pair's Love Energy ledger.

        Returns the gain.  Every rule here is copied from the device-side
        ledger (main/couple_state.c) so that the number the relay hands back
        is the number the two screens should agree on.
        """
        if etype not in (EV_MISS, EV_HUG_END, EV_HEART_SYNC, EV_HUG_SYNC):
            return 0
        score = pair.setdefault("score", _new_score())

        if etype == EV_HUG_END:
            # A hug that was never really held is not an interaction at all:
            # no energy, no counter, not even a "first interaction today".
            held_ms = struct.unpack(">H", value)[0] if len(value) == 2 else 0
            if held_ms < HUG_TIER_SMALL_MS:
                return 0

        today = _day_index(now)
        if today != score["day"]:
            score["day"] = today
            score["day_first"] = 0

        gain = 0
        # The "first interaction of the day" settlement runs before the
        # per-type amounts, and it is shared by every scored event -- a heart
        # sync counts as the day's first interaction just like a miss does.
        if score["day_first"] == 0:
            score["day_first"] = 1
            if score["streak"] > 0 and score["last_day"] + 1 == today:
                score["streak"] += 1
                gain += ENERGY_DAILY_STREAK
            elif score["streak"] > 0 and score["last_day"] == today:
                pass
            else:
                # A missed day neither punishes nor resets: the streak just
                # restarts.  Nothing here should ever feel like a reprimand.
                score["streak"] = 1
        score["last_day"] = today

        if etype == EV_MISS:
            gain += ENERGY_MISS
            score["total"] += 1
        elif etype == EV_HUG_END:
            score["total"] += 1
        elif etype == EV_HEART_SYNC:
            gain += ENERGY_HEART_SYNC
        elif etype == EV_HUG_SYNC:
            gain += ENERGY_HUG_SYNC

        # Milestones pay their own value once, on landing exactly on them.
        for index, threshold in enumerate(MILESTONE_VALUES):
            bit = 1 << index
            if score["total"] == threshold and not score["milestones"] & bit:
                score["milestones"] |= bit
                gain += threshold
                break

        score["energy"] = (score["energy"] + gain) & 0xFFFFFFFF
        self._dirty = True
        return gain

    def _collect_sync_response(self, devhex, pair, now):
        dev = self._device(devhex)
        inbox = dev["inbox"]
        out = []
        if any(ev["off"] for ev in inbox):
            misses = sum(1 for ev in inbox if ev["off"] and ev["t"] == EV_MISS)
            for ev in inbox:
                ev["off"] = False
            out.append((EV_OFFLINE_SUMMARY, bytes((min(misses, 255),))))
        while inbox and len(out) < MAX_EVENTS_PER_SYNC:
            ev = inbox.pop(0)
            out.append((ev["t"], ev["v"]))

        body = bytearray()
        body.append(ST_OK)
        body.append(len(out))
        for etype, value in out:
            body += encode_event(etype, value)
        body += struct.pack(">II", self.server_seq & 0xFFFFFFFF, int(now) & 0xFFFFFFFF)

        partner = self._partner_of(pair, devhex)
        if partner is None:
            online, holding = 0, 0
        else:
            peer = self.devices.get(partner)
            online = 1 if (peer is not None and (now - peer["last_seen"]) <= self.presence_stale_sec) else 0
            holding = 1 if (peer is not None and peer["holding"]) else 0
        body.append(online)
        body.append(holding)
        body.append(int(pair["state"].get(devhex, PAIR_STATE_PAIRED)) & 0xFF)
        # Trailing u32: the pair's Love Energy, authoritative here.  The device
        # overwrites whatever it counted locally with this value, which is what
        # keeps both screens on the same number.
        score = pair.setdefault("score", _new_score())
        body += struct.pack(">I", int(score["energy"]) & 0xFFFFFFFF)
        return bytes(body), out

    # ------------------------------------------------------------ unpair

    def handle_unpair(self, devhex, pairhex, raw, now=None):
        now = time.time() if now is None else now
        with self.cond:
            pair = self._resolve_pair(devhex, pairhex)
            decode_signed_request(raw, pair["key"], T_UNPAIR_REQ)
            key = pair["key"]
            self._touch(devhex, now)
            self._dissolve(pair, devhex, now)
            self._dirty = True
            self._wake()
            return ST_OK, bytes((ST_OK,)), key, 0, 0

    def _dissolve(self, pair, devhex, now):
        partner = self._partner_of(pair, devhex)
        dev = self._device(devhex)
        dev["holding"] = False
        dev["last_miss"] = None
        dev["inbox"] = []
        pair["members"] = [m for m in pair["members"] if m != devhex]
        pair["hug_latch"] = False
        self.dev_pair.pop(devhex, None)
        if partner is None:
            self.pairs.pop(pair["id"].hex(), None)
            return
        pair["dissolved"] = True
        peer = self._device(partner)
        peer["holding"] = False
        pair["state"][partner] = PAIR_STATE_PEER_LEFT
        self._push(partner, EV_UNPAIR, b"", now)

    # ------------------------------------------------------------ shutdown

    def shutdown(self):
        with self.cond:
            self._shutdown = True
            self._wake()
        self._stop_flusher.set()
        if self._flusher is not None:
            self._flusher.join(timeout=5.0)
        self.save()

    # ------------------------------------------------------------ logging

    def log_request(self, path, devhex, status, events):
        self.log.info(
            "%s dev=%s status=%s events=%d",
            path,
            (devhex[:8] if devhex else "-"),
            STATUS_NAMES.get(status, status),
            events,
        )

    # ------------------------------------------------------------ persistence

    def _flush_loop(self):
        while not self._stop_flusher.wait(0.25):
            try:
                self._maybe_persist()
            except Exception:  # never let the flusher die
                self.log.exception("state flush failed")

    def _maybe_persist(self):
        with self.lock:
            if not self._dirty:
                return
            now = time.time()
            if now - self._last_write < self.persist_debounce_sec:
                return
            snapshot = self._snapshot_locked(now)
            self._dirty = False
            self._last_write = now
        self._write_snapshot(snapshot)

    def save(self):
        """Force a state file write (used on shutdown)."""
        if not self.data_file:
            return
        with self.lock:
            snapshot = self._snapshot_locked(time.time())
            self._dirty = False
            self._last_write = time.time()
        self._write_snapshot(snapshot)

    def _snapshot_locked(self, now):
        devices = {}
        for devhex, dev in self.devices.items():
            devices[devhex] = {
                "last_seen": dev["last_seen"],
                "last_accepted": dev["last_accepted"],
                "holding": bool(dev["holding"]),
                "last_miss": dev["last_miss"],
                "inbox": [
                    {"t": ev["t"], "v": ev["v"].hex(), "off": bool(ev["off"]), "seq": ev["seq"]}
                    for ev in dev["inbox"]
                ],
            }
        pairs = {}
        for pairhex, pair in self.pairs.items():
            pairs[pairhex] = {
                "id": pair["id"].hex(),
                "key": pair["key"].hex(),
                "members": list(pair["members"]),
                "names": dict(pair["names"]),
                "state": dict(pair["state"]),
                "dissolved": bool(pair["dissolved"]),
                "hug_latch": bool(pair["hug_latch"]),
                "created": pair.get("created", now),
                # the Love Energy ledger, relay-authoritative
                "score": dict(pair.get("score") or _new_score()),
                # server-internal proof that each member holds the pair key
                "seen": {k: float(v) for k, v in (pair.get("seen") or {}).items()},
            }
        pending = {}
        for devhex, entry in self.pending.items():
            if entry["expires"] <= now:
                continue
            ident = entry["identity"]
            pending[devhex] = {
                "code": entry["code"],
                "token": entry["token"].hex(),
                "name": entry["name"],
                "created": entry["created"],
                "expires": entry["expires"],
                "state": entry["state"],
                "attempts": int(entry.get("attempts", 0)),
                "identity": None
                if ident is None
                else {
                    "pair_id": ident["pair_id"].hex(),
                    "key": ident["key"].hex(),
                    "partner_id": ident["partner_id"],
                    "name": ident["name"],
                },
            }
        return {
            "version": VERSION,
            "instance": self.instance,
            "saved_at": now,
            "server_seq": self.server_seq,
            "devices": devices,
            "pairs": pairs,
            "dev_pair": dict(self.dev_pair),
            "pending": pending,
        }

    def _write_snapshot(self, snapshot):
        if not self.data_file:
            return
        tmp = self.data_file + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as handle:
                json.dump(snapshot, handle, separators=(",", ":"), sort_keys=True)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp, self.data_file)
        except OSError as exc:
            self.log.error("could not write state file %s: %s", self.data_file, exc)
            try:
                if os.path.exists(tmp):
                    os.remove(tmp)
            except OSError:
                pass
        else:
            self.log.debug("state written to %s", self.data_file)

    def _load(self):
        self._last_write = time.time()
        if not self.data_file or not os.path.exists(self.data_file):
            return
        try:
            with open(self.data_file, "r", encoding="utf-8") as handle:
                raw = json.load(handle)
            if not isinstance(raw, dict):
                raise ValueError("state is not a JSON object")
            now = time.time()
            server_seq = int(raw.get("server_seq", 0)) & 0xFFFFFFFF
            devices = {}
            for devhex, dev in (raw.get("devices") or {}).items():
                if not _is_hex(devhex, 16) or not isinstance(dev, dict):
                    raise ValueError("bad device record")
                inbox = []
                for ev in dev.get("inbox") or []:
                    etype = int(ev["t"])
                    if etype not in EVENT_LEN:
                        raise ValueError("bad event type in inbox")
                    value = bytes.fromhex(ev.get("v") or "")
                    if len(value) != EVENT_LEN[etype]:
                        raise ValueError("bad event value in inbox")
                    inbox.append(
                        {"t": etype, "v": value, "off": bool(ev.get("off")), "seq": int(ev.get("seq", 0))}
                    )
                last_miss = dev.get("last_miss")
                devices[devhex] = {
                    "last_seen": float(dev.get("last_seen", 0.0)),
                    "last_accepted": int(dev.get("last_accepted", 0)),
                    "holding": bool(dev.get("holding")),
                    "last_miss": None if last_miss is None else float(last_miss),
                    "inbox": inbox[-self.inbox_cap :],
                }
            pairs = {}
            for pairhex, pair in (raw.get("pairs") or {}).items():
                if not _is_hex(pairhex, 8) or not isinstance(pair, dict):
                    raise ValueError("bad pair record")
                members = [m for m in pair.get("members", []) if _is_hex(m, 16)]
                if not members:
                    continue
                pairs[pairhex] = {
                    "id": bytes.fromhex(pair["id"]),
                    "key": bytes.fromhex(pair["key"]),
                    "members": members,
                    "names": {k: v for k, v in (pair.get("names") or {}).items() if _is_hex(k, 16)},
                    "state": {k: int(v) for k, v in (pair.get("state") or {}).items() if _is_hex(k, 16)},
                    "dissolved": bool(pair.get("dissolved")),
                    "hug_latch": bool(pair.get("hug_latch")),
                    "created": float(pair.get("created", now)),
                    # state files written before the ledger existed get a
                    # fresh one.  _parse_score never raises: an exception in
                    # this block would discard every pairing in the file.
                    "score": _parse_score(pair.get("score")),
                    # state files written before this field exist carry no
                    # proof either way, and an empty map is the safe reading:
                    # the pair is re-confirmed by the next sync from each side.
                    "seen": _parse_seen(pair.get("seen"), members),
                }
            pending = {}
            codes = {}
            for devhex, entry in (raw.get("pending") or {}).items():
                if not _is_hex(devhex, 16) or not isinstance(entry, dict):
                    raise ValueError("bad pending record")
                expires = float(entry["expires"])
                if expires <= now:
                    continue
                ident = entry.get("identity")
                pending[devhex] = {
                    "code": str(entry["code"]),
                    "token": bytes.fromhex(entry["token"]),
                    "name": str(entry.get("name", "")),
                    "created": float(entry.get("created", now)),
                    "expires": expires,
                    "state": str(entry.get("state", "waiting")),
                    # state files written before the attempt cap have no field
                    "attempts": int(entry.get("attempts", 0)),
                    "identity": None
                    if not ident
                    else {
                        "pair_id": bytes.fromhex(ident["pair_id"]),
                        "key": bytes.fromhex(ident["key"]),
                        "partner_id": str(ident["partner_id"]),
                        "name": str(ident.get("name", "")),
                    },
                }
                codes[pending[devhex]["code"]] = devhex
        except Exception as exc:
            self.log.warning("ignoring unusable state file %s (%s); starting fresh", self.data_file, exc)
            self.server_seq = 0
            self.devices = {}
            self.pairs = {}
            self.dev_pair = {}
            self.pending = {}
            self.codes = {}
            return

        self.server_seq = server_seq
        self.devices = devices
        self.pairs = pairs
        self.pending = pending
        self.codes = codes
        # rebuild the device->pair index from the pairs themselves
        dev_pair = {}
        for pass_dissolved in (False, True):
            for pairhex, pair in pairs.items():
                if bool(pair["dissolved"]) != pass_dissolved:
                    continue
                for member in pair["members"]:
                    dev_pair[member] = pairhex
        self.dev_pair = dev_pair
        self.log.info(
            "loaded state from %s: %d pair(s), %d device(s), %d pending",
            self.data_file,
            len(self.pairs),
            len(self.devices),
            len(self.pending),
        )


def _is_hex(text, nbytes):
    if not isinstance(text, str) or len(text) != nbytes * 2:
        return False
    try:
        bytes.fromhex(text)
    except ValueError:
        return False
    return True


def _parse_seen(raw, members):
    """Read a pair's ``seen`` map, dropping anything unusable.

    Nothing may escape from here: ``Relay._load`` answers any exception by
    throwing the whole state file away and starting from scratch, so one bad
    entry would cost every pair and every queued event.
    """
    seen = {}
    if not isinstance(raw, dict):
        return seen
    for devhex, stamp in raw.items():
        if not _is_hex(devhex, 16) or devhex not in members:
            continue
        try:
            seen[devhex] = float(stamp)
        except (TypeError, ValueError):
            continue
    return seen


# ---------------------------------------------------------------- HTTP


ROUTES = {
    "/v1/pair/begin": (T_PAIR_BEGIN_REQ, T_PAIR_BEGIN_RSP),
    "/v1/pair/claim": (T_PAIR_CLAIM_REQ, T_PAIR_CLAIM_RSP),
    "/v1/pair/await": (T_PAIR_AWAIT_REQ, T_PAIR_AWAIT_RSP),
    "/v1/sync": (T_SYNC_REQ, T_SYNC_RSP),
    "/v1/unpair": (T_UNPAIR_REQ, T_UNPAIR_RSP),
}


class RelayHTTPRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = SERVER_VERSION
    sys_version = ""
    timeout = 60

    # -------------------------------------------------------- plumbing

    def version_string(self):
        return SERVER_VERSION

    def log_message(self, fmt, *args):
        self.relay.log.debug("http: " + fmt, *args)

    @property
    def relay(self):
        return self.server.relay

    def _send_bytes(self, body, content_type, code=200):
        try:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass
        finally:
            self.close_connection = True

    def _send_frame(self, frame):
        self._send_bytes(frame, "application/octet-stream")

    def _send_text(self, code, text):
        self._send_bytes(text.encode("utf-8"), "text/plain; charset=utf-8", code)

    def _hex_header(self, name, nbytes):
        raw = self.headers.get(name)
        if raw is None:
            raise RequestError(ST_BAD_REQUEST, "missing %s header" % name)
        raw = raw.strip().lower()
        if len(raw) != nbytes * 2:
            raise RequestError(ST_BAD_REQUEST, "%s must be %d hex bytes" % (name, nbytes))
        try:
            return bytes.fromhex(raw)
        except ValueError:
            raise RequestError(ST_BAD_REQUEST, "%s is not hex" % name)

    def _read_body(self):
        raw_len = self.headers.get("Content-Length")
        if raw_len is None:
            return b""
        try:
            length = int(raw_len)
        except ValueError:
            raise RequestError(ST_BAD_REQUEST, "bad Content-Length")
        if length < 0:
            raise RequestError(ST_BAD_REQUEST, "negative Content-Length")
        if length > MAX_HTTP_BODY:
            self._drain_body(length)
            raise RequestError(ST_BAD_REQUEST, "body of %d bytes exceeds %d" % (length, MAX_HTTP_BODY))
        if length == 0:
            return b""
        try:
            data = self.rfile.read(length)
        except socket.timeout:
            raise RequestError(ST_BAD_REQUEST, "timed out reading body")
        if len(data) != length:
            raise RequestError(ST_BAD_REQUEST, "short body")
        return data

    def _drain_body(self, length):
        """Discard an oversized body so the peer sees the reply instead of an RST."""
        remaining = min(length, 1 << 20)
        try:
            self.connection.settimeout(0.5)
            while remaining > 0:
                chunk = self.rfile.read(min(remaining, 65536))
                if not chunk:
                    break
                remaining -= len(chunk)
        except (OSError, socket.timeout):
            pass
        finally:
            try:
                self.connection.settimeout(None)
            except OSError:
                pass

    def _error_reply(self, rsp_type, status, devhex=None, pairhex=None, detail=""):
        key = None
        if rsp_type in SIGNED_TYPES:
            pair = self.relay.pairs.get(pairhex) if pairhex else None
            key = pair["key"] if pair is not None else ZERO_KEY
        seq = self.relay.server_seq & 0xFFFFFFFF
        ack = 0
        if devhex:
            ack = self.relay.devices.get(devhex, {}).get("last_accepted", 0)
        payload = error_rsp_payload(rsp_type, status, seq)
        self._send_frame(encode_frame(rsp_type, seq, ack, payload, key))
        if detail:
            self.relay.log.debug("rejected request: %s", detail)
        return status, 0

    # -------------------------------------------------------- verbs

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/healthz":
            self._send_text(200, "ok")
            return
        self._send_text(404, "not found")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        route = ROUTES.get(path)
        if route is None:
            self._send_text(404, "not found")
            return
        _req_type, rsp_type = route
        relay = self.relay
        devhex = None
        pairhex = None
        status = ST_BAD_REQUEST
        events = 0
        try:
            devhex = self._hex_header("X-Device-Id", 16).hex()
            pairhex = self._hex_header("X-Pair-Id", 8).hex() if _req_type in (T_SYNC_REQ, T_UNPAIR_REQ) else None
            token = self._hex_header("X-Pair-Token", 32) if _req_type == T_PAIR_AWAIT_REQ else None
            body = self._read_body()

            if _req_type == T_PAIR_BEGIN_REQ:
                status, payload, key, ack, events = relay.handle_begin(devhex, body)
            elif _req_type == T_PAIR_CLAIM_REQ:
                status, payload, key, ack, events = relay.handle_claim(devhex, body)
            elif _req_type == T_PAIR_AWAIT_REQ:
                status, payload, key, ack, events = relay.handle_await(devhex, token)
            elif _req_type == T_SYNC_REQ:
                status, payload, key, ack, events = relay.handle_sync(devhex, pairhex, body)
            else:
                status, payload, key, ack, events = relay.handle_unpair(devhex, pairhex, body)

            if rsp_type in SIGNED_TYPES and key is None:
                key = ZERO_KEY
            frame = encode_frame(rsp_type, relay.server_seq & 0xFFFFFFFF, ack, payload, key)
            self._send_frame(frame)
        except RequestError as exc:
            status, events = self._error_reply(rsp_type, exc.status, devhex, pairhex, exc.detail)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except Exception as exc:
            relay.log.exception("unhandled error serving %s", path)
            try:
                status, events = self._error_reply(rsp_type, ST_BAD_REQUEST, devhex, pairhex, str(exc))
            except Exception:
                self.close_connection = True
        finally:
            relay.log_request(path, devhex, status, events)


class RelayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, relay):
        self.relay = relay
        super().__init__(address, RelayHTTPRequestHandler)


def make_server(host, port, relay):
    return RelayHTTPServer((host, port), relay)


# ---------------------------------------------------------------- CLI


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="AI PASSPORT internet relay server")
    parser.add_argument("--host", default=DEFAULT_HOST, help="bind address (default %(default)s)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="bind port (default %(default)s)")
    parser.add_argument(
        "--data-file",
        default=None,
        help="state file path (default: %s next to relay.py)" % DEFAULT_DATA_FILE,
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="log verbosity (default %(default)s)",
    )
    return parser.parse_args(argv)


def install_signal_handlers(stop_event, log):
    def handler(signum, _frame):
        log.info("received signal %s, shutting down", signum)
        stop_event.set()

    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        sig = getattr(signal, name, None)
        if sig is None:
            continue
        try:
            signal.signal(sig, handler)
        except (ValueError, OSError, RuntimeError):
            pass


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level, logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    log = logging.getLogger("relay")

    data_file = args.data_file
    if not data_file:
        data_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), DEFAULT_DATA_FILE)

    relay = Relay(data_file=data_file, logger=log)
    try:
        httpd = make_server(args.host, args.port, relay)
    except OSError as exc:
        log.error("cannot bind %s:%d: %s", args.host, args.port, exc)
        relay.shutdown()
        return 1

    host, port = httpd.server_address[0], httpd.server_address[1]
    print("AI PASSPORT relay %s listening on http://%s:%d (data file: %s)" % (SERVER_VERSION, host, port, data_file), flush=True)
    log.info("serving /healthz and /v1/* on %s:%d", host, port)

    stop_event = threading.Event()
    install_signal_handlers(stop_event, log)
    worker = threading.Thread(
        target=httpd.serve_forever, kwargs={"poll_interval": 0.2}, name="relay-http", daemon=True
    )
    worker.start()
    try:
        # Wait with a timeout rather than blocking forever: on Windows a
        # no-timeout Event.wait() parks the main thread in a non-interruptible
        # lock, so the signal handler registered above would never get to run.
        while not stop_event.wait(0.25):
            pass
    except KeyboardInterrupt:
        log.info("keyboard interrupt, shutting down")
    finally:
        stop_event.set()
        httpd.shutdown()
        httpd.server_close()
        relay.shutdown()
        worker.join(timeout=5.0)
    log.info("relay stopped cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
