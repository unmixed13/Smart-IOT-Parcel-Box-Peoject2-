#!/usr/bin/env python3
"""
Builds thesis table 4.4 (code type, result and response time per trial) from two real records.

  1. serial.txt - the ESP32 Serial Monitor output (115200) saved to a file. One line per scan:
         [123456] verify: granted, coil on in 842 ms
         [124981] verify: denied in 910 ms
         [126400] verify: no reply in 3000 ms
     "in N ms" = scanner data received -> server answer -> coil / buzzer (limit 3000 ms).
  2. scans.csv  - the phpMyAdmin export (CSV) of
         SELECT created_at, event_type, result, notes FROM access_logs
         WHERE event_type = 'qr_scan' AND created_at BETWEEN '<start>' AND '<end>'
         ORDER BY created_at;
     It tells which code type each scan was (allowed / already_used / invalid_code / expired).

Scans are paired by order (trial 1 = first line in both files), so both files must cover exactly
the same trials. The script stops with an error when the counts differ instead of guessing.

Usage:  python3 response_time_table.py serial.txt scans.csv [--limit-ms 3000] [--csv out.csv]
"""
import argparse
import csv
import re
import sys

VERIFY = re.compile(r"verify:\s*(granted, coil on|denied|no reply)\s+in\s+(\d+)\s*ms")
OK = "√"       # √
BAD = "×"      # ×


def read_serial(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = VERIFY.search(line)
            if m:
                rows.append((m.group(1), int(m.group(2))))
    return rows


def read_scans(path):
    rows = []
    with open(path, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if (r.get("event_type") or "").strip() != "qr_scan":
                continue
            notes = (r.get("notes") or "").strip()
            result = (r.get("result") or "").strip().lower()
            if result == "granted":
                kind = "allowed"
            elif notes in ("already_used", "invalid_code", "expired"):
                kind = notes
            else:
                kind = notes or "denied"
            rows.append((r.get("created_at", ""), kind))
    return rows


LABEL = {
    "allowed": "ได้รับอนุญาต",
    "already_used": "ไม่อนุญาต (already_used)",
    "invalid_code": "ไม่อนุญาต (invalid_code)",
    "expired": "ไม่อนุญาต (expired)",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("serial")
    ap.add_argument("scans")
    ap.add_argument("--limit-ms", type=int, default=3000)
    ap.add_argument("--csv", help="also write the table as CSV")
    a = ap.parse_args()

    serial, scans = read_serial(a.serial), read_scans(a.scans)
    if len(serial) != len(scans):
        sys.exit(f"ERROR: {len(serial)} verify lines in {a.serial} but {len(scans)} qr_scan rows in {a.scans}. "
                 "Both files must contain the same trials (check the date range and the Serial log).")

    table, times = [], []
    for i, ((outcome, ms), (_, kind)) in enumerate(zip(serial, scans), 1):
        expected_grant = kind == "allowed"
        got_grant = outcome.startswith("granted")
        read_ok = outcome != "no reply"
        correct = read_ok and got_grant == expected_grant
        times.append(ms)
        table.append((i, LABEL.get(kind, kind), "ปลดล็อก" if expected_grant else "ปฏิเสธ",
                      OK if read_ok else BAD, OK if correct else BAD, ms / 1000.0, ms <= a.limit_ms))

    print("| ครั้งที่ทดสอบ | ประเภทรหัส | ผลที่ออกแบบไว้ | การอ่านรหัสสำเร็จ | การตอบสนองของกลอนถูกต้อง | เวลาตอบสนอง (วินาที) |")
    print("|---|---|---|---|---|---|")
    for r in table:
        print(f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4]} | {r[5]:.2f} |")
    n = len(times)
    within = sum(1 for r in table if r[6])
    print(f"| เวลาตอบสนองเฉลี่ย (วินาที) | | | | | {sum(times) / n / 1000.0:.2f} |")
    print(f"| เวลาตอบสนองสูงสุด (วินาที) | | | | | {max(times) / 1000.0:.2f} |")
    print()
    print(f"ตอบสนองถูกต้อง {sum(1 for r in table if r[4] == OK)}/{n} ครั้ง; "
          f"ไม่เกิน {a.limit_ms / 1000:g} วินาที {within}/{n} ครั้ง ({100.0 * within / n:.1f}%)")
    over = [r[0] for r in table if not r[6]]
    if over:
        print("ครั้งที่เกินเกณฑ์:", ", ".join(map(str, over)))

    if a.csv:
        with open(a.csv, "w", encoding="utf-8-sig", newline="") as f:
            w = csv.writer(f)
            w.writerow(["trial", "code_type", "expected", "read_ok", "correct", "response_s"])
            for r in table:
                w.writerow([r[0], r[1], r[2], r[3], r[4], f"{r[5]:.2f}"])


if __name__ == "__main__":
    main()
