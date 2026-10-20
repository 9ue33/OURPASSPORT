<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI PASSPORT relay server

A small, dependency-free internet relay for a pair of ESP32-C3 **AI PASSPORT**
handhelds. Two devices register with the relay, pair with each other using a
short one-time code, and from then on exchange "hug" and heartbeat events
through it. The relay also tells each device whether its partner is currently
online and whether the partner is holding their passport right now — that is
what makes a hug feel immediate across the internet.

Everything is a single Python file (`relay.py`) that uses only the standard
library: no `pip install`, no database, no daemon framework. Requests are HTTP
`POST`s carrying a compact binary "HL" frame; replies are binary frames too.
There is exactly one plain-text health endpoint.

```
relay/
├── relay.py        the server
├── test_relay.py   end-to-end test suite (41 tests, stdlib unittest)
├── deploy/         systemd unit and Cloudflare Tunnel config templates
└── README.md       this file
```

## Requirements and running

- Python 3.9 or newer, standard library only.

```sh
cd relay
python3 relay.py --host 0.0.0.0 --port 8787 --data-file relay-state.json
```

The startup banner is printed to stdout and also logged:

```
AI PASSPORT relay AIPASSPORT-Relay/1.0 listening on http://0.0.0.0:8787 (data file: relay-state.json)
```

Options:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--host` | `0.0.0.0` | bind address |
| `--port` | `8787` | bind port |
| `--data-file` | `relay-state.json` next to `relay.py` | state file path |
| `--log-level` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL` |

If the port is already in use the relay logs `cannot bind <host>:<port>` and
exits with status 1 instead of crashing.

### Stopping it

Press **Ctrl+C** (SIGINT) or **Ctrl+Break** (SIGBREAK) in the console. The
relay runs its shutdown path — stop accepting requests, flush the state file,
log `relay stopped cleanly` — and exits with status 0.

> **Windows caveat:** `os.kill(pid, signal.SIGTERM)` and `taskkill /F` are hard
> kills on Windows. They terminate the process without running the shutdown
> path, so the last few seconds of state may not be written. Use Ctrl+C or
> Ctrl+Break for a clean stop. (SIGTERM handling is still registered for POSIX
> hosts and for consoles that can deliver it.)

## Deploying it on a server

The relay is a single file with no dependencies, so "deployment" is: get
`relay.py` onto a host that both devices can reach, and keep it running.

```sh
sudo mkdir -p /opt/aipassport-relay
sudo cp relay.py /opt/aipassport-relay/
sudo cp deploy/aipassport-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aipassport-relay
journalctl -u aipassport-relay -f
```

`deploy/aipassport-relay.service` binds `0.0.0.0:8787` and keeps its state in
`/opt/aipassport-relay/relay-state.json`. Edit both paths in the unit if your
layout differs. For an internet-facing relay, prefer the tunnel setup below and
bind `--host 127.0.0.1` instead so nothing but the tunnel can reach the port.

Verify from anywhere:

```sh
curl -s http://<your-host>:8787/healthz    # -> ok
```

Then point the devices at it: either the provisioning page (`Heartlink-XXXX`
SoftAP → `http://192.168.4.1/`) or the compile-time default
`COUPLE_DEFAULT_RELAY_URL` in `../main/couple_config.h`. The address must be
plain `http://` — the firmware has no TLS stack.

### Optional: hide the origin behind Cloudflare Tunnel

The devices cannot speak TLS, so exposing the relay's port directly means
plaintext frames on the open internet. A Cloudflare Tunnel avoids that without
touching the firmware: Cloudflare terminates TLS for your hostname, and the
relay only ever sees a loopback connection.

`deploy/cloudflared-config.yml.example` is a filled-in template; the steps are
in its header comment. Two things that will silently break the pairing if you
get them wrong:

- The hostname's **Always Use HTTPS** toggle must be **off**. A 301 redirect is
  not something the device can follow, so every sync fails.
- Keep the scheme in the firmware at `http://` even though users reach the
  hostname over `https://` — that conversion happens at Cloudflare, not on the
  device.

### Health check

```sh
curl -s -i http://127.0.0.1:8787/healthz
```

```
HTTP/1.1 200 OK
Server: AIPASSPORT-Relay/1.0
Content-Type: text/plain; charset=utf-8
Content-Length: 2

ok
```

Any other path, including a `GET` of a `/v1/*` route, gets `404` with the body
`not found`. Methods other than `GET` and `POST` get the standard library's
`501 Unsupported method`.

## Protocol overview

