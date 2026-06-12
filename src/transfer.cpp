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

void send_control(UdpSocket& sock, const Endpoint& peer, PacketType type, uint32_t session, uint32_t seq, uint32_t total) {
    Packet packet;
    packet.type = type;
    packet.session = session;
    packet.seq = seq;
    packet.total = total;
    const auto bytes = encode_packet(packet);
    sock.send_bytes(peer.host, peer.port, bytes.data(), bytes.size());
}

void log_line(const LogFn& log, const std::string& line);

IncomingFile parse_incoming_file(const Packet& meta, const Endpoint& peer) {
    const std::string meta_payload(meta.payload.begin(), meta.payload.end());
    const auto split = meta_payload.find('\n');
    IncomingFile info;
    info.filename = filename_only(split == std::string::npos ? meta_payload : meta_payload.substr(0, split));
    info.checksum = split == std::string::npos ? std::string{} : meta_payload.substr(split + 1);
    info.peer_host = peer.host;
    info.peer_port = peer.port;
    info.session = meta.session;
    info.total_chunks = meta.total;
    info.file_size = meta.file_size;
    return info;
}

std::string preserved_output_path(const std::string& dir, const std::string& name) {
    make_dirs(dir);
    return join_path(dir, name);
}

bool wait_for_meta_accept(UdpSocket& sock,
                          const std::string& host,
                          uint16_t port,
                          const Packet& meta,
                          const TransferOptions& options,
                          const LogFn& log) {
    const auto encoded = encode_packet(meta);
    int attempts = 0;
    int wait_rounds = 0;
    bool wait_logged = false;
    while (attempts < options.retries && wait_rounds < 300) {
        if (!sock.send_bytes(host, port, encoded.data(), encoded.size())) {
            return false;
        }

        Endpoint from;
        std::vector<uint8_t> raw;
        bool peer_waiting = false;
        while (sock.recv_bytes(raw, from, kMaxDatagram)) {
            Packet response;
            if (!decode_packet(raw.data(), raw.size(), response) ||
                response.session != meta.session ||
                response.seq != meta.seq) {
                continue;
            }
            if (response.type == PacketType::MetaAck) {
                return true;
            }
            if (response.type == PacketType::MetaReject) {
                log_line(log, "Receiver rejected the file.");
                return false;
            }
            if (response.type == PacketType::MetaWait) {
                peer_waiting = true;
                if (!wait_logged) {
                    log_line(log, "Receiver is asking for user confirmation...");
                    wait_logged = true;
                }
                break;
            }
        }

        if (peer_waiting) {
            ++wait_rounds;
        } else {
            ++attempts;
        }
    }
    return false;
}

void log_line(const LogFn& log, const std::string& line) {
    if (log) {
        log(line);
    }
}

bool receive_accepted_session(UdpSocket& sock,
                              const Packet& meta,
                              const Endpoint& peer,
                              const std::string& output_dir,
                              const IncomingFile& info,
                              const StopFn& should_stop,
                              const LogFn& log) {
    const auto out_path = preserved_output_path(output_dir, info.filename);
    send_control(sock, peer, PacketType::MetaAck, meta.session, meta.seq, meta.total);

    log_line(log, "Receiving " + info.filename + " from " + peer.host + ":" + std::to_string(peer.port));
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        log_line(log, "Unable to open output path: " + out_path);
        return false;
    }

    uint64_t hash = 14695981039346656037ull;
    uint32_t expected_seq = 1;
    Endpoint from;
    std::vector<uint8_t> raw;
    while (expected_seq <= meta.total) {
        if (should_stop && should_stop()) {
            return false;
        }
        if (!sock.recv_bytes(raw, from, kMaxDatagram)) {
            continue;
        }
        Packet chunk;
        if (!decode_packet(raw.data(), raw.size(), chunk) || chunk.session != meta.session) {
            continue;
        }
        if (chunk.type == PacketType::Meta) {
            send_control(sock, peer, PacketType::MetaAck, meta.session, meta.seq, meta.total);
            continue;
        }
        if (chunk.type != PacketType::Data) {
            continue;
        }
        send_control(sock, peer, PacketType::DataAck, meta.session, chunk.seq, meta.total);
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
        if (should_stop && should_stop()) {
            return false;
        }
        if (!sock.recv_bytes(raw, from, kMaxDatagram)) {
            continue;
        }
        Packet done;
        if (!decode_packet(raw.data(), raw.size(), done) || done.session != meta.session) {
            continue;
        }
        if (done.type == PacketType::Meta) {
            send_control(sock, peer, PacketType::MetaAck, meta.session, meta.seq, meta.total);
            continue;
        }
        if (done.type != PacketType::Done) {
            continue;
        }
        send_control(sock, peer, PacketType::DoneAck, meta.session, done.seq, meta.total);
        break;
    }

    const auto actual_digest = hex64(hash);
    if (!info.checksum.empty() && actual_digest != info.checksum) {
        log_line(log, "Checksum mismatch. expected=" + info.checksum + " actual=" + actual_digest);
        return false;
    }
    log_line(log, "Saved to " + out_path + " checksum=" + actual_digest);
    return true;
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
    if (!wait_for_meta_accept(sock, host, port, meta, options, log)) {
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

    const auto info = parse_incoming_file(meta, peer);
    return receive_accepted_session(sock, meta, peer, output_dir, info, StopFn(), log);
}

bool receive_forever(uint16_t port,
                     const std::string& output_dir,
                     const TransferOptions& options,
                     const AcceptFn& accept,
                     const StopFn& should_stop,
                     const LogFn& log) {
    UdpSocket sock;
    if (!sock.valid() || !sock.bind_port(port) || !sock.set_timeout(std::min(options.timeout_ms, 250))) {
        log_line(log, "Unable to bind UDP port " + std::to_string(port));
        return false;
    }

    log_line(log, "Listening on UDP port " + std::to_string(port));
    Endpoint peer;
    std::vector<uint8_t> raw;
    while (!should_stop || !should_stop()) {
        if (!sock.recv_bytes(raw, peer, kMaxDatagram)) {
            continue;
        }
        Packet meta;
        if (!decode_packet(raw.data(), raw.size(), meta) || meta.type != PacketType::Meta) {
            continue;
        }
        const auto info = parse_incoming_file(meta, peer);
        log_line(log, "Incoming " + info.filename + " (" + std::to_string(info.file_size) + " bytes) from " + peer.host);
        send_control(sock, peer, PacketType::MetaWait, meta.session, meta.seq, meta.total);
        if (accept && !accept(info)) {
            send_control(sock, peer, PacketType::MetaReject, meta.session, meta.seq, meta.total);
            log_line(log, "Rejected " + info.filename);
            continue;
        }
        if (!receive_accepted_session(sock, meta, peer, output_dir, info, should_stop, log)) {
            log_line(log, "Receive failed for " + info.filename);
        }
    }
    return true;
}

} // namespace ttrans
