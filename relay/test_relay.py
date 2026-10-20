#!/usr/bin/env python3
"""Tests for the AI PASSPORT relay server (relay.py).

Standard library only; run with:

    python3 test_relay.py

The suite starts the real HTTP server on an ephemeral loopback port inside the
test process and drives it with a small device-side client implemented here.
"""

import hmac
import hashlib
import json
import logging
import os
import secrets
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import relay  # noqa: E402  (import after sys.path is prepared)

HTTP_TIMEOUT = 30.0


# ---------------------------------------------------------------- responses


class BeginResult(object):
    def __init__(self, payload):
        self.raw = payload
        self.status = payload[0]
        self.code = payload[1:7].decode("ascii", "replace")
        (self.ttl_secs,) = struct.unpack_from(">H", payload, 7)
        self.token = payload[9:41]

    def __repr__(self):
        return "BeginResult(status=%s, code=%s)" % (self.status, self.code)


class ClaimResult(object):
    def __init__(self, payload):
        self.raw = payload
        self.status = payload[0]
        self.pair_id = self.key = self.partner_id = self.name = None
        if self.status == relay.ST_OK:
            self.pair_id = payload[1:9]
            self.key = payload[9:41]
            self.partner_id = payload[41:57]
            name_len = payload[57]
            self.name = payload[58 : 58 + name_len].decode("utf-8")

    def __repr__(self):
        return "ClaimResult(status=%s)" % (self.status,)


class AwaitResult(object):
    def __init__(self, payload):
        self.raw = payload
        self.status = payload[0]
        self.state = payload[1] if len(payload) > 1 else None
        self.pair_id = self.key = self.partner_id = self.name = None
        if self.status == relay.ST_OK and self.state == relay.AWAIT_PAIRED:
            self.pair_id = payload[2:10]
            self.key = payload[10:42]
            self.partner_id = payload[42:58]
            name_len = payload[58]
            self.name = payload[59 : 59 + name_len].decode("utf-8")

    def __repr__(self):
        return "AwaitResult(status=%s, state=%s)" % (self.status, self.state)


class SyncResult(object):
    def __init__(self, payload):
        self.raw = payload
        self.status = payload[0] if payload else None
        self.count = payload[1] if len(payload) > 1 else 0
        self.events = []
        offset = 2
        for _ in range(self.count):
            etype = payload[offset]
            elen = payload[offset + 1]
            offset += 2
            self.events.append((etype, payload[offset : offset + elen]))
            offset += elen
        self.server_seq = None
        self.server_time = None
        self.partner_online = None
        self.partner_holding = None
        self.pair_state = None
        self.love_energy = None
        if len(payload) >= offset + 11:
            self.server_seq, self.server_time = struct.unpack_from(">II", payload, offset)
            offset += 8
            self.partner_online = payload[offset]
            self.partner_holding = payload[offset + 1]
            self.pair_state = payload[offset + 2]
            offset += 3
            if len(payload) >= offset + 4:
                (self.love_energy,) = struct.unpack_from(">I", payload, offset)

    @property
    def types(self):
        return [etype for etype, _value in self.events]

    def count_of(self, etype):
        return sum(1 for et in self.types if et == etype)

    def value_of(self, etype):
        for et, value in self.events:
            if et == etype:
                return value
        return None

    def __repr__(self):
        return "SyncResult(status=%s, events=%s)" % (self.status, self.types)


# ---------------------------------------------------------------- client


class DeviceClient(object):
    """A minimal device: frames requests, signs them, and parses responses."""

    def __init__(self, port, name, dev_id=None):
        self.port = port
        self.name = name
        self.dev_id = dev_id if dev_id is not None else secrets.token_bytes(16)
        self.seq = 0
        self.pair_id = None
        self.key = None
        self.token = None

    # -- transport ---------------------------------------------------

    def _url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def post(self, path, body, headers=None, with_device_id=True, timeout=HTTP_TIMEOUT):
        hdrs = {"Content-Type": "application/octet-stream"}
        if with_device_id:
            hdrs["X-Device-Id"] = self.dev_id.hex()
        if headers:
            hdrs.update(headers)
        request = urllib.request.Request(self._url(path), data=body, headers=hdrs, method="POST")
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()

    def get(self, path, timeout=HTTP_TIMEOUT):
        with urllib.request.urlopen(self._url(path), timeout=timeout) as response:
            return response.status, response.read()

    # -- pairing -----------------------------------------------------

    def begin(self, name=None):
        name = self.name if name is None else name
        name_bytes = name.encode("utf-8")
        payload = bytes((len(name_bytes),)) + name_bytes
        raw = relay.encode_frame(relay.T_PAIR_BEGIN_REQ, 0, 0, payload)
        ftype, _seq, _ack, body, _mac = relay.split_frame(self.post("/v1/pair/begin", raw))
        assert ftype == relay.T_PAIR_BEGIN_RSP, hex(ftype)
        result = BeginResult(body)
        if result.status == relay.ST_OK:
            self.token = result.token
        return result

    def claim(self, code, name=None):
        name = self.name if name is None else name
        name_bytes = name.encode("utf-8")
        payload = code.encode("ascii") + bytes((len(name_bytes),)) + name_bytes
        raw = relay.encode_frame(relay.T_PAIR_CLAIM_REQ, 0, 0, payload)
        ftype, _seq, _ack, body, _mac = relay.split_frame(self.post("/v1/pair/claim", raw))
        assert ftype == relay.T_PAIR_CLAIM_RSP, hex(ftype)
        result = ClaimResult(body)
        if result.status == relay.ST_OK:
            self.pair_id = result.pair_id
            self.key = result.key
        return result

    def await_pair(self, token=None, timeout=HTTP_TIMEOUT):
        token = self.token if token is None else token
        raw = relay.encode_frame(relay.T_PAIR_AWAIT_REQ, 0, 0, b"")
        body = self.post("/v1/pair/await", raw, {"X-Pair-Token": token.hex()}, timeout=timeout)
        ftype, _seq, _ack, payload, _mac = relay.split_frame(body)
        assert ftype == relay.T_PAIR_AWAIT_RSP, hex(ftype)
        result = AwaitResult(payload)
        if result.status == relay.ST_OK and result.state == relay.AWAIT_PAIRED:
            self.pair_id = result.pair_id
            self.key = result.key
        return result

    def await_in_thread(self, token=None, timeout=HTTP_TIMEOUT):
        box = {}

        def worker():
            try:
                box["result"] = self.await_pair(token, timeout=timeout)
            except Exception as exc:  # pragma: no cover - surfaced by the test
                box["error"] = exc

        thread = threading.Thread(target=worker, name="await-%s" % self.name)
        thread.start()
        return thread, box

    # -- sync --------------------------------------------------------

    def next_seq(self):
        self.seq += 1
        return self.seq

    def build_sync(self, events=(), local_flags=0, seq=None, key=None, tamper_mac=False):
        use_key = self.key if key is None else key
        if seq is None:
            seq = self.next_seq()
        else:
            self.seq = max(self.seq, seq)
        payload = relay.encode_events(events) + bytes((local_flags & 0xFF,))
        raw = relay.encode_frame(relay.T_SYNC_REQ, seq, 0, payload, use_key)
        if tamper_mac:
            raw = raw[:-1] + bytes((raw[-1] ^ 0xFF,))
        return seq, raw

    def send_sync(self, raw, key=None, verify=True, pair_id=None, timeout=HTTP_TIMEOUT):
        pairhex = (self.pair_id if pair_id is None else pair_id).hex()
        body = self.post("/v1/sync", raw, {"X-Pair-Id": pairhex}, timeout=timeout)
        ftype, _seq, ack, payload, mac = relay.split_frame(body)
        assert ftype == relay.T_SYNC_RSP, hex(ftype)
        if verify:
            use_key = self.key if key is None else key
            relay.verify_frame_mac(body, use_key)
        result = SyncResult(payload)
        result.ack = ack
        return result

    def sync(self, events=(), local_flags=0, seq=None, key=None, verify=True, pair_id=None,
             tamper_mac=False, timeout=HTTP_TIMEOUT):
        seq, raw = self.build_sync(events, local_flags, seq=seq, key=key, tamper_mac=tamper_mac)
        return self.send_sync(raw, key=key, verify=verify, pair_id=pair_id, timeout=timeout)

    def raw_sync(self, body, pair_id=None, key=None, verify=False, timeout=HTTP_TIMEOUT):
        """POST an arbitrary body to /v1/sync and return (ftype, payload, mac)."""
        pairhex = (self.pair_id if pair_id is None else pair_id).hex()
        raw = self.post("/v1/sync", body, {"X-Pair-Id": pairhex}, timeout=timeout)
        ftype, _seq, ack, payload, mac = relay.split_frame(raw)
        if verify:
            relay.verify_frame_mac(raw, self.key if key is None else key)
        return ftype, ack, payload

    # -- unpair ------------------------------------------------------

    def unpair(self, pair_id=None, key=None, tamper_mac=False):
        use_key = self.key if key is None else key
        pairhex = (self.pair_id if pair_id is None else pair_id).hex()
        raw = relay.encode_frame(relay.T_UNPAIR_REQ, self.next_seq(), 0, b"", use_key)
        if tamper_mac:
            raw = raw[:-1] + bytes([raw[-1] ^ 0xFF])
        body = self.post("/v1/unpair", raw, {"X-Pair-Id": pairhex})
        ftype, _seq, _ack, payload, _mac = relay.split_frame(body)
        assert ftype == relay.T_UNPAIR_RSP, hex(ftype)
        return payload[0]


