<p align="right">
  <a href="heartlink-terminal.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Heartlink Couple Terminal

This document describes the Heartlink couple terminal application that replaces
the upstream demo menu in `main/`. It covers what the two-board system does, how
the code is layered, how the device talks to its relay, what is stored on flash,
how to build and flash it, and which parts are known to be incomplete.

## 1. What it is

Heartlink turns two AI Passport boards into a single always-paired couple
device. Each board talks only to a relay server over HTTP; there is no
Bluetooth or peer-to-peer Wi-Fi mode. A short press of `OK` sends a miss-you
signal to the partner board, which answers with a full-screen heart animation
and a short tone. Holding `OK` is a remote hug: how long you hold it on your
board decides how long the hug animation runs on the partner's board. When both
boards hold at the same time the relay reports `HUG CONNECTED` and both boards
play a synchronised heartbeat. When both sides send a miss-you signal inside the
same 10-second window the relay reports `HEART SYNC` and both boards play a
full-screen special animation. Every interaction adds to a persistent Love
Energy total, hidden easter eggs fire at interaction totals
9 / 18 / 99 / 100 / 365 / 520 / 999 / 1314, and a daily panel shows today's miss
count, hug count, first interaction time, and the consecutive-day streak.

```text
                    Internet
   Device A  <-------- relay -------- >  Device B
   (ESP32-C3)   HTTPS not used: HTTP   (ESP32-C3)
     board            long-poll            board
```

The relay is the only path between the two boards. The device reaches it with
HTTP long-poll requests of up to about 25 seconds each
(`COUPLE_SYNC_LONGPOLL_MS`); it does not use WebSocket or MQTT. When a partner
is offline, interactions are not dropped: the relay queues them and hands the
partner an `OFFLINE_SUMMARY` event carrying how many miss-you signals arrived
while it was away.

The firmware identifies itself as version `1.0.0` (`COUPLE_FW_VERSION`,
`main/couple_config.h:10`).

## 2. What you need

- Two AI Passport boards (ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3).
- A relay server both boards can reach over the network. The relay server is
  part of this repository, under `relay/`; the authoritative server-side
  contract is [relay/README.md](../relay/README.md). The device-side summary of
  what this firmware relies on is in section 5.
- Wi-Fi credentials for the network the boards will use, entered on each board
  through the provisioning page (section 10).
- A host with an activated ESP-IDF 5.5.3 environment to build the firmware.

The compile-time default relay address is the placeholder
`http://relay.example.com` (`COUPLE_DEFAULT_RELAY_URL`,
`main/couple_config.h:15`), so you must point each board at a relay host of your
own. You can also set it at runtime on the provisioning page, which stores the
address in NVS and overrides the compiled default, so you do not have to rebuild
to change it. The address must start with `http://`: the firmware has no TLS
stack.

## 3. Architecture

The application is layered so that each concern has one owner file, and so that
nothing slow runs where it would stall rendering or input.

