#include "UnixSocket.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string RuntimeDir() {
    if (const char* xdg = getenv("XDG_RUNTIME_DIR"); xdg && *xdg) return xdg;
    return "/tmp/steamlesscontroller-" + std::to_string(getuid());
}

bool FillAddr(sockaddr_un& addr, const std::string& path) {
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    return true;
}

}  // namespace

std::string UnixSocket::DefaultSocketPath() {
    return RuntimeDir() + "/steamlesscontroller/control.sock";
}

int UnixSocket::Listen(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    chmod(fs::path(path).parent_path().c_str(), 0700);

    unlink(path.c_str());  // remove a stale socket from a previous, unclean exit

    sockaddr_un addr{};
    if (!FillAddr(addr, path)) return -1;

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0600);

    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int UnixSocket::Connect(const std::string& path) {
    sockaddr_un addr{};
    if (!FillAddr(addr, path)) return -1;

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
