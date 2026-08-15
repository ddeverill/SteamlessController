#include "LinuxHidDevice.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>

bool LinuxHidDevice::Open(const std::string& path) {
    Close();
    m_path = path;
    m_fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

bool LinuxHidDevice::Reopen(HidShare) {
    // hidraw has no exclusivity concept — every opener gets its own read ring
    // buffer regardless of share mode — so there is nothing to change here.
    // Succeeding unconditionally (rather than no-op-ing the call away
    // entirely) keeps SteamController::ClaimGameModeAccess's Windows-shaped
    // control flow working unmodified: it always gets AccessClaim::Exclusive
    // on the first Reopen() call.
    return m_fd >= 0;
}

void LinuxHidDevice::Close() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

bool LinuxHidDevice::SendOutputReport(const uint8_t* data, size_t size) {
    // HIDIOCSOUTPUT is the control-pipe SET_REPORT(Output) path — the
    // hidraw analog of Windows' HidD_SetOutputReport. Sent at exactly the
    // caller's size: unlike Windows, hidraw does not require padding to a
    // fixed maximum report length.
    struct Buf { uint8_t data[64]; } buf{};
    if (size > sizeof(buf.data)) size = sizeof(buf.data);
    memcpy(buf.data, data, size);
    if (ioctl(m_fd, HIDIOCSOUTPUT(size), &buf) < 0) {
        fprintf(stderr, "SendOutputReport(0x%02X) failed on %s: %s\n",
                data[0], m_path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

bool LinuxHidDevice::WriteOutputReport(const uint8_t* data, size_t size) {
    // Interrupt-OUT path. write()'s count goes straight to the URB, so the
    // exact declared report size is what must be sent — padding it (as the
    // Windows path does, to satisfy HidD_SetOutputReport) would send extra
    // bytes the firmware does not expect.
    const ssize_t written = write(m_fd, data, size);
    if (written < 0 || static_cast<size_t>(written) != size) {
        fprintf(stderr, "WriteOutputReport(0x%02X, %zu bytes) failed on %s: %s\n",
                data[0], size, m_path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

bool LinuxHidDevice::SendFeatureReport(const uint8_t* data, size_t size) {
    struct Buf { uint8_t data[64]; } buf{};
    if (size > sizeof(buf.data)) size = sizeof(buf.data);
    memcpy(buf.data, data, size);

    // The controller's firmware occasionally still be processing the
    // previous feature report when this one's SET_REPORT lands on the
    // control endpoint, which it answers with a STALL (surfaced here as
    // EPIPE) rather than queuing it — observed in practice as one command in
    // a back-to-back sequence (e.g. disable-lizard-mode's second or third
    // call) failing while its neighbors succeed. Harmless and transient,
    // unlike every other failure this function can hit, so it alone is
    // worth a few milliseconds of retry rather than failing the whole
    // sequence over firmware that was simply still catching up.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (ioctl(m_fd, HIDIOCSFEATURE(size), &buf) >= 0) return true;
        if (errno != EPIPE) break;
        usleep(4000 << attempt);  // 4ms, 8ms, 16ms
    }
    fprintf(stderr, "SendFeatureReport(reportId=0x%02X cmd=0x%02X) failed on %s: %s\n",
            buf.data[0], size > 1 ? buf.data[1] : 0, m_path.c_str(), strerror(errno));
    return false;
}

size_t LinuxHidDevice::ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs) {
    struct pollfd pfd{ m_fd, POLLIN, 0 };
    const int rc = poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (rc <= 0) return 0;  // timeout or error
    if (!(pfd.revents & POLLIN)) return 0;

    const ssize_t n = read(m_fd, buffer, size);
    return n > 0 ? static_cast<size_t>(n) : 0;
}