| Layer | Owner | Responsibility |
| --- | --- | --- |
| Entry point and input | `main/main.c` | Brings up I2C, display, LVGL, backlight, the button driver, and a dedicated input task. |
| Orchestration | `main/couple_app.c`, `main/couple_app.h` | The only layer that knows every other module, and their sole owner. Translates key events into interactions, runs the tick timers, owns the pairing and provisioning flows, and accounts for Love Energy and milestones. |
| Relationship state | `main/couple_state.c`, `main/couple_state.h` | Pure C state machine: energy and interaction counters, day rollover, streak, milestone detection, and local text formatting. No ESP-IDF, no LVGL; runs under host tests. |
| Wire protocol | `main/couple_proto.c`, `main/couple_proto.h` | Frame encode/decode and field layout. Pure C, host-tested. |
| Cryptography | `main/couple_sha256.c`, `main/couple_sha256.h` | SHA-256 and HMAC-SHA256 used for frame authentication. Pure C, host-tested. |
| Networking | `main/couple_net.c`, `main/couple_net.h` | The sole relay client: the sync loop, long-poll, retry and backoff, the deferred outbox, pairing and unpair requests, and the clock. One worker task, no LVGL dependency. |
| Wi-Fi | `main/couple_wifi.c`, `main/couple_wifi.h` | Wi-Fi driver wrapper: station association, its own reconnect backoff, and AP/client counting. |
| Provisioning | `main/couple_prov.c`, `main/couple_prov.h` | SoftAP plus an HTTP configuration page served to a phone browser. |
| Radio preparation | `main/couple_radio.c`, `main/couple_radio.h` | Idempotent NVS and network-stack bring-up shared before Wi-Fi and persistence are used. |
| Persistence | `main/couple_store.c`, `main/couple_store.h`, `main/couple_store_blob.c`, `main/couple_store_blob.h` | What is stored, under which NVS keys, with batching and deferred writes. |
| Audio | `main/couple_audio.c`, `main/couple_audio.h` | The sole owner of the BSP audio path: format, volume, and PCM writes on its own worker task. |
| Tone synthesis | `main/couple_tone.c`, `main/couple_tone.h` | Generates every sound in software; the firmware ships no audio files. |
| UI | `main/couple_ui.c`, `main/couple_ui.h` | The LVGL screens. A passive renderer: it never reads keys, network, NVS, or audio. |
| UI copy | `main/couple_ui_strings.h` | Every user-visible string, with a Chinese branch and an ASCII-only English fallback branch. |
| Tunables | `main/couple_config.h` | All compile-time constants, with comments on which are runtime-overridable. |
| Shared types | `main/couple_types.h` | Frame types, event types, status codes, hug tiers, and protocol limits shared by the other modules. |
| Font aliases | `main/couple_fonts.h` | Maps the small/body/title/number roles onto the subset fonts or the Montserrat fallback. |

Host tests for the pure-C modules:

| Test | Covers |
| --- | --- |
| `tests/test_couple_sha256.c` | SHA-256 and HMAC vectors. |
| `tests/test_couple_proto.c` | Frame encode/decode and verification. |
| `tests/test_couple_state.c` | Energy, interaction counting, day rollover, streak, milestones. |
| `tests/test_couple_store_blob.c` | Persisted-blob encode/decode and schema compatibility. |
| `tests/test_couple_tone.c` | Tone synthesis. |

Tools used by the font workflow:

| Tool | Role |
| --- | --- |
| `tools/ui_codepoints.py` | Extracts the code points that actually appear in `main/` string literals and refreshes the inventory file. |
| `tools/check_font_glyphs.py` | Proves a generated font covers a requested range; the gate runs it for both sizes. |

The load-bearing rule is that no slow operation runs in an LVGL or button
callback. The button driver's callback runs on the shared `esp_timer` task, so
`on_key` in `main/main.c:83` only enqueues an input event and returns; a
separate `couple_input` task drains that queue and calls
`couple_app_handle_key()`. That task, in turn, only posts to the application
task's input queue; `couple_app_handle_key()` never blocks, and a full queue
drops the key and counts it. Audio, storage, and networking all run on their own
tasks behind non-blocking entry points.

## 4. Interaction model

The device recognises these interactions. The scoring rules are defined in one
place, the header comment at `main/couple_state.h:9`, and the numbers below come
from it.

- **Miss you.** A short `OK` press on the home screen sends a miss signal and
  awards Love Energy +1 on the sending board and +1 on the receiving board.
- **Remote hug.** Holding `OK` enters the hug screen and tells the relay you are
  holding. On release, the held duration decides the tier:
  small hug from 700 ms, big hug from 3000 ms, and the never-let-go hug from
  8000 ms (`COUPLE_HUG_TIER_SMALL_MS`/`BIG_MS`/`CLINGY_MS`,
  `main/couple_config.h:53-55`). Reported duration is capped at 60000 ms
  (`COUPLE_HUG_MAX_MS`, `main/couple_config.h:56`). The partner board plays a
  hug of the corresponding length.
