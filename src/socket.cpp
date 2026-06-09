#include "ttrans.hpp"

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace ttrans {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

NativeSocket native(uintptr_t value) {
    return static_cast<NativeSocket>(value);
}

void close_native(NativeSocket sock) {
    if (sock == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

} // namespace

bool ensure_socket_runtime() {
#ifdef _WIN32
    static bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
}

UdpSocket::UdpSocket() {
    if (!ensure_socket_runtime()) {
        return;
    }
    const auto sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        return;
    }
    handle_ = static_cast<uintptr_t>(sock);
}

UdpSocket::~UdpSocket() {
    close_native(native(handle_));
}

bool UdpSocket::valid() const {
    return native(handle_) != kInvalidSocket && handle_ != 0;
}

bool UdpSocket::bind_port(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return ::bind(native(handle_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::set_timeout(int timeout_ms) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    return setsockopt(native(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(native(handle_), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

bool UdpSocket::send_bytes(const std::string& host, uint16_t port, const uint8_t* data, std::size_t size) {
#ifdef _WIN32
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        hostent* entry = gethostbyname(host.c_str());
        if (!entry || !entry->h_addr_list || !entry->h_addr_list[0]) {
            return false;
        }
        std::memcpy(&addr.sin_addr, entry->h_addr_list[0], sizeof(addr.sin_addr));
    }
    const auto sent = ::sendto(native(handle_),
                               reinterpret_cast<const char*>(data),
                               static_cast<int>(size),
                               0,
                               reinterpret_cast<sockaddr*>(&addr),
                               sizeof(addr));
    return sent == static_cast<int>(size);
#else
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || !result) {
        return false;
    }
    bool ok = false;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        const auto sent = ::sendto(native(handle_),
                                   reinterpret_cast<const char*>(data),
                                   static_cast<int>(size),
                                   0,
                                   it->ai_addr,
                                   static_cast<int>(it->ai_addrlen));
        if (sent == static_cast<int>(size)) {
            ok = true;
            break;
        }
    }
    freeaddrinfo(result);
    return ok;
#endif
}

bool UdpSocket::recv_bytes(std::vector<uint8_t>& data, Endpoint& from, std::size_t max_size) {
    data.assign(max_size, 0);
    sockaddr_in src{};
#ifdef _WIN32
    int src_len = sizeof(src);
#else
    socklen_t src_len = sizeof(src);
#endif
    const auto count = ::recvfrom(native(handle_),
                                  reinterpret_cast<char*>(data.data()),
                                  static_cast<int>(data.size()),
                                  0,
                                  reinterpret_cast<sockaddr*>(&src),
                                  &src_len);
    if (count <= 0) {
        data.clear();
        return false;
    }
    data.resize(static_cast<std::size_t>(count));
#ifdef _WIN32
    from.host = inet_ntoa(src.sin_addr);
#else
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
    from.host = ip;
#endif
    from.port = ntohs(src.sin_port);
    return true;
}

} // namespace ttrans
