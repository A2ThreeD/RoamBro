# RoamBro

RoamBro is a cassette-style portable music player project built around an RP2350, a PN532 NFC reader, a microSD card, and a PCM5102 DAC.

The core idea is simple:

- NFC tags act like cassettes
- each tag maps to an album folder on the SD card
- playback behaves like a basic tape deck
- inserting a tag does not auto-play
- pressing `play/pause` starts or resumes playback

## Features

- MP3 and WAV playback from microSD
- NFC tag to album-folder mapping
- per-tag resume state stored on the SD card
- play / pause / next / previous controls
- long-press fast-forward and rewind scrubbing
- optional cassette-style sound mode
- battery-friendlier NFC behavior while idle

## Hardware

The current sketch header targets:

- Waveshare RP2350 Zero
- PN532 NFC reader
- PCM5102 DAC
- microSD card module
- 10K potentiometer for volume
- 4 momentary buttons
- 1 LED

## Wiring

A wiring diagram is included here:

- [wiring_diagram.svg](/Users/morrisaaron/Documents/Arduino/RoamBro/wiring_diagram.svg)

## SD Card Layout

Each NFC tag should contain an album name in an NDEF text record.

That album name maps directly to a folder on the SD card:

- NFC tag text: `songs from the big chair`
- SD folder: `/songs from the big chair`

Supported audio files:

- `.mp3`
- `.wav`

Tracks are sorted numerically when filenames begin with numbers, so names like `01 Intro.mp3`, `02 Song.mp3`, `10 Outro.mp3` play in the expected order.

## Controls

- `Play/Pause` short press: starts playback, pauses playback, or resumes playback
- `Play/Pause` long hold: resets the loaded album to the first track and stops
- `Next` short press: next track
- `Previous` short press: restart current track if enough of it has played, otherwise previous track
- `Next` long hold: fast-forward scrub
- `Previous` long hold: rewind scrub
- `Function` short press: toggles cassette sound mode

## Build

Compile with:

```bash
arduino-cli compile -b rp2040:rp2040:waveshare_rp2350_zero RoamBro.ino
```

Upload with:

```bash
arduino-cli compile --upload \
  -b rp2040:rp2040:waveshare_rp2350_zero \
  -p /dev/cu.usbmodem31101 \
  RoamBro.ino
```

If your serial port changes, check it with:

```bash
arduino-cli board list
```

## Project Files

- [RoamBro.ino](/Users/morrisaaron/Documents/Arduino/RoamBro/RoamBro.ino)
  main sketch
- [PLAYER_LOGIC.md](/Users/morrisaaron/Documents/Arduino/RoamBro/PLAYER_LOGIC.md)
  detailed behavior and state-machine notes
- [AGENTS.md](/Users/morrisaaron/Documents/Arduino/RoamBro/AGENTS.md)
  repo-specific instructions for future Codex work

## Notes

- Resume state is stored on the SD card, not internal MCU flash.
- During playback, NFC polling is reduced to UID presence checks to avoid unnecessary audio interruption.
- The project is intentionally tuned toward simple tape-deck behavior rather than smart autoplay behavior.
