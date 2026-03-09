#include <Arduino.h>
#line 1 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
/**
 * A2ThreeD WalkBro "Tape Emulator" - Plays MP3/WAV files and emulates a walkman style tape device.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 * Code Name: WalkBro
 * Description: Plays MP3/WAV files and emulates a walkman style tape device.
 * Date: 3/07/2026
 * Notes: V1.0 - First major version of code
 *
 * Hardware:
 * RP2350 "zero" microcontroller
 * PCM5102 DAC
 * MicroSD card reader
 * PN532 NFC reader
 * 10K potentiometer for volume control
 * 3 Buttons for play/pause, next, and previous
 * 1 Function button for future use
 * 1 LED for status indication
 * 
 * Functionality:
 * Emulates a cassette tape device that can be controlled with play/pause, next, and previous buttons. The "tapes" are represented 
 * by NFC tags that contain the album name in an NDEF text record. When a tag is detected, the corresponding album folder is loaded 
 * from the SD card and tracks can be played.
 *
 * Reads MP3/WAV files from SD card and plays through DAC.
 * 
 * If a user presses play/pause while a tape is playing, it will pause. Pressing play/pause again will resume. 
 * 
 * If the tape is removed while paused, it will remember the position and resume from there if the same tape is reinserted.
 * If a different tape is inserted, it will start from the beginning of that tape.
 * 
 * If the user presses next/previous while playing, it will skip to the next/previous track. 
 * If they long-press next/previous, it will enter a fast-forward/rewind mode that seeks through the track until released.
 * 
 * The LED indicates status: off when idle, solid when playing, slow blink when paused, and different animations for tape insertion/removal.
 * 
 * Volume is controlled by the potentiometer, which adjusts the gain of the audio output.

 * 
 */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <PN532_I2C.h>
#include <PN532.h>

#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>
#include <AudioTools/AudioCodecs/CodecWAV.h>

#include <vector>
#include <algorithm>

// ========================================================
// PINS
// ========================================================

#define I2C_SDA 8
#define I2C_SCL 9

#define SD_CS_PIN 5
#define SD_MISO_PIN 4
#define SD_MOSI_PIN 3
#define SD_CLK_PIN 2

#define I2S_BCLK 10
#define I2S_LRCLK 11
#define I2S_DOUT 12

#define BTN_PREV_PIN 28
#define BTN_PLAY_PIN 27
#define BTN_NEXT_PIN 29

#define LED_PIN 6
#define VOLUME_ADC_PIN 26

#define VOLUME_ADC_RAW_MIN 220
#define VOLUME_ADC_RAW_MAX 3920

#define SPI_SPEED SD_SCK_MHZ(40)
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 450
#define SCAN_STEP_INTERVAL_MS 120
#define SCAN_PLAYBACK_MULTIPLIER 2.5f
#define WAV_MIN_DATA_OFFSET 44
#define AUDIO_PUMP_CHUNKS_PER_LOOP 2
#define MP3_FRAME_SEARCH_WINDOW 8192

// ========================================================
// NFC
// ========================================================

PN532_I2C pn532_i2c(Wire);
PN532 nfc(pn532_i2c);

// ========================================================
// AUDIO
// ========================================================

I2SStream audioOutput;
VolumeStream volumeControl(audioOutput);
MP3DecoderHelix mp3Decoder;
WAVDecoder wavDecoder;
EncodedAudioStream decoderStream(&volumeControl, &mp3Decoder);
AudioDecoder *activeDecoder = nullptr;

enum CodecType {
  CODEC_NONE,
  CODEC_MP3,
  CODEC_WAV
};

CodecType activeCodec = CODEC_NONE;

float currentGain = 0.5f;
float filteredPotNorm = 0.5f;

// ========================================================
// PLAYLIST
// ========================================================

std::vector<String> playlist;
int currentTrackIndex = -1;

File currentTrackFile;
uint8_t fileBuffer[512];

// ========================================================
// BUTTON STRUCT
// ========================================================

struct ButtonState {
  uint8_t pin;
  bool stableLevel;
  bool lastRawLevel;
  bool longHandled;
  uint32_t lastChange;
  uint32_t pressedAt;
};

ButtonState btnPrev = {BTN_PREV_PIN, true, true, false, 0, 0};
ButtonState btnPlay = {BTN_PLAY_PIN, true, true, false, 0, 0};
ButtonState btnNext = {BTN_NEXT_PIN, true, true, false, 0, 0};

