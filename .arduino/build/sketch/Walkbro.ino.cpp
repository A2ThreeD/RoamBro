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

#line 147 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isMp3File(const String &name);
#line 151 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isWavFile(const String &name);
#line 155 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool isSupportedFile(const String &name);
#line 162 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void clearPlaylist();
#line 167 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void stopAudioPipeline(bool flushOutput);
#line 184 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
String readNDEFText();
#line 240 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void updateLED();
#line 294 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool startTrackByIndex(int idx);
#line 338 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void nextTrack();
#line 343 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void previousTrack();
#line 350 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void startPlaybackFromAlbum(const String &albumName);
#line 386 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void handleRemoval();
#line 399 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
uint8_t updateButton(ButtonState &b, uint32_t now);
#line 416 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
bool readCassetteAlbum(String &album);
#line 437 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void handleButtons();
#line 478 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
size_t activeAvailableForWrite();
#line 483 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
size_t activeWrite(const void *data, size_t len);
#line 488 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void pumpAudio();
#line 518 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void checkCassetteRemoval();
#line 548 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void updateVolume();
#line 577 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void setup();
#line 636 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
void loop();
#line 147 "/Users/morrisaaron/Documents/Arduino/WalkBro/Walkbro/Walkbro.ino"
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

void stopAudioPipeline(bool flushOutput) {
  if (currentTrackFile) currentTrackFile.close();

  decoderStream.end();

  if (flushOutput) {
    audioOutput.flush();
  }

  activeDecoder = nullptr;
  activeCodec = CODEC_NONE;
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
  }
}

void handleRemoval() {
  ledMode = LED_REMOVE_ANIM;

  stopAudioPipeline(false);

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
      playerState = STATE_PAUSED;
    }

    else if (playerState == STATE_PAUSED) {
      playerState = STATE_PLAYING;
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

  if (prevEvt == 2) previousTrack();
  if (nextEvt == 2) nextTrack();
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

  while (currentTrackFile && activeAvailableForWrite() >= sizeof(fileBuffer)) {
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

  if (playerState != STATE_PLAYING && playerState != STATE_PAUSED) return;

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

  updateVolume();
  pumpAudio();
  handleButtons();
  checkCassetteRemoval();

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

