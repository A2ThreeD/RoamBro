# RoamBro Player Logic

This document describes how the player in [RoamBro.ino](/Users/morrisaaron/Documents/Arduino/WalkBro/RoamBro/RoamBro.ino) currently behaves.

## Overview

RoamBro emulates a basic cassette player using NFC tags as "tapes."

- Each NFC tag contains an album name in an NDEF text record.
- The album name maps to a folder on the SD card.
- Audio files in that folder become the playlist for that cassette.
- Playback only starts or resumes when the user presses the `play/pause` button.
- Tag insertion does not auto-play.

## Main Concepts

The sketch tracks two different cassette identities:

- `present tag`
  The tag currently sitting on the NFC reader.
- `loaded tag`
  The tag whose album/position is currently loaded in the player.

This distinction is important because the player is meant to behave like a tape deck:

- inserting a tag does not automatically start playback
- removing a tag while playing pauses playback
- pressing play decides whether to resume the same cassette or start a different one

## Player States

The player uses three states:

- `STATE_IDLE`
  No active playback pipeline. A cassette may or may not be present.
- `STATE_PLAYING`
  Audio is actively being decoded and sent to the DAC.
- `STATE_PAUSED`
  Playback context is retained, but audio is not advancing.

Pause reasons:

- `PAUSE_USER`
  The user paused playback manually.
- `PAUSE_TAG_REMOVED`
  The loaded cassette was removed or swapped during playback.

## Startup Behavior

On boot the sketch:

1. Initializes I2C, SPI, SD, I2S audio, ADC, and the PN532 reader.
2. Mounts the SD card.
3. Configures audio output and codecs.
4. Starts the PN532 in `SAMConfig()` mode.
5. Enters `STATE_IDLE`.

## NFC Tag Behavior

The sketch only polls the PN532 continuously while playback is active.

When `play/pause` is pressed from idle or paused:

- the sketch performs a one-shot tag read
- if a tag is found, its UID and NDEF album name become the `present tag`
- if no tag is found, the `present tag` is cleared

While playing:

- the loop polls only for tag UID presence, removal, or a different tag appearing
- playback-time polling does not read the tag's NDEF album text
- playback-time presence checks use a short NFC read timeout and a faster poll interval so tag removal pauses more promptly
- if no tag is detected for two consecutive playback polls, playback pauses with `PAUSE_TAG_REMOVED`
- if a different tag appears, playback also pauses and that new tag becomes the `present tag`
- on removal or swap, the sketch flushes the I2S output with silence so buffered old audio is cleared immediately

This keeps idle NFC activity lower for battery savings while preserving cassette removal detection during playback.

## Album Mapping

Album lookup is simple:

- NFC text `"myalbum"`
- SD folder `"/myalbum"`

Supported files:

- `.mp3`
- `.wav`

Hidden files are ignored.

## Playlist Construction

When an album is loaded:

1. The corresponding folder is opened from the SD card.
2. Supported files are collected into `playlist`.
3. The playlist is sorted.

Sorting rules:

- if filenames begin with numbers, those numbers are used for ordering
- this handles `1`, `2`, `10` correctly
- this also handles `01`, `02`, `03`
- if files do not start with numbers, normal filename ordering is used

Examples:

- `1 Intro.mp3`
- `02 Song.mp3`
- `10 Finale.mp3`

These will play in numeric order.

## Playback Start and Resume

Pressing `play/pause` performs different actions depending on state.

### From Idle

If a valid `present tag` exists:

- the player loads that album
- it checks SD-backed resume data for the tag UID
- if resume data exists, it opens the saved track and seeks to the saved byte position
- otherwise it starts from track 0 at byte position 0

If no valid tag is present:

- nothing starts
- debug output reports that no cassette is detected

### From Paused

If the `present tag` matches the `loaded tag`:

- playback resumes from the paused position

If the `present tag` is different:

- the new tag's album is loaded
- if that tag has saved resume data, playback resumes from its stored position
- otherwise that album starts from the beginning

If no tag is present:

- no action occurs

### From Playing

A short press of `play/pause` pauses playback.

## Long Hold on Play/Pause

Holding `play/pause` for `PLAY_RESET_HOLD_MS`:

- stops audio
- resets the loaded cassette's saved position to track 0, position 0
- clears the active playback pipeline
- puts the player into `STATE_IDLE`

It does not auto-start playback on release.

The release event after the long hold is explicitly ignored.

## Previous / Next Behavior

### While Playing

Short press `next`:

- advances to the next track
- wraps at the end of the album

Short press `previous`:

- if the current track has actually played beyond `PREVIOUS_RESTART_THRESHOLD_SEC` (currently `1.5` seconds), it restarts the current track
- otherwise it goes to the previous track
- wraps to the last track if already at the beginning