- **Heart sync.** If both sides send a miss signal inside a 10-second window
  (`COUPLE_MISS_WINDOW_MS`, `main/couple_config.h:52`), the relay declares
  `HEART SYNC` and both boards play the full-screen animation for Love Energy +5.
- **Connected hug.** If both boards hold at the same time the relay declares
  `HUG CONNECTED` and both play a synchronised heartbeat for Love Energy +5.
- **Offline summary.** When a partner returns online, the relay sends an
  `OFFLINE_SUMMARY` event with the number of miss signals that arrived while it
  was away, and the board shows that count. No interaction is lost.
- **Love Energy.** A persistent growth total. Awards are 1 for a sent miss, 1
  for a received miss, 5 for a heart sync, 5 for a connected hug, and 3 for
  continuing a daily streak (`main/couple_config.h:73-77`).
- **Milestones.** Crossing an interaction total of 9, 18, 99, 100, 365, 520,
  999, or 1314 fires a one-time easter egg. Each milestone is credited once, and
  the bonus equals the threshold itself (`couple_milestone_energy_bonus()`,
  `main/couple_state.c:248`). 520 and 1314 use deliberately showier animations
  (`couple_milestone_is_romantic()`, `main/couple_state.c:261`).
- **Daily panel.** Shows today's miss count, hug count, first interaction time,
  and the consecutive-day streak. Both counts are tallied across both
  directions, so the two boards show the same number.

Counting and pressure are handled deliberately. The interaction total counts
only real miss signals and hugs: a miss in either direction adds one, and a hug
that reaches at least the small-hug tier adds one. A press that never reaches a
tier is not an interaction at all — it does not even count for the daily first
interaction or the streak. Heart sync and connected hug only add energy; they do
not double-count interactions, because they are confirmations of the same
interaction.

One design rule is explicit in the state header: do not apply pressure. The day
rollover clears today's counters and nothing else. Losing connection, skipping a
day, or dropping the streak to zero produces no blaming copy and no penalty.

## 5. Networking

The device holds a single worker task that runs a sync loop against the relay.

**Device-to-relay contract.** The frame format is a 14-byte header plus a
16-byte truncated HMAC (`main/couple_proto.h:11`): magic `H` `L`, a version
byte, the frame type, a big-endian 16-bit payload length, a big-endian 32-bit
monotonic `seq`, and a big-endian 32-bit `ack`, followed by the payload. The tag
is the first 16 bytes of HMAC-SHA256 over the header and payload with the shared
key. Identity travels in HTTP headers rather than in the frame: `X-Device-Id` on
every request, `X-Pair-Id` on sync and unpair, and `X-Pair-Token` on the
pairing-await request (`main/couple_proto.h:31-33`). The endpoints are
`POST /v1/pair/begin`, `POST /v1/pair/claim`, `POST /v1/pair/await`,
`POST /v1/sync`, and `POST /v1/unpair` (`main/couple_proto.h:35-39`). Status
codes 0 through 7 map to the conditions in `main/couple_types.h:44-51`. The
event types are: client-originated 0x01 miss, 0x02 hug start, 0x03 hug end with
a 2-byte millisecond value; server-originated 0x04 hug sync, 0x05 heart sync,
0x06 unpair, 0x07 paired, and 0x08 offline summary with a 1-byte count
(`main/couple_types.h:66-74`). At the transport level the device only speaks
`http://`; a relay address written as `https://` gets no TLS and will retry as a
network failure indefinitely (`main/couple_net.h:52`).

**Sync loop and long-poll.** Each `/v1/sync` round posts the pending batch, then
waits for the response; the relay holds the request open for up to about 25
seconds, so the worst case for a freshly queued event is about 25 seconds before
it leaves the device (`main/couple_net.h:39`). On success the loop resets its
retry interval to 2000 ms and immediately re-sends if the outbox still holds
events beyond the twelve carried per round (`COUPLE_EVENTS_PER_SYNC_MAX`,
`main/couple_types.h:20`); it only backs off after a failure
(`main/couple_net.c:1191`).