Every `/v1/*` endpoint takes one binary frame as the HTTP request body and
answers with one binary frame as the HTTP response body. Plain text is only
ever used for `/healthz` and for the `404`.

### Frame layout

```
offset  size  field        notes
0       2     magic        always ASCII "HL"
2       1     version      always 1
3       1     type         frame type, see the table below
4       2     payload_len  big-endian u16, 0..512
6       4     seq          big-endian u32, sender's frame counter
10      4     ack          big-endian u32, highest seq the sender accepted
14      N     payload      payload_len bytes
14+N    16    hmac         first 16 bytes of HMAC-SHA256 over bytes 0..14+N
```

The 16-byte HMAC tag is present **only** on the signed types `0x07`, `0x08`,
`0x09`, `0x0A` (sync and unpair). The unsigned pairing types never carry it.
The HMAC key is the 32-byte pair key handed out at claim time.

Frame types:

| Type | Name | Direction |
| --- | --- | --- |
| `0x01` | `PAIR_BEGIN_REQ` | device → relay |
| `0x02` | `PAIR_BEGIN_RSP` | relay → device |
| `0x03` | `PAIR_CLAIM_REQ` | device → relay |
| `0x04` | `PAIR_CLAIM_RSP` | relay → device |
| `0x05` | `PAIR_AWAIT_REQ` | device → relay |
| `0x06` | `PAIR_AWAIT_RSP` | relay → device |
| `0x07` | `SYNC_REQ` | device → relay (signed) |
| `0x08` | `SYNC_RSP` | relay → device (signed) |
| `0x09` | `UNPAIR_REQ` | device → relay (signed) |
| `0x0A` | `UNPAIR_RSP` | relay → device (signed) |

Validation is strict and a violation is reported as status `BAD_REQUEST`:

- body shorter than 14 bytes, bad magic, or a version other than 1;
- unknown frame type, or a type that does not match the endpoint;
- `payload_len > 512`, or a body length that does not equal
  `14 + payload_len` (plus 16 for signed types);
- an HMAC mismatch on a signed frame, or a signed frame with no key
  (reported as `UNAUTHORIZED`);
- a `seq` of 0 on sync (sequence numbers start at 1).

Oversized request bodies (over 4096 bytes) are read and discarded first so the
client still receives the error frame rather than a connection reset.

### Endpoints and required headers

| Endpoint | Request type | Headers (all hex, case-insensitive names) |
| --- | --- | --- |
| `POST /v1/pair/begin` | `0x01` | `X-Device-Id` — 16 bytes |
| `POST /v1/pair/claim` | `0x03` | `X-Device-Id` — 16 bytes |
| `POST /v1/pair/await` | `0x05` | `X-Device-Id` — 16 bytes, `X-Pair-Token` — 32 bytes |
| `POST /v1/sync` | `0x07` | `X-Device-Id` — 16 bytes, `X-Pair-Id` — 8 bytes |
| `POST /v1/unpair` | `0x09` | `X-Device-Id` — 16 bytes, `X-Pair-Id` — 8 bytes |

A missing, malformed, or wrong-length header is a `BAD_REQUEST`. `X-Device-Id`
is the device's own random 16-byte identity; it is the only credential the
pairing endpoints rely on, so a device that leaks its id can be impersonated
during setup. `X-Pair-Token` is the 32-byte secret returned by `pair/begin` and
is what proves that the caller of `pair/await` is the device that began the
pairing.

### Payloads

`PAIR_BEGIN_REQ` (body): `u8 name_len` + UTF-8 name (max 64 bytes).
`PAIR_BEGIN_RSP`: `u8 status` + 6-byte code + `u16` ttl seconds + 32-byte token
(41 bytes). On an error it keeps the same 41-byte shape with the code field
filled with `------`, a TTL of 0, and a 32-zero-byte token.

`PAIR_CLAIM_REQ`: 6-byte ASCII code + `u8 name_len` + UTF-8 name. The code is
upper-cased before lookup.
`PAIR_CLAIM_RSP` on success: `u8 status` + 8-byte pair id + 32-byte pair key +
16-byte partner device id + `u8 name_len` + the **awaiting** device's name.
On an error it is a bare status byte.

`PAIR_AWAIT_REQ` has an empty body — the identity comes from the
`X-Pair-Token` header.
`PAIR_AWAIT_RSP`: `u8 status` + `u8 state`. The state is `0` (still waiting,
retry), `1` (paired — followed by pair id + key + partner id + partner name,
exactly like a successful claim response) or `2` (the code expired).
Errors are a bare status byte.

