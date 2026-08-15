// Dumps every device publishing the ViGEm bus interface.
//
// A "vigem_connect failed" report cannot say whether the machine had no bus at
// all, or one that belongs to somebody else — the client takes the first
// device that answers and never records which. That ambiguity is what made #65
// unfalsifiable from the logs, so this is the thing to ask a reporter to run.
//
// Setup runs it too, which is why the exit code is part of the contract:
//
//   0  at least one usable bus is present
//   1  none — the driver is absent, or its device is switched off
//
// Every cheaper answer to "did ViGEmBus install?" has been measured and found
// to lie. The bundle returns 0 when it detects a related product and declines
// to act; the registry records the product whether or not a driver landed; and
// the driver's own version is no guide either, since 1.22.0 ships a binary
// still stamped 1.21.442.0. Enumerating the interface is the only check that
// answers the question the app actually asks at runtime.

#include "platform/win/ViGEmBusInfo.h"

#include <cstdio>

int wmain() {
    const auto buses = ViGEmBusInfo::Enumerate();

    wprintf(L"%ls\n\n", ViGEmBusInfo::Describe(buses).c_str());

    for (size_t i = 0; i < buses.size(); ++i) {
        wprintf(L"[%zu] %ls\n", i + 1,
                buses[i].friendlyName.empty() ? L"(no friendly name)"
                                              : buses[i].friendlyName.c_str());
        wprintf(L"    %ls\n", buses[i].devicePath.c_str());
    }

    if (buses.size() > 1)
        wprintf(L"\nMore than one bus is present. The client connects to whichever\n"
                L"answers first, so which one it gets is not up to this app.\n");

    return buses.empty() ? 1 : 0;
}