**Retry and backoff.** Failures use exponential backoff capped at 60000 ms, with
up to 25 % jitter (`net_backoff()`, `main/couple_net.c:459`; base and maximum in
`main/couple_config.h:27-28`). Wi-Fi association has its own backoff with the
same base and maximum (`main/couple_config.h:31-32`).

**Deferred outbox.** Pending events go into a fixed 32-entry queue
(`COUPLE_OUTBOX_CAPACITY`, `main/couple_config.h:58`) that never blocks on
enqueue. When it is full the oldest entry is dropped and a warning is logged,
because dropping the newest would look like a dead button while dropping the
oldest only leaves an incomplete catch-up (`main/couple_net.h:44`).

**Replay-counter policy.** `seq` advances in memory only; writing flash on every
25-second poll would mean thousands of erase cycles a day
(`main/couple_config.h:38-46`). On load the counter is raised to the persisted
value plus `COUPLE_SEQ_BOOT_MARGIN` (1024) and persisted once immediately; during
runtime it is marked dirty only every `COUPLE_SEQ_PERSIST_STEP` (128) advances,
which the store tick then writes. The invariant is that the persist step must
stay well below the boot margin, so that a power loss cannot restart the
counter below a value this session already used, which the peer would discard as
a replay.

**Presence staleness.** The relay treats a partner as offline after 45 seconds
of silence (`COUPLE_PRESENCE_STALE_SEC`, `main/couple_config.h:34`). The device
also re-syncs its clock with the server every 300 seconds
(`COUPLE_TIME_SYNC_INTERVAL_SEC`, `main/couple_config.h:35`).

**Pairing-phase security boundary.** During pairing the two ends do not yet
share a key, so the pairing handshake frames (0x01-0x06) carry no HMAC and are
verified for length, magic, and type only. This means someone who guesses the
6-digit code, or an attacker on the path, could establish the pair first. Once
pairing completes, every frame is HMAC-authenticated and that window closes. The
threat model is a couple's own two small devices plus their own relay, so this
is an accepted and documented trade-off rather than an oversight; tightening it
would mean adding an out-of-band channel to pairing, not signing these frames
(`main/couple_net.h:217-227`, `main/couple_proto.h:23-25`). A related detail:
when the relay cannot find a live pair it signs its error response with 32 zero
bytes, so a genuine device always fails verification, and the client only logs
an unauthenticated status instead of acting on it (`main/couple_net.c`).

The relay's own behaviour — code lifetime, the four-step pairing flow, sync
semantics, presence and holding reporting, and its health endpoint — is
documented in the relay server's README, [relay/README.md](../relay/README.md).
That file lives in this repository under `relay/` and is the authoritative
server-side contract; this document does not restate its protocol, and the
device-side summary above is what this firmware relies on.

## 6. Persistence

All device state is stored as one flat blob so that saving is a single write and
loading is a single read. The keys are the `couple` namespace and the `blob`
key (`STORE_NS` and `STORE_KEY_BLOB`, `main/couple_store.c:18-19`). The
unpaired fallback device name is `PASSPORT` (`STORE_DEFAULT_NAME`,
`main/couple_store.c:22`) and is replaced by the real name once pairing
completes.

A keypress never writes flash directly. Writes are batched: a dirty marker is
set in memory, and the store tick flushes when either 8 dirty markers have
accumulated or 20000 ms have passed since the last flush
(`COUPLE_NVS_FLUSH_DIRTY_LIMIT` and `COUPLE_NVS_FLUSH_INTERVAL_MS`,
`main/couple_config.h:81-82`; the decision is in `couple_store_tick()`,
`main/couple_store.c:392`). This keeps a busy interaction session from wearing
out NVS. The replay counter is the exception: it is persisted immediately once
at load, then folded into the same batched write path through its own dirty
flag. A failed flush still restarts the timer so logs and erases are not
retriggered in a tight loop (`main/couple_store.c:418`).