// ========================================================
// STATE
// ========================================================

enum PlayerState {
  STATE_IDLE,
  STATE_PLAYING,
  STATE_PAUSED
};

enum PauseReason {
  PAUSE_NONE,
  PAUSE_USER,
  PAUSE_TAG_REMOVED
};

enum ScanMode {
  SCAN_NONE,
  SCAN_REWIND,
  SCAN_FAST_FORWARD
};

enum LedMode {
  LED_OFF,
  LED_SOLID,
  LED_BLINK_SLOW,
  LED_BOOT_ANIM,
  LED_INSERT_ANIM,
  LED_REMOVE_ANIM
};

LedMode ledMode = LED_OFF;
PlayerState playerState = STATE_IDLE;
PauseReason pauseReason = PAUSE_NONE;
ScanMode scanMode = SCAN_NONE;
uint32_t lastScanStepMs = 0;
String currentAlbumName;

// ========================================================
// UTIL
// ========================================================

#line 201 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isMp3File(const String &name);
#line 205 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isWavFile(const String &name);
#line 209 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isSupportedFile(const String &name);
#line 216 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void clearPlaylist();
#line 222 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void stopAudioPipeline(bool flushOutput);
#line 235 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void restartDecoderAtCurrentPosition();
#line 244 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void setPaused(PauseReason reason);
#line 251 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void resumePlayback();
#line 257 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
float encodedBytesPerSecond();
#line 269 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isLikelyMp3FrameHeader(uint32_t header);
#line 285 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
long findMp3FrameBoundary(long anchor, bool searchBackward);
#line 337 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool seekCurrentTrackRelative(long deltaBytes);
#line 368 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void updateScanPlayback();
#line 390 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void startScanPlayback(ScanMode mode);
#line 396 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void stopScanPlayback();
#line 404 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
String readNDEFText();
#line 460 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void updateLED();
#line 514 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool startTrackByIndex(int idx);
#line 560 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void nextTrack();
#line 565 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void previousTrack();
#line 572 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void startPlaybackFromAlbum(const String &albumName);
#line 610 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void handleCassetteRemoval();
#line 619 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
uint8_t updateButton(ButtonState &b, uint32_t now);
#line 641 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool readCassetteAlbum(String &album);
#line 662 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void handleButtons();
#line 746 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
size_t activeAvailableForWrite();
#line 751 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
size_t activeWrite(const void *data, size_t len);
#line 756 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void pumpAudio();
#line 792 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void checkCassetteRemoval();
#line 822 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void updateVolume();
#line 851 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void setup();
#line 910 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void loop();
#line 201 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isMp3File(const String &name) {
  return name.endsWith(".mp3");
}

bool isWavFile(const String &name) {
  return name.endsWith(".wav");
}

bool isSupportedFile(const String &name) {
  if (name.startsWith(".")) return false;
  String lower = name;
  lower.toLowerCase();
  return isMp3File(lower) || isWavFile(lower);
}

void clearPlaylist() {
  playlist.clear();
  currentTrackIndex = -1;
  currentAlbumName = "";
}

void stopAudioPipeline(bool flushOutput) {
  if (currentTrackFile) currentTrackFile.close();

  decoderStream.end();

  if (flushOutput) {
    audioOutput.flush();
  }

  activeDecoder = nullptr;
  activeCodec = CODEC_NONE;
}

void restartDecoderAtCurrentPosition() {
  if (activeDecoder == nullptr) return;

  decoderStream.end();
  decoderStream.setDecoder(activeDecoder);
  decoderStream.begin();
  volumeControl.setVolume(currentGain);
}

void setPaused(PauseReason reason) {
  if (playerState == STATE_IDLE) return;
  playerState = STATE_PAUSED;
  pauseReason = reason;
  scanMode = SCAN_NONE;
}

void resumePlayback() {
  if (playerState == STATE_IDLE) return;
  playerState = STATE_PLAYING;
  pauseReason = PAUSE_NONE;
}

float encodedBytesPerSecond() {
  if (activeCodec == CODEC_WAV) {
    WAVAudioInfo info = wavDecoder.audioInfoEx();
    if (info.byte_rate > 0) return info.byte_rate;
  } else if (activeCodec == CODEC_MP3) {
    MP3FrameInfo info = mp3Decoder.audioInfoEx();
    if (info.bitrate > 0) return info.bitrate / 8.0f;
  }

  return 16000.0f;
}

