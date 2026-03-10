/**
 * A2ThreeD RoamBro "Tape Emulator" - Plays MP3/WAV files and emulates a walkman style tape device.

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
 * Code Name: RoamBro
 * Description: Plays MP3/WAV files and emulates a walkman style tape device.
 * Date: 3/07/2026
 * Notes: V1.0.5 - Added tape-style NFC behavior, resume state, debug logging, and the first cassette-effect controls
 *        V1.0.10 - Refined cassette sound processing with hiss, saturation, and filter changes
 *        V1.0.15 - Iterated on MP3 track-start pop handling, then removed the ineffective first-pass fixes
 *        V1.0.20 - Reworked scrub stability and fixed resume-state startup behavior
 *        V1.0.25 - Added simple test mode and continued MP3 startup transient experiments
 *        V1.0.30 - Refined transport behavior, battery-saving NFC polling, cassette swap silence flush, and playback-time UID-only tag checks
 *        V1.0.31 - Collapsed header version history into 0.5 summary entries
 *
 * Hardware:
 * RP2350 "zero" microcontroller
 * PCM5102 DAC connected to I2S output pins
 * MicroSD card reader breakout board with SPI interface
 * PN532 NFC reader
 * 10K potentiometer for volume control
 * 3 Buttons for play/pause, next, and previous
 * 1 Function button for future use
 * 1 LED with 330ohm resistor for status indication
 * TP4056 charging module connected to li-ion battery and to 5V input on RP2350 "zero"
 * SPST switch on line between TP4056 output and RP2350 5V input for power control
 * 
 * Functionality:
 * Emulates a cassette tape device that can be controlled with play/pause, next, and previous buttons. The "tapes" are represented 
 * by NFC tags that contain the album name in an NDEF text record. When a tag is detected, the corresponding album folder is loaded 
 * from the SD card and tracks can be played. This behavior mimics a standard cassette player where inserting or removing a tape
 * requires the user to press play/pause to start or resume playback, and the player remembers the position of the tape when removed.
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
#define BTN_FUNC_PIN 7

#define LED_PIN 6
#define VOLUME_ADC_PIN 26

#define VOLUME_ADC_RAW_MIN 220
#define VOLUME_ADC_RAW_MAX 3920

#define SPI_SPEED SD_SCK_MHZ(40)
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 450
#define PLAY_RESET_HOLD_MS 2000
#define BASE_SAMPLE_RATE 44100
#define SCAN_STEP_INTERVAL_MS 100
#define SCAN_PLAYBACK_MULTIPLIER 3.0f
#define PREVIOUS_RESTART_THRESHOLD_SEC 4.0f
#define WAV_MIN_DATA_OFFSET 44
#define AUDIO_PUMP_CHUNKS_PER_LOOP 2
#define MP3_FRAME_SEARCH_WINDOW 8192
// Number of output samples (per channel) to mute after MP3 decoder init.
// At 44100 Hz stereo, 4410 = ~50 ms. Increase up to 8820 (~100 ms) for
// problematic files; decrease to 2205 (~25 ms) if the gap feels too long.
#define MP3_START_MUTE_SAMPLES 4410    // ~50ms, fresh track start
#define MP3_SEEK_MUTE_SAMPLES  2205    // ~25ms, mid-track decoder resync
#define RESUME_STATE_FILE "/walkbro_state.dat"
#define RESUME_STATE_MAGIC 0x5742524FUL
#define RESUME_STATE_VERSION 1
#define MAX_RESUME_RECORDS 32
#define DEBUG_LOGGING 1
#define SIMPLE_TEST_MODE 0
#define SIMPLE_TEST_ALBUM "songs from the big chair"
#define CASSETTE_SOUND_ENABLED 0
#define CASSETTE_FILTER_ENABLED 1
#define CASSETTE_HISS_LEVEL 30
#define CASSETTE_LOWPASS_HZ 13000.0f
#define CASSETTE_DISTORTION_PERCENT 2.0f
#define SCAN_MONITOR_GAIN 1.0f

class CassetteSoundStream : public AudioStream {
 public:
  explicit CassetteSoundStream(AudioStream &output) : out(output) {}

  void setEnabled(bool enabled) {
    is_enabled = enabled;
    leftState = 0.0f;
    rightState = 0.0f;
  }

  bool enabled() const { return is_enabled; }

  // Mute this many output samples after MP3 decoder init to absorb warmup
  // transients. The silence is written to the DAC so there is no gap in the
  // I2S clock, which avoids a secondary click from the output going idle.
  void setMuteOutputSamples(int samples) {
    muteOutputSamplesRemaining = samples;
  }

  void setAudioInfo(AudioInfo newInfo) override {
    AudioStream::setAudioInfo(newInfo);
    out.setAudioInfo(newInfo);
    updateFilterCoefficient(newInfo.sample_rate);
    leftState = 0.0f;
    rightState = 0.0f;
  }

  size_t write(const uint8_t *data, size_t len) override {
    // Output-side mute: replace decoder output with silence during warmup.
    // We still forward the correct number of bytes so the I2S clock keeps
    // running and the DAC does not produce a click from going idle.
    if (muteOutputSamplesRemaining > 0) {
      size_t sampleCount = len / sizeof(int16_t);

      if (muteOutputSamplesRemaining >= (int)sampleCount) {
        // Entire chunk is still in the mute window — send silence.
        muteOutputSamplesRemaining -= sampleCount;
        silenceBuffer.assign(len, 0);
        return out.write(silenceBuffer.data(), len);
      } else {
        // Partial chunk: silence the muted portion, pass the rest normally.
        size_t muteBytes = (size_t)muteOutputSamplesRemaining * sizeof(int16_t);
        muteOutputSamplesRemaining = 0;

        silenceBuffer.resize(len);
        memset(silenceBuffer.data(), 0, muteBytes);
        memcpy(silenceBuffer.data() + muteBytes, data + muteBytes, len - muteBytes);
        data = silenceBuffer.data();
        // Fall through so the cassette processing runs on the unsilenced tail.
      }
    }

    if (!is_enabled || info.bits_per_sample != 16 || len < sizeof(int16_t)) {
      return out.write(data, len);
    }

    tempBuffer.resize(len);
    if (tempBuffer.size() < len) {
      return out.write(data, len);
    }

    memcpy(tempBuffer.data(), data, len);
    int16_t *samples = reinterpret_cast<int16_t *>(tempBuffer.data());
    size_t sampleCount = len / sizeof(int16_t);

    for (size_t i = 0; i < sampleCount; i++) {
      float sample = (float)samples[i];

      if (is_enabled && hissEnabled) {
        float noise = (float)random(-CASSETTE_HISS_LEVEL, CASSETTE_HISS_LEVEL + 1);
        sample += noise;
      }

      bool isLeft = (i % 2 == 0);
      float &lpState = isLeft ? leftState : rightState;

      if (is_enabled && filterEnabled) {
        lpState += lowpassAlpha * (sample - lpState);
      } else {
        lpState = sample;
      }

      float processed = lpState;
      if (is_enabled && distortionEnabled) {
        float driven = lpState * distortionDrive;
        processed = tanhf(driven / 32768.0f) * 32768.0f / distortionDrive;
      }

      long clipped = lroundf(processed);
      if (clipped > 32767) clipped = 32767;
      if (clipped < -32768) clipped = -32768;
      samples[i] = (int16_t)clipped;
    }

    return out.write(tempBuffer.data(), len);
  }

  int availableForWrite() override { return out.availableForWrite(); }

  void flush() override { out.flush(); }

 private:
  AudioStream &out;
  bool is_enabled = false;
  float lowpassAlpha = 1.0f;
  float distortionDrive = 1.02f;
  bool filterEnabled = CASSETTE_FILTER_ENABLED;
  bool hissEnabled = CASSETTE_HISS_LEVEL > 0;
  bool distortionEnabled = CASSETTE_DISTORTION_PERCENT > 0.0f;
  float leftState = 0.0f;
  float rightState = 0.0f;
  std::vector<uint8_t> tempBuffer;
  std::vector<uint8_t> silenceBuffer;
  int muteOutputSamplesRemaining = 0;

  void updateFilterCoefficient(uint32_t sampleRate) {
    filterEnabled = CASSETTE_FILTER_ENABLED;
    hissEnabled = CASSETTE_HISS_LEVEL > 0;
    distortionEnabled = CASSETTE_DISTORTION_PERCENT > 0.0f;
    distortionDrive = 1.0f + (CASSETTE_DISTORTION_PERCENT / 100.0f);
    if (distortionDrive < 1.0f) distortionDrive = 1.0f;

    if (sampleRate == 0) {
      lowpassAlpha = 1.0f;
      return;
    }

    float dt = 1.0f / (float)sampleRate;
    float lowpassRc = 1.0f / (2.0f * PI * CASSETTE_LOWPASS_HZ);

    lowpassAlpha = dt / (lowpassRc + dt);
    if (lowpassAlpha < 0.0f) lowpassAlpha = 0.0f;
    if (lowpassAlpha > 1.0f) lowpassAlpha = 1.0f;
  }
};

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
CassetteSoundStream cassetteSound(volumeControl);
MP3DecoderHelix mp3Decoder;
WAVDecoder wavDecoder;
EncodedAudioStream decoderStream(&cassetteSound, &mp3Decoder);
AudioDecoder *activeDecoder = nullptr;

enum CodecType {
  CODEC_NONE,
  CODEC_MP3,
  CODEC_WAV
};

CodecType activeCodec = CODEC_NONE;

float currentGain = 0.5f;
float filteredPotNorm = 0.5f;
bool cassetteSoundEnabled = CASSETTE_SOUND_ENABLED;

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
ButtonState btnFunc = {BTN_FUNC_PIN, true, true, false, 0, 0};

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
String presentAlbumName;
uint8_t loadedTagUid[7] = {0};
uint8_t loadedTagUidLength = 0;
bool hasLoadedTag = false;
uint8_t presentTagUid[7] = {0};
uint8_t presentTagUidLength = 0;
bool hasPresentTag = false;
bool ignorePlayRelease = false;
uint32_t trackElapsedBeforePauseMs = 0;
uint32_t trackPlayStartedAtMs = 0;

struct ResumeRecord {
  uint8_t uidLength;
  uint8_t uid[7];
  uint16_t trackIndex;
  uint32_t filePosition;
  uint32_t sequence;
};

struct ResumeStore {
  uint32_t magic;
  uint16_t version;
  uint16_t recordCount;
  ResumeRecord records[MAX_RESUME_RECORDS];
};

ResumeStore resumeStore = {
  RESUME_STATE_MAGIC,
  RESUME_STATE_VERSION,
  MAX_RESUME_RECORDS,
  {}
};
bool resumeStoreLoaded = false;

#if DEBUG_LOGGING
#define DBG_PRINT(x) Serial.print(x)
#define DBG_PRINTLN(x) Serial.println(x)
#else
#define DBG_PRINT(x) do {} while (0)
#define DBG_PRINTLN(x) do {} while (0)
#endif

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

String baseNameFromPath(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash < 0) return path;
  return path.substring(slash + 1);
}

bool parseLeadingNumber(const String &text, long &valueOut) {
  valueOut = 0;
  bool foundDigit = false;

  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c < '0' || c > '9') break;
    foundDigit = true;
    valueOut = (valueOut * 10) + (c - '0');
  }

  return foundDigit;
}

bool playlistSortLess(const String &lhsPath, const String &rhsPath) {
  String lhsName = baseNameFromPath(lhsPath);
  String rhsName = baseNameFromPath(rhsPath);

  long lhsNumber = 0;
  long rhsNumber = 0;
  bool lhsHasNumber = parseLeadingNumber(lhsName, lhsNumber);
  bool rhsHasNumber = parseLeadingNumber(rhsName, rhsNumber);

  if (lhsHasNumber && rhsHasNumber) {
    if (lhsNumber != rhsNumber) return lhsNumber < rhsNumber;
  }

  return lhsName < rhsName;
}

void clearPlaylist() {
  playlist.clear();
  currentTrackIndex = -1;
  currentAlbumName = "";
}

bool tagUidMatches(const uint8_t *lhs, uint8_t lhsLength,
                   const uint8_t *rhs, uint8_t rhsLength) {
  if (lhsLength != rhsLength) return false;
  return memcmp(lhs, rhs, lhsLength) == 0;
}

bool loadedTagMatches(const uint8_t *uid, uint8_t uidLength) {
  if (!hasLoadedTag) return false;
  return tagUidMatches(loadedTagUid, loadedTagUidLength, uid, uidLength);
}

bool presentTagMatches(const uint8_t *uid, uint8_t uidLength) {
  if (!hasPresentTag) return false;
  return tagUidMatches(presentTagUid, presentTagUidLength, uid, uidLength);
}

void rememberLoadedTag(const uint8_t *uid, uint8_t uidLength) {
  if (uidLength > sizeof(loadedTagUid)) uidLength = sizeof(loadedTagUid);
  memcpy(loadedTagUid, uid, uidLength);
  loadedTagUidLength = uidLength;
  hasLoadedTag = true;
}

void clearLoadedTag() {
  hasLoadedTag = false;
  loadedTagUidLength = 0;
}

void rememberPresentTag(const uint8_t *uid, uint8_t uidLength, const String &albumName) {
  if (uidLength > sizeof(presentTagUid)) uidLength = sizeof(presentTagUid);
  memcpy(presentTagUid, uid, uidLength);
  presentTagUidLength = uidLength;
  hasPresentTag = true;
  presentAlbumName = albumName;
}

void clearPresentTag() {
  hasPresentTag = false;
  presentTagUidLength = 0;
  presentAlbumName = "";
}

const char *playerStateName(PlayerState state) {
  switch (state) {
    case STATE_IDLE: return "IDLE";
    case STATE_PLAYING: return "PLAYING";
    case STATE_PAUSED: return "PAUSED";
    default: return "UNKNOWN";
  }
}

const char *pauseReasonName(PauseReason reason) {
  switch (reason) {
    case PAUSE_NONE: return "NONE";
    case PAUSE_USER: return "USER";
    case PAUSE_TAG_REMOVED: return "TAG_REMOVED";
    default: return "UNKNOWN";
  }
}

void logPlayerState(const char *context) {
  DBG_PRINT("[STATE] ");
  DBG_PRINT(context);
  DBG_PRINT(" -> ");
  DBG_PRINT(playerStateName(playerState));
  if (playerState == STATE_PAUSED) {
    DBG_PRINT(" reason=");
    DBG_PRINT(pauseReasonName(pauseReason));
  }
  DBG_PRINT(" trackIndex=");
  DBG_PRINT(currentTrackIndex);
  DBG_PRINT(" album=");
  DBG_PRINTLN(currentAlbumName);
}

void logTagUid(const char *prefix, const uint8_t *uid, uint8_t uidLength) {
  #if DEBUG_LOGGING
  Serial.print(prefix);
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 16) Serial.print("0");
    Serial.print(uid[i], HEX);
    if (i + 1 < uidLength) Serial.print(":");
  }
  Serial.println("");
  #else
  (void)prefix;
  (void)uid;
  (void)uidLength;
  #endif
}

uint32_t currentTrackPositionBytes() {
  if (!currentTrackFile) return 0;
  long pos = (long)currentTrackFile.position();
  if (pos < 0) pos = 0;
  return (uint32_t)pos;
}

bool loadResumeStore() {
  if (resumeStoreLoaded) return true;

  memset(&resumeStore, 0, sizeof(resumeStore));
  resumeStore.magic = RESUME_STATE_MAGIC;
  resumeStore.version = RESUME_STATE_VERSION;
  resumeStore.recordCount = MAX_RESUME_RECORDS;

  File stateFile = SD.open(RESUME_STATE_FILE, FILE_READ);
  if (!stateFile) {
    DBG_PRINTLN("[SAVE] No existing resume store, starting fresh");
    resumeStoreLoaded = true;
    return true;
  }

  if (stateFile.size() == (int)sizeof(ResumeStore)) {
    stateFile.read((uint8_t *)&resumeStore, sizeof(ResumeStore));
  }
  stateFile.close();

  if (resumeStore.magic != RESUME_STATE_MAGIC ||
      resumeStore.version != RESUME_STATE_VERSION ||
      resumeStore.recordCount != MAX_RESUME_RECORDS) {
    DBG_PRINTLN("[SAVE] Resume store invalid, resetting");
    memset(&resumeStore, 0, sizeof(resumeStore));
    resumeStore.magic = RESUME_STATE_MAGIC;
    resumeStore.version = RESUME_STATE_VERSION;
    resumeStore.recordCount = MAX_RESUME_RECORDS;
  }

  DBG_PRINTLN("[SAVE] Resume store loaded");
  resumeStoreLoaded = true;
  return true;
}

bool saveResumeStore() {
  if (!loadResumeStore()) return false;

  SD.remove(RESUME_STATE_FILE);
  File stateFile = SD.open(RESUME_STATE_FILE, FILE_WRITE);
  if (!stateFile) {
    DBG_PRINTLN("[SAVE] Unable to open resume store");
    return false;
  }

  size_t written = stateFile.write((const uint8_t *)&resumeStore, sizeof(ResumeStore));
  stateFile.flush();
  stateFile.close();

  if (written != sizeof(ResumeStore)) {
    DBG_PRINTLN("[SAVE] Unable to write resume store");
    return false;
  }

  DBG_PRINT("[SAVE] Resume store written bytes=");
  DBG_PRINTLN((int)written);
  return true;
}

int findResumeRecordIndex(const uint8_t *uid, uint8_t uidLength) {
  if (!loadResumeStore()) return -1;

  for (int i = 0; i < MAX_RESUME_RECORDS; i++) {
    const ResumeRecord &record = resumeStore.records[i];
    if (record.sequence == 0 || record.uidLength == 0) continue;
    if (tagUidMatches(record.uid, record.uidLength, uid, uidLength)) {
      return i;
    }
  }

  return -1;
}

int findResumeRecordSlot() {
  if (!loadResumeStore()) return -1;

  int oldestIndex = 0;
  uint32_t oldestSequence = UINT32_MAX;

  for (int i = 0; i < MAX_RESUME_RECORDS; i++) {
    const ResumeRecord &record = resumeStore.records[i];
    if (record.sequence == 0 || record.uidLength == 0) return i;
    if (record.sequence < oldestSequence) {
      oldestSequence = record.sequence;
      oldestIndex = i;
    }
  }

  return oldestIndex;
}

uint32_t nextResumeSequence() {
  uint32_t nextSequence = 1;
  for (int i = 0; i < MAX_RESUME_RECORDS; i++) {
    uint32_t candidate = resumeStore.records[i].sequence + 1;
    if (candidate > nextSequence) nextSequence = candidate;
  }
  return nextSequence;
}

bool loadResumeRecord(const uint8_t *uid, uint8_t uidLength, ResumeRecord &recordOut) {
  int index = findResumeRecordIndex(uid, uidLength);
  if (index < 0) return false;
  recordOut = resumeStore.records[index];
  DBG_PRINT("[SAVE] Loaded resume record slot=");
  DBG_PRINT(index);
  DBG_PRINT(" track=");
  DBG_PRINT(recordOut.trackIndex);
  DBG_PRINT(" pos=");
  DBG_PRINTLN((unsigned long)recordOut.filePosition);
  return true;
}

bool saveResumeRecord(const uint8_t *uid, uint8_t uidLength,
                      uint16_t trackIndex, uint32_t filePosition) {
  if (!loadResumeStore()) return false;

  int index = findResumeRecordIndex(uid, uidLength);
  if (index < 0) {
    index = findResumeRecordSlot();
    if (index < 0) return false;
  }

  ResumeRecord &record = resumeStore.records[index];
  memset(&record, 0, sizeof(record));
  if (uidLength > sizeof(record.uid)) uidLength = sizeof(record.uid);
  record.uidLength = uidLength;
  memcpy(record.uid, uid, uidLength);
  record.trackIndex = trackIndex;
  record.filePosition = filePosition;
  record.sequence = nextResumeSequence();

  DBG_PRINT("[SAVE] Storing resume slot=");
  DBG_PRINT(index);
  DBG_PRINT(" track=");
  DBG_PRINT(trackIndex);
  DBG_PRINT(" pos=");
  DBG_PRINTLN((unsigned long)filePosition);

  return saveResumeStore();
}

void stopAudioPipeline(bool flushOutput) {
  if (currentTrackFile) currentTrackFile.close();
  decoderStream.end();

  if (flushOutput) {
    flushI2SWithSilence();  // push silence through DMA buffers instead of flush()
  }

  activeDecoder = nullptr;
  activeCodec = CODEC_NONE;
}

void restartDecoderAtCurrentPosition() {
  if (activeDecoder == nullptr) return;

  if (activeCodec == CODEC_MP3) {
    cassetteSound.setMuteOutputSamples(MP3_SEEK_MUTE_SAMPLES);
  }

  decoderStream.end();
  decoderStream.setDecoder(activeDecoder);
  decoderStream.begin();
  applyPlaybackVolume();
}

void applyPlaybackVolume() {
  float effectiveGain = currentGain;
  if (scanMode != SCAN_NONE) {
    effectiveGain *= SCAN_MONITOR_GAIN;
  }
  volumeControl.setVolume(effectiveGain);
}

void setCassetteSoundEnabled(bool enabled) {
  cassetteSoundEnabled = enabled;
  cassetteSound.setEnabled(enabled);
  DBG_PRINT("[CASSETTE] ");
  DBG_PRINTLN(cassetteSoundEnabled ? "Enabled" : "Disabled");
}

void setPaused(PauseReason reason) {
  if (playerState == STATE_IDLE) return;
  if (playerState == STATE_PLAYING) {
    trackElapsedBeforePauseMs += millis() - trackPlayStartedAtMs;
  }
  saveLoadedTagResumeState();
  playerState = STATE_PAUSED;
  pauseReason = reason;
  scanMode = SCAN_NONE;
  logPlayerState("Paused");
}

void resumePlayback() {
  if (playerState == STATE_IDLE) return;
  playerState = STATE_PLAYING;
  pauseReason = PAUSE_NONE;
  trackPlayStartedAtMs = millis();
  logPlayerState("Resumed");
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

bool seekCurrentTrackAbsolute(long target) {
  if (!currentTrackFile || activeCodec == CODEC_NONE) return false;

  long minPos = 0;
  if (activeCodec == CODEC_WAV) {
    minPos = WAV_MIN_DATA_OFFSET;
  }

  long maxPos = (long)currentTrackFile.size() - 1;
  if (maxPos < minPos) maxPos = minPos;

  if (target < minPos) target = minPos;
  if (target > maxPos) target = maxPos;

  if (activeCodec == CODEC_MP3) {
    target = findMp3FrameBoundary(target, false);
  }

  if (target == (long)currentTrackFile.position()) return true;
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
  applyPlaybackVolume();
}

void stopScanPlayback() {
  scanMode = SCAN_NONE;
  audioOutput.flush();
  if (activeCodec == CODEC_MP3 && activeDecoder != nullptr) {
    restartDecoderAtCurrentPosition();
  }
  applyPlaybackVolume();
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

void saveLoadedTagResumeState() {
  if (!hasLoadedTag || currentTrackIndex < 0) return;
  DBG_PRINT("[SAVE] Saving loaded tag state track=");
  DBG_PRINT(currentTrackIndex);
  DBG_PRINT(" pos=");
  DBG_PRINTLN((unsigned long)currentTrackPositionBytes());
  saveResumeRecord(loadedTagUid, loadedTagUidLength,
                   (uint16_t)currentTrackIndex,
                   currentTrackPositionBytes());
}

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
  trackElapsedBeforePauseMs = 0;
  trackPlayStartedAtMs = millis();

  // For MP3 files, mute the output for a short window after decoder init so
  // that Helix warmup transients (Huffman table setup, synthesis filterbank
  // priming) are replaced with silence rather than forwarded to the DAC.
  if (nextCodec == CODEC_MP3) {
    cassetteSound.setMuteOutputSamples(MP3_START_MUTE_SAMPLES);
    DBG_PRINT("[PLAY] MP3 startup mute samples=");
    DBG_PRINTLN(MP3_START_MUTE_SAMPLES);
  }

  decoderStream.setDecoder(activeDecoder);
  if (!decoderStream.begin()) {
    stopAudioPipeline(false);
    return false;
  }

  applyPlaybackVolume();
  playerState = STATE_PLAYING;

  DBG_PRINT("[PLAY] Track started idx=");
  DBG_PRINT(idx);
  DBG_PRINT(" path=");
  DBG_PRINTLN(path);
  logPlayerState("TrackStart");
  return true;
}

void nextTrack() {
  if (playlist.empty()) return;
  if (startTrackByIndex((currentTrackIndex + 1) % playlist.size()) && hasLoadedTag) {
    saveLoadedTagResumeState();
  }
}

uint32_t currentTrackPlayedMs() {
  uint32_t playedMs = trackElapsedBeforePauseMs;
  if (playerState == STATE_PLAYING) {
    playedMs += millis() - trackPlayStartedAtMs;
  }
  return playedMs;
}

void previousTrack() {
  if (playlist.empty()) return;

  if (currentTrackIndex >= 0) {
    uint32_t restartThresholdMs = (uint32_t)(PREVIOUS_RESTART_THRESHOLD_SEC * 1000.0f);
    if (currentTrackPlayedMs() > restartThresholdMs) {
      if (startTrackByIndex(currentTrackIndex) && hasLoadedTag) {
        saveLoadedTagResumeState();
      }
      return;
    }
  }

  int prev = currentTrackIndex - 1;
  if (prev < 0) prev = playlist.size() - 1;
  if (startTrackByIndex(prev) && hasLoadedTag) {
    saveLoadedTagResumeState();
  }
}

bool loadAlbumPlaylist(const String &albumName) {

  clearPlaylist();

  String folder = "/" + albumName;
  File dir = SD.open(folder);

  if (!dir || !dir.isDirectory()) {
    DBG_PRINTLN("[PLAY] Album folder not found");
    return false;
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
    DBG_PRINTLN("[PLAY] No tracks found");
    return false;
  }

  std::sort(playlist.begin(), playlist.end(), playlistSortLess);
  currentAlbumName = albumName;
  return true;
}

bool startPlaybackFromAlbum(const String &albumName, int trackIndex = 0, long resumePosition = 0) {
  if (!loadAlbumPlaylist(albumName)) return false;

  if (trackIndex < 0 || trackIndex >= (int)playlist.size()) {
    trackIndex = 0;
    resumePosition = 0;
  }

  if (!startTrackByIndex(trackIndex)) {
    DBG_PRINTLN("[PLAY] Unable to start requested track");
    clearPlaylist();
    clearLoadedTag();
    playerState = STATE_IDLE;
    logPlayerState("StartFailed");
    return false;
  }

  if (resumePosition > 0) {
    DBG_PRINT("[PLAY] Applying resume position=");
    DBG_PRINTLN((unsigned long)resumePosition);
    if (!seekCurrentTrackAbsolute(resumePosition)) {
      DBG_PRINTLN("[PLAY] Resume seek failed, restarting track from beginning");
      seekCurrentTrackAbsolute(0);
    }
  }

  return true;
}

bool startPlaybackForTag(const String &albumName, const uint8_t *uid, uint8_t uidLength) {
  ResumeRecord record;
  bool hasRecord = loadResumeRecord(uid, uidLength, record);
  logTagUid("[TAG] Starting for UID ", uid, uidLength);

  int trackIndex = hasRecord ? record.trackIndex : 0;
  long resumePosition = hasRecord ? (long)record.filePosition : 0;
  if (!startPlaybackFromAlbum(albumName, trackIndex, resumePosition)) {
    return false;
  }

  rememberLoadedTag(uid, uidLength);
  return true;
}

void handleCassetteRemoval() {
  ledMode = LED_REMOVE_ANIM;
  DBG_PRINTLN("[TAG] Cassette removed or changed while playing");
  flushI2SWithSilence();
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

bool readCassetteAlbum(String &album, uint8_t *uidOut = nullptr, uint8_t *uidLengthOut = nullptr) {

  uint8_t uid[7];
  uint8_t uidLength;

  for (int i = 0; i < 4; i++) {
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
      album = readNDEFText();
      album.trim();
      album.toLowerCase();

      if (album.length() > 0) {
        if (uidOut != nullptr) memcpy(uidOut, uid, uidLength);
        if (uidLengthOut != nullptr) *uidLengthOut = uidLength;
        return true;
      }

      DBG_PRINTLN("[TAG] Tag present but NDEF unreadable");
    }
    delay(30);
  }

  return false;
}

bool refreshPresentCassetteOnDemand() {
  String album;
  uint8_t uid[7];
  uint8_t uidLength = 0;

  if (readCassetteAlbum(album, uid, &uidLength)) {
    rememberPresentTag(uid, uidLength, album);
    logTagUid("[TAG] On-demand UID ", uid, uidLength);
    return true;
  }

  clearPresentTag();
  DBG_PRINTLN("[TAG] Cassette not detected");
  return false;
}

bool tryStartPresentAlbum() {
  #if SIMPLE_TEST_MODE
  if (currentAlbumName == SIMPLE_TEST_ALBUM && !playlist.empty()) {
    return startTrackByIndex(0);
  }

  return startPlaybackFromAlbum(SIMPLE_TEST_ALBUM, 0, 0);
  #endif

  if (!hasPresentTag || presentAlbumName.length() == 0) {
    DBG_PRINTLN("[TAG] Cassette not detected");
    return false;
  }

  DBG_PRINT("[TAG] Present album=");
  DBG_PRINTLN(presentAlbumName);
  if (startPlaybackForTag(presentAlbumName, presentTagUid, presentTagUidLength)) {
    ledMode = LED_INSERT_ANIM;
    return true;
  }

  return false;
}

void resetLoadedAlbumToStart() {
  #if SIMPLE_TEST_MODE
  if (currentAlbumName.length() == 0) return;

  DBG_PRINTLN("[PLAY] Resetting test album to first track");
  stopAudioPipeline(true);
  clearPlaylist();
  currentTrackIndex = -1;
  playerState = STATE_IDLE;
  pauseReason = PAUSE_NONE;
  scanMode = SCAN_NONE;
  ignorePlayRelease = true;
  logPlayerState("ResetToStart");
  return;
  #endif

  if (!hasLoadedTag || currentAlbumName.length() == 0) return;

  DBG_PRINTLN("[PLAY] Resetting loaded album to first track");
  saveResumeRecord(loadedTagUid, loadedTagUidLength, 0, 0);
  stopAudioPipeline(true);
  clearPlaylist();
  currentTrackIndex = -1;
  playerState = STATE_IDLE;
  pauseReason = PAUSE_NONE;
  scanMode = SCAN_NONE;
  ignorePlayRelease = true;
  logPlayerState("ResetToStart");
}

void handlePlayButtonShortPress() {
  #if SIMPLE_TEST_MODE
  if (playerState == STATE_PLAYING) {
    setPaused(PAUSE_USER);
  } else if (playerState == STATE_PAUSED) {
    resumePlayback();
  } else if (playerState == STATE_IDLE) {
    tryStartPresentAlbum();
  }
  return;
  #endif

  if (playerState == STATE_PLAYING) {
    setPaused(PAUSE_USER);
  }

  else if (playerState == STATE_PAUSED) {
    if (!refreshPresentCassetteOnDemand()) {
    } else if (loadedTagMatches(presentTagUid, presentTagUidLength)) {
      resumePlayback();
    } else {
      DBG_PRINT("[TAG] Album changed to ");
      DBG_PRINTLN(presentAlbumName);
      if (startPlaybackForTag(presentAlbumName, presentTagUid, presentTagUidLength)) {
        ledMode = LED_INSERT_ANIM;
      }
    }
  }

  else if (playerState == STATE_IDLE) {
    if (refreshPresentCassetteOnDemand()) {
      tryStartPresentAlbum();
    }
  }
}

void handleButtons() {

  uint32_t now = millis();

  uint8_t prevEvt = updateButton(btnPrev, now);
  uint8_t playEvt = updateButton(btnPlay, now);
  uint8_t nextEvt = updateButton(btnNext, now);
  uint8_t funcEvt = updateButton(btnFunc, now);

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

  if (!btnPlay.stableLevel && !btnPlay.longHandled &&
      (now - btnPlay.pressedAt) >= PLAY_RESET_HOLD_MS) {
    btnPlay.longHandled = true;
    resetLoadedAlbumToStart();
  }

  if (playEvt == 2 && ignorePlayRelease) {
    ignorePlayRelease = false;
  } else if (playEvt == 2 && !btnPlay.longHandled) {
    handlePlayButtonShortPress();
  }

  if (funcEvt == 2 && !btnFunc.longHandled) {
    setCassetteSoundEnabled(!cassetteSoundEnabled);
  }

  if (prevEvt == 2) {
    if (scanMode == SCAN_REWIND) {
      stopScanPlayback();
    } else if (!btnPrev.longHandled && playerState == STATE_PLAYING) {
      previousTrack();
    }
  }

  if (nextEvt == 2) {
    if (scanMode == SCAN_FAST_FORWARD) {
      stopScanPlayback();
    } else if (!btnNext.longHandled && playerState == STATE_PLAYING) {
      nextTrack();
    }
  }
}

void flushI2SWithSilence() {
  const size_t silenceBytes = 8 * 512; // buffer_count * buffer_size
  uint8_t silence[64] = {0};
  size_t written = 0;
  while (written < silenceBytes) {
    size_t chunk = min(sizeof(silence), silenceBytes - written);
    written += audioOutput.write(silence, chunk);
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
// NFC STATUS
// ========================================================

void checkCassetteStatus() {

  static uint32_t last = 0;
  static uint8_t missCount = 0;

  if (millis() - last < 300) return;
  last = millis();

  uint8_t uid[7], uidLen;

  bool seen = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

  if (seen) {
    missCount = 0;

    if (presentTagMatches(uid, uidLen)) {
      return;
    }

    rememberPresentTag(uid, uidLen, "");
    logTagUid("[TAG] Present UID ", uid, uidLen);

    if (playerState == STATE_PLAYING && !loadedTagMatches(uid, uidLen)) {
      handleCassetteRemoval();
    }

    return;
  }

  if (++missCount >= 3) {
    missCount = 0;
    clearPresentTag();
    DBG_PRINTLN("[TAG] No cassette detected");
    if (playerState == STATE_PLAYING) {
      handleCassetteRemoval();
    }
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
    applyPlaybackVolume();
    lastGain = currentGain;
    DBG_PRINT("[VOL] Gain=");
    DBG_PRINTLN(currentGain);
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
  pinMode(btnFunc.pin, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_CLK_PIN);
  SPI.begin();

  if (!SD.begin(SD_CS_PIN, SPI_SPEED, SPI)) {
    DBG_PRINTLN("[BOOT] SD mount failed");
    while (1);
  }

  auto audioConfig = audioOutput.defaultConfig(TX_MODE);
  audioConfig.sample_rate = BASE_SAMPLE_RATE;
  audioConfig.bits_per_sample = 16;
  audioConfig.channels = 2;
  audioConfig.pin_bck = I2S_BCLK;
  audioConfig.pin_ws = I2S_LRCLK;
  audioConfig.pin_data = I2S_DOUT;
  audioConfig.buffer_count = 8;
  audioConfig.buffer_size = 512;
  audioOutput.begin(audioConfig);

  auto volumeConfig = volumeControl.defaultConfig();
  volumeConfig.sample_rate = BASE_SAMPLE_RATE;
  volumeConfig.bits_per_sample = 16;
  volumeConfig.channels = 2;
  volumeConfig.volume = currentGain;
  volumeControl.begin(volumeConfig);

  mp3Decoder.addNotifyAudioChange(volumeControl);
  wavDecoder.addNotifyAudioChange(volumeControl);
  cassetteSound.setEnabled(cassetteSoundEnabled);

  analogReadResolution(12);
  randomSeed(analogRead(VOLUME_ADC_PIN));

  nfc.begin();
  nfc.SAMConfig();

  delay(200);

  ledMode = LED_BOOT_ANIM;
  DBG_PRINTLN("[BOOT] RoamBro ready");
  DBG_PRINT("[CASSETTE] Startup=");
  DBG_PRINTLN(cassetteSoundEnabled ? "Enabled" : "Disabled");
  #if SIMPLE_TEST_MODE
  DBG_PRINT("[TEST] Simple mode album=");
  DBG_PRINTLN(SIMPLE_TEST_ALBUM);
  #endif
  logPlayerState("Boot");
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
    updateVolume();
    updateLED();
    return;
  }

  #if !SIMPLE_TEST_MODE
  if (playerState == STATE_PLAYING) {
  checkCassetteStatus();
  }
  #endif
  handleButtons();
  updateVolume();
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
