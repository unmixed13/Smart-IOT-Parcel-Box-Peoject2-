// Host-side unit tests for firmware/parcel_box_main/box_logic.h
// Build and run (from this folder):
//   g++ -std=c++11 -Wall -Wextra -fsanitize=address,undefined -I../parcel_box_main test_box_logic.cpp -o t && ./t
#include <stdio.h>
#include <string>
#include <vector>
#include "box_logic.h"

using namespace pb;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) { g_pass++; } else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct FakeHal : Hal {
  uint32_t t = 0;
  bool doorClosed = true, button = false, coilV = false, buzzV = false, camV = false;
  int coilOnCount = 0, camPulses = 0, buzzOn = 0;
  uint32_t camHighAt = 0, camPulseLen = 0;
  uint32_t millis() override { return t; }
  void coil(bool e) override { if (e && !coilV) coilOnCount++; coilV = e; }
  void buzzer(bool on) override { if (on && !buzzV) buzzOn++; buzzV = on; }
  void camPin(bool h) override {
    if (h && !camV) { camPulses++; camHighAt = t; }
    if (!h && camV) camPulseLen = t - camHighAt;
    camV = h;
  }
  bool doorClosedRaw() override { return doorClosed; }
  bool buttonPressedRaw() override { return button; }
  void log(const char *) override {}
};

struct Rep { ReportType type; std::string id; uint32_t valid; };
struct FakeNet : Net {
  std::vector<std::string> verifies; std::vector<Rep> reports; int manuals = 0;
  void requestVerify(const char *c) override { verifies.push_back(c); }
  void queueReport(ReportType t, const char *id, uint32_t v) override { reports.push_back({t, id, v}); }
  void queueManual() override { manuals++; }
  int count(ReportType t) { int n = 0; for (auto &r : reports) if (r.type == t) n++; return n; }
};

static const char *CODE = "123e4567-e89b-12d3-a456-426614174000";
static const char *EV1  = "aaaaaaaa-1111-2222-3333-444444444444";

struct Rig {
  FakeHal hal; FakeNet net; BoxLogic box; 
  explicit Rig(uint32_t start = 1000) : box(hal, net) { hal.t = start; box.begin(); }
  void run(uint32_t ms, uint32_t step = 5) { for (uint32_t i = 0; i < ms; i += step) { hal.t += step; box.loop(); } }
  void grant(const char *ev = EV1, uint32_t unlockSec = 10, uint32_t alertSec = 60) {
    box.onScan(CODE); box.onVerifyResult(true, true, ev, unlockSec, alertSec);
  }
  void openDoor()  { hal.doorClosed = false; run(150); }
  void closeDoor() { hal.doorClosed = true;  run(150); }
  void press()     { hal.button = true; run(150); hal.button = false; run(150); }
};

#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, name}); } } r_##name; static void name()
struct TC { const char *n; void (*f)(); };
static std::vector<TC> &tests() { static std::vector<TC> v; return v; }

