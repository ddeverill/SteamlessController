#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace HandleFinder {

struct Holder {
    DWORD        pid = 0;
    std::wstring image;  // base name, e.g. "HapticTester.exe"
};

// Every process holding an open handle to the device behind `devicePath`
// (a \\?\ interface path), excluding this one.
//
// This exists because PnP will not tell us. A removal veto reports
// PNP_VetoOutstandingOpen and names the blocked *device*, never the process
// holding it — and since no devnode teardown can evict such a handle (disable
// and query-remove are both refused, at the node and at its parent), naming
// the holder is the only actionable thing left to report.
//
// Elevation is needed for full coverage: handles living in processes we cannot
// open are invisible, so a short list is not proof of absence.
//
// Cost and safety: this walks every handle in the system, so it is not cheap —
// call it only once something has already gone wrong. It is also built on
// undocumented NT structures, which can drift between Windows versions; every
// step degrades to "found nothing" rather than failing loudly. NtQueryObject
// can block forever on some handle types, so candidates are pre-filtered by
// GetFileType and the whole scan is backstopped by a deadline.
// What the scan did, not just what it found. An empty holder list has several
// very different causes — nothing matched, the deadline hit, the device name
// never resolved — and they are indistinguishable from the result alone.
struct ScanReport {
    std::vector<Holder> holders;
    std::wstring        targetName;      // NT name we matched against
    unsigned            typeIndex = 0;   // File object type index
    unsigned            candidates = 0;  // handles of that type in the snapshot
    unsigned            examined  = 0;   // candidates a worker got to
    // Names actually resolved. Far smaller than examined and not a coverage
    // figure: a candidate is skipped before this when its process already has a
    // known holder, when OpenProcess is refused (protected or higher
    // integrity), when DuplicateHandle fails, or when the object turns out not
    // to be device-like. Reading it as "percent scanned" is wrong.
    unsigned            queried   = 0;
    // False means the deadline arrived before every worker reported back. That
    // usually means a worker is wedged inside NtQueryObject, NOT that the scan
    // missed candidates - compare examined against candidates for that.
    bool                completed = false;
    DWORD               elapsedMs = 0;
};

// The budget is small on purpose. The scan never runs to completion: a handful
// of handles block inside NtQueryObject indefinitely, so the workers that hit
// them never come back and `processed` never reaches the total. What matters is
// that the useful work lands in the first second or so — waiting longer only
// buys stalled threads, and the tray app shows its verdict ~3s after the device
// re-arrives, so a slow scan misses the message it exists to fill in.
ScanReport FindDeviceHolders(const std::wstring& devicePath, DWORD budgetMs = 2000);

// "HapticTester.exe (36276); Steam.exe (1234)" — for logs and the balloon.
std::wstring DescribeHolders(const std::vector<Holder>& holders);

}