`SYNC_REQ`: `u8 count` + `count` × (`u8 type`, `u8 len`, `len` value bytes) +
`u8 local_flags`. At most 12 events per request.
`SYNC_RSP` on success: `u8 status` + `u8 count` + the events +
`u32 server_seq` + `u32 server_time` (Unix seconds) + `u8 partner_online` +
`u8 partner_holding` + `u8 pair_state`. On an error it keeps this 13-byte
shape with an empty event list and zeroed flags.

`UNPAIR_REQ` has an empty body. `UNPAIR_RSP` is a bare status byte.

Every response frame carries `seq` = the relay's global server counter and
`ack` = the requesting device's `last_accepted` (0 for unsigned endpoints).
Error responses are **HTTP 200** with the failure reported inside the frame, so
a device always parses one uniform reply shape. A signed error reply is HMAC'd
with the pair key when the pair id resolves to a live pair (so the device can
verify it), and with 32 zero bytes when it does not.

### Status codes

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | `OK` | success |
| 1 | `BAD_REQUEST` | malformed frame, headers, or payload |
| 2 | `CODE_NOT_FOUND` | pairing code unknown |
| 3 | `CODE_EXPIRED` | pairing code known but past its TTL |
| 4 | `UNAUTHORIZED` | bad HMAC, bad token, or not a member of the pair |
| 5 | `ALREADY_PAIRED` | the device is already in a live pair (or the code was used) |
| 6 | `SERVER_BUSY` | no pairing code could be allocated |
| 7 | `PAIR_NOT_FOUND` | unknown `X-Pair-Id`, or a dissolved pair addressed by a non-member |

## Pairing

1. **Device A** calls `POST /v1/pair/begin` with its name. The relay returns a
   6-digit code (alphabet `0123456789`, because the device's pairing screen is
   a digit wheel), a 300-second TTL, and a 32-byte token. A device can
   only hold one pending code at a time; beginning again replaces the old one.
   A device that is already in a live pair gets `ALREADY_PAIRED`.
2. **Device A** long-polls `POST /v1/pair/await` with the token. It blocks for
   up to 25 seconds and answers `state=0` if nothing happened, so the device
   simply calls it again. An unknown or mismatched token is `UNAUTHORIZED`.
3. **Device B** calls `POST /v1/pair/claim` with the 6-digit code and its
   own name. On success both sides learn the 8-byte pair id and the 32-byte
   pair key. Device A's pending `await` returns `state=1` with B's identity.
4. An expired code returns `CODE_EXPIRED` to a client that still has it cached,
   and `CODE_NOT_FOUND` after a relay restart (see *State file* below).

Trying to claim your own code is `BAD_REQUEST`; claiming a code that has
already been used, or claiming while you are already paired, is
`ALREADY_PAIRED`. A device can be in at most one live pair; starting a new
pairing forgets any previously dissolved pair.

A wrong code is `CODE_NOT_FOUND`. Because digits leave only 10^6 codes, the
relay counts wrong claims: after 8 (`MAX_CODE_ATTEMPTS`) the pending entry is
deleted — the code is burned — and both it and every later claim keep getting
`CODE_NOT_FOUND`. Malformed claims, `BAD_REQUEST` (claiming your own code) and
`ALREADY_PAIRED` / `CODE_EXPIRED` outcomes do not use up attempts. A claim
carries no target id, so the relay cannot tell which pending session a wrong
guess was aimed at: each wrong claim is charged to every live pending session.

## Sync

`POST /v1/sync` is the steady-state call. Both devices poll it; a device should
keep one sync request in flight at a time.

**Client events** (sent in `SYNC_REQ`):

| Type | Name | Value |
| --- | --- | --- |
| `0x01` | `MISS` | none — "I miss you" |
| `0x02` | `HUG_START` | none — hug begins (tidings/grip closed) |
| `0x03` | `HUG_END` | 2 bytes — hug duration in milliseconds |

**Server events** (delivered in `SYNC_RSP`):

| Type | Name | Value |
| --- | --- | --- |
| `0x04` | `HUG_SYNC` | none — both passports were held at the same time |
| `0x05` | `HEART_SYNC` | none — both devices missed each other within the heartbeat window |
| `0x06` | `UNPAIR` | none — the partner left the pair |
| `0x07` | `PAIRED` | none |
| `0x08` | `OFFLINE_SUMMARY` | 1 byte — how many missed events happened while you were away |

Client-generated events are relayed to the partner; a device may not send the
server-generated types `0x04`–`0x08`.

**Response fields.**

- `partner_online` is 1 when the partner was seen within the last 45 seconds,
  else 0.
