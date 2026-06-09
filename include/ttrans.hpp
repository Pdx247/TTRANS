#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ttrans {

using LogFn = std::function<void(const std::string&)>;

struct TransferOptions {
    std::size_t chunk_size = 1024;
    int timeout_ms = 700;
    int retries = 16;
};

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

enum class PacketType : uint8_t {
    Meta = 1,
    MetaAck = 2,
    Data = 3,
    DataAck = 4,
    Done = 5,
    DoneAck = 6,
};

struct Packet {
    PacketType type = PacketType::Meta;
    uint32_t session = 0;
    uint32_t seq = 0;
    uint32_t total = 0;
    uint64_t file_size = 0;
    std::vector<uint8_t> payload;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool valid() const;
    bool bind_port(uint16_t port);
    bool set_timeout(int timeout_ms);
    bool send_bytes(const std::string& host, uint16_t port, const uint8_t* data, std::size_t size);
    bool recv_bytes(std::vector<uint8_t>& data, Endpoint& from, std::size_t max_size);

private:
    uintptr_t handle_ = 0;
};

bool ensure_socket_runtime();
std::vector<uint8_t> encode_packet(const Packet& packet);
bool decode_packet(const uint8_t* data, std::size_t size, Packet& packet);

uint64_t fnv1a_file(const std::string& path);
std::string hex64(uint64_t value);

bool send_file(const std::string& host,
               uint16_t port,
               const std::string& path,
               const TransferOptions& options,
               const LogFn& log);

bool receive_once(uint16_t port,
                  const std::string& output_dir,
                  const TransferOptions& options,
                  const LogFn& log);

int run_web_gui(uint16_t http_port, const TransferOptions& options);

} // namespace ttrans