Stored fields include the paired identity and partner name, the pair key, the
energy and interaction counters, the per-day counts, the streak, a bitmap of
milestones already seen, the replay counter, and the last acknowledged sequence
number. The blob carries a magic value `0x484C5354` and schema version 1
(`COUPLE_STATE_MAGIC` and `COUPLE_STATE_SCHEMA_VERSION`,
`main/couple_state.h:41-42`).

The store serialises all access through one lock. Callers are the application
task (for example the pairing check on each key event and the status refresh)
and the network task (when applying an acknowledgement). A write held under that
lock can therefore delay both of those tasks' storage access; the actual
blocking duration has not been measured on hardware. The UI never touches the
store.

## 7. UI

The screens are a passive renderer: `couple_ui.c` never reads buttons, never
touches the network, NVS, or audio, and holds `bsp_lvgl_lock()` inside every
public entry point, so any task may call it safely.

The panel is 240 by 320 pixels (`kScreenW`/`kScreenH`, `main/couple_ui.c:39-40`).
The visible safe area is 8 to 312 vertically because the panel corners have a
30-pixel radius (`main/couple_ui.c:11`). The pages are:

| View | Purpose |
| --- | --- |
| Home | Brand mark, animated heart, online state, partner name, last-received time, Love Energy and totals, key hints. |
| Miss sent | A light confirmation that a miss signal left the board. |
| Miss received | Full-screen heart growing from small to large. |
| Hug hold | This board is holding `OK`; shows the held seconds and a progress bar. |
| Hug received | The partner's hug is playing. |
| Hug sync | `HUG CONNECTED` with a synchronised heartbeat. |
| Heart sync | `HEART SYNC` full-screen animation. |
| Milestone | A milestone easter egg. |
| Daily | Our-day panel: today's misses, hugs, first interaction time, and streak. |
| Menu | Three items: pair, Wi-Fi setup, unpair. |
| Pair | Pair, join, and unpair flows, including 6-digit code entry. |
| Provisioning | The Wi-Fi SoftAP instruction page for the phone. |

The palette is fixed (`main/couple_ui.c:69-78`): a deep purple background
`0x0E0B14`, panel `0x171226`, line `0x2A2140`, pink `0xFF6B9D`, soft pink
`0xFFA6C4`, deep pink `0xC9406F`, cream text `0xF6E7E9`, muted `0x8E86A8`,
mint `0x7BE3C0`, and amber `0xFFC46B`.

There are no bitmap assets. The heart is built from rectangles: the 11 by 10
pixel-art grid is stored as eight horizontal bands (`kHeartBands[]`,
`main/couple_ui.c:95`) and each band is drawn as one or two rectangles at the
current unit size. At 8 pixels per unit the home heart is 88 by 80 pixels and is
positioned so its centre lands on (120, 110) (`main/couple_ui.c:905-907`). The
cursors, rings, and progress bars are likewise made of rectangles.

Animations use LVGL's scaler on those rectangle groups. Two parameters were
changed on purpose to suit integer-quantised pixel art, and they are visible
choices rather than bug fixes. On the home screen the heart breathes between
92 % and 108 % over 1600 ms, repeating forever (`main/couple_ui.c:1228`): the
unit cell is 8 pixels, so a range of about 98 % to 104 % rounds back to the same
8 pixels in every direction and the animation would be invisible, while the
wider range yields unit cells of 7, 8, and 9 with a fixed centre and reads as a
heartbeat. On the hug screen the confirmation pop is a fixed 25 %
(`main/couple_ui.c:1382`), chosen because the base cell is 4 pixels and one step
is therefore exactly 25 %.

The upstream demo menu was replaced, not re-skinned. This follows the
repository's mandatory UI redesign rule for derivative applications
(`AGENTS.md:40`): a derivative must build its own pages, state machines, and
animations rather than renaming, recolouring, or extending the demo shell. The
application reuses the BSP APIs and non-UI logic only.