- `partner_holding` is 1 while the partner is holding their passport.
- `pair_state` is `0` (not paired), `1` (paired), or `2` (partner left).

**Holding.** `local_flags` bit 0 is the device's *live* grip level and is
authoritative: it is 1 while the button is held and 0 once released, and it
overrides the event-derived state from the same batch — except that a
`HUG_START` **in the same batch** always means holding. When both devices are
holding and the pair is not already latched, the relay broadcasts one
`HUG_SYNC` to both; the latch clears as soon as either side lets go, so one hug
produces exactly one `HUG_SYNC`.

**Heartbeat.** When a device sends `MISS`, the relay notes the time. If the
partner also sent `MISS` within a 10-second window, both miss markers are
cleared and a `HEART_SYNC` is broadcast to both devices.

**Offline tagging.** Each queued event is tagged with whether the recipient
looked offline at the moment it was produced. When the recipient next syncs and
its inbox contains any offline-tagged events, the relay prepends a single
`OFFLINE_SUMMARY` event carrying the count of offline `MISS` events, then
delivers everything in order. The tag survives a restart, so a device that was
offline still learns that things happened while it was away.

**Long polling and limits.** A sync request blocks until the caller has
something to deliver or 25 seconds pass, then returns with an empty event list.
At most 12 events are returned per response; the rest stay queued for the next
call. Each device inbox holds up to 64 events — if it overflows, the oldest is
dropped and the relay logs a warning.

**Retransmission.** A `SYNC_REQ` whose `seq` is less than or equal to the
device's `last_accepted` is treated as a retransmission: its events are *not*
ingested again, but the device is still marked as seen and gets a normal
response. This makes a retry after a lost reply safe.

## Unpair

`POST /v1/unpair` (signed, with `X-Pair-Id`) removes the caller from the pair
and immediately queues `UNPAIR` plus `pair_state=2` for the partner. The
partner can keep syncing that pair id until it unpairs as well — it gets the
pending `UNPAIR` event rather than an error, and any further syncs from it
return `partner_online=0`. The relay keeps the dissolved record until the last
member starts a new pairing, so the orphaned side always has a chance to learn
what happened.

## State file

`--data-file` (default `relay-state.json` next to `relay.py`) holds the whole
relay state as JSON: devices (last seen, `last_accepted`, holding, inbox with
offline tags), pairs (id, key, members, names, states, latch), the device → pair
index, and pending pairing sessions (code, token, expiry, wrong-claim count).
It is written atomically (temp file + `os.replace`), debounced to at most once
per second while dirty, and always written again on a clean shutdown. A missing
file is a normal first start; an unreadable or malformed file is logged as a
warning and the relay starts empty rather than refusing to boot. Expired
pending sessions are not carried across a restart.

> **Security note:** the state file stores pair keys and pending pair tokens in
> plaintext. Anyone who can read it can forge signed frames for those pairs or
> hijack a pending pairing. Treat it as a secret.

Because the code index is rebuilt from the state file, a pairing code that has
already expired is reported as `CODE_NOT_FOUND` after a restart even if the
device still has the code cached — the "expired" distinction only exists while
the relay process that issued the code is still running.

## Tests

```sh
cd relay
python3 test_relay.py
```

`test_relay.py` boots a real relay on an ephemeral port with a temporary state
file and drives it over real HTTP sockets with hand-built frames. It covers the
pairing happy path and its failure modes, sync rejections, retransmission,
event delivery, `HEART_SYNC`, offline summaries, hug overlap, unpair, restart
persistence, and framing validation — 41 tests in total. Because sync long
polls, the suite takes around 40 seconds.

## Known limitations

- **No TLS.** Frames travel in plain HTTP; run it behind a TLS-terminating
  proxy if the path between device and relay is untrusted.
- **Weak setup authentication.** Pairing is protected only by the 6-digit
  code plus the 32-byte token, and by the device id in the header. There is no
  account, no signature, and no general rate limiting: the only cap is that a
  pending code dies after 8 wrong claims, so a remote attacker can still hammer
  the endpoints — and burn live pending codes — at will.
- **One pair per device.** A device cannot belong to two pairs at once.
- **In-memory inbox.** Queued events live in RAM and are capped at 64; a very
  long offline stretch loses the oldest ones (the `OFFLINE_SUMMARY` count still
  tells the device that it missed something).
- **Thread per request.** `ThreadingHTTPServer` plus blocking long polls means
  one thread per in-flight sync; fine for a handful of devices, not a fleet.
- **No soak testing.** Behaviour under sustained load, clock changes, or a
  state file on a failing disk has not been measured.