bool isLikelyMp3FrameHeader(uint32_t header) {
  if ((header & 0xFFE00000UL) != 0xFFE00000UL) return false;

  uint8_t versionBits = (header >> 19) & 0x03;
  uint8_t layerBits = (header >> 17) & 0x03;
  uint8_t bitrateIndex = (header >> 12) & 0x0F;
  uint8_t sampleRateIndex = (header >> 10) & 0x03;

  if (versionBits == 0x01) return false;
  if (layerBits == 0x00) return false;
  if (bitrateIndex == 0x00 || bitrateIndex == 0x0F) return false;
  if (sampleRateIndex == 0x03) return false;

  return true;
}

long findMp3FrameBoundary(long anchor, bool searchBackward) {
  if (!currentTrackFile) return anchor;

  long originalPos = (long)currentTrackFile.position();
  long fileSize = (long)currentTrackFile.size();
  long endLimit = fileSize - 4;

  if (anchor < 0) anchor = 0;
  if (anchor > endLimit) anchor = endLimit;

  long start = searchBackward ? max(0L, anchor - MP3_FRAME_SEARCH_WINDOW) : anchor;
  long end = searchBackward ? anchor : min(endLimit, anchor + MP3_FRAME_SEARCH_WINDOW);

  uint8_t searchBuffer[259];
  int carry = 0;
  long found = -1;
  long pos = start;

  while (pos <= end) {
    if (!currentTrackFile.seek(pos)) break;

    size_t chunkLen = (size_t)min(256L, end - pos + 1);
    int bytesRead = currentTrackFile.read(searchBuffer + carry, chunkLen);
    if (bytesRead <= 0) break;

    int total = carry + bytesRead;
    for (int i = 0; i <= total - 4; i++) {
      uint32_t header = ((uint32_t)searchBuffer[i] << 24) |
                        ((uint32_t)searchBuffer[i + 1] << 16) |
                        ((uint32_t)searchBuffer[i + 2] << 8) |
                        ((uint32_t)searchBuffer[i + 3]);

      if (isLikelyMp3FrameHeader(header)) {
        long candidate = pos - carry + i;
        if (searchBackward) {
          found = candidate;
        } else {
          currentTrackFile.seek(originalPos);
          return candidate;
        }
      }
    }

    carry = min(3, total);
    memmove(searchBuffer, searchBuffer + total - carry, carry);
    pos += bytesRead;
  }

  currentTrackFile.seek(originalPos);
  return found >= 0 ? found : anchor;
}

bool seekCurrentTrackRelative(long deltaBytes) {
  if (!currentTrackFile || activeCodec == CODEC_NONE) return false;

  long minPos = 0;
  if (activeCodec == CODEC_WAV) {
    minPos = WAV_MIN_DATA_OFFSET;
  }

  long maxPos = (long)currentTrackFile.size() - 1;
  if (maxPos < minPos) maxPos = minPos;

  long target = (long)currentTrackFile.position() + deltaBytes;
  if (target < minPos) target = minPos;
  if (target > maxPos) target = maxPos;

  if (activeCodec == CODEC_MP3) {
    target = findMp3FrameBoundary(target, deltaBytes < 0);
  }

  if (target == (long)currentTrackFile.position()) return false;
  if (!currentTrackFile.seek(target)) return false;

  audioOutput.flush();

  if (activeCodec == CODEC_MP3) {
    restartDecoderAtCurrentPosition();
  }

  return true;
}

void updateScanPlayback() {
  if (scanMode == SCAN_NONE || playerState != STATE_PLAYING || !currentTrackFile) return;

  uint32_t now = millis();
  if (now - lastScanStepMs < SCAN_STEP_INTERVAL_MS) return;

  uint32_t elapsed = now - lastScanStepMs;
  lastScanStepMs = now;

  float extraFactor = SCAN_PLAYBACK_MULTIPLIER - 1.0f;
  if (scanMode == SCAN_REWIND) {
    extraFactor = -(SCAN_PLAYBACK_MULTIPLIER + 1.0f);
  }

  long deltaBytes = (long)(encodedBytesPerSecond() * extraFactor * (elapsed / 1000.0f));
  if (deltaBytes == 0) {
    deltaBytes = scanMode == SCAN_REWIND ? -512 : 512;
  }

  seekCurrentTrackRelative(deltaBytes);
}

void startScanPlayback(ScanMode mode) {
  if (playerState != STATE_PLAYING || !currentTrackFile) return;
  scanMode = mode;
  lastScanStepMs = millis();
}

