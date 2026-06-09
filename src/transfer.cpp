#include "ttrans.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

namespace ttrans {
namespace {

constexpr std::size_t kMaxDatagram = 1400;

bool regular_file(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG) != 0;
}

uint64_t file_size_of(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    return static_cast<uint64_t>(in.tellg());
}

std::string filename_only(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        return "file.bin";
    }
    for (auto& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return name;
}

bool path_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

void make_dir_one(const std::string& path) {
    if (path.empty() || path_exists(path)) {
        return;
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

void make_dirs(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
    std::string current;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        current.push_back(dir[i]);
        if (dir[i] == '/' || dir[i] == '\\') {
            if (current.size() > 1 && current[current.size() - 2] != ':') {
                make_dir_one(current);
            }
        }
    }
    make_dir_one(dir);
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    const auto last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + name;
    }
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

std::string unique_output_path(const std::string& dir, const std::string& name) {
    make_dirs(dir);
    auto candidate = join_path(dir, name);
    if (!path_exists(candidate)) {
        return candidate;
    }
    const auto dot = name.find_last_of('.');
    const auto stem = dot == std::string::npos ? name : name.substr(0, dot);
    const auto ext = dot == std::string::npos ? std::string{} : name.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        auto next = join_path(dir, stem + "_" + std::to_string(i) + ext);
        if (!path_exists(next)) {
            return next;
        }
    }
    return join_path(dir, stem + "_copy" + ext);
}

uint32_t random_session() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xffffffffu);
    return dist(gen);
}

bool wait_for(UdpSocket& sock, PacketType type, uint32_t session, uint32_t seq, Packet& packet) {
    Endpoint from;
    std::vector<uint8_t> raw;
    while (sock.recv_bytes(raw, from, kMaxDatagram)) {
        Packet decoded;
        if (decode_packet(raw.data(), raw.size(), decoded) &&
            decoded.type == type &&
            decoded.session == session &&
            decoded.seq == seq) {
            packet = std::move(decoded);
            return true;
        }
    }
    return false;
}

bool send_with_ack(UdpSocket& sock,
                   const std::string& host,
                   uint16_t port,
                   const Packet& packet,
                   PacketType ack_type,
                   const TransferOptions& options) {
    const auto encoded = encode_packet(packet);
    for (int attempt = 0; attempt < options.retries; ++attempt) {
        if (!sock.send_bytes(host, port, encoded.data(), encoded.size())) {
            return false;
        }
        Packet ack;
        if (wait_for(sock, ack_type, packet.session, packet.seq, ack)) {
            return true;
        }
    }
    return false;
}

void log_line(const LogFn& log, const std::string& line) {
    if (log) {
        log(line);
    }
}

} // namespace

uint64_t fnv1a_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    uint64_t hash = 14695981039346656037ull;
    std::array<char, 8192> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool send_file(const std::string& host,
               uint16_t port,
               const std::string& path,
               const TransferOptions& options,
               const LogFn& log) {
    if (!regular_file(path)) {
        log_line(log, "File not found: " + path);
        return false;
    }
    const auto file_size = file_size_of(path);
    const auto total = static_cast<uint32_t>((file_size + options.chunk_size - 1) / options.chunk_size);
    const auto checksum = fnv1a_file(path);
    const auto session = random_session();
    const auto name = filename_only(path);

    UdpSocket sock;
    if (!sock.valid() || !sock.set_timeout(options.timeout_ms)) {
        log_line(log, "Unable to create UDP socket.");
        return false;
    }

    Packet meta;
    meta.type = PacketType::Meta;
    meta.session = session;
    meta.total = total;
    meta.file_size = file_size;
    std::string payload = name + "\n" + hex64(checksum);
    meta.payload.assign(payload.begin(), payload.end());

    log_line(log, "Sending " + name + " (" + std::to_string(file_size) + " bytes) to " + host + ":" + std::to_string(port));
    if (!send_with_ack(sock, host, port, meta, PacketType::MetaAck, options)) {
        log_line(log, "Receiver did not acknowledge metadata.");
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<char> buffer(options.chunk_size);
    for (uint32_t seq = 1; seq <= total; ++seq) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        Packet chunk;
        chunk.type = PacketType::Data;
        chunk.session = session;
        chunk.seq = seq;
        chunk.total = total;
        chunk.file_size = file_size;
        chunk.payload.assign(buffer.begin(), buffer.begin() + count);
        if (!send_with_ack(sock, host, port, chunk, PacketType::DataAck, options)) {
            log_line(log, "Chunk " + std::to_string(seq) + " timed out.");
            return false;
        }
        if (seq % 64 == 0 || seq == total) {
            log_line(log, "Progress " + std::to_string(seq) + "/" + std::to_string(total));
        }
    }

    Packet done;
    done.type = PacketType::Done;
    done.session = session;
    done.seq = total + 1;
    done.total = total;
    done.file_size = file_size;
    const auto digest = hex64(checksum);
    done.payload.assign(digest.begin(), digest.end());
    if (!send_with_ack(sock, host, port, done, PacketType::DoneAck, options)) {
        log_line(log, "Final confirmation timed out.");
        return false;
    }
    log_line(log, "Send complete. checksum=" + digest);
    return true;
}

