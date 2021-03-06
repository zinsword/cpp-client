#include "infinispan/hotrod/exceptions.h"
#include "hotrod/sys/Socket.h"

#include <winsock2.h>
#include <Ws2tcpip.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>

namespace infinispan {
namespace hotrod {
namespace sys {

// Part of a straw man IO layer



namespace windows {

class Socket: public infinispan::hotrod::sys::Socket {
  public:
    Socket();
    virtual void connect(const std::string& host, int port, int timeout);
    virtual void close();
    virtual void setTcpNoDelay(bool tcpNoDelay);
    virtual void setTimeout(int timeout);
    virtual size_t read(char *p, size_t n);
    virtual void write(const char *p, size_t n);
  private:
    SOCKET fd;
    std::string host;
    int port;
    static bool started;
};

namespace {
// TODO: centralized hotrod exceptions with file name and line number
void throwIOErr (const std::string& host, int port, const char *msg, int errnum) {
    std::string m(msg);
    if (errno != 0) {
        char buf[200];
        if (strerror_s(buf, 200, errnum) == 0) {
            m += " ";
            m += buf;
        }
        else {
            m += " ";
            m += strerror(errnum);
        }
    }
    throw TransportException(host, port, m);
}

} /* namespace */

bool Socket::started = false;

Socket::Socket() : fd(INVALID_SOCKET) {
    if (!started) {
        /* Request WinSock 2.2 */
        WORD wsa_ver = MAKEWORD(2, 2);
        WSADATA unused;
        int err = WSAStartup(wsa_ver, &unused);
        if (err)
            throwIOErr("", 0, "windows WSAStartup failed", err);
        started = true;
    }
}

void Socket::connect(const std::string& h, int p, int timeout) {
    host = h;
    port = p;
    if (fd != INVALID_SOCKET) throwIOErr(host, port, "reconnect attempt", 0);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = 0;
    struct addrinfo *addr;
    std::ostringstream ostr;
    ostr << port;
    int ec = getaddrinfo(host.c_str(), ostr.str().c_str(), NULL, &addr);
    if (ec) throwIOErr(host, port,"Error while invoking getaddrinfo", WSAGetLastError());

    SOCKET sock = socket(addr->ai_family, SOCK_STREAM, getprotobyname("tcp")->p_proto);
    if (sock == INVALID_SOCKET) throwIOErr(host, port,"connect", WSAGetLastError());

    // Make the socket non-blocking for the connection
    u_long non_blocking = 1;
    ioctlsocket(sock, FIONBIO, &non_blocking);

    // Connect
    int s = ::connect(sock, addr->ai_addr, addr->ai_addrlen);
    int error = WSAGetLastError();
    freeaddrinfo(addr);

    if (s < 0) {
        if (error == WSAEWOULDBLOCK) {
            struct timeval tv;
            tv.tv_sec = timeout / 1000;
            tv.tv_usec = timeout % 1000;
            fd_set sock_set;
            FD_ZERO(&sock_set);
            FD_SET(sock, &sock_set);
            // Wait for the socket to become ready
            s = select(sock + 1, NULL, &sock_set, NULL, &tv);
            if (s > 0) {
                int opt;
                socklen_t optlen = sizeof(opt);
                s = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)(&opt), &optlen);
            } else {
                error = WSAGetLastError();
            }
        } else {
            s = -1;
        }
    }
    if (s < 0) {
        close();
        throwIOErr(host, port, "Error during connection", error);
    }

    // Set to blocking mode again
    non_blocking = 0;
    ioctlsocket(sock, FIONBIO, &non_blocking);
    fd = sock;
}

void Socket::close() {
    ::closesocket(fd);
    fd = INVALID_SOCKET;
}

void Socket::setTcpNoDelay(bool tcpNoDelay) {
    int flag = tcpNoDelay;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &flag, sizeof(flag)) < 0) {
        throwIOErr(host, port, "Failure setting TCP_NODELAY", WSAGetLastError());
    }
}

void Socket::setTimeout(int timeout) {
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = timeout % 1000;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof(tv)) < 0) {
        throwIOErr(host, port, "Failure setting receive socket timeout", WSAGetLastError());
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof(tv)) < 0) {
        throwIOErr(host, port, "Failure setting send socket timeout", WSAGetLastError());
    }
}

size_t Socket::read(char *p, size_t length) {
    while(1) {
        ssize_t n =  recv(fd, p, (int) length, 0);
        if (n == SOCKET_ERROR)
            throwIOErr(host, port, "read", WSAGetLastError());
        else if (n == 0)
            return 0;
        else
            return n;
    }
}

void Socket::write(const char *p, size_t length) {
    ssize_t n = send(fd, p, (int) length, 0);
    if (n == SOCKET_ERROR) throwIOErr (host, port, "write", WSAGetLastError());
    if ((size_t) n != length) throwIOErr (host, port, "write error incomplete", 0);
}

} /* windows namespace */


Socket* Socket::create() {
    return new windows::Socket();
}

}}} /* namespace */