void stopScanPlayback() {
  scanMode = SCAN_NONE;
}

// ========================================================
// NDEF TEXT
// ========================================================

String readNDEFText() {

  uint8_t buffer[4];
  uint8_t raw[128];
  int rawIndex = 0;

  for (uint8_t page = 4; page < 36; page++) {
    if (!nfc.mifareultralight_ReadPage(page, buffer)) break;
    for (int i = 0; i < 4; i++) {
      raw[rawIndex++] = buffer[i];
    }
  }

  int i = 0;
  while (i < rawIndex && raw[i] != 0x03) i++;
  if (i >= rawIndex) return "";

  i++;
  int len = raw[i++];
  int end = i + len;

  while (i < end) {
    uint8_t header = raw[i++];
    bool shortRecord = header & 0x10;
    uint8_t typeLen = raw[i++];

    uint32_t payloadLen = shortRecord ? raw[i++] :
      (raw[i] << 24) | (raw[i + 1] << 16) | (raw[i + 2] << 8) | raw[i + 3];

    if (!shortRecord) i += 4;

    uint8_t type = raw[i];
    i += typeLen;

    if (type == 0x54) {
      uint8_t status = raw[i++];
      uint8_t langLen = status & 0x3F;
      i += langLen;

      String text;
      for (int j = 0; j < payloadLen - 1 - langLen; j++)
        text += (char)raw[i++];

      return text;
    }

    i += payloadLen;
  }

  return "";
}

// ========================================================
// LED
// ========================================================

void updateLED() {

  static uint32_t timer = 0;
  static int step = 0;

  uint32_t now = millis();

  switch (ledMode) {

    case LED_OFF:
      digitalWrite(LED_PIN, LOW);
      break;

    case LED_SOLID:
      digitalWrite(LED_PIN, HIGH);
      break;

    case LED_BLINK_SLOW:
      if (now - timer > 500) {
        timer = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      }
      break;

    case LED_BOOT_ANIM:
      if (now - timer > 120) {
        timer = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        if (++step > 6) { ledMode = LED_OFF; step = 0; }
      }
      break;

    case LED_INSERT_ANIM:
      if (now - timer > 80) {
        timer = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        if (++step > 4) { ledMode = LED_SOLID; step = 0; }
      }
      break;

    case LED_REMOVE_ANIM:
      if (now - timer > 150) {
        timer = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        if (++step > 6) { ledMode = LED_OFF; step = 0; }
      }
      break;
  }
}

// ========================================================
// TRACK CONTROL
// ========================================================

bool startTrackByIndex(int idx) {

  if (idx < 0 || idx >= (int)playlist.size()) return false;

  String path = playlist[idx];
  String pathLower = path;
  pathLower.toLowerCase();

  AudioDecoder *nextDecoder = nullptr;
  CodecType nextCodec = CODEC_NONE;

  if (isMp3File(pathLower)) {
    nextDecoder = &mp3Decoder;
    nextCodec = CODEC_MP3;
  } else if (isWavFile(pathLower)) {
    nextDecoder = &wavDecoder;
    nextCodec = CODEC_WAV;
  } else {
    return false;
  }

  File nextFile = SD.open(path.c_str(), FILE_READ);
  if (!nextFile) return false;

  stopAudioPipeline(true);

  currentTrackFile = nextFile;
  currentTrackIndex = idx;
  activeDecoder = nextDecoder;
  activeCodec = nextCodec;
  pauseReason = PAUSE_NONE;
  scanMode = SCAN_NONE;

  decoderStream.setDecoder(activeDecoder);
  if (!decoderStream.begin()) {
    stopAudioPipeline(false);
    return false;
  }

  volumeControl.setVolume(currentGain);
  playerState = STATE_PLAYING;

  Serial.println(path);
  return true;
}

void nextTrack() {
  if (playlist.empty()) return;
  startTrackByIndex((currentTrackIndex + 1) % playlist.size());
}

void previousTrack() {
  if (playlist.empty()) return;
  int prev = currentTrackIndex - 1;
  if (prev < 0) prev = playlist.size() - 1;
  startTrackByIndex(prev);
}

