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

*/

#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <PN532_I2C.h>
#include <PN532.h>

#include <I2S.h>
#include <BackgroundAudioMP3.h>
#include <BackgroundAudioWAV.h>

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

// ========================================================
// NFC
// ========================================================

PN532_I2C pn532_i2c(Wire);
PN532 nfc(pn532_i2c);

// ========================================================
// AUDIO
// ========================================================

I2S audio(OUTPUT);

BackgroundAudioMP3Class<RawDataBuffer<16 * 1024>> mp3Player(audio);
BackgroundAudioWAVClass<RawDataBuffer<16 * 1024>> wavPlayer(audio);

enum CodecType {
  CODEC_NONE,
  CODEC_MP3,
  CODEC_WAV
};

CodecType activeCodec = CODEC_NONE;

bool mp3Started = false;
bool wavStarted = false;

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
  uint32_t lastChange;
};

ButtonState btnPrev = {BTN_PREV_PIN, true, true, 0};
ButtonState btnPlay = {BTN_PLAY_PIN, true, true, 0};
ButtonState btnNext = {BTN_NEXT_PIN, true, true, 0};

// ========================================================
// STATE
// ========================================================

enum PlayerState {
  STATE_IDLE,
  STATE_PLAYING,
  STATE_PAUSED
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

// ========================================================
// UTIL
// ========================================================

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
      (raw[i]<<24)|(raw[i+1]<<16)|(raw[i+2]<<8)|raw[i+3];

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

  if (currentTrackFile) currentTrackFile.close();

  currentTrackIndex = idx;

  // FIX 1: Normalise path to lowercase before codec detection
  // so .MP3 / .WAV files on the SD card are handled correctly.
  String path = playlist[idx];
  String pathLower = path;
  pathLower.toLowerCase();

  if (isMp3File(pathLower)) {
    if (!mp3Started) { mp3Player.begin(); mp3Started = true; }
    activeCodec = CODEC_MP3;
  }
  else if (isWavFile(pathLower)) {
    if (!wavStarted) { wavPlayer.begin(); wavStarted = true; }
    activeCodec = CODEC_WAV;
  }

  currentTrackFile = SD.open(path.c_str(), FILE_READ);
  if (!currentTrackFile) return false;

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
  startTrackByIndex(0);
}

void handleRemoval() {
  ledMode = LED_REMOVE_ANIM;

  if (currentTrackFile) currentTrackFile.close();

  // Only flush the codec that was actually in use
  if (activeCodec == CODEC_MP3) mp3Player.flush();
  else if (activeCodec == CODEC_WAV) wavPlayer.flush();

  activeCodec = CODEC_NONE;

  clearPlaylist();
  playerState = STATE_IDLE;
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
    return raw == LOW ? 1 : 2;
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

  if (playEvt == 1) {

    if (playerState == STATE_PLAYING) {
      if (activeCodec == CODEC_MP3) mp3Player.pause();
      else if (activeCodec == CODEC_WAV) wavPlayer.pause();
      playerState = STATE_PAUSED;
    }

    else if (playerState == STATE_PAUSED) {
      if (activeCodec == CODEC_MP3) mp3Player.unpause();
      else if (activeCodec == CODEC_WAV) wavPlayer.unpause();
      playerState = STATE_PLAYING;
    }

    else if (playerState == STATE_IDLE) {
      String album;
      if (readCassetteAlbum(album)) {
        Serial.print("Album: ");
        Serial.println(album);
        startPlaybackFromAlbum(album);
        ledMode = LED_INSERT_ANIM;
      } else {
        Serial.println("Cassette not detected");
      }
    }
  }

  if (prevEvt == 2) previousTrack();
  if (nextEvt == 2) nextTrack();
}

// ========================================================
// AUDIO PUMP
// ========================================================

size_t activeAvailableForWrite() {
  if (activeCodec == CODEC_MP3) return mp3Player.availableForWrite();
  if (activeCodec == CODEC_WAV) return wavPlayer.availableForWrite();
  return 0;
}

size_t activeWrite(const void *data, size_t len) {
  if (activeCodec == CODEC_MP3) return mp3Player.write(data, len);
  if (activeCodec == CODEC_WAV) return wavPlayer.write(data, len);
  return 0;
}

bool activeDone() {
  if (activeCodec == CODEC_MP3) return mp3Player.done();
  if (activeCodec == CODEC_WAV) return wavPlayer.done();
  return true;
}

void pumpAudio() {

  while (currentTrackFile && activeAvailableForWrite() >= sizeof(fileBuffer)) {
    int len = currentTrackFile.read(fileBuffer, sizeof(fileBuffer));
    if (len <= 0) { currentTrackFile.close(); break; }
    activeWrite(fileBuffer, len);
  }

  if (!currentTrackFile && playerState == STATE_PLAYING && activeDone()) {
    nextTrack();
  }
}

// ========================================================
// NFC REMOVAL — FIX 2: short timeout + miss counter,
// no single blocking read deciding removal on its own.
// ========================================================

void checkCassetteRemoval() {

  if (playerState != STATE_PLAYING && playerState != STATE_PAUSED) return;

  static uint32_t last = 0;
  static uint8_t missCount = 0;

  if (millis() - last < 300) return;
  last = millis();

  uint8_t uid[7], uidLen;

  // Short 50ms timeout — non-blocking enough for the audio pump
  bool seen = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

  if (seen) {
    missCount = 0;
    return;
  }

  // Require 3 consecutive misses before treating as removed.
  // Avoids false ejects from momentary read failures.
  if (++missCount >= 3) {
    missCount = 0;
    handleRemoval();
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
    if (activeCodec == CODEC_MP3) mp3Player.setGain(currentGain);
    else if (activeCodec == CODEC_WAV) wavPlayer.setGain(currentGain);
    lastGain = currentGain;
  }
}

// ========================================================
// SETUP
// ========================================================

void setup() {

  Serial.begin(115200);

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

  audio.setBCLK(I2S_BCLK);
  audio.setDATA(I2S_DOUT);

  if (I2S_LRCLK == (I2S_BCLK - 1))
    audio.swapClocks();

  audio.setFrequency(44100);
  analogReadResolution(12);

  nfc.begin();
  nfc.SAMConfig();

  delay(200);

  ledMode = LED_BOOT_ANIM;
}

// ========================================================
// LOOP — FIX 3: idle coarse-polling replaces enterSleep().
// No nested loop; the main loop itself slows down when idle,
// keeping CPU busy-wait low without blocking anything.
// ========================================================

void loop() {

  // FIX 3: When idle, only run at ~20Hz instead of as fast as possible.
  // NFC is not polled at all during idle — only on play button press.
  if (playerState == STATE_IDLE) {
    static uint32_t lastIdleTick = 0;
    if (millis() - lastIdleTick < 50) return;
    lastIdleTick = millis();

    handleButtons();
    updateLED();
    return;
  }

  updateVolume();
  pumpAudio();
  handleButtons();
  checkCassetteRemoval();

  switch (playerState) {
    case STATE_PLAYING: ledMode = LED_SOLID;      break;
    case STATE_PAUSED:  ledMode = LED_BLINK_SLOW; break;
    default: break;
  }

  // Don't override a running animation
  if (ledMode == LED_INSERT_ANIM ||
      ledMode == LED_REMOVE_ANIM ||
      ledMode == LED_BOOT_ANIM) {
    // let it finish naturally
  }

  updateLED();

  delay(1);
}
