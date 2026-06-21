#pragma once
// ── Cross-platform socket compatibility shim ─────────────────────────────────
// The worker link uses an AF_UNIX stream socket with a small POSIX I/O surface
// (poll, writev, read/write, close, unlink). Windows 10 (1803+) supports AF_UNIX
// natively via <afunix.h>; this header maps the rest of the POSIX surface onto
// the Winsock2 equivalents so WorkerLink.cpp / wl_dump.cpp stay single-source.
//
// Conventions used by callers:
//   zm_poll / zm_writev / zm_readfd / zm_writefd / zm_closefd / zm_unlink
//   zm_last_error()  -> errno (POSIX) / WSAGetLastError() (Windows)
//   ZM_EWOULDBLOCK / ZM_EAGAIN / ZM_EINTR  -> matching error constants
//   zm_net_init()    -> WSAStartup once (no-op on POSIX)
//   struct iovec, pollfd, POLLIN/POLLOUT/POLLHUP/POLLERR  -> provided on both

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <io.h>
#include <basetsd.h>
#include <cstdint>
#include <cstddef>
#include <string>

// POSIX ssize_t — MSVC ships SSIZE_T in <basetsd.h>.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

// scatter/gather buffer (POSIX iovec ↔ Winsock WSABUF have swapped field order).
struct iovec {
    void*  iov_base;
    size_t iov_len;
};

// Winsock2 already defines pollfd / WSAPOLLFD and POLLIN/POLLOUT/POLLERR/POLLHUP.

inline int zm_net_init() {
    static int rc = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa);
    }();
    return rc;
}

inline int zm_last_error() { return WSAGetLastError(); }

inline int  zm_poll(pollfd* fds, unsigned long n, int timeout_ms) {
    return WSAPoll(fds, n, timeout_ms);
}
inline ssize_t zm_readfd(int fd, void* buf, size_t n) {
    return ::recv((SOCKET)fd, static_cast<char*>(buf), static_cast<int>(n), 0);
}
inline ssize_t zm_writefd(int fd, const void* buf, size_t n) {
    return ::send((SOCKET)fd, static_cast<const char*>(buf), static_cast<int>(n), 0);
}
inline ssize_t zm_writev(int fd, const iovec* iov, int iovcnt) {
    // Winsock WSASend takes WSABUF{ULONG len; CHAR* buf} (fields swapped vs iovec).
    WSABUF bufs[8];
    if (iovcnt > 8) iovcnt = 8;
    for (int i = 0; i < iovcnt; ++i) {
        bufs[i].len = static_cast<ULONG>(iov[i].iov_len);
        bufs[i].buf = static_cast<CHAR*>(iov[i].iov_base);
    }
    DWORD sent = 0;
    int rc = WSASend((SOCKET)fd, bufs, static_cast<DWORD>(iovcnt), &sent, 0, nullptr, nullptr);
    if (rc != 0) return -1;
    return static_cast<ssize_t>(sent);
}
inline int zm_closefd(int fd) { return ::closesocket((SOCKET)fd); }
inline int zm_unlink(const char* path) { return ::_unlink(path); }
inline std::string zm_strerror(int err) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, (DWORD)err, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = msg ? msg : "(unknown)";
    if (msg) LocalFree(msg);
    return s;
}

#define ZM_EWOULDBLOCK WSAEWOULDBLOCK
#define ZM_EAGAIN      WSAEWOULDBLOCK
#define ZM_EINTR       WSAEINTR
#define ZM_INVALID_FD  ((int)INVALID_SOCKET)

#else // ───────────────────────────── POSIX ─────────────────────────────────

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <string>

inline int  zm_net_init() { return 0; }
inline int  zm_last_error() { return errno; }
inline int  zm_poll(pollfd* fds, unsigned long n, int timeout_ms) {
    return ::poll(fds, n, timeout_ms);
}
inline ssize_t zm_readfd(int fd, void* buf, size_t n)  { return ::read(fd, buf, n); }
inline ssize_t zm_writefd(int fd, const void* buf, size_t n) { return ::write(fd, buf, n); }
inline ssize_t zm_writev(int fd, const iovec* iov, int iovcnt) {
    return ::writev(fd, iov, iovcnt);
}
inline int  zm_closefd(int fd) { return ::close(fd); }
inline int  zm_unlink(const char* path) { return ::unlink(path); }
inline std::string zm_strerror(int err) { return std::strerror(err); }

#define ZM_EWOULDBLOCK EWOULDBLOCK
#define ZM_EAGAIN      EAGAIN
#define ZM_EINTR       EINTR
#define ZM_INVALID_FD  (-1)

#endif