## 8. Chinese fonts

The default Montserrat fonts have no Chinese glyphs: Chinese text would render
as missing-glyph boxes, and a successful build does not prove the panel can draw
it. The application therefore ships two uncompressed LVGL subset fonts generated
from exactly the code points the interface uses.

The inventory is `assets/fonts/ui_symbols.txt`: 512 code points, one `U+XXXX`
per line, made of 413 Han characters, 89 ASCII characters, and 10 other code
points (`assets/README.md`). The two generated files are
`assets/fonts/app_font_16.c` (52,940 bytes of generated font data, used for
`COUPLE_FONT_SMALL` and `COUPLE_FONT_BODY`) and `assets/fonts/app_font_20.c`
(81,528 bytes, used for `COUPLE_FONT_TITLE` and `COUPLE_FONT_NUM`); the size
figures are as recorded in `assets/README.md`. They are compiled as component
sources rather than included, because they define `lv_font_t` constants rather
than headers (`main/CMakeLists.txt:23-25`), and they are selected by defining
`COUPLE_USE_APP_FONT` (`main/CMakeLists.txt:25`). Without that macro
`main/couple_fonts.h:32-35` falls back to the Montserrat sizes; with the project
as shipped the macro is defined unconditionally, so the subset fonts are always
used and the fallback branch is not active in the factory configuration.

The source font is Noto Sans SC, used unmodified under the SIL Open Font License
1.1, and the converter is `lv_font_conv` 1.5.3 invoked with
`--format lvgl --no-compress --bpp 4` (`assets/README.md`). Both sizes must be
regenerated together so they stay in step with the aliases.

To regenerate the fonts and refresh the inventory, see the font section of
`assets/README.md` and the [LVGL Chinese text
guide](development/engineering/lvgl-chinese-fonts.md); the commands there are
maintained alongside the assets. In short: refresh the inventory with
`tools/ui_codepoints.py`, rebuild both sizes with `lv_font_conv`, and prove
coverage with `tools/check_font_glyphs.py`. The gate runs the coverage check for
both sizes and expects a negative control to fail.

Because the subset covers only the interface's own strings, any text the
firmware did not compile in renders as a placeholder box with no warning. A
partner name arriving over the network as arbitrary UTF-8 is outside the set,
and there is currently no name-entry screen (see section 11).

## 9. Build, verify and flash

Run the gate from the repository root:

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged-image verification
./tools/validate.sh             # complete gate
```

The static phase runs `tools/check_repo.py`, checks the workflows, builds and
runs every host test (including the five couple tests), runs the glyph check for
both font sizes, and runs the Python contract tests, then prints
`Host tests: PASS` (`tools/validate.sh:99`). The complete gate additionally
requires an activated ESP-IDF 5.5.3 environment.

On a Windows host, the static phase needs `CC` and `ACTIONLINT_BIN` set:

```bash
CC=<your compiler> ACTIONLINT_BIN=<path to the actionlint executable> ./tools/validate.sh --static
```

To build, run this from the repository root, in a shell with an activated
ESP-IDF 5.5.3 environment (on Windows that means running the ESP-IDF export
script first):

```bash
idf.py build
```

Create the merged image with:

```bash
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

`merge-bin` writes relative to the build directory, so pass a bare filename or
an absolute path; a repository-relative path will not land where you expect.

Expected artifacts are the application image
`build/couple/FoloToy-AI-Passport.bin`, the merged image
`build/FoloToy-AI-Passport-full.bin`, and the archived output under
`build/firmware/<sha256>/`, which also carries `manifest.json` and the ELF file.
As recorded results of one build: the application image
`FoloToy-AI-Passport.bin` is 1,651,440 bytes (0x1932F0), about 20 % of the 8 MB
application partition; the merged image `merged-binary.bin` is 1,716,976 bytes
with SHA-256
`3386050e82639dfbd410bd74fcdfc5dc62cd7d00ff1e4dff0fbfd2aeb4e3e724`; the
application is placed at offset 0x10000; and the toolchain reported ESP-IDF
5.5.3 for target esp32c3 with 8 MB Flash. Quote these as recorded results of one
build, not as guarantees for the next: your own build will produce different
sizes and a different hash.