// ------------------------------------------------------------------ QR flow
TEST(granted_scan_energises_coil_and_beeps) {
  Rig r; r.box.onScan(CODE);
  CHECK(r.net.verifies.size() == 1 && r.box.verifying());
  CHECK(!r.hal.coilV);                                   // not before the server says yes
  r.box.onVerifyResult(true, true, EV1, 10, 60);
  CHECK(r.hal.coilV && r.box.state() == State::Unlocked && std::string(r.box.eventId()) == EV1);
}
TEST(denied_and_network_error_never_unlock) {
  Rig r; r.box.onScan(CODE); r.box.onVerifyResult(true, false, "", 0, 0);
  CHECK(!r.hal.coilV && r.box.state() == State::Locked);
  r.run(3000);
  r.box.onScan(CODE); r.box.onVerifyResult(false, false, "", 0, 0);
  CHECK(!r.hal.coilV && r.box.state() == State::Locked && r.hal.buzzOn >= 3);
}
TEST(verify_timeout_3s_denies_and_late_grant_is_ignored) {
  Rig r; r.box.onScan(CODE);
  r.run(2900); CHECK(r.box.verifying());
  r.run(200);  CHECK(!r.box.verifying());                // 3 s elapsed
  r.box.onVerifyResult(true, true, EV1, 10, 60);          // too late
  CHECK(!r.hal.coilV && r.box.state() == State::Locked);
}
TEST(unlock_times_out_after_10s_relocks_and_reports) {
  Rig r; r.grant();
  r.run(9800);  CHECK(r.hal.coilV);
  r.run(400);   CHECK(!r.hal.coilV && r.box.state() == State::Locked);
  CHECK(r.net.count(ReportType::UnlockTimeout) == 1 && r.net.reports.back().id == EV1);
  CHECK(r.net.reports.back().valid == 15000);             // Tunlock + 5 s retry margin
  CHECK(r.hal.camPulses == 0);                            // no photo for an unused unlock
}
TEST(rescan_same_code_restarts_the_timer) {
  Rig r; r.grant();
  r.run(8000);
  r.box.onScan(CODE); r.run(500);                         // allowed again after the repeat window
  r.box.onVerifyResult(true, true, EV1, 10, 60);          // same Event-ID, answered in time
  r.run(9000);  CHECK(r.hal.coilV);                       // would have expired at 10 s without the rescan
  r.run(1100);  CHECK(!r.hal.coilV);
  CHECK(r.hal.coilOnCount == 1);                          // never released and re-energised
}
TEST(scanner_repeat_reads_are_debounced) {
  Rig r; r.box.onScan(CODE); r.box.onVerifyResult(true, false, "", 0, 0);
  r.run(500); r.box.onScan(CODE); r.box.onScan(CODE);
  CHECK(r.net.verifies.size() == 1);
  r.run(2000); r.box.onScan(CODE);
  CHECK(r.net.verifies.size() == 2);
}
TEST(garbage_scans_are_rejected_locally_without_network) {
  Rig r;
  const char *bad[] = {"", "hello", "' OR 1=1 --", "123e4567-e89b-12d3-a456-42661417400",   // 35
                       "123e4567-e89b-12d3-a456-4266141740000", "123e4567_e89b_12d3_a456_426614174000",
                       "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz", "../../../../../../../../../../../../etc"};
  for (auto b : bad) r.box.onScan(b);
  CHECK(r.net.verifies.empty() && !r.hal.coilV);
  r.box.onScan("  123E4567-E89B-12D3-A456-426614174000\r\n");               // trimmed, upper-case ok
  CHECK(r.net.verifies.size() == 1);
}
TEST(server_overrides_are_clamped) {
  Rig r; r.grant(EV1, 3600, 5);                                              // absurd values
  CHECK(r.box.config().unlockMs == kUnlockMax && r.box.config().doorOpenAlertMs == kAlertMin);
  Rig r2; r2.grant(EV1, 1, 99999);
  CHECK(r2.box.config().unlockMs == kUnlockMin && r2.box.config().doorOpenAlertMs == kAlertMax);
}