void startPlaybackFromAlbum(const String &albumName) {

  clearPlaylist();

  String folder = "/" + albumName;
  File dir = SD.open(folder);

  if (!dir || !dir.isDirectory()) {
    Serial.println("Album folder not found");
    return;
  }

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String name = entry.name();
    if (!entry.isDirectory() && isSupportedFile(name))
      playlist.push_back(folder + "/" + name);
    entry.close();
  }

  dir.close();

  if (playlist.empty()) {
    Serial.println("No tracks found");
    return;
  }

  std::sort(playlist.begin(), playlist.end());
  if (!startTrackByIndex(0)) {
    Serial.println("Unable to start first track");
    clearPlaylist();
    playerState = STATE_IDLE;
  } else {
    currentAlbumName = albumName;
  }
}

void handleCassetteRemoval() {
  ledMode = LED_REMOVE_ANIM;
  setPaused(PAUSE_TAG_REMOVED);
}

// ========================================================
// BUTTONS
// ========================================================

uint8_t updateButton(ButtonState &b, uint32_t now) {

  bool raw = digitalRead(b.pin);

  if (raw != b.lastRawLevel) {
    b.lastRawLevel = raw;
    b.lastChange = now;
  }

  if ((now - b.lastChange) >= BUTTON_DEBOUNCE_MS && raw != b.stableLevel) {
    b.stableLevel = raw;
    if (raw == LOW) {
      b.longHandled = false;
      b.pressedAt = now;
      return 1;
    }
    return 2;
  }

  return 0;
}

bool readCassetteAlbum(String &album) {

  uint8_t uid[7];
  uint8_t uidLength;

  for (int i = 0; i < 4; i++) {
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
      album = readNDEFText();
      album.trim();
      album.toLowerCase();

      if (album.length() > 0) return true;

      Serial.println("Tag present but NDEF unreadable");
    }
    delay(30);
  }

  return false;
}

void handleButtons() {

  uint32_t now = millis();

  uint8_t prevEvt = updateButton(btnPrev, now);
  uint8_t playEvt = updateButton(btnPlay, now);
  uint8_t nextEvt = updateButton(btnNext, now);

  if (!btnPrev.stableLevel && !btnPrev.longHandled &&
      (now - btnPrev.pressedAt) >= BUTTON_LONG_PRESS_MS) {
    btnPrev.longHandled = true;
    startScanPlayback(SCAN_REWIND);
  }

  if (!btnNext.stableLevel && !btnNext.longHandled &&
      (now - btnNext.pressedAt) >= BUTTON_LONG_PRESS_MS) {
    btnNext.longHandled = true;
    startScanPlayback(SCAN_FAST_FORWARD);
  }

  if (playEvt == 1) {

    if (playerState == STATE_PLAYING) {
      setPaused(PAUSE_USER);
    }

    else if (playerState == STATE_PAUSED) {
      if (pauseReason == PAUSE_TAG_REMOVED) {
        String album;
        if (readCassetteAlbum(album)) {
          if (album != currentAlbumName) {
            Serial.print("Album changed: ");
            Serial.println(album);
            startPlaybackFromAlbum(album);
            if (playerState == STATE_PLAYING) {
              ledMode = LED_INSERT_ANIM;
            }
          } else {
            resumePlayback();
          }
        } else {
          Serial.println("Cassette not detected");
        }
      } else {
        resumePlayback();
      }
    }

    else if (playerState == STATE_IDLE) {
      String album;
      if (readCassetteAlbum(album)) {
        Serial.print("Album: ");
        Serial.println(album);
        startPlaybackFromAlbum(album);
        if (playerState == STATE_PLAYING) {
          ledMode = LED_INSERT_ANIM;
        }
      } else {
        Serial.println("Cassette not detected");
      }
    }
  }

  if (prevEvt == 2) {
    if (scanMode == SCAN_REWIND) {
      stopScanPlayback();
    } else if (!btnPrev.longHandled) {
      previousTrack();
    }
  }

  if (nextEvt == 2) {
    if (scanMode == SCAN_FAST_FORWARD) {
      stopScanPlayback();
    } else if (!btnNext.longHandled) {
      nextTrack();
    }
  }
}

// ========================================================
// AUDIO PUMP
// ========================================================

size_t activeAvailableForWrite() {
  if (activeDecoder == nullptr) return 0;
  return decoderStream.availableForWrite();
}

size_t activeWrite(const void *data, size_t len) {
  if (activeDecoder == nullptr) return 0;
  return decoderStream.write((const uint8_t *)data, len);
}

