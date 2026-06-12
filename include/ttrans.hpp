#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ttrans {

using LogFn = std::function<void(const std::string&)>;
using StopFn = std::function<bool()>;

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
    MetaReject = 7,
    MetaWait = 8,
};

struct IncomingFile {
    std::string filename;
    std::string peer_host;
    uint16_t peer_port = 0;
    uint32_t session = 0;
    uint32_t total_chunks = 0;
    uint64_t file_size = 0;
    std::string checksum;
};

using AcceptFn = std::function<bool(const IncomingFile&)>;

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
bool path_is_regular_file(const std::string& path);
uint64_t path_file_size(const std::string& path);
std::string read_file_head(const std::string& path, std::size_t max_bytes);

bool send_file(const std::string& host,
               uint16_t port,
               const std::string& path,
               const TransferOptions& options,
               const LogFn& log);

bool receive_once(uint16_t port,
                  const std::string& output_dir,
                  const TransferOptions& options,
                  const LogFn& log);

bool receive_forever(uint16_t port,
                     const std::string& output_dir,
                     const TransferOptions& options,
                     const AcceptFn& accept,
                     const StopFn& should_stop,
                     const LogFn& log);

int run_imgui_gui(uint16_t udp_port, const std::string& output_dir, const TransferOptions& options);

} // namespace ttrans