// -------------------------------------------------------------- door / lock
TEST(door_open_then_close_reports_locks_and_pulses_camera_once) {
  Rig r; r.grant();
  r.run(3000); r.openDoor();
  CHECK(r.box.state() == State::DoorOpen && r.hal.coilV);   // coil stays on while the door is open
  CHECK(r.net.count(ReportType::DoorOpened) == 1);
  r.run(5000);
  r.closeDoor();
  CHECK(!r.hal.coilV && r.box.state() == State::Locked);
  CHECK(r.net.count(ReportType::Locked) == 1 && r.net.reports.back().id == EV1);
  CHECK(r.hal.camPulses == 1 && r.hal.camV);                 // trigger line is high now ...
  r.run(300);
  CHECK(!r.hal.camV && r.hal.camPulseLen >= 195 && r.hal.camPulseLen <= 215);   // ... for ~200 ms
  CHECK(std::string(r.box.eventId()).empty());
  CHECK(r.net.count(ReportType::UnlockTimeout) == 0);        // Tunlock must not fire afterwards
  r.run(20000); CHECK(r.net.count(ReportType::UnlockTimeout) == 0 && r.net.count(ReportType::Tamper) == 0);
}
TEST(door_bounce_shorter_than_debounce_is_ignored) {
  Rig r; r.grant();
  for (int i = 0; i < 6; i++) { r.hal.doorClosed = false; r.run(30); r.hal.doorClosed = true; r.run(30); }
  CHECK(r.box.state() == State::Unlocked && r.net.count(ReportType::DoorOpened) == 0);
  r.run(200); CHECK(r.net.reports.empty());
}
TEST(opening_a_locked_door_is_tamper_but_rebound_after_lock_is_not) {
  Rig r; r.grant(); r.openDoor();
  r.hal.doorClosed = true; r.run(60);                        // closes ...
  r.run(60); 
  r.hal.doorClosed = true; r.run(100);
  CHECK(r.box.state() == State::Locked);
  r.hal.doorClosed = false; r.run(150);                      // door closer rebound 300 ms after lock
  CHECK(r.net.count(ReportType::Tamper) == 0);
  r.hal.doorClosed = true; r.run(150);
  r.run(3000);
  r.hal.doorClosed = false; r.run(150);                      // later: a real forced opening
  CHECK(r.net.count(ReportType::Tamper) == 1);
}
TEST(door_left_open_alerts_once_after_60s_and_alarm_stops_on_close) {
  Rig r; r.grant(); r.openDoor();
  r.run(58000); CHECK(r.net.count(ReportType::DoorLeftOpen) == 0);
  r.run(3000);  CHECK(r.net.count(ReportType::DoorLeftOpen) == 1 && r.net.reports.back().id == EV1);
  int beepsBefore = r.hal.buzzOn;
  r.run(10000); CHECK(r.net.count(ReportType::DoorLeftOpen) == 1);   // one alert per event
  CHECK(r.hal.buzzOn > beepsBefore);                                  // buzzer keeps sounding
  r.closeDoor(); beepsBefore = r.hal.buzzOn;
  r.run(10000); CHECK(r.hal.buzzOn == beepsBefore && !r.hal.buzzV);   // silent after close
}
TEST(coil_thermal_cap_cuts_power_even_if_door_stays_open) {
  Rig r; r.grant(); r.openDoor();
  r.run(29000); CHECK(r.hal.coilV);
  r.run(2000);  CHECK(!r.hal.coilV && r.box.state() == State::DoorOpen);
  r.closeDoor(); CHECK(r.net.count(ReportType::Locked) == 1 && r.box.state() == State::Locked);
}
TEST(scan_is_ignored_while_door_is_open) {
  Rig r; r.grant(); r.openDoor();
  r.box.onScan(CODE); CHECK(r.net.verifies.size() == 1);
}
TEST(grant_that_arrives_after_door_opened_is_ignored) {
  Rig r; r.grant(); r.run(2100);
  r.box.onScan(CODE);                                        // rescan in flight ...
  r.openDoor();                                              // ... door opens meanwhile
  r.box.onVerifyResult(true, true, EV1, 10, 60);
  CHECK(r.box.state() == State::DoorOpen && r.hal.coilOnCount == 1);
}
TEST(boot_with_door_open_is_not_tamper_but_still_alerts_after_tajar) {
  FakeHal h; h.doorClosed = false; h.t = 5; FakeNet n; BoxLogic b(h, n); b.begin();
  for (int i = 0; i < 2000; i++) { h.t += 5; b.loop(); }     // 10 s
  CHECK(n.count(ReportType::Tamper) == 0 && !h.coilV);
  for (int i = 0; i < 12000; i++) { h.t += 5; b.loop(); }    // +60 s
  CHECK(n.count(ReportType::DoorLeftOpen) == 1 && n.reports.back().id.empty());
}

// ------------------------------------------------------------ manual button
TEST(manual_button_unlocks_locally_without_any_network_reply) {
  Rig r; r.press();
  CHECK(r.hal.coilV && r.box.state() == State::Unlocked && r.net.manuals == 1);
  CHECK(r.net.verifies.empty());
  r.openDoor(); r.closeDoor();
  CHECK(!r.hal.coilV && r.box.state() == State::Locked);
  CHECK(r.hal.camPulses == 0);                               // never photographed
  CHECK(r.net.reports.empty());                              // no event id arrived -> nothing to report
}
TEST(manual_event_id_attaches_and_close_is_reported_without_photo) {
  Rig r; r.press(); r.box.onManualEventId(EV1);
  r.openDoor(); r.closeDoor();
  CHECK(r.net.count(ReportType::DoorOpened) == 1 && r.net.count(ReportType::Locked) == 1);
  CHECK(r.hal.camPulses == 0);
}
TEST(manual_unlock_expires_without_unlock_timeout_report) {
  Rig r; r.press(); r.box.onManualEventId(EV1); r.run(11000);
  CHECK(!r.hal.coilV && r.net.count(ReportType::UnlockTimeout) == 0);
}
TEST(button_held_down_or_bouncing_unlocks_once) {
  Rig r;
  for (int i = 0; i < 5; i++) { r.hal.button = true; r.run(20); r.hal.button = false; r.run(20); }
  CHECK(r.net.manuals == 0);                                  // bounce shorter than debounce
  r.hal.button = true; r.run(5000);
  CHECK(r.net.manuals == 1);
}
TEST(button_works_while_a_verification_is_pending) {
  Rig r; r.box.onScan(CODE); r.press();
  CHECK(r.hal.coilV && r.box.state() == State::Unlocked);
}

