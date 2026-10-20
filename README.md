<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# OurPassport · a two-device couple terminal

**Two palm-sized machines that recognise exactly one other person.**

Tap OK once and a heart lights up thousands of kilometres away, growing from
nothing until it fills the screen.

[![License: MIT](https://img.shields.io/badge/License-MIT-black.svg)](LICENSE)
[![Target: ESP32-C3](https://img.shields.io/badge/target-ESP32--C3-blue.svg)](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
[![ESP-IDF 5.5.3](https://img.shields.io/badge/ESP--IDF-5.5.3-red.svg)](docs/development/engineering/build-and-test.md)
[![UI: LVGL](https://img.shields.io/badge/UI-LVGL-9cf.svg)](docs/README.md)

---

## What this is

Two FoloToy **AI PASSPORT** handhelds (ESP32-C3, 240x320 screen, no PSRAM),
reflashed into a pair of terminals that only each other can touch.

- **Not a chat app.** No keyboard, no message list, no read receipts.
- **Only your partner can affect your device.** A stranger's board can push
  nothing at you; before pairing, the two devices are simply invisible to each
  other.
- **The screen is tiny on purpose.** 240x320 has no room for a feed, only room
  for one heart.

There is nothing to type on it, which forces a different kind of conversation:
you cannot explain, cannot hedge, cannot send a paragraph. You can only press
the button and let the other board light up.

The firmware ships with a Chinese UI (the primary audience is one specific
person). It also carries a complete English string table; see
[Switching the UI to English](#switching-the-ui-to-english).

---

## How to play

### Miss you

Press OK once.

The other board fills with a heart that grows from small to full-screen, plays a
short cute sound, and then shows the English string table's wording:

```
      TA misses you
      Last miss 3 h ago
```

The sender gets a light confirmation (`Miss delivered`, energy +1) plus a small
line under it: `Waiting for TA`. The receiving board also remembers how long it
has been since the previous miss arrived, so you can see exactly how long your
partner held out before pressing.

Being offline does not lose anything. Interactions go into an outbox (capacity
32) and land as soon as the partner is back, with a banner on top:

```
While you were away, TA missed you 3 times.
```

### Remote hug

**Hold OK down.** However long you hold, that is how long the other board's hug
animation lasts.

| Hold time | Tier |
| --- | --- |
| >= 0.7 s | Small hug |
| >= 3 s | Big hug |
| >= 8 s | Clingy hug |

While you hold, your own screen counts up in real time (`Held 4.2 s`). The hug
is sent when you release, capped at 60 seconds. The receiver sees `TA hugged
you` and the actual duration.

**If both of you hold OK at the same time**, both boards enter:

```
      HUG CONNECTED
  Both holding - LOVE ENERGY +5
       Don't let go
```

A synchronised heartbeat animation runs on both screens at once. This is the
part that gets addictive.

### Hearts in sync

**Both of you press "miss you" within 10 seconds.**

That is not a coincidence, and both boards know it. Both enter a full-screen
`HEART SYNC` view with an animation completely unlike the normal one, and
energy +5.

You will start pressing and then staring at the screen for three seconds,
hoping to collide.

### LOVE ENERGY

Every interaction feeds one shared currency. It is the backbone of every
progression feature that comes after it:

| What happened | Earned |
| --- | --- |
| Sent a miss | +1 |
| Received a miss | +1 |
| Hearts in sync | +5 |
| Mutual hug | +5 |
| Any interaction that day (streak bonus) | +3 |

That number on the home screen is the total. It is not a score; it is more like
a savings account the two of you pay into. It only goes up.

### Milestone eggs

When the lifetime interaction count crosses one of these numbers, a one-off egg
fires and awards a bonus equal to the threshold itself:

`9` · `18` · `99` · `100` · `365` · `520` · `999` · `1314`

Two of them are screenshot material:

- **520** - `520 hearts, and none beats you.`
- **1314** - `A whole lifetime, starting here.`

The first means "I love you", the second means "forever". On a 240x320 screen,
but both get a dedicated animation.

### Random flavour

A received miss sometimes swaps the default line for one of these:

```
Someone is missing you in secret.
Warning: too much missing detected.
TA is requesting a hug.
SYSTEM ERROR: PARTNER TOO CUTE
Heartbeat data synced.
Distance is shrinking.
Sugar level over the limit.
TA hid a miss in this message.
```

One more is hidden: on the **7th miss of the day** it always appears -

```
Miss #7 today, a bit clingy.
```

### Our day

Scroll up for today's ledger:

```
         Our day
Misses today       12 times
Hugs today          4 times
First today         09:41
Streak              26 days
```

**Design red line: nothing here is allowed to pressure you.**

- A day with no interaction produces no scolding - it reads `Not yet today`.
- A broken streak does not blame anyone - it reads `Starts today`.
- No leaderboard, no "your partner contributes more", no read receipts, no
  mechanism that can make either person feel guilty.

The point is to bring two people closer, not to make anyone prove something.

---

## Thirty-second start

1. **Flash the firmware** (pick either deployment route below)
2. **Join Wi-Fi**: `Menu -> Wi-Fi setup`, connect your phone to the board's
   hotspot and fill in your Wi-Fi and server address
3. **Pair**: on one board `Menu -> Pair -> OK (request a code)`, read the code
   out loud; on the other `Menu -> Pair -> join`, dial the 6 digits with the
   up/down button
4. **Play**: short-press OK on one board and the other lights up

---

## Deployment A: do it by hand

For people who already have ESP-IDF and want to modify the code. About fifteen
minutes, excluding the SDK download.

### 1. Environment

You need **ESP-IDF 5.5.3**. See
[build-and-test](docs/development/engineering/build-and-test.md) for the
official install and activation flow.

### 2. Build

```sh
cd ai-passport
idf.py build
```

### 3. Flash

```sh
idf.py -p <your-port> flash monitor
```

> **Do not flash `build/merged-binary.bin`.**
>
> The merged image is one contiguous blob starting at address `0x0` (gaps padded
> with 0xFF), and [`partitions.csv`](partitions.csv) places the NVS partition at
> `0x9000`, which falls inside that range. Flashing the merged image overwrites
> the NVS area and **wipes the pairing relationship, the Wi-Fi credentials, and
> the Love Energy total**.
>
> Use `idf.py flash` (which writes bootloader, partition table, and app only),
> or name the three offsets explicitly:
>
> ```sh
> python -m esptool --chip esp32c3 -p <your-port> -b 460800 \
>   write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
>   0x0 build/bootloader/bootloader.bin \
>   0x8000 build/partition_table/partition-table.bin \
>   0x10000 build/FoloToy-AI-Passport.bin
> ```

### 4. Provisioning

`Menu -> Wi-Fi setup`. The board raises a hotspot:

| Item | Value |
| --- | --- |
| SSID | `Heartlink-XXXX` (XXXX is the last two MAC bytes, shown on screen) |
| Password | `heartlink` |
| Config page | open `http://192.168.4.1/` in your phone browser |

Four fields: your **2.4 GHz Wi-Fi SSID**, its **password**, the **Heartlink
server address**, and **your name**.

The server address must start with `http://`. The firmware has no TLS stack, so
an `https://` address will retry forever as a network failure. Everything is
stored in NVS and survives a reboot.

### 5. Run the relay yourself

When the two boards are not on the same LAN, they need a host both can reach:

```
Device A  <->  Internet Relay  <->  Device B
```

The server lives in [`relay/`](relay/README.md) in this repository: **one Python
file, standard library only, no dependencies, no database.**

```sh
cd relay
python3 relay.py --host 0.0.0.0 --port 8787 --data-file relay-state.json
```

A systemd unit template is included for keeping it running:

```sh
sudo mkdir -p /opt/aipassport-relay
sudo cp relay.py /opt/aipassport-relay/
sudo cp deploy/aipassport-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aipassport-relay
```

Verify:

```sh
curl -s http://<your-host>:8787/healthz    # -> ok
```

If you would rather not expose your server's real address, put a Cloudflare
Tunnel in front of it; the template is
[`relay/deploy/cloudflared-config.yml.example`](relay/deploy/cloudflared-config.yml.example).
The two traps are documented in that file's header, in particular that the
hostname's **Always Use HTTPS toggle must be off** (the device cannot follow a
301) while the address in the firmware **stays `http://`** (TLS terminates at
Cloudflare, not on the device).

The full server contract - frame layout, endpoints, status codes, pairing,
sync, unpair, state file - is in [`relay/README.md`](relay/README.md).

### 6. Pair the two boards

1. Board A: `Menu -> Pair`, press OK to **request a pair code**. A 6-digit code
   appears with a countdown.
2. Board B: `Menu -> Pair -> join`, dial the digits with up/down, press OK for
   the next digit.
3. After the 6th digit, **press OK once more to submit**. This is the step
   everyone misses.
4. Both boards show `Paired` / `Counting together from today`.

The pairing lives in NVS and survives reboots, network loss, and a Wi-Fi change.
To undo it: `Menu -> Unpair`, then **hold OK** to confirm. The other board sees
`TA unpaired`; energy and memories stay.

---

## Deployment B: hand it to an AI

This repository is written for AI agents. The root [`AGENTS.md`](AGENTS.md) is
the entry point and carries a routing table mapping tasks to the documents that
must be read first. [`skills/`](skills/README.md) ships five skills:

| Skill | Purpose |
| --- | --- |
| `passport-setup` | toolchain install, ESP-IDF clone |
| `passport-build` | builds, partitions, image verification |
| `passport-develop` | BSP, UI, and state machine work |
| `passport-device-test` | on-device flashing and serial verification |
| `passport-debug` | crashes, reboots, unresponsive boards |

So there is exactly one thing for you to do: **paste the following into your
AI**.

```text
Read AGENTS.md at the root of this repository and follow its routing table to
set up the development environment. Then build the OurPassport couple terminal
firmware in this repository. When the build finishes, report the firmware path,
the image size, and its SHA-256. Do not flash any device until I say so.
```

It will read the documents, install what it needs, build, and report. Later, to
change wording, add an animation, retune the hug tiers, or add a new interaction,
hand it `AGENTS.md` again and just describe what you want.

This is also why the comments and docs here are more verbose than a toy project
usually is: **they are instructions for the next AI that touches the code.**

---

## Hardware and implementation

| Item | Value |
| --- | --- |
| SoC | ESP32-C3 |
| Flash | 8 MB |
| PSRAM | none |
| Display | ST7789P3 240x320, 4-line SPI |
| Framework | ESP-IDF 5.5.3 + LVGL |
| Firmware version | 1.0.0 (`main/couple_config.h`) |

This firmware does **not** reuse the official demo test menu. The home screen,
animations, and menus are a redesign, as the repository rules require for
derivative applications. The BSP layer is reused as-is.

The code is layered by responsibility, and no slow operation ever runs inside an
LVGL callback or a button callback:

```
main/
├── couple_ui.c        every screen and animation (LVGL context only)
├── couple_app.c       state machine: key events -> interaction -> screen
├── couple_state.c     trusted pure logic: energy, streak, milestones, hug tiers
├── couple_net.c       the sync loop against the relay (long poll)
├── couple_proto.c     HL frame codec and replay protection
├── couple_store.c     NVS persistence (batched every 20 s or 8 dirty flags)
├── couple_wifi.c      Wi-Fi association and reconnect backoff
├── couple_prov.c      SoftAP provisioning plus the phone config page
├── couple_audio.c     audio worker (queued, never blocks a button)
├── couple_radio.c     battery, backlight, brightness
├── couple_tone.c      tone synthesis (pure logic, host-testable)
└── couple_config.h    every tunable in one place
```

- **Networking**: HTTP long poll carrying compact binary frames, an outbox of 32
  pending interactions, and exponential reconnect backoff (2 s up to 60 s).
- **Persistence**: Flash is never written on every button press. Dirty flags
  accumulate and are flushed every 20 seconds or at 8 flags; the replay counter
  persists in steps of 128 with a boot margin of 1024, so it stays monotonic
  across a reboot without wearing out the Flash.
- **Tests**: the state machine, protocol, tone synthesis, and storage encoding
  all have host-side unit tests under `tests/`, with no hardware required.

### Switching the UI to English

`main/CMakeLists.txt` defines `COUPLE_USE_APP_FONT=1`, which selects the Chinese
subset fonts in `assets/fonts/` and the Chinese string branch. Removing that
definition builds the same firmware with the English string table quoted
throughout this document, at the cost of the Chinese glyph coverage on screen.

---

## Known limitations

- What has been verified on real hardware is **basic two-way interaction**:
  pairing, miss you, and hugs. The two upper hug tiers, `HUG CONNECTED` (both
  boards holding at once), the offline pickup queue, and end-to-end latency have
  not been measured item by item.
- The firmware has **no TLS stack**. The relay address must be `http://`. For
  encryption, put a Cloudflare Tunnel in front; do not change the scheme in the
  firmware.
- The Chinese UI depends on pre-generated subset fonts committed under
  [`assets/fonts/`](assets/README.md). LVGL renders a missing glyph as blank
  rather than a box, so wording changes must be followed by the glyph check
  tooling.
- The relay is a single process with a single JSON state file. That is plenty
  for two people and not intended to scale.
- On Windows, three cases in `tests/test_install_passport_skills.py` fail
  because of filesystem symlink permissions (`[WinError 1314]`). That is an
  environment limitation, not a code defect.

---

## Roadmap

The architecture is built to keep growing: partner identity, the event protocol,
Love Energy, and the sync mechanism are all extensible. Next:

- **Two-player virtual pet** - raised and fed together
- **Our secret universe** - a world only the two of you can see
- **Pixel couple house** - a home inside one screen
- **Future letters** - written now, opened on a chosen date
- **Daily couple quests** - done together, never graded
- **Couple chemistry quiz** - both boards ask the same question at once
- **Couple fate machine** - whose turn to wash the dishes
- **Prank system** - make the other screen flinch

There is exactly one acceptance test:

> If this were a little device my partner gave me, would this feature surprise me?

If the answer is only "that is useful", redesign it.

---

## Credits and license

- Hardware and BSP come from [FoloToy AI PASSPORT](https://github.com/FoloToy/ai-passport).
  Thanks for open-sourcing such a fun little board.
- The application layer, UI, wording, and relay server added here: MIT, see
  [LICENSE](LICENSE).

May the other board be pressing at the same moment you do.