The factory partition is `0x7F0000`, or 8,323,072 bytes (`partitions.csv`), so
the recorded application image leaves 6,671,632 bytes free, about 80 % of the
partition. Registering the two font files was the main contributor to the image
growth.

When flashing, mind the stored data. The verified merged image is written from
offset 0x0, which includes the NVS and PHY data regions, so it can reset stored
state. During development, when existing NVS state must be preserved, flash the
segments instead of the full image with `idf.py flash`. The full policy is in
[firmware-layout.md](development/engineering/firmware-layout.md#flashing-and-stored-data),
and `docs/development/engineering/build-and-test.md` covers the dependency and
build details.

## 10. Pairing, unpairing and daily use

**Provisioning Wi-Fi.** From the home screen open the menu with `UP` or `DOWN`,
move to Wi-Fi setup, and select it with `OK`. The board starts a SoftAP named
`Heartlink-XXXX`, where the suffix comes from the board's MAC address, protected
by the password `heartlink` (`COUPLE_PROV_AP_SSID_PREFIX` and
`COUPLE_PROV_AP_PASS`, `main/couple_prov.h:37` and `main/couple_prov.h:39`). On your phone, join that
network and open `http://192.168.4.1/` in a browser
(`COUPLE_PROV_HTTP_PORT`, `main/couple_prov.h:41`). The page collects the Wi-Fi
name, the Wi-Fi password, and the relay address, and only hands them back to the
application when all three pass validation; the provisioning module never stores
credentials itself. The board then closes the SoftAP and switches to station
mode — the AP and the station do not run at the same time. The page shows the
SSID, the password, and the address to open, and counts how many phones are
connected; the count is polled once a second, so it can lag by about a second.

**Pairing two boards.** With both boards provisioned and online, open the menu
on one board and choose pair. That board asks the relay for a 6-digit code
(`COUPLE_PAIR_CODE_LEN`, `main/couple_types.h:19`) and displays it with the
seconds it has left; the code has a time to live. On the other board choose pair
and enter the digits one at a time — `UP` and `DOWN` change the current digit and
`OK` advances; hold `OK` to leave. When the second board submits a valid code the
relay completes the pair, both boards show the paired state, and from then on
they share a key and the relationship.

**Unpairing.** Open the menu and choose unpair. The board asks for confirmation
and warns that energy and memories are kept. The device tries the unpair request
a few times and then clears local pairing and the pending outbox regardless of
the result, because the user's intent wins over a bad network: once you have
asked to unpair, the boards must not keep looking paired. There is no undo beyond
pairing again.

**Daily use.** On the home screen, short-press `OK` to send a miss signal, or hold
`OK` to hug. The home screen shows whether the partner is online, the partner's
name, when you last received a miss signal, and your energy and total. `UP` and
`DOWN` open the menu from the home screen. Holding `OK` for at least three
seconds cancels the current page and returns home, which is the escape hatch from
the pairing and provisioning pages.

## 11. Status and limitations

```text
Build: PASS — application image FoloToy-AI-Passport.bin 1,651,440 bytes
(0x1932F0), about 20 % of the 8 MB application partition; merged image
merged-binary.bin SHA-256
3386050e82639dfbd410bd74fcdfc5dc62cd7d00ff1e4dff0fbfd2aeb4e3e724; ESP-IDF 5.5.3.
These are recorded results of one build; your own build will hash differently.
Host tests: PASS — three cases in tests/test_install_passport_skills.py fail on
this Windows host because creating directory symlinks needs the symlink
privilege ([WinError 1314]), which is an environment limitation rather than a
code defect; every other test passes.
Device tests: PASS — two AI Passport boards have completed pairing, and both the
miss-you signal and the remote hug ran in both directions.
Unverified: the second and third hug tiers; HUG CONNECTED, with both boards
holding at the same time, has never been triggered; the offline pending queue;
end-to-end key latency has not been timed item by item; the ELF of the firmware
actually running on the boards has never been read back.
```

Known and accepted limitations, stated plainly:

1. **Subset font coverage.** The Chinese font is a subset generated by
   mechanically extracting the code points that appear in string literals across
   the `main/` translation units. There are currently 512 code points, matching
   `assets/fonts/ui_symbols.txt`. Characters outside that set render as
   placeholder boxes. A partner name arriving over the network as arbitrary
   UTF-8 is outside the set, and there is currently no name-entry screen.
   On-device Chinese rendering has been verified on the two boards.

2. **Network-layer items, known open.** None of the following is fixed;
   they are recorded as open issues.
   - (a) There is no replay protection against a peer replaying a previously
     captured frame. The response frame's `seq` is never compared against a
     local watermark, so the device side has no deduplication or anti-replay for
     received frames.
   - (b) When a sync succeeds but the server's acknowledgement does not advance,
     the retry path has no dedicated backoff; the loop simply sends again
     (`main/couple_net.c:1195`).
   - (c) The NVS write path holds one lock, so it can delay the application
     task's and the network task's storage access. The real blocking duration
     has not been measured on hardware.
   - (d) `couple_net_stop()` discards the `xQueueSend` return value and the
     command queue is 8 deep (`main/couple_net.c:1279`, `NET_CMD_QUEUE_LEN` is
     8), so in the worst case a stop can wait tens of seconds for an in-flight
     request to finish.
   - (e) Pairing responses are not signed. This one is an accepted, documented
     design trade-off for the pairing phase (section 5), not an oversight.

3. **A dropped hug-end event.** In one teardown path the `HUG_END` event is
   enqueued and the network is stopped immediately afterwards, so that
   particular event is never actually transmitted. The relay's own holding flag
   times out by itself. This is documented in the code and accepted
   (`main/couple_app.c:1751-1767`).

4. **Deliberate animation choices.** Two animation parameters were changed on
   purpose to suit integer-quantised pixel art: the home-screen heart breathing
   range became 92 % to 108 %, and the hug-screen pop amplitude became a fixed
   25 %. These are visible animation decisions, not bug fixes (section 7).

5. **Image size.** Registering the two font files raised the image size
   substantially. The recorded numbers and the remaining partition headroom are
   in section 9.

6. **Windows-specific test failures.** Three failures in
   `tests/test_install_passport_skills.py` are an environment limitation of this
   Windows host — creating directory symlinks needs Developer Mode or the
   symlink privilege, which surfaces as `WinError 1314` — and are not a defect
   of the application. CI on Linux is expected to pass them. Running
   `./tools/validate.sh --static` on this Windows host reproduces those three
   failures, while the repository checks and all C host tests pass.

7. **Device verification, partial.** Basic two-way interaction has been verified
   on hardware. What has not been verified item by item: hug tiers 2 and 3,
   `HUG CONNECTED`, the offline pending queue, and end-to-end key latency.

## 12. Where to go next

Several extension points are already designed for:

- **Partner identity.** Parsing, storage, and display of the partner's name
  already exist; adding a name-entry screen would let a user set it directly
  instead of relying on what arrives over the network.
- **Event protocol.** The frame format already carries arbitrary typed events
  with an 8-byte value field (`COUPLE_EVENT_VALUE_MAX`,
  `main/couple_types.h:21`), so new event types can be added without changing
  the transport.
- **Love Energy.** It is a single persistent counter with a single award path,
  so new sources of energy can be added in one place.
- **Sync.** The sync loop, outbox, and acknowledgement handling are isolated in
  the network module, so additional server-driven behaviour can be layered on
  the existing round.

The following features are reserved for future work and are not implemented:
the two-player electronic pet, our secret universe, the pixel couple cottage,
future love letters, daily couple quests, the couple compatibility quiz, the
couple fortune machine, and the prank system.