# ---------------------------------------------------------------- harness


class ServerHandle(object):
    def __init__(self, relay_obj, data_file=None):
        self.relay = relay_obj
        self.data_file = data_file
        self.httpd = relay.make_server("127.0.0.1", 0, relay_obj)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.1},
                                       name="relay-http-test", daemon=True)
        self.thread.start()
        self._stopped = False

    def stop(self):
        if self._stopped:
            return
        self._stopped = True
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5.0)
        self.relay.shutdown()


class RelayTestCase(unittest.TestCase):
    """Base class: temp dirs, servers and clients created per test."""

    long_poll_sec = 1.0
    presence_stale_sec = 30.0

    def setUp(self):
        self._tmpdirs = []
        self._servers = []
        self._logger = logging.getLogger("relay-test")
        self._logger.addHandler(logging.NullHandler())
        self._logger.propagate = False
        self._logger.setLevel(logging.WARNING)

    def tearDown(self):
        for server in self._servers:
            try:
                server.stop()
            except Exception:
                pass
        for tmpdir in self._tmpdirs:
            for name in os.listdir(tmpdir):
                try:
                    os.remove(os.path.join(tmpdir, name))
                except OSError:
                    pass
            try:
                os.rmdir(tmpdir)
            except OSError:
                pass

    def tmpdir(self):
        path = tempfile.mkdtemp(prefix="relay-test-")
        self._tmpdirs.append(path)
        return path

    def new_server(self, data_file=None, **kwargs):
        kwargs.setdefault("long_poll_sec", self.long_poll_sec)
        kwargs.setdefault("presence_stale_sec", self.presence_stale_sec)
        kwargs.setdefault("persist_debounce_sec", 0.1)
        relay_obj = relay.Relay(data_file=data_file, logger=self._logger, **kwargs)
        handle = ServerHandle(relay_obj, data_file=data_file)
        self._servers.append(handle)
        return handle

    def new_client(self, server, name, dev_id=None):
        return DeviceClient(server.port, name, dev_id=dev_id)

    @staticmethod
    def wrong_code(code, offset=1):
        """A six-digit code that differs from ``code``."""
        return "%06d" % ((int(code) + offset) % 1000000)

    def pair(self, server, name_a="Alice", name_b="Bob", timeout=10.0):
        """Pair two fresh devices; returns (a, b, begin, claim, awaited)."""
        a = self.new_client(server, name_a)
        b = self.new_client(server, name_b)
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK, "PAIR_BEGIN should succeed")
        thread, box = a.await_in_thread(begin.token)
        time.sleep(0.2)
        claim = b.claim(begin.code)
        self.assertEqual(claim.status, relay.ST_OK, "PAIR_CLAIM should succeed")
        thread.join(timeout=timeout)
        self.assertFalse(thread.is_alive(), "PAIR_AWAIT long poll did not return")
        self.assertNotIn("error", box, box.get("error"))
        return a, b, begin, claim, box["result"]


# ---------------------------------------------------------------- tests