void pumpAudio() {

  if (playerState != STATE_PLAYING || activeDecoder == nullptr) return;

  int chunksProcessed = 0;

  while (currentTrackFile &&
         activeAvailableForWrite() >= sizeof(fileBuffer) &&
         chunksProcessed < AUDIO_PUMP_CHUNKS_PER_LOOP) {
    int len = currentTrackFile.read(fileBuffer, sizeof(fileBuffer));
    if (len <= 0) {
      currentTrackFile.close();
      break;
    }

    size_t written = activeWrite(fileBuffer, len);
    if (written == 0) break;

    if (written < (size_t)len) {
      currentTrackFile.seek(currentTrackFile.position() - (len - written));
      break;
    }

    chunksProcessed++;
  }

  if (!currentTrackFile && activeDecoder != nullptr) {
    audioOutput.flush();
    nextTrack();
  }
}

// ========================================================
// NFC REMOVAL
// ========================================================

void checkCassetteRemoval() {

  if (playerState != STATE_PLAYING) return;

  static uint32_t last = 0;
  static uint8_t missCount = 0;

  if (millis() - last < 300) return;
  last = millis();

  uint8_t uid[7], uidLen;

  bool seen = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

  if (seen) {
    missCount = 0;
    return;
  }

  if (++missCount >= 3) {
    missCount = 0;
    handleCassetteRemoval();
  }
}

// ========================================================
// VOLUME
// ========================================================

void updateVolume() {

  static uint32_t lastRead = 0;
  static float lastGain = -1;

  if (millis() - lastRead < 20) return;
  lastRead = millis();

  int raw = analogRead(VOLUME_ADC_PIN);

  float norm = (raw - VOLUME_ADC_RAW_MIN) /
               (float)(VOLUME_ADC_RAW_MAX - VOLUME_ADC_RAW_MIN);

  if (norm < 0) norm = 0;
  if (norm > 1) norm = 1;

  filteredPotNorm = filteredPotNorm * 0.85f + norm * 0.15f;
  currentGain = filteredPotNorm;

  if (fabs(currentGain - lastGain) > 0.01f) {
    volumeControl.setVolume(currentGain);
    lastGain = currentGain;
  }
}

// ========================================================
// SETUP
// ========================================================

void setup() {

  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  Wire.setSDA(I2C_SDA);
  Wire.setSCL(I2C_SCL);
  Wire.begin();

  pinMode(btnPrev.pin, INPUT_PULLUP);
  pinMode(btnPlay.pin, INPUT_PULLUP);
  pinMode(btnNext.pin, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_CLK_PIN);
  SPI.begin();

  if (!SD.begin(SD_CS_PIN, SPI_SPEED, SPI)) {
    Serial.println("SD mount failed");
    while (1);
  }

  auto audioConfig = audioOutput.defaultConfig(TX_MODE);
  audioConfig.sample_rate = 44100;
  audioConfig.bits_per_sample = 16;
  audioConfig.channels = 2;
  audioConfig.pin_bck = I2S_BCLK;
  audioConfig.pin_ws = I2S_LRCLK;
  audioConfig.pin_data = I2S_DOUT;
  audioConfig.buffer_count = 8;
  audioConfig.buffer_size = 512;
  audioOutput.begin(audioConfig);

  auto volumeConfig = volumeControl.defaultConfig();
  volumeConfig.sample_rate = 44100;
  volumeConfig.bits_per_sample = 16;
  volumeConfig.channels = 2;
  volumeConfig.volume = currentGain;
  volumeControl.begin(volumeConfig);

  mp3Decoder.addNotifyAudioChange(volumeControl);
  wavDecoder.addNotifyAudioChange(volumeControl);

  analogReadResolution(12);

  nfc.begin();
  nfc.SAMConfig();

  delay(200);

  ledMode = LED_BOOT_ANIM;
}

// ========================================================
// LOOP
// ========================================================

void loop() {

  if (playerState == STATE_IDLE) {
    static uint32_t lastIdleTick = 0;
    if (millis() - lastIdleTick < 50) return;
    lastIdleTick = millis();

    handleButtons();
    updateLED();
    return;
  }

  handleButtons();
  updateVolume();
  checkCassetteRemoval();
  updateScanPlayback();
  pumpAudio();

  if (ledMode != LED_INSERT_ANIM &&
      ledMode != LED_REMOVE_ANIM &&
      ledMode != LED_BOOT_ANIM) {
    switch (playerState) {
      case STATE_PLAYING: ledMode = LED_SOLID;      break;
      case STATE_PAUSED:  ledMode = LED_BLINK_SLOW; break;
      default: break;
    }
  }

  updateLED();

  delay(1);
}