bool receive_once(uint16_t port,
                  const std::string& output_dir,
                  const TransferOptions& options,
                  const LogFn& log) {
    UdpSocket sock;
    if (!sock.valid() || !sock.bind_port(port) || !sock.set_timeout(options.timeout_ms)) {
        log_line(log, "Unable to bind UDP port " + std::to_string(port));
        return false;
    }

    log_line(log, "Waiting on UDP port " + std::to_string(port));
    Packet meta;
    Endpoint peer;
    std::vector<uint8_t> raw;
    while (true) {
        if (!sock.recv_bytes(raw, peer, kMaxDatagram)) {
            continue;
        }
        if (decode_packet(raw.data(), raw.size(), meta) && meta.type == PacketType::Meta) {
            break;
        }
    }

    const std::string meta_payload(meta.payload.begin(), meta.payload.end());
    const auto split = meta_payload.find('\n');
    const auto name = filename_only(split == std::string::npos ? meta_payload : meta_payload.substr(0, split));
    const auto expected_digest = split == std::string::npos ? std::string{} : meta_payload.substr(split + 1);
    const auto out_path = unique_output_path(output_dir, name);

    Packet ack;
    ack.type = PacketType::MetaAck;
    ack.session = meta.session;
    ack.seq = meta.seq;
    ack.total = meta.total;
    auto ack_bytes = encode_packet(ack);
    sock.send_bytes(peer.host, peer.port, ack_bytes.data(), ack_bytes.size());

    log_line(log, "Receiving " + name + " from " + peer.host + ":" + std::to_string(peer.port));
    std::ofstream out(out_path, std::ios::binary);
    uint64_t hash = 14695981039346656037ull;
    uint32_t expected_seq = 1;
    while (expected_seq <= meta.total) {
        if (!sock.recv_bytes(raw, peer, kMaxDatagram)) {
            continue;
        }
        Packet chunk;
        if (!decode_packet(raw.data(), raw.size(), chunk) || chunk.session != meta.session || chunk.type != PacketType::Data) {
            continue;
        }
        Packet data_ack;
        data_ack.type = PacketType::DataAck;
        data_ack.session = meta.session;
        data_ack.seq = chunk.seq;
        data_ack.total = meta.total;
        auto data_ack_bytes = encode_packet(data_ack);
        sock.send_bytes(peer.host, peer.port, data_ack_bytes.data(), data_ack_bytes.size());
        if (chunk.seq != expected_seq) {
            continue;
        }
        out.write(reinterpret_cast<const char*>(chunk.payload.data()), static_cast<std::streamsize>(chunk.payload.size()));
        for (auto b : chunk.payload) {
            hash ^= b;
            hash *= 1099511628211ull;
        }
        if (expected_seq % 64 == 0 || expected_seq == meta.total) {
            log_line(log, "Progress " + std::to_string(expected_seq) + "/" + std::to_string(meta.total));
        }
        ++expected_seq;
    }
    out.close();

    while (true) {
        if (!sock.recv_bytes(raw, peer, kMaxDatagram)) {
            continue;
        }
        Packet done;
        if (!decode_packet(raw.data(), raw.size(), done) || done.session != meta.session || done.type != PacketType::Done) {
            continue;
        }
        Packet done_ack;
        done_ack.type = PacketType::DoneAck;
        done_ack.session = meta.session;
        done_ack.seq = done.seq;
        done_ack.total = meta.total;
        auto done_ack_bytes = encode_packet(done_ack);
        sock.send_bytes(peer.host, peer.port, done_ack_bytes.data(), done_ack_bytes.size());
        break;
    }

    const auto actual_digest = hex64(hash);
    if (!expected_digest.empty() && actual_digest != expected_digest) {
        log_line(log, "Checksum mismatch. expected=" + expected_digest + " actual=" + actual_digest);
        return false;
    }
    log_line(log, "Saved to " + out_path + " checksum=" + actual_digest);
    return true;
}

} // namespace ttrans
