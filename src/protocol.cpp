#include "ttrans.hpp"

#include <array>
#include <sstream>

namespace ttrans {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'T', 'T', 'R', 'N'};
constexpr uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 32;

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}

uint32_t get32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t get64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

} // namespace

std::vector<uint8_t> encode_packet(const Packet& packet) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + packet.payload.size());
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(packet.type));
    out.push_back(0);
    out.push_back(0);
    put32(out, packet.session);
    put32(out, packet.seq);
    put32(out, packet.total);
    put32(out, static_cast<uint32_t>(packet.payload.size()));
    put64(out, packet.file_size);
    out.insert(out.end(), packet.payload.begin(), packet.payload.end());
    return out;
}

bool decode_packet(const uint8_t* data, std::size_t size, Packet& packet) {
    if (size < kHeaderSize) {
        return false;
    }
    if (data[0] != kMagic[0] || data[1] != kMagic[1] || data[2] != kMagic[2] || data[3] != kMagic[3]) {
        return false;
    }
    if (data[4] != kVersion) {
        return false;
    }
    const auto payload_size = get32(data + 20);
    if (kHeaderSize + payload_size != size) {
        return false;
    }
    packet.type = static_cast<PacketType>(data[5]);
    packet.session = get32(data + 8);
    packet.seq = get32(data + 12);
    packet.total = get32(data + 16);
    packet.file_size = get64(data + 24);
    packet.payload.assign(data + kHeaderSize, data + size);
    return true;
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(16);
    out.fill('0');
    out << value;
    return out.str();
}

} // namespace ttrans
