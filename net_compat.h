#pragma once
//
// net_compat.h — the ONLY file in this project that knows which operating
// system we are compiling for.
//
// The whole point of this header is containment. Everything below is behind a
// single #if defined(_WIN32) / #else pair, and it exports a small vocabulary
// (socket_t, INVALID_SOCK, closesock, net_init, ignore_sigpipe, sock_recv,
// sock_send) that the rest of the codebase uses unconditionally. There is not
// one #ifdef anywhere else in this project. If you ever want to drop Windows
// support entirely, delete the _WIN32 branch below and nothing else changes.
//
// Primary target is POSIX/Linux (GitHub Codespaces, WSL2). The Windows branch
// exists so the project still builds with MinGW-w64 g++ if you want it to.
//
#if defined(_WIN32)

  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>

  using socket_t      = SOCKET;
  using sockopt_arg_t = const char*;          // Winsock setsockopt wants char*
  static const socket_t INVALID_SOCK = INVALID_SOCKET;

  // NOTE: on Windows, INVALID_SOCKET is not -1 and SOCKET is unsigned, which is
  // exactly why no code in this project ever tests a socket with "< 0".

  inline int  closesock(socket_t s) { return ::closesocket(s); }
  inline bool net_init()            { WSADATA w; return ::WSAStartup(MAKEWORD(2, 2), &w) == 0; }
  inline void ignore_sigpipe()      { /* Windows has no SIGPIPE. Nothing to do. */ }

  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
  #ifndef SHUT_RDWR
  #define SHUT_RDWR SD_BOTH
  #endif

#else

  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <csignal>

  using socket_t      = int;
  using sockopt_arg_t = const void*;
  static const socket_t INVALID_SOCK = -1;

  inline int  closesock(socket_t s) { return ::close(s); }
  inline bool net_init()            { return true; }

  // SIGPIPE: on Linux, send()ing to a socket whose peer has already gone away
  // raises SIGPIPE, and SIGPIPE's DEFAULT DISPOSITION IS TO KILL THE PROCESS.
  // A client that hits Ctrl-C mid-command would take the whole server down.
  // We ignore it process-wide here (one call, covers every thread) and rely on
  // send() returning -1/EPIPE instead. We ALSO pass MSG_NOSIGNAL on every send
  // below, which is the per-call version of the same protection -- belt and
  // braces, because macOS historically lacked MSG_NOSIGNAL entirely.
  inline void ignore_sigpipe()      { ::signal(SIGPIPE, SIG_IGN); }

  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0    // e.g. macOS; the signal(SIG_IGN) above still covers us
  #endif

#endif

#include <cstddef>

// recv/send take size_t on POSIX and int on Winsock, and return ssize_t vs int.
// These two inlines absorb that difference so no call site needs a cast.
// Return value is widened to long long; callers treat n <= 0 as "stop".
inline long long sock_recv(socket_t s, char* buf, size_t len) {
    return static_cast<long long>(::recv(s, buf, static_cast<int>(len), 0));
}
inline long long sock_send(socket_t s, const char* buf, size_t len) {
    return static_cast<long long>(::send(s, buf, static_cast<int>(len), MSG_NOSIGNAL));
}