class T01PairingHappyPath(RelayTestCase):
    def test_pairing_happy_path(self):
        server = self.new_server()
        a, b, begin, claim, awaited = self.pair(server)

        # the code is six digits: the device's pairing screen is a digit wheel
        self.assertEqual(len(begin.code), relay.CODE_LEN)
        self.assertTrue(begin.code.isdigit(), begin.code)
        self.assertEqual(begin.ttl_secs, int(server.relay.pair_ttl_sec))
        for char in begin.code:
            self.assertIn(char, relay.CODE_ALPHABET)
        self.assertEqual(len(begin.token), 32)

        # the claimant learns the awaiting device's identity
        self.assertEqual(claim.partner_id, a.dev_id)
        self.assertEqual(claim.name, "Alice")
        self.assertEqual(len(claim.pair_id), 8)
        self.assertEqual(len(claim.key), 32)

        # the awaiting device's long poll returns the same pair identity
        self.assertEqual(awaited.status, relay.ST_OK)
        self.assertEqual(awaited.state, relay.AWAIT_PAIRED)
        self.assertEqual(awaited.pair_id, claim.pair_id)
        self.assertEqual(awaited.key, claim.key)
        self.assertEqual(awaited.partner_id, b.dev_id)
        self.assertEqual(awaited.name, "Bob")

        # a signed round trip now works from both sides
        a_result = a.sync([(relay.EV_MISS, b"")], 0)
        self.assertEqual(a_result.status, relay.ST_OK)
        b_result = b.sync([], 0)
        self.assertEqual(b_result.status, relay.ST_OK)
        self.assertEqual(b_result.types, [relay.EV_MISS])

        # await is idempotent while the pending entry is alive
        again = a.await_pair(begin.token)
        self.assertEqual(again.state, relay.AWAIT_PAIRED)
        self.assertEqual(again.pair_id, claim.pair_id)

    def test_already_paired_is_reported(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        self.assertEqual(a.begin().status, relay.ST_ALREADY_PAIRED)
        other = self.new_client(server, "Carol")
        other_begin = other.begin()
        self.assertEqual(other_begin.status, relay.ST_OK)
        self.assertEqual(a.claim(other_begin.code).status, relay.ST_ALREADY_PAIRED)


class T02PairingFailures(RelayTestCase):
    def test_wrong_code(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Bob")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        result = b.claim(self.wrong_code(begin.code))
        self.assertEqual(result.status, relay.ST_CODE_NOT_FOUND)
        self.assertEqual(len(result.raw), 1, "error bodies are a bare status byte")

    def test_begin_returns_six_digits(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        for _ in range(24):
            begin = a.begin()
            self.assertEqual(begin.status, relay.ST_OK)
            self.assertEqual(len(begin.code), relay.CODE_LEN)
            self.assertTrue(begin.code.isdigit(), begin.code)

    def test_eight_wrong_claims_burn_the_code(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Mallory")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        for offset in range(1, relay.MAX_CODE_ATTEMPTS + 1):
            result = b.claim(self.wrong_code(begin.code, offset))
            self.assertEqual(result.status, relay.ST_CODE_NOT_FOUND)
        self.assertEqual(b.claim(begin.code).status, relay.ST_CODE_NOT_FOUND,
                         "the pending code must be burned after %d wrong claims"
                         % relay.MAX_CODE_ATTEMPTS)
        self.assertNotIn(a.dev_id.hex(), server.relay.pending,
                         "burning removes the pending session")

    def test_seven_wrong_claims_leave_the_code_usable(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Bob")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        for offset in range(1, relay.MAX_CODE_ATTEMPTS):
            result = b.claim(self.wrong_code(begin.code, offset))
            self.assertEqual(result.status, relay.ST_CODE_NOT_FOUND)
        claim = b.claim(begin.code)
        self.assertEqual(claim.status, relay.ST_OK, "the code must still be usable below the cap")
        self.assertEqual(claim.partner_id, a.dev_id)
        self.assertEqual(a.await_pair(begin.token).state, relay.AWAIT_PAIRED)

    def test_expired_code(self):
        server = self.new_server(pair_ttl_sec=0.5)
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Bob")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        self.assertEqual(begin.ttl_secs, 0)
        time.sleep(0.6)
        self.assertEqual(b.claim(begin.code).status, relay.ST_CODE_EXPIRED)
        self.assertEqual(server.relay.pending[a.dev_id.hex()]["attempts"], 0,
                         "an expired code must not use up attempts")
        # the awaiting side is told the request expired rather than hanging
        awaited = a.await_pair(begin.token)
        self.assertEqual(awaited.status, relay.ST_OK)
        self.assertEqual(awaited.state, relay.AWAIT_EXPIRED)

    def test_malformed_and_terminal_claims_do_not_count(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Bob")
        begin = a.begin()
        # a claim body too short to parse never reaches the counter
        bad = relay.encode_frame(relay.T_PAIR_CLAIM_REQ, 0, 0, b"12")
        ftype, _seq, _ack, payload, _mac = relay.split_frame(b.post("/v1/pair/claim", bad))
        self.assertEqual(ftype, relay.T_PAIR_CLAIM_RSP)
        self.assertEqual(payload[0], relay.ST_BAD_REQUEST)
        # claiming your own code is BAD_REQUEST as well
        self.assertEqual(a.claim(begin.code).status, relay.ST_BAD_REQUEST)
        self.assertEqual(server.relay.pending[a.dev_id.hex()]["attempts"], 0,
                         "only a code that matches nothing may use up attempts")

    def test_claim_with_initiator_device_id(self):
        # decided: claiming with your own device id is BAD_REQUEST
        server = self.new_server()
        a = self.new_client(server, "Alice")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        self.assertEqual(a.claim(begin.code).status, relay.ST_BAD_REQUEST)

    def test_await_with_wrong_token(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        result = a.await_pair(secrets.token_bytes(32))
        self.assertEqual(result.status, relay.ST_UNAUTHORIZED)

    def test_duplicate_claim_is_already_paired(self):
        server = self.new_server()
        a = self.new_client(server, "Alice")
        b = self.new_client(server, "Bob")
        begin = a.begin()
        self.assertEqual(b.claim(begin.code).status, relay.ST_OK)
        self.assertEqual(b.claim(begin.code).status, relay.ST_ALREADY_PAIRED)


class T03SyncRejections(RelayTestCase):
    def test_bad_hmac(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        _seq, raw = a.build_sync([(relay.EV_MISS, b"")], 0, tamper_mac=True)
        ftype, _ack, payload = a.raw_sync(raw)
        self.assertEqual(ftype, relay.T_SYNC_RSP)
        self.assertEqual(payload[0], relay.ST_UNAUTHORIZED)

    def test_wrong_pair_id(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        _c, _d, _cbegin, c_claim, _cawaited = self.pair(server, "Carol", "Dave")
        # the device is a member of its own pair only -> UNAUTHORIZED
        _seq, raw = a.build_sync([(relay.EV_MISS, b"")], 0)
        _ftype, _ack, payload = a.raw_sync(raw, pair_id=c_claim.pair_id)
        self.assertEqual(payload[0], relay.ST_UNAUTHORIZED)

    def test_unknown_pair(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        _seq, raw = a.build_sync([(relay.EV_MISS, b"")], 0)
        _ftype, _ack, payload = a.raw_sync(raw, pair_id=secrets.token_bytes(8))
        self.assertEqual(payload[0], relay.ST_PAIR_NOT_FOUND)

    def test_oversized_body(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        result = a.sync([(relay.EV_MISS, b"")], 0, pair_id=a.pair_id)
        self.assertEqual(result.status, relay.ST_OK)
        body = b"X" * (relay.MAX_HTTP_BODY + 512)
        _ftype, _ack, payload = a.raw_sync(body)
        self.assertEqual(payload[0], relay.ST_BAD_REQUEST)

    def test_unknown_frame_type(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        header = struct.pack(">2sBBHII", relay.MAGIC, relay.VERSION, 0x7F, 0, 1, 0)
        _ftype, _ack, payload = a.raw_sync(header)
        self.assertEqual(payload[0], relay.ST_BAD_REQUEST)

    def test_truncated_frame(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        _ftype, _ack, payload = a.raw_sync(relay.MAGIC + b"\x01\x07")
        self.assertEqual(payload[0], relay.ST_BAD_REQUEST)

    def test_bad_magic_and_version(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        bad_magic = struct.pack(">2sBBHII", b"XX", relay.VERSION, relay.T_SYNC_REQ, 0, 1, 0)
        self.assertEqual(a.raw_sync(bad_magic)[2][0], relay.ST_BAD_REQUEST)
        bad_version = struct.pack(">2sBBHII", relay.MAGIC, 9, relay.T_SYNC_REQ, 0, 1, 0)
        self.assertEqual(a.raw_sync(bad_version)[2][0], relay.ST_BAD_REQUEST)

    def test_seq_zero_rejected(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        _seq, raw = a.build_sync([(relay.EV_MISS, b"")], 0, seq=0)
        self.assertEqual(a.raw_sync(raw)[2][0], relay.ST_BAD_REQUEST)

    def test_server_generated_event_rejected(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        result = a.sync([(relay.EV_HUG_SYNC, b"")], 0)
        self.assertEqual(result.status, relay.ST_BAD_REQUEST)

    def test_wrong_event_value_length_rejected(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        self.assertEqual(a.sync([(relay.EV_HUG_END, b"\x01")], 0).status, relay.ST_BAD_REQUEST)
        self.assertEqual(a.sync([(relay.EV_MISS, b"\x00")], 0).status, relay.ST_BAD_REQUEST)
        self.assertEqual(a.sync([(0x7F, b"")], 0).status, relay.ST_BAD_REQUEST)


class T04Retransmission(RelayTestCase):
    def test_retransmission_is_ingested_once(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        seq, raw = a.build_sync([(relay.EV_MISS, b"")], 0)
        first = a.send_sync(raw)
        second = a.send_sync(raw)
        self.assertEqual(first.status, relay.ST_OK)
        self.assertEqual(second.status, relay.ST_OK)
        self.assertEqual(first.ack, seq)
        self.assertEqual(second.ack, seq)
        result = b.sync([], 0)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertEqual(result.types, [relay.EV_MISS], "the event must arrive exactly once")

        # a genuinely newer seq is still ingested
        a.sync([(relay.EV_MISS, b"")], 0)
        self.assertEqual(b.sync([], 0).types, [relay.EV_MISS])

    def test_repairing_resets_the_sequence_watermark(self):
        """A re-paired device starts its counter over and must still be heard.

        ``couple_store_clear_pair()`` zeroes the device's sequence counter, so
        a device that unpairs and pairs again signs its first request with a
        small seq -- far below the watermark the previous pairing pushed up.
        Carried across, that watermark makes every batch look like a
        retransmission: the events are dropped, yet the response still carries
        the high ack, so the sender prunes its outbox and the new partner never
        sees anything.
        """
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)

        # the first pairing drives the watermark up, as months of use would
        a.sync([(relay.EV_MISS, b"")], 0, seq=15000)
        self.assertEqual(b.sync([], 0).types, [relay.EV_MISS])
        self.assertEqual(server.relay.devices[a.dev_id.hex()]["last_accepted"], 15000)

        self.assertEqual(a.unpair(), relay.ST_OK)
        a.seq = 0  # the device cleared its counter along with the pair

        c = self.new_client(server, "Carol")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        self.assertEqual(c.claim(begin.code).status, relay.ST_OK)
        self.assertEqual(a.await_pair(begin.token).state, relay.AWAIT_PAIRED)

        result = a.sync([(relay.EV_MISS, b"")], 0, seq=1)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertEqual(result.ack, 1, "the stale watermark must not be answered back")
        self.assertEqual(c.sync([], 0).types, [relay.EV_MISS],
                         "the new partner must receive the event")


class T05EventDelivery(RelayTestCase):
    def test_miss_is_delivered_and_server_time_is_accurate(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        before = int(time.time())
        result = a.sync([(relay.EV_MISS, b"")], 0)
        self.assertEqual(result.status, relay.ST_OK)
        after = int(time.time())

        self.assertIsNotNone(result.server_seq)
        self.assertIsNotNone(result.server_time)
        self.assertGreaterEqual(result.server_seq, 1)
        self.assertTrue(before - 5 <= result.server_time <= after + 5,
                        "server_time %d not within a few seconds of %d" % (result.server_time, before))
        self.assertEqual(result.partner_online, 1)
        self.assertEqual(result.pair_state, relay.PAIR_STATE_PAIRED)

        delivered = b.sync([], 0)
        self.assertEqual(delivered.status, relay.ST_OK)
        self.assertEqual(delivered.types, [relay.EV_MISS])
        self.assertEqual(delivered.partner_online, 1)
        self.assertEqual(delivered.pair_state, relay.PAIR_STATE_PAIRED)

    def test_long_poll_returns_early_on_event(self):
        server = self.new_server(long_poll_sec=5.0)
        a, b, _begin, _claim, _awaited = self.pair(server)
        box = {}

        def worker():
            started = time.time()
            box["result"] = b.sync([], 0)
            # Measure the poll itself: the sending call below also long-polls
            # (a's own inbox stays empty), so a wall-clock span taken outside
            # the thread would include that wait and say nothing about b.
            box["elapsed"] = time.time() - started

        thread = threading.Thread(target=worker, name="poll")
        thread.start()
        time.sleep(0.3)
        a.sync([(relay.EV_MISS, b"")], 0)
        thread.join(timeout=10)
        self.assertFalse(thread.is_alive(), "the long poll should return as soon as an event arrives")
        self.assertEqual(box["result"].types, [relay.EV_MISS])
        self.assertLess(box["elapsed"], 4.0,
                        "long poll did not return early (%.2fs)" % box["elapsed"])


class T06HeartSync(RelayTestCase):
    def test_mutual_miss_fires_heart_sync_once(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)

        first = a.sync([(relay.EV_MISS, b"")], 0)
        self.assertEqual(first.count_of(relay.EV_HEART_SYNC), 0, "no peer miss remembered yet")

        second = b.sync([(relay.EV_MISS, b"")], 0)
        self.assertEqual(second.count_of(relay.EV_HEART_SYNC), 1, "both sides missed within 10 s")

        third = a.sync([], 0)
        self.assertEqual(third.count_of(relay.EV_HEART_SYNC), 1)
        self.assertEqual(third.count_of(relay.EV_MISS), 1)

        # the mutual fire cleared both remembered miss times, so a lonely miss
        # right afterwards is not matched against a stale one
        self.assertEqual(a.sync([(relay.EV_MISS, b"")], 0).count_of(relay.EV_HEART_SYNC), 0)


class T07OfflineSummary(RelayTestCase):
    presence_stale_sec = 1.0

    def test_offline_summary_precedes_the_events(self):
        server = self.new_server(presence_stale_sec=1.0, long_poll_sec=1.0)
        a, b, _begin, _claim, _awaited = self.pair(server)
        self.assertEqual(b.sync([], 0).status, relay.ST_OK)  # B is known to be present
        time.sleep(1.3)

        hug_end = struct.pack(">H", 1500)
        events = [
            (relay.EV_MISS, b""),
            (relay.EV_MISS, b""),
            (relay.EV_MISS, b""),
            (relay.EV_HUG_END, hug_end),
        ]
        result = a.sync(events, 0)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertEqual(result.partner_online, 0, "B has not polled within PRESENCE_STALE_SEC")

        delivered = b.sync([], 0)
        self.assertEqual(delivered.status, relay.ST_OK)
        self.assertEqual(delivered.types[0], relay.EV_OFFLINE_SUMMARY)
        self.assertEqual(delivered.value_of(relay.EV_OFFLINE_SUMMARY), b"\x03")
        self.assertEqual(
            delivered.types,
            [relay.EV_OFFLINE_SUMMARY, relay.EV_MISS, relay.EV_MISS, relay.EV_MISS, relay.EV_HUG_END],
        )
        self.assertEqual(delivered.value_of(relay.EV_HUG_END), hug_end)
        self.assertEqual(delivered.count_of(relay.EV_MISS), 3)

        # tags are cleared, so a later poll has no summary
        self.assertEqual(b.sync([], 0).types, [])


class T08HugOverlap(RelayTestCase):
    def test_hug_sync_fires_once_per_overlap(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)

        a.sync([(relay.EV_HUG_START, b"")], relay.LOCAL_FLAG_HOLDING)
        second = b.sync([(relay.EV_HUG_START, b"")], relay.LOCAL_FLAG_HOLDING)
        self.assertEqual(second.count_of(relay.EV_HUG_SYNC), 1, "the overlap should fire HUG_SYNC")
        self.assertEqual(second.partner_holding, 1, "A is holding")

        third = a.sync([], relay.LOCAL_FLAG_HOLDING)
        self.assertEqual(third.count_of(relay.EV_HUG_SYNC), 1, "the first broadcast reaches A")
        self.assertEqual(third.partner_holding, 1, "B is holding")

        fourth = a.sync([], relay.LOCAL_FLAG_HOLDING)
        self.assertEqual(fourth.count_of(relay.EV_HUG_SYNC), 0, "no second HUG_SYNC while both hold")

        # releasing and overlapping again fires a fresh HUG_SYNC
        b.sync([(relay.EV_HUG_END, struct.pack(">H", 900))], 0)
        a.sync([(relay.EV_HUG_END, struct.pack(">H", 900))], 0)
        a.sync([(relay.EV_HUG_START, b"")], relay.LOCAL_FLAG_HOLDING)
        again = b.sync([(relay.EV_HUG_START, b"")], relay.LOCAL_FLAG_HOLDING)
        self.assertEqual(again.count_of(relay.EV_HUG_SYNC), 1)

    def test_flags_bit_zero_reports_holding(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        a.sync([], relay.LOCAL_FLAG_HOLDING)
        self.assertEqual(b.sync([], 0).partner_holding, 1, "bit 0 set means holding")
        a.sync([], 0)
        self.assertEqual(b.sync([], 0).partner_holding, 0, "bit 0 clear means not holding")


class T09Unpair(RelayTestCase):
    def test_unpair_notifies_the_partner_and_frees_the_device(self):
        server = self.new_server()
        a, b, _begin, _claim, _awaited = self.pair(server)
        old_pair_id = a.pair_id
        old_key = a.key

        self.assertEqual(a.unpair(), relay.ST_OK)

        result = b.sync([], 0)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertIn(relay.EV_UNPAIR, result.types)
        self.assertEqual(result.pair_state, relay.PAIR_STATE_PEER_LEFT)
        self.assertEqual(result.partner_online, 0)

        # the stale pair id is gone for the departing device
        stale = a.sync([], 0, pair_id=old_pair_id, key=old_key, verify=False)
        self.assertEqual(stale.status, relay.ST_PAIR_NOT_FOUND)

        # the departing device can pair again
        c = self.new_client(server, "Carol")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        claim = c.claim(begin.code)
        self.assertEqual(claim.status, relay.ST_OK)
        self.assertEqual(claim.partner_id, a.dev_id)
        self.assertNotEqual(claim.pair_id, old_pair_id)
        self.assertNotEqual(claim.key, old_key)
        awaited = a.await_pair(begin.token)
        self.assertEqual(awaited.state, relay.AWAIT_PAIRED)
        self.assertEqual(awaited.pair_id, claim.pair_id)
        self.assertEqual(awaited.name, "Carol")
        self.assertEqual(a.sync([(relay.EV_MISS, b"")], 0).status, relay.ST_OK)
        self.assertEqual(c.sync([], 0).types, [relay.EV_MISS])

    def test_unpair_requires_hmac_and_membership(self):
        server = self.new_server()
        a, _b, _begin, _claim, _awaited = self.pair(server)
        other = self.new_client(server, "Carol")
        self.assertEqual(a.unpair(key=secrets.token_bytes(32), tamper_mac=True), relay.ST_UNAUTHORIZED)
        self.assertEqual(other.raw_sync(b"", pair_id=a.pair_id)[2][0], relay.ST_UNAUTHORIZED,
                         "a non-member of a live pair is rejected before the body is parsed")


class T10Persistence(RelayTestCase):
    def test_pair_and_inbox_survive_a_restart(self):
        tmpdir = self.tmpdir()
        data_file = os.path.join(tmpdir, "state.json")

        first = self.new_server(data_file=data_file)
        a, b, _begin, claim, _awaited = self.pair(first)
        pair_id, key = claim.pair_id, claim.key

        a.sync([(relay.EV_HUG_START, b"")], relay.LOCAL_FLAG_HOLDING)
        time.sleep(0.5)  # let the debounced flusher write
        self.assertTrue(os.path.exists(data_file), "the server should have written the state file")
        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        self.assertIn(pair_id.hex(), snapshot["pairs"])
        first.stop()  # also forces a final save
        self.assertTrue(os.path.exists(data_file))

        second = self.new_server(data_file=data_file)
        self.assertEqual(second.relay.server_seq, first.relay.server_seq)
        self.assertIn(pair_id.hex(), second.relay.pairs)

        # same clients, same pair id / key, new port
        a.port = second.port
        b.port = second.port
        a.pair_id = pair_id
        a.key = key
        b.pair_id = pair_id
        b.key = key

        pending = b.sync([], 0)  # verifies the HMAC with the original key
        self.assertEqual(pending.status, relay.ST_OK)
        self.assertIn(relay.EV_HUG_START, pending.types, "the queued inbox event survived")
        self.assertEqual(pending.pair_state, relay.PAIR_STATE_PAIRED)
        self.assertEqual(pending.partner_online, 1)

        # both directions still work
        self.assertEqual(a.sync([(relay.EV_MISS, b"")], 0).status, relay.ST_OK)
        self.assertEqual(b.sync([], 0).types, [relay.EV_MISS])
        self.assertEqual(a.sync([], 0).partner_online, 1)

    def test_wrong_claim_attempts_survive_a_restart(self):
        tmpdir = self.tmpdir()
        data_file = os.path.join(tmpdir, "state.json")

        first = self.new_server(data_file=data_file)
        a = self.new_client(first, "Alice")
        b = self.new_client(first, "Mallory")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        for offset in range(1, 4):
            self.assertEqual(b.claim(self.wrong_code(begin.code, offset)).status,
                             relay.ST_CODE_NOT_FOUND)
        first.stop()  # forces a final save
        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        self.assertEqual(snapshot["pending"][a.dev_id.hex()]["attempts"], 3)

        second = self.new_server(data_file=data_file)
        self.assertEqual(second.relay.pending[a.dev_id.hex()]["attempts"], 3,
                         "the attempt counter must be restored from the state file")
        b.port = second.port
        for offset in range(4, relay.MAX_CODE_ATTEMPTS + 1):
            self.assertEqual(b.claim(self.wrong_code(begin.code, offset)).status,
                             relay.ST_CODE_NOT_FOUND)
        self.assertEqual(b.claim(begin.code).status, relay.ST_CODE_NOT_FOUND,
                         "the remaining attempts after the restart still burn the code")

    def test_state_without_attempts_loads_as_zero(self):
        tmpdir = self.tmpdir()
        data_file = os.path.join(tmpdir, "state.json")

        first = self.new_server(data_file=data_file)
        a = self.new_client(first, "Alice")
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        first.stop()  # forces a final save
        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        for entry in snapshot["pending"].values():
            entry.pop("attempts", None)  # what an older relay wrote
        with open(data_file, "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle)

        second = self.new_server(data_file=data_file)
        self.assertEqual(second.relay.pending[a.dev_id.hex()]["attempts"], 0)
        b = self.new_client(second, "Bob")
        self.assertEqual(b.claim(begin.code).status, relay.ST_OK,
                         "a state file without the field must still pair")

    def test_corrupt_state_file_is_a_clean_start(self):
        tmpdir = self.tmpdir()
        data_file = os.path.join(tmpdir, "state.json")
        with open(data_file, "w", encoding="utf-8") as handle:
            handle.write("{not json at all")
        server = self.new_server(data_file=data_file)
        self.assertEqual(server.relay.pairs, {})
        a = self.new_client(server, "Alice")
        self.assertEqual(a.begin().status, relay.ST_OK)


class T11Framing(RelayTestCase):
    def test_round_trip_every_frame_type(self):
        all_types = sorted(relay.KNOWN_TYPES)
        self.assertEqual(len(all_types), 10)
        for index, ftype in enumerate(all_types, start=1):
            key = b"k" * 32 if ftype in relay.SIGNED_TYPES else None
            payload = bytes((0xAB, index))
            frame = relay.encode_frame(ftype, index, index - 1, payload, key)
            out_type, seq, ack, out_payload, mac = relay.split_frame(frame)
            self.assertEqual(out_type, ftype)
            self.assertEqual(seq, index)
            self.assertEqual(ack, index - 1)
            self.assertEqual(out_payload, payload)
            if key is None:
                self.assertEqual(mac, b"")
                self.assertEqual(len(frame), relay.HEADER_LEN + len(payload))
            else:
                self.assertEqual(len(mac), relay.HMAC_LEN)
                self.assertEqual(len(frame), relay.HEADER_LEN + len(payload) + relay.HMAC_LEN)
                expected = hmac.new(key, frame[:-relay.HMAC_LEN], hashlib.sha256).digest()[: relay.HMAC_LEN]
                self.assertTrue(hmac.compare_digest(expected, mac))
                relay.verify_frame_mac(frame, key)
                with self.assertRaises(relay.RequestError) as caught:
                    relay.verify_frame_mac(frame, b"wrong" * 7)
                self.assertEqual(caught.exception.status, relay.ST_UNAUTHORIZED)

    def test_encoder_rejects_oversized_payload(self):
        with self.assertRaises(ValueError):
            relay.encode_frame(relay.T_PAIR_BEGIN_REQ, 1, 0, b"x" * (relay.MAX_PAYLOAD + 1))
        relay.encode_frame(relay.T_PAIR_BEGIN_REQ, 1, 0, b"x" * relay.MAX_PAYLOAD)

    def test_encoder_rejects_unknown_type_and_unsigned_signed_type(self):
        with self.assertRaises(ValueError):
            relay.encode_frame(0x7F, 1, 0, b"")
        with self.assertRaises(ValueError):
            relay.encode_frame(relay.T_SYNC_REQ, 1, 0, b"")
        with self.assertRaises(ValueError):
            relay.encode_frame(relay.T_UNPAIR_REQ, 1, 0, b"", None)

    def test_split_frame_rejects_malformed_frames(self):
        cases = [
            b"",
            relay.MAGIC,
            b"XX" + relay.encode_frame(relay.T_PAIR_BEGIN_REQ, 1, 0, b"")[2:],
            struct.pack(">2sBBHII", relay.MAGIC, 2, relay.T_PAIR_BEGIN_REQ, 0, 1, 0),
            struct.pack(">2sBBHII", relay.MAGIC, relay.VERSION, 0x7F, 0, 1, 0),
            # payload_len above the cap
            struct.pack(">2sBBHII", relay.MAGIC, relay.VERSION, relay.T_PAIR_BEGIN_REQ,
                        relay.MAX_PAYLOAD + 1, 1, 0),
            # declared payload missing
            struct.pack(">2sBBHII", relay.MAGIC, relay.VERSION, relay.T_PAIR_BEGIN_REQ, 4, 1, 0) + b"ab",
            # signed frame that is one byte short of its HMAC
            relay.encode_frame(relay.T_SYNC_REQ, 1, 0, b"", b"k" * 32)[:-1],
        ]
        for frame in cases:
            with self.assertRaises(relay.RequestError) as caught:
                relay.split_frame(frame)
            self.assertEqual(caught.exception.status, relay.ST_BAD_REQUEST, repr(frame))

    def test_every_response_begins_with_a_status_byte(self):
        server = self.new_server()
        routes = [
            ("/v1/pair/begin", relay.T_PAIR_BEGIN_RSP),
            ("/v1/pair/claim", relay.T_PAIR_CLAIM_RSP),
            ("/v1/pair/await", relay.T_PAIR_AWAIT_RSP),
            ("/v1/sync", relay.T_SYNC_RSP),
            ("/v1/unpair", relay.T_UNPAIR_RSP),
        ]
        client = self.new_client(server, "Mallory")
        for path, rsp_type in routes:
            body = client.post(path, b"", with_device_id=False)
            ftype, _seq, _ack, payload, _mac = relay.split_frame(body)
            self.assertEqual(ftype, rsp_type, path)
            self.assertGreaterEqual(len(payload), 1, path)
            self.assertEqual(payload[0], relay.ST_BAD_REQUEST, path)
            if rsp_type == relay.T_SYNC_RSP:
                # status + count + seq/time + online/holding/pair_state + energy
                self.assertEqual(len(payload), 17, "error sync bodies stay well formed")
                self.assertEqual(payload[13:17], b"\x00\x00\x00\x00", "no pair, no energy")
            if rsp_type == relay.T_PAIR_BEGIN_RSP:
                self.assertEqual(len(payload), 41, "error begin bodies stay well formed")
                self.assertEqual(payload[9:41], relay.ZERO_KEY, "no real token is leaked")

    def test_healthz(self):
        server = self.new_server()
        client = self.new_client(server, "Operator")
        status, body = client.get("/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"ok")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            client.get("/nope")
        self.assertEqual(caught.exception.code, 404)

    def test_parse_helpers(self):
        events, flags = relay.parse_sync_request(
            relay.encode_events([(relay.EV_MISS, b""), (relay.EV_HUG_END, b"\x01\x02")]) + b"\x01"
        )
        self.assertEqual(events, [(relay.EV_MISS, b""), (relay.EV_HUG_END, b"\x01\x02")])
        self.assertEqual(flags, 1)
        self.assertEqual(relay.parse_begin_request(b"\x05Alice"), "Alice")
        self.assertEqual(relay.parse_begin_request(bytes((relay.MAX_NAME_BYTES,)) + b"a" * relay.MAX_NAME_BYTES),
                         "a" * relay.MAX_NAME_BYTES)
        self.assertEqual(relay.parse_claim_request(b"234567\x03Bob"), ("234567", "Bob"))
        over_long = bytes((relay.MAX_NAME_BYTES + 1,)) + b"a" * (relay.MAX_NAME_BYTES + 1)
        for bad in (b"", b"\x0d", b"\x01a\x00", b"\x01" + b"a" * 65, b"\x01\xff", over_long):
            with self.assertRaises(relay.RequestError) as caught:
                relay.parse_begin_request(bad)
            self.assertEqual(caught.exception.status, relay.ST_BAD_REQUEST)
        with self.assertRaises(relay.RequestError):
            relay.parse_sync_request(
                relay.encode_events([(relay.EV_MISS, b"")] * (relay.MAX_EVENTS_PER_SYNC + 1)) + b"\x00"
            )
        with self.assertRaises(relay.RequestError):
            relay.parse_sync_request(relay.encode_events([(relay.EV_HUG_END, b"\x00\x01")]))
        self.assertEqual(len(relay.error_rsp_payload(relay.T_PAIR_AWAIT_RSP, relay.ST_BAD_REQUEST)), 1)


class T12HalfBornPair(RelayTestCase):
    """A pair whose initiator never received its key must not be a dead end.

    ``handle_claim`` creates the pair immediately, but the initiator only
    learns its identity from one long-poll answer.  If that answer never
    arrives the server holds a live pair that one side knows nothing about:
    its next "begin" would be answered ALREADY_PAIRED forever, and it cannot
    unpair either, because it never learned the pair id.  The server heals
    that state from the caller's own evidence -- a signed sync.
    """

    def half_born_pair(self, server, name_a="Alice", name_b="Bob"):
        """Pair two devices and hand back both clients plus the pair id."""
        a, b, _begin, claim, _awaited = self.pair(server, name_a=name_a, name_b=name_b)
        a.pair_id, a.key = claim.pair_id, claim.key
        b.pair_id, b.key = claim.pair_id, claim.key
        return a, b, claim.pair_id

    def age_pair(self, server, pair_id, seconds):
        """Backdate a pair's creation stamp, as if it were formed long ago."""
        with server.relay.lock:
            server.relay.pairs[pair_id.hex()]["created"] -= seconds

    def test_begin_heals_a_pair_older_than_the_grace_period(self):
        server = self.new_server()
        a, b, pair_id = self.half_born_pair(server)

        # inside the grace period the pair is still believed to be forming
        self.assertEqual(a.begin().status, relay.ST_ALREADY_PAIRED)

        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)
        begin = a.begin()
        self.assertEqual(begin.status, relay.ST_OK,
                         "a pair this device never confirmed must not block it")

        # the partner is released through the ordinary unpair path
        result = b.sync([], 0)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertIn(relay.EV_UNPAIR, result.types)
        self.assertEqual(result.pair_state, relay.PAIR_STATE_PEER_LEFT)
        self.assertEqual(result.partner_online, 0)

        # and the healed pair can be re-formed from the same code
        claim = b.claim(begin.code)
        self.assertEqual(claim.status, relay.ST_OK)
        self.assertNotEqual(claim.pair_id, pair_id)
        self.assertEqual(claim.partner_id, a.dev_id)
        awaited = a.await_pair(begin.token)
        self.assertEqual(awaited.state, relay.AWAIT_PAIRED)
        self.assertEqual(awaited.pair_id, claim.pair_id)
        self.assertEqual(a.sync([(relay.EV_MISS, b"")], 0).status, relay.ST_OK)
        self.assertEqual(b.sync([], 0).types, [relay.EV_MISS])

    def test_partner_confirmation_does_not_count_for_the_caller(self):
        server = self.new_server()
        a, b, pair_id = self.half_born_pair(server)
        self.assertEqual(b.sync([], 0).status, relay.ST_OK)  # only b proved its key
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)

        self.assertEqual(a.begin().status, relay.ST_OK,
                         "the partner's traffic must not vouch for this device")
        self.assertEqual(b.sync([], 0).pair_state, relay.PAIR_STATE_PEER_LEFT)

    def test_a_confirmed_pair_is_left_alone(self):
        server = self.new_server()
        a, b, pair_id = self.half_born_pair(server)
        self.assertEqual(a.sync([], 0).status, relay.ST_OK)
        self.assertEqual(b.sync([], 0).status, relay.ST_OK)
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)

        self.assertEqual(a.begin().status, relay.ST_ALREADY_PAIRED)
        self.assertEqual(b.begin().status, relay.ST_ALREADY_PAIRED)
        self.assertIn(pair_id.hex(), server.relay.pairs)
        self.assertFalse(server.relay.pairs[pair_id.hex()]["dissolved"])

    def test_claim_heals_a_pair_the_claimer_lost(self):
        server = self.new_server()
        a, b, pair_id = self.half_born_pair(server)
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)

        carol = self.new_client(server, "Carol")
        begin = carol.begin()
        self.assertEqual(begin.status, relay.ST_OK)
        claim = b.claim(begin.code)  # b lost its key, so it may pair again
        self.assertEqual(claim.status, relay.ST_OK,
                         "a claimer that never confirmed must not be locked out")
        self.assertNotEqual(claim.pair_id, pair_id)
        self.assertEqual(claim.partner_id, carol.dev_id)

        # the left-behind partner is told by the unpair path
        result = a.sync([], 0)
        self.assertEqual(result.status, relay.ST_OK)
        self.assertIn(relay.EV_UNPAIR, result.types)
        self.assertEqual(result.pair_state, relay.PAIR_STATE_PEER_LEFT)

    def test_confirmation_survives_a_restart(self):
        tmpdir = self.tmpdir()
        data_file = os.path.join(tmpdir, "state.json")

        first = self.new_server(data_file=data_file)
        a, b, pair_id = self.half_born_pair(first)
        self.assertEqual(a.sync([], 0).status, relay.ST_OK)
        self.assertEqual(b.sync([], 0).status, relay.ST_OK)
        first.stop()  # forces a final save

        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        seen = snapshot["pairs"][pair_id.hex()]["seen"]
        self.assertEqual(set(seen), {a.dev_id.hex(), b.dev_id.hex()})

        second = self.new_server(data_file=data_file)
        a.port = b.port = second.port
        self.age_pair(second, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)
        self.assertIn(pair_id.hex(), second.relay.pairs)
        self.assertEqual(a.begin().status, relay.ST_ALREADY_PAIRED,
                         "the confirmation evidence must survive a restart")

    MISSING = object()

    def saved_state(self, seen_builder):
        """Pair a fresh couple, sync both sides, and doctor the saved ``seen``.

        ``seen_builder(a, b)`` returns the value to store, or ``MISSING`` to
        delete the field altogether (a state file from an older relay).
        """
        data_file = os.path.join(self.tmpdir(), "state.json")
        server = self.new_server(data_file=data_file)
        a, b, pair_id = self.half_born_pair(server)
        self.assertEqual(a.sync([], 0).status, relay.ST_OK)
        self.assertEqual(b.sync([], 0).status, relay.ST_OK)
        server.stop()  # forces a final save

        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        value = seen_builder(a, b)
        if value is self.MISSING:
            snapshot["pairs"][pair_id.hex()].pop("seen")
        else:
            snapshot["pairs"][pair_id.hex()]["seen"] = value
        with open(data_file, "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle)
        return data_file, a, b, pair_id

    def test_state_file_without_seen_still_loads(self):
        # a state file written by an older relay has no "seen" field at all
        data_file, a, b, pair_id = self.saved_state(lambda a, b: self.MISSING)

        server = self.new_server(data_file=data_file)
        a.port = b.port = server.port
        self.assertEqual(server.relay.pairs[pair_id.hex()]["seen"], {},
                         "a missing field is materialised as an empty history")
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)
        self.assertEqual(a.begin().status, relay.ST_OK,
                         "an unknown confirmation history reads as unconfirmed")

    def test_a_seen_field_of_the_wrong_type_loads_as_empty(self):
        data_file, a, b, pair_id = self.saved_state(lambda a, b: "not a dict")

        server = self.new_server(data_file=data_file)
        a.port = b.port = server.port
        self.assertIn(pair_id.hex(), server.relay.pairs,
                      "one unusable field must not cost the whole state file")
        self.assertEqual(server.relay.pairs[pair_id.hex()]["seen"], {})
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)
        self.assertEqual(a.begin().status, relay.ST_OK)

    def test_unusable_seen_entries_are_dropped(self):
        def mixed(a, b):
            return {
                a.dev_id.hex(): 1792489400.5,
                "0011223344556677": 1792489400.5,   # valid hex, not a member
                "zz": 1.0,                          # not hex
                "abcd": 1.0,                        # wrong length
                b.dev_id.hex(): "nope",             # not a timestamp
            }

        data_file, a, b, pair_id = self.saved_state(mixed)
        server = self.new_server(data_file=data_file)
        a.port = b.port = server.port
        self.assertEqual(set(server.relay.pairs[pair_id.hex()]["seen"]), {a.dev_id.hex()})
        self.age_pair(server, pair_id, relay.PAIR_CONFIRM_GRACE_SEC + 60.0)
        self.assertEqual(a.begin().status, relay.ST_ALREADY_PAIRED)
        self.assertEqual(b.begin().status, relay.ST_OK,
                         "the surviving entry covers only its own device")


# ------------------------------------------------------- love energy


class T10LoveEnergy(RelayTestCase):
    """The relay, not the handheld, is the authority on Love Energy.

    Both devices keep counting locally so the home screen never waits for the
    network, but every sync answer now carries the pair total and the device
    overwrites its own number with it.  Without that the two screens drift
    apart for good the first time an event fails to arrive -- which is exactly
    what the two handhelds were caught doing (91 against 84).

    The rules asserted here are the device-side rules restated on the server
    (main/couple_state.c): a miss pays 1, a heart sync pays 5, a two-sided hug
    pays 5, a hug pays nothing however long it lasted, a hug too short to count
    is not an interaction at all, the day's first interaction pays a 3-point
    streak bonus once, and a total landing exactly on a milestone pays the
    milestone itself once.
    """

    # pairing plus a dozen syncs: the short poll keeps this class quick
    long_poll_sec = 0.5

    def couple(self, server, name_a="Alice", name_b="Bob"):
        a, b, _begin, claim, _awaited = self.pair(server, name_a=name_a, name_b=name_b)
        a.pair_id, a.key = claim.pair_id, claim.key
        b.pair_id, b.key = claim.pair_id, claim.key
        return a, b, claim.pair_id

    def ledger(self, server, pair_id):
        with server.relay.lock:
            return dict(server.relay.pairs[pair_id.hex()]["score"])

    def energy_seen_by(self, client):
        """What this device's screen will show after its next sync."""
        result = client.sync()
        self.assertEqual(result.status, relay.ST_OK)
        self.assertIsNotNone(result.love_energy, "sync answers must carry Love Energy")
        return result.love_energy

    def test_a_miss_is_worth_one_point_on_both_screens(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        sent = a.sync([(relay.EV_MISS, b"")])
        self.assertEqual(sent.love_energy, 1)
        self.assertEqual(b.sync().count_of(relay.EV_MISS), 1, "the miss must reach the other side")
        self.assertEqual(self.energy_seen_by(b), 1)
        self.assertEqual(self.energy_seen_by(a), 1, "both screens must show the same number")

    def test_a_hug_counts_as_an_interaction_but_pays_no_energy(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        result = a.sync([(relay.EV_HUG_END, struct.pack(">H", 1200))])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["energy"], 0, "a hug is measured in time, not in points")
        self.assertEqual(score["total"], 1, "but it is still an interaction")
        self.assertEqual(result.love_energy, 0)
        self.assertEqual(self.energy_seen_by(b), 0)

    def test_a_hug_too_short_to_count_changes_nothing_at_all(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        a.sync([(relay.EV_HUG_END, struct.pack(">H", relay.HUG_TIER_SMALL_MS - 1))])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["energy"], 0)
        self.assertEqual(score["total"], 0)
        self.assertEqual(score["day_first"], 0, "not even the day's first interaction")

    def test_mutual_misses_within_the_window_pay_a_heart_sync(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        a.sync([(relay.EV_MISS, b"")])
        b.sync([(relay.EV_MISS, b"")])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["energy"], relay.ENERGY_MISS * 2 + relay.ENERGY_HEART_SYNC)
        self.assertEqual(score["total"], 2, "a heart sync is not an interaction of its own")
        self.assertEqual(self.energy_seen_by(a), 7)
        self.assertEqual(self.energy_seen_by(b), 7)

    def test_a_two_sided_hold_pays_a_hug_sync(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        a.sync([(relay.EV_HUG_START, b"")], local_flags=0x01)
        self.assertEqual(self.ledger(server, pair_id)["energy"], 0, "starting a hug pays nothing")
        b.sync([(relay.EV_HUG_START, b"")], local_flags=0x01)
        score = self.ledger(server, pair_id)
        self.assertEqual(score["energy"], relay.ENERGY_HUG_SYNC)
        self.assertEqual(score["total"], 0)
        self.assertEqual(self.energy_seen_by(a), relay.ENERGY_HUG_SYNC)
        self.assertEqual(self.energy_seen_by(b), relay.ENERGY_HUG_SYNC)

    def test_a_replayed_sync_is_not_scored_twice(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)

        _seq, raw = a.build_sync([(relay.EV_MISS, b"")])
        a.send_sync(raw)
        self.assertEqual(self.ledger(server, pair_id)["energy"], 1)

        replay = a.send_sync(raw)
        self.assertEqual(replay.status, relay.ST_OK)
        score = self.ledger(server, pair_id)
        self.assertEqual(score["energy"], 1, "a replayed sequence number must not pay again")
        self.assertEqual(score["total"], 1)

    def test_landing_on_a_milestone_pays_it_exactly_once(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)
        with server.relay.lock:
            score = server.relay.pairs[pair_id.hex()]["score"]
            score["total"] = 519        # one miss away from 520
            score["energy"] = 100       # a number she would not want to lose

        a.sync([(relay.EV_MISS, b"")])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["total"], 520)
        self.assertEqual(score["energy"], 100 + relay.ENERGY_MISS + 520)

        a.sync([(relay.EV_MISS, b"")])
        self.assertEqual(self.ledger(server, pair_id)["energy"], 100 + 1 + 520 + 1,
                         "moving past a milestone must not pay it again")

    def test_the_daily_streak_pays_once_on_the_first_interaction_of_a_day(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)
        today = relay._day_index(time.time())
        with server.relay.lock:
            score = server.relay.pairs[pair_id.hex()]["score"]
            score.update({"day": today - 1, "day_first": 1, "streak": 4, "last_day": today - 1})

        a.sync([(relay.EV_MISS, b"")])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["streak"], 5, "yesterday's streak continues")
        self.assertEqual(score["energy"], relay.ENERGY_DAILY_STREAK + relay.ENERGY_MISS)

        # a second miss on the same day continues the streak but pays no bonus
        a.sync([(relay.EV_MISS, b"")])
        self.assertEqual(self.ledger(server, pair_id)["energy"],
                         relay.ENERGY_DAILY_STREAK + relay.ENERGY_MISS * 2)

    def test_a_missed_day_restarts_the_streak_without_punishing_anyone(self):
        server = self.new_server()
        a, b, pair_id = self.couple(server)
        today = relay._day_index(time.time())
        with server.relay.lock:
            score = server.relay.pairs[pair_id.hex()]["score"]
            # the last interaction was three days ago
            score.update({"day": today - 1, "day_first": 1, "streak": 9, "last_day": today - 3})

        a.sync([(relay.EV_MISS, b"")])
        score = self.ledger(server, pair_id)
        self.assertEqual(score["streak"], 1, "the streak restarts at one")
        self.assertEqual(score["energy"], relay.ENERGY_MISS, "and nothing is taken away")

    def test_the_ledger_survives_a_restart(self):
        data_file = os.path.join(self.tmpdir(), "state.json")
        server = self.new_server(data_file=data_file)
        a, b, pair_id = self.couple(server)
        a.sync([(relay.EV_MISS, b"")])
        server.stop()  # forces a final save

        restarted = self.new_server(data_file=data_file)
        a.port = b.port = restarted.port
        self.assertEqual(self.ledger(restarted, pair_id)["energy"], 1)
        self.assertEqual(self.energy_seen_by(b), 1)

    def test_a_state_file_without_a_ledger_still_loads(self):
        data_file = os.path.join(self.tmpdir(), "state.json")
        server = self.new_server(data_file=data_file)
        a, b, pair_id = self.couple(server)
        server.stop()

        with open(data_file, "r", encoding="utf-8") as handle:
            snapshot = json.load(handle)
        snapshot["pairs"][pair_id.hex()].pop("score")
        with open(data_file, "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle)

        restarted = self.new_server(data_file=data_file)
        a.port = b.port = restarted.port
        self.assertIn(pair_id.hex(), restarted.relay.pairs,
                      "a missing ledger must not cost the pairing")
        self.assertEqual(self.ledger(restarted, pair_id)["energy"], 0)
        self.assertEqual(self.energy_seen_by(b), 0)


# ---------------------------------------------------------------- entry


def _summary(result):
    print("-" * 68)
    print("tests run: %d | failures: %d | errors: %d | skipped: %d"
          % (result.testsRun, len(result.failures), len(result.errors), len(result.skipped)))
    if result.wasSuccessful():
        print("ALL TESTS PASSED")
    else:
        print("TESTS FAILED")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    runner = unittest.TextTestRunner(verbosity=2)
    sys.exit(_summary(runner.run(suite)))