### While Paused

- `previous` does nothing
- `next` does nothing

## Long Press Scrubbing

While playing:

- long-hold `previous` enters rewind scan mode
- long-hold `next` enters fast-forward scan mode
- scan mode keeps playback audible while seeking through the file
- releasing scan flushes output and re-syncs the decoder at the current position

Scan speed is controlled by:

- `SCAN_PLAYBACK_MULTIPLIER`
- `SCAN_STEP_INTERVAL_MS`

Scrubbing moves by file byte position, with MP3 frame-boundary correction when needed.

## Audio Pipeline

The player supports:

- MP3 via `MP3DecoderHelix`
- WAV via `WAVDecoder`

The file is read from SD in chunks and written into the decoder stream.

The `pumpAudio()` loop:

- reads file data from the current track
- feeds the decoder
- advances playback while `STATE_PLAYING`

When a track ends:

- the next track is started automatically

For fresh MP3 track starts only:

- decoded PCM output is muted for a short startup window after the track begins
- the mute duration is timed by output samples, not encoded input chunks
- this does not run on pause/resume

## Volume Control

Volume is read from an analog potentiometer.

Behavior:

- raw ADC input is normalized between configured min/max values
- the value is smoothed with a simple filter
- output gain is updated only when the value changes meaningfully

## Cassette Sound Mode

The sketch includes an optional cassette-sound effect controlled by:

- `CASSETTE_SOUND_ENABLED`

This flag sets the startup default.

When enabled:

- decoded PCM audio is passed through a cassette-effect stage before final output
- low-level hiss is added to the signal
- a simple low-pass filter rolls off high frequencies at about `15 kHz`
- a mild saturation stage adds subtle cassette-like distortion

If `CASSETTE_FILTER_ENABLED` is `0`:

- the filter stage is fully bypassed

If `CASSETTE_HISS_LEVEL` is `0`:

- hiss injection is fully bypassed

If `CASSETTE_DISTORTION_PERCENT` is `0.0f`:

- the saturation stage is fully bypassed

When disabled:

- decoded PCM audio is sent through normally with no cassette-effect coloration

The function button can toggle cassette sound on or off at runtime:

- each short press flips the current cassette-sound state
- the effect applies immediately to active playback

## Scrub Release

When fast-forward or rewind is released:

- the output buffer is flushed before normal playback resumes
- MP3 playback re-syncs the decoder at the current file position
- WAV playback does not restart the decoder, because seeking lands directly in PCM data rather than at a new WAV header

## Resume Storage

Resume information is stored on the SD card in a binary file:

- `"/walkbro_state.dat"`

This avoids wearing out RP2350 internal flash.

Each record stores:

- tag UID
- last track index
- last file byte position
- sequence number for replacement ordering

The store supports up to `MAX_RESUME_RECORDS` tags.

Current behavior:

- when playback is paused, resume state is saved
- when a cassette is removed during playback, resume state is saved
- when track changes occur, resume state is updated
- when a tag is played again later, its saved track and byte position are restored
- if a saved byte offset cannot be applied, playback falls back to the start of that track
- loading a tag does not immediately overwrite its saved position; the next write happens on a later pause, removal, reset, or track change

If the resume store does not exist or is invalid:

- the player creates a fresh in-memory store
- it writes the file on the next save event

## LED Behavior

LED modes:

- `LED_OFF`
  any non-playing steady state after animations finish
- `LED_SOLID`
  playing
- `LED_BLINK_SLOW`
  currently unused in the steady-state transport flow
- `LED_BOOT_ANIM`
  startup animation
- `LED_INSERT_ANIM`
  cassette insertion/start animation
- `LED_REMOVE_ANIM`
  cassette removal/pause animation

## Debug Logging

Debug output is controlled by:

- `DEBUG_LOGGING`

When enabled, serial output includes:

- player state changes
- tag detection and tag UID info
- selected album and track
- volume changes
- save/load activity for resume records
- cassette removal or no-cassette events

## Simple Test Mode

If `SIMPLE_TEST_MODE` is set to `1`:

- NFC cassette detection is skipped
- resume-state loading and saving are bypassed for starting playback
- pressing play from idle starts album playback from `SIMPLE_TEST_ALBUM`
- play/pause and next/previous still work for basic transport testing
- a long play-button hold resets the test album back to track 1 and stops playback

## Summary of Intended Tape-Deck Behavior

The player is intentionally not an auto-play device.

- placing a tag only makes a cassette available
- pressing play decides whether to start or resume it
- removing a tag while playing pauses playback
- reinserting the same tag allows resume
- inserting a different tag prepares that cassette, but still waits for play
- long-holding play rewinds the cassette to the beginning and stops