// ------------------------------------------------------------------ safety
TEST(coil_is_never_on_at_boot_or_idle) {
  Rig r; r.run(120000);
  CHECK(!r.hal.coilV && r.hal.coilOnCount == 0);
}
TEST(millis_wraparound_does_not_break_any_timer) {
  Rig r(0xFFFFFFFFu - 4000);                                  // wraps 4 s after boot
  r.grant(); r.run(9800); CHECK(r.hal.coilV);
  r.run(400); CHECK(!r.hal.coilV && r.net.count(ReportType::UnlockTimeout) == 1);
  Rig r2(0xFFFFFFFFu - 2000);
  r2.grant(); r2.run(1000); r2.openDoor(); r2.run(5000); r2.closeDoor(); r2.run(300);
  CHECK(r2.net.count(ReportType::Locked) == 1 && r2.hal.camPulses == 1 && !r2.hal.camV);
  Rig r3(0xFFFFFFFFu - 30000);
  r3.grant(); r3.openDoor(); r3.run(61000);
  CHECK(r3.net.count(ReportType::DoorLeftOpen) == 1);
}
TEST(second_qr_event_while_unlocked_does_not_replace_the_first) {
  Rig r; r.grant(EV1); r.run(2100);
  r.box.onScan("00000000-0000-0000-0000-000000000001");
  r.box.onVerifyResult(true, true, "bbbbbbbb-1111-2222-3333-444444444444", 10, 60);
  CHECK(std::string(r.box.eventId()) == EV1);
}

// --------------------------------------------------------------- report queue
TEST(queue_retries_expires_and_keeps_order) {
  ReportQueue q; ReportQueue::Item it;
  q.push(ReportType::DoorOpened, EV1, 1000, 15000);
  q.push(ReportType::Locked, EV1, 1100, 60000);
  CHECK(q.front(1000, it) && it.type == ReportType::DoorOpened);
  q.retryIn(1000, 1000);
  CHECK(!q.front(1500, it));                                  // head-of-line waits for its retry slot
  CHECK(q.front(2000, it) && it.type == ReportType::DoorOpened && it.attempts == 1);
  q.done();
  CHECK(q.front(2000, it) && it.type == ReportType::Locked);
  CHECK(!q.front(1100 + 60000, it) && q.size() == 0 && q.expired() == 1);
}
TEST(queue_drops_oldest_when_full_and_survives_wrap) {
  ReportQueue q; ReportQueue::Item it;
  for (int i = 0; i < 12; i++) q.push(i % 2 ? ReportType::Locked : ReportType::Tamper, "", 100 + i, 60000);
  CHECK(q.size() == ReportQueue::kCap && q.dropped() == 4);
  ReportQueue w; uint32_t t0 = 0xFFFFFFFFu - 500;
  w.push(ReportType::Locked, EV1, t0, 15000);
  CHECK(w.front(t0, it)); w.retryIn(t0, 1000);
  CHECK(!w.front(t0 + 900, it) && w.front(t0 + 1000, it));    // retry slot crosses the wrap
  CHECK(!w.front(t0 + 15000, it) && w.size() == 0);           // and so does expiry
}

int main() {
  for (auto &t : tests()) {
    int before = g_fail;
    t.f();
    printf("%s  %s\n", g_fail == before ? "PASS" : "FAIL", t.n);
  }
  printf("\n%d checks passed, %d failed, %d tests\n", g_pass, g_fail, (int)tests().size());
  return g_fail ? 1 : 0;
}
