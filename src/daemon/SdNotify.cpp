#include "SdNotify.h"
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void SdNotify::Send(const std::string& state) {
    const char* path = getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;  // not running under systemd — nothing to do

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string sockPath = path;
    // "@" prefix denotes an abstract-namespace socket — replace with a NUL,
    // systemd's own convention.
    if (sockPath.front() == '@') sockPath[0] = '\0';
    if (sockPath.size() >= sizeof(addr.sun_path)) return;
    std::memcpy(addr.sun_path, sockPath.data(), sockPath.size());
    const socklen_t addrLen = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + sockPath.size());

    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sendto(fd, state.data(), state.size(), 0, reinterpret_cast<sockaddr*>(&addr), addrLen);
    close(fd);
}
