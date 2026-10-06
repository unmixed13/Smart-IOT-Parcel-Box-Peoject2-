/*
  Smart Parcel Box - main controller logic (hardware independent)
  ============================================================================
  Everything that decides *what the box does* lives here, with no Arduino or
  FreeRTOS dependency, so it is compiled and unit-tested on a PC with a fake
  clock (see firmware/tests). parcel_box_main.ino only wires it to the pins,
  the scanner UART and the network task.

  Implements design document section 3.2.1:
    (1) QR scan -> server verifies (3 s limit) -> coil energised for Tunlock
        (10 s, server-supplied, clamped); rescanning restarts the timer.
    (2) door closes -> coil off immediately (auto-lock); door open longer than
        Tajar (60 s) -> buzzer + one LINE alert per event.
    (3) after locking -> camera trigger pulse + "locked" report (Event-ID).
    (4) push button -> local unlock that never depends on the network,
        reported as a manual event and never photographed.

  Safety rules baked in (each one has a unit test):
    * The lock is energised ONLY by a granted verification or the button, and
      every path back to "locked" is timer- or door-driven, never network-driven.
    * The coil is cut after COIL_MAX regardless of state (thermal protection).
    * All time comparisons are wrap-safe (millis() rolls over after ~49 days).
    * Nothing in here blocks: no delay(), no network calls.
*/
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace pb {

// ---------------------------------------------------------------------------
// Configuration (defaults follow the thesis; the server may refine two values)
// ---------------------------------------------------------------------------
struct Config {
  uint32_t unlockMs          = 10000;   // Tunlock
  uint32_t doorOpenAlertMs   = 60000;   // Tajar
  uint32_t verifyTimeoutMs   = 3000;    // "no answer within 3 s -> deny"
  uint32_t doorDebounceMs    = 100;
  uint32_t buttonDebounceMs  = 80;
  uint32_t camPulseMs        = 200;
  uint32_t coilMaxMs         = 30000;   // thermal cap for the solenoid coil
  uint32_t tamperGraceMs     = 1500;    // ignore door rebound right after locking
  uint32_t scanRepeatMs      = 2000;    // GM66 may re-read the same code repeatedly
  uint32_t alarmMaxMs        = 120000;  // stop beeping eventually
  uint32_t alarmPeriodMs     = 2000;
  uint32_t reportValidMs     = 60000;   // default lifetime of a queued report
  uint32_t unlockReportValidMs = 15000; // Tunlock + 5 s retry margin (thesis note)
};

// Buzzer patterns: on/off durations in ms, starting with "on"; 0 terminates.
// (Namespace-scope tables rather than `inline` static members: the ESP32
// Arduino core 2.x compiles with C++11, where inline variables do not exist.)
static const uint16_t kBeepConfirm[] = {120, 0};
static const uint16_t kBeepDenied[]  = {120, 120, 120, 0};
static const uint16_t kBeepError[]   = {120, 120, 120, 120, 120, 0};
static const uint16_t kBeepAlarm[]   = {450, 0};

static const uint32_t kUnlockMin = 5000,   kUnlockMax = 20000;
static const uint32_t kAlertMin  = 20000,  kAlertMax  = 300000;
static const size_t   kIdLen     = 40;     // UUID (36) + NUL, with margin

inline bool elapsed(uint32_t now, uint32_t since, uint32_t dur) {
  return (uint32_t)(now - since) >= dur;   // wrap-safe
}

// ---------------------------------------------------------------------------
// Interfaces to the outside world
// ---------------------------------------------------------------------------
class Hal {
 public:
  virtual ~Hal() {}
  virtual uint32_t millis() = 0;
  virtual void coil(bool energise) = 0;
  virtual void buzzer(bool on) = 0;
  virtual void camPin(bool high) = 0;
  virtual bool doorClosedRaw() = 0;       // reed switch: LOW = closed
  virtual bool buttonPressedRaw() = 0;    // active-low button
  virtual void log(const char *msg) = 0;
};

enum class ReportType : uint8_t { DoorOpened, UnlockTimeout, Locked, DoorLeftOpen, Tamper };

inline const char *reportName(ReportType t) {
  switch (t) {
    case ReportType::DoorOpened:    return "door_opened";
    case ReportType::UnlockTimeout: return "unlock_timeout";
    case ReportType::Locked:        return "locked";
    case ReportType::DoorLeftOpen:  return "door_ajar";
    case ReportType::Tamper:        return "tamper";
  }
  return "";
}

class Net {
 public:
  virtual ~Net() {}
  virtual void requestVerify(const char *code) = 0;                 // async
  virtual void queueReport(ReportType t, const char *eventId, uint32_t validForMs) = 0;
  virtual void queueManual() = 0;                                   // async
};

// ---------------------------------------------------------------------------
// Bounded retry queue for reports (head-of-line, expiry, drop-oldest)
// ---------------------------------------------------------------------------
class ReportQueue {
 public:
  struct Item {
    ReportType type;
    char eventId[kIdLen];
    uint32_t createdAt;
    uint32_t validMs;
    uint32_t nextTry;
    uint8_t attempts;
  };
  static const int kCap = 8;

  void push(ReportType t, const char *eventId, uint32_t now, uint32_t validMs) {
    if (n_ == kCap) {  // full: the oldest report is the least useful one
      for (int i = 1; i < n_; i++) q_[i - 1] = q_[i];
      n_--;
      dropped_++;
    }
    Item &it = q_[n_++];
    it.type = t;
    strncpy(it.eventId, eventId ? eventId : "", kIdLen - 1);
    it.eventId[kIdLen - 1] = 0;
    it.createdAt = now;
    it.validMs = validMs;
    it.nextTry = now;
    it.attempts = 0;
  }
  // Returns true and fills `out` if the head item is due now. Expired heads are discarded.
  bool front(uint32_t now, Item &out) {
    while (n_ > 0 && elapsed(now, q_[0].createdAt, q_[0].validMs)) { popFront(); expired_++; }
    if (n_ == 0) return false;
    if ((int32_t)(now - q_[0].nextTry) < 0) return false;        // wrap-safe "not due yet"
    out = q_[0];
    return true;
  }
  void done() { if (n_ > 0) popFront(); }
  void retryIn(uint32_t now, uint32_t delayMs) {
    if (n_ == 0) return;
    q_[0].attempts++;
    q_[0].nextTry = now + delayMs;
  }
  int size() const { return n_; }
  int dropped() const { return dropped_; }
  int expired() const { return expired_; }

 private:
  void popFront() {
    for (int i = 1; i < n_; i++) q_[i - 1] = q_[i];
    n_--;
  }
  Item q_[kCap];
  int n_ = 0, dropped_ = 0, expired_ = 0;
};

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------
enum class State : uint8_t { Locked, Unlocked, DoorOpen };

class BoxLogic {
 public:
  BoxLogic(Hal &hal, Net &net, Config cfg = Config()) : hal_(hal), net_(net), cfg_(cfg) {}

  void begin() {
    setCoil(false);
    hal_.buzzer(false);
    hal_.camPin(false);
    uint32_t now = hal_.millis();
    rawDoorClosed_ = hal_.doorClosedRaw();
    doorClosed_ = rawDoorClosed_;
    rawDoorSince_ = now;
    lockedAt_ = now;
    if (!doorClosed_) {            // booted with the door open: not a tamper, but Tajar applies
      doorOpenSince_ = now;
      hal_.log("boot: door open");
    }
    startBeep(kBeepConfirm);
  }

  void loop() {
    uint32_t now = hal_.millis();
    pollDoor(now);
    pollButton(now);
    serviceTimers(now);
    serviceBeep(now);
  }

  // --- inputs from the scanner / network task ----------------------------
  void onScan(const char *raw) {
    uint32_t now = hal_.millis();
    char code[kIdLen];
    if (!sanitize(raw, code)) {            // not shaped like one of our UUID codes
      hal_.log("scan: rejected locally (not a valid code)");
      startBeep(kBeepDenied);
      return;
    }
    if (state_ == State::DoorOpen) return;                  // nothing to do while open
    if (verifying_) return;                                 // one request at a time
    if (lastScan_[0] && strcmp(lastScan_, code) == 0 && !elapsed(now, lastScanAt_, cfg_.scanRepeatMs))
      return;                                               // scanner re-reading the same code
    strncpy(lastScan_, code, kIdLen - 1);
    lastScan_[kIdLen - 1] = 0;
    lastScanAt_ = now;
    verifying_ = true;
    verifyAt_ = now;
    net_.requestVerify(code);
  }

  void onVerifyResult(bool reachedServer, bool granted, const char *eventId,
                      uint32_t unlockSec, uint32_t alertSec) {
    if (!verifying_) return;               // arrived after the 3 s limit: ignored (thesis)
    verifying_ = false;
    uint32_t now = hal_.millis();
    if (!reachedServer) { startBeep(kBeepError); return; }
    if (!granted) { startBeep(kBeepDenied); return; }
    if (state_ == State::DoorOpen) return; // door opened while we were waiting

    if (unlockSec) cfg_.unlockMs = clampu(unlockSec * 1000UL, kUnlockMin, kUnlockMax);
    if (alertSec) cfg_.doorOpenAlertMs = clampu(alertSec * 1000UL, kAlertMin, kAlertMax);

    if (state_ == State::Locked) {
      setCoil(true);
      state_ = State::Unlocked;
      manual_ = false;
      copyId(eventId);
    } else {                               // Unlocked: rescan restarts the timer
      if (manual_ || !eventId_[0]) { manual_ = false; copyId(eventId); }
      else if (eventId && strcmp(eventId_, eventId) != 0) return;   // a different code: ignore
    }
    unlockAt_ = now;
    startBeep(kBeepConfirm);
  }

  void onManualEventId(const char *eventId) {
    if (state_ != State::Locked && manual_ && !eventId_[0]) copyId(eventId);
  }

  // --- introspection (tests, status heartbeat) -------------------------
  State state() const { return state_; }
  bool coilOn() const { return coilOn_; }
  bool doorClosed() const { return doorClosed_; }
  bool verifying() const { return verifying_; }
  const char *eventId() const { return eventId_; }
  const Config &config() const { return cfg_; }

 private:
  static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

  void copyId(const char *id) {
    strncpy(eventId_, id ? id : "", kIdLen - 1);
    eventId_[kIdLen - 1] = 0;
  }

  // Accept exactly "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (hex + dashes), after trimming.
  static bool sanitize(const char *raw, char out[kIdLen]) {
    if (!raw) return false;
    while (*raw == ' ' || *raw == '\r' || *raw == '\n' || *raw == '\t') raw++;
    size_t n = strlen(raw);
    while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == '\r' || raw[n - 1] == '\n' || raw[n - 1] == '\t')) n--;
    if (n != 36) return false;
    for (size_t i = 0; i < 36; i++) {
      char c = raw[i];
      bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (dash ? c != '-' : !hex) return false;
      out[i] = c;
    }
    out[36] = 0;
    return true;
  }

  void setCoil(bool on) {
    if (on && !coilOn_) coilOnAt_ = hal_.millis();
    coilOn_ = on;
    hal_.coil(on);
  }

  // The report is bound to an event; without an Event-ID (manual unlock whose
  // id has not arrived yet) there is nothing the server could match it to.
  void reportEvent(ReportType t, uint32_t validMs) {
    if (!eventId_[0]) return;
    net_.queueReport(t, eventId_, validMs);
  }

  void relock(uint32_t now) {
    setCoil(false);
    state_ = State::Locked;
    lockedAt_ = now;
    eventId_[0] = 0;
    manual_ = false;
  }

  // --- door ---------------------------------------------------------------
  void pollDoor(uint32_t now) {
    bool raw = hal_.doorClosedRaw();
    if (raw != rawDoorClosed_) { rawDoorClosed_ = raw; rawDoorSince_ = now; }
    if (raw == doorClosed_ || !elapsed(now, rawDoorSince_, cfg_.doorDebounceMs)) return;
    doorClosed_ = raw;
    if (doorClosed_) onDoorClosed(now); else onDoorOpened(now);
  }

  void onDoorOpened(uint32_t now) {
    hal_.log("door: opened");
    doorOpenSince_ = now;
    doorAlertSent_ = false;
    if (state_ == State::Unlocked) {
      state_ = State::DoorOpen;
      reportEvent(ReportType::DoorOpened, cfg_.unlockReportValidMs);
    } else if (state_ == State::Locked && elapsed(now, lockedAt_, cfg_.tamperGraceMs)) {
      hal_.log("door: opened while locked -> tamper");
      net_.queueReport(ReportType::Tamper, "", 20000);
      startBeep(kBeepError);
    }
  }

  void onDoorClosed(uint32_t now) {
    hal_.log("door: closed");
    alarmActive_ = false;
    if (state_ == State::Unlocked || state_ == State::DoorOpen) {
      bool wasManual = manual_;
      reportEvent(ReportType::Locked, cfg_.reportValidMs);
      relock(now);                                  // coil off FIRST, bookkeeping after
      if (!wasManual) {                             // manual override is never photographed
        camPulseUntil_ = now + cfg_.camPulseMs;
        camPulsing_ = true;
        hal_.camPin(true);
      }
    } else {
      lockedAt_ = now;                              // door settled after a tamper/boot-open
    }
  }

  // --- push button --------------------------------------------------------
  void pollButton(uint32_t now) {
    bool raw = hal_.buttonPressedRaw();
    if (raw != rawButton_) { rawButton_ = raw; rawButtonSince_ = now; }
    if (raw == button_ || !elapsed(now, rawButtonSince_, cfg_.buttonDebounceMs)) return;
    button_ = raw;
    if (!button_ || state_ != State::Locked) return;
    hal_.log("button: manual unlock");
    setCoil(true);               // local, immediate, independent of Wi-Fi/server
    state_ = State::Unlocked;
    manual_ = true;
    eventId_[0] = 0;
    unlockAt_ = now;
    startBeep(kBeepConfirm);
    net_.queueManual();
  }

  // --- timers ---------------------------------------------------------------
  void serviceTimers(uint32_t now) {
    if (verifying_ && elapsed(now, verifyAt_, cfg_.verifyTimeoutMs)) {
      verifying_ = false;
      hal_.log("verify: timeout");
      startBeep(kBeepError);
    }
    if (state_ == State::Unlocked && elapsed(now, unlockAt_, cfg_.unlockMs)) {
      hal_.log("unlock: Tunlock expired");
      bool wasManual = manual_;
      if (!wasManual) reportEvent(ReportType::UnlockTimeout, cfg_.unlockReportValidMs);
      relock(now);
    }
    if (coilOn_ && elapsed(now, coilOnAt_, cfg_.coilMaxMs)) {
      hal_.log("coil: thermal cap reached, de-energised");
      setCoil(false);
    }
    if (camPulsing_ && (int32_t)(now - camPulseUntil_) >= 0) {
      camPulsing_ = false;
      hal_.camPin(false);
    }
    if (!doorClosed_ && !doorAlertSent_ && elapsed(now, doorOpenSince_, cfg_.doorOpenAlertMs)) {
      doorAlertSent_ = true;
      hal_.log("door: left open (Tajar)");
      net_.queueReport(ReportType::DoorLeftOpen, eventId_, 20000);
      alarmActive_ = true;
      alarmStart_ = now;
      alarmNext_ = now;
    }
    if (alarmActive_) {
      if (doorClosed_ || elapsed(now, alarmStart_, cfg_.alarmMaxMs)) alarmActive_ = false;
      else if ((int32_t)(now - alarmNext_) >= 0) { startBeep(kBeepAlarm); alarmNext_ = now + cfg_.alarmPeriodMs; }
    }
  }

  // --- buzzer sequencer (non-blocking) -----------------------------------
  void startBeep(const uint16_t *seq) {
    seq_ = seq;
    seqIdx_ = 1;
    seqOn_ = true;
    hal_.buzzer(true);
    seqEdge_ = hal_.millis() + seq[0];
  }
  void serviceBeep(uint32_t now) {
    if (!seq_ || (int32_t)(now - seqEdge_) < 0) return;
    uint16_t d = seq_[seqIdx_];
    if (d == 0) { hal_.buzzer(false); seq_ = nullptr; return; }
    seqOn_ = !seqOn_;
    hal_.buzzer(seqOn_);
    seqEdge_ = now + d;
    seqIdx_++;
  }

  Hal &hal_;
  Net &net_;
  Config cfg_;

  State state_ = State::Locked;
  bool manual_ = false;
  char eventId_[kIdLen] = {0};
  bool coilOn_ = false;
  uint32_t coilOnAt_ = 0, unlockAt_ = 0, lockedAt_ = 0;

  bool rawDoorClosed_ = true, doorClosed_ = true;
  uint32_t rawDoorSince_ = 0, doorOpenSince_ = 0;
  bool doorAlertSent_ = false;

  bool rawButton_ = false, button_ = false;
  uint32_t rawButtonSince_ = 0;

  bool verifying_ = false;
  uint32_t verifyAt_ = 0;
  char lastScan_[kIdLen] = {0};
  uint32_t lastScanAt_ = 0;

  bool camPulsing_ = false;
  uint32_t camPulseUntil_ = 0;

  bool alarmActive_ = false;
  uint32_t alarmStart_ = 0, alarmNext_ = 0;

  const uint16_t *seq_ = nullptr;
  uint8_t seqIdx_ = 0;
  bool seqOn_ = false;
  uint32_t seqEdge_ = 0;
};

}  // namespace pb
