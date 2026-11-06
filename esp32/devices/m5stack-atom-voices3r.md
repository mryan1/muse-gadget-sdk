<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# M5Stack Atom VoiceS3R setup

How to put Muse back on an Atom VoiceS3R (sold as the Atom EchoS3R, model
C126), starting from whatever it's running: M5Stack's factory firmware, or
something else entirely. The summary is in [README.md](README.md#m5stack-atom-voices3r).

## What it does

- No screen and no status light. The full Muse UI runs against a 128×128
  display whose pixels go nowhere (`components/muse/boards/board_m5stack_atom_voices3r.c`).
- **Hold** the front button for a voice note: Muse transcribes it and answers
  in the Muse app as text.
- **Tap** it for a live call, where Muse answers out loud through the speaker
  (`CONFIG_MUSE_REALTIME_VOICE`, on in the overlay). This needs Muse's VM to
  accept calls. As of November 2026, Muse's voice chat wasn't enabled in
  Canada, and the VM answered `POST /voice/realtime` with 404: the log shows
  `MUSE DIDN'T TAKE THE CALL`, and on the board nothing seems to happen.
  Once it's enabled, no reflash is needed.

Tested on one unit (2026-11-06, SDK commit `463bd2a`, ESP-IDF v6.0.3):
boot with the ES8311 at 0x18, speaker playback, 10/10 cold boots, push-to-talk
voice notes, pairing, and reconnecting after a reboot.

## Hardware

ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB octal PSRAM), ES8311 codec at I2C 0x18
with one MEMS mic and an NS4150B amp, front button, USB-C, no battery.
Pins, from M5Unified's `board_M5AtomVoiceS3R` (M5Stack's product page lists
several of them wrongly):

| Signal | GPIO |
|---|---|
| I2S MCLK / BCLK / WS | 11 / 17 / 3 |
| I2S DOUT (speaker) / DIN (mic) | 48 / 4 |
| ES8311 I2C SDA / SCL | 45 / 0 (SCL is also the BOOT strap) |
| Amp enable | 18 |
| Front button (talk) | 41 |
| Grove (unused) | 1, 2 |

## 1. Toolchain

ESP-IDF **v6.0.3** exactly, on Linux or macOS:

```sh
git clone -b v6.0.3 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
cd ~/esp/esp-idf-v6.0.3 && ./install.sh esp32s3
python tools/idf_tools.py install cmake ninja   # if `idf.py` says cmake isn't on PATH
. ./export.sh
```

On Ubuntu without `python3-venv` (and no sudo to install it), give
`install.sh` a Python that has venv, for example uv's:
`uv python install 3.12`, then put a `python3` link to it first on `PATH`
for the install. Point it at a permanent path: the venv remembers it.

On Linux, flashing needs the `dialout` group. Until you log in again after
being added, run serial commands through `sg dialout -c "..."`.

## 2. Source

This port lives on the `atom-voices3r-port` branch of
[mryan1/muse-gadget-sdk](https://github.com/mryan1/muse-gadget-sdk), a fork
of facebookincubator/muse-gadget-sdk:

```sh
git clone -b atom-voices3r-port https://github.com/mryan1/muse-gadget-sdk.git
cd muse-gadget-sdk/esp32
```

## 3. SDK token

Every build needs the Muse SDK token (`mgst_…`). Get it either way:

- From gadgets.muse.ai > Account > SDK tokens.
- From the inky device (the Linux Muse gadget), which already holds it:
  `sudo cat /var/lib/musegadget/sdk_token`

The token goes in the build folder's generated `sdkconfig`, which is
gitignored. Never commit it. Build once so the folder exists (it warns
about the missing token), then set it with the hidden prompt:

```sh
tools/muse/board.sh build atom-voices3r
tools/muse/set-sdk-token.sh                # writes build-muse-m5stack-atom-voices3r/sdkconfig
tools/muse/board.sh build atom-voices3r    # no "No SDK token" warning this time
```

The build log (`/tmp/muse_build_atom-voices3r.log`) echoes the token; delete
it or redact it if you keep it.

## 4. Flash

**From Muse's firmware** (it's on the board already), esptool resets it
through its own USB port:

```sh
tools/muse/board.sh flash atom-voices3r
```

**From anything else** (M5Stack's factory firmware, or firmware of your own
that doesn't expose the S3's USB-Serial-JTAG), get the chip into its ROM
loader first:

- M5Stack's factory firmware is a USB audio device (`303a:8000`, "Atom
  EchoS3R"), but for about four seconds after it's plugged in the chip's own
  USB-Serial-JTAG (`303a:1001`) appears. Start this loop, then replug the
  board; it catches the port and leaves the chip in its loader:

  ```sh
  until ls /dev/serial/by-id/*Espressif* >/dev/null 2>&1; do sleep 0.1; done
  python -m esptool --chip esp32s3 -p /dev/serial/by-id/*Espressif* --before default-reset --after no-reset read-mac
  ```

- Or hold the small reset button on the side for 2–3 seconds (the S3R
  Atoms' download gesture). There's no LED; `lsusb` showing `303a:1001`
  means it worked.

Then flash as above. Flashing leaves NVS alone, so a board that was paired
before keeps its pairing and Wi-Fi.

Before overwriting firmware you might want back, save the whole flash:

```sh
python -m esptool --chip esp32s3 -p PORT --before no-reset read-flash 0 0x800000 atom-voices3r-backup.bin
# later: python -m esptool --chip esp32s3 -p PORT write-flash 0 atom-voices3r-backup.bin
```

## 5. Pair and check

Watch the boot with `tools/muse/monitor.py $(tools/muse/ports.py atom-voices3r) 30`
(opening the port resets the board). Expect `muse: board: M5Stack Atom VoiceS3R`,
`board: ES8311 answers at 0x18` and `link.ble: advertising as MuseGadget-XXXXXX`.

In the Muse app (Developer mode on): Settings > Devices, add
`MuseGadget-XXXXXX`, give it Wi-Fi, and press the front button once when the
app asks you to confirm. The heartbeat line should reach `ws=up`. If it stays
`ws=down` with `esp-tls ... select() timeout`, the Wi-Fi network is blocking
the board from reaching `api.muse.ai`.

To forget pairing and Wi-Fi and start over:
`python -m esptool --chip esp32s3 -p PORT erase-flash`, then flash again.
