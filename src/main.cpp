#include "ttrans.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_help() {
    std::cout
        << "TTrans - lightweight UDP LAN file transfer\n\n"
        << "Usage:\n"
        << "  ttrans send --host <ip> --port <port> --file <path>\n"
        << "  ttrans receive --port <port> --out <dir>\n"
        << "  ttrans gui [--port <port>] [--out <dir>]\n\n"
        << "Options:\n"
        << "  --chunk <bytes>      UDP payload size, default 1024\n"
        << "  --timeout <ms>       ACK timeout, default 700\n"
        << "  --retries <count>    Retries per packet, default 16\n";
}

std::string value_after(const std::vector<std::string>& args, const std::string& key, const std::string& fallback = {}) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) {
            return args[i + 1];
        }
    }
    return fallback;
}

bool has_arg(const std::vector<std::string>& args, const std::string& key) {
    for (const auto& arg : args) {
        if (arg == key) {
            return true;
        }
    }
    return false;
}

uint16_t parse_port(const std::string& text, uint16_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    const auto value = std::stoi(text);
    if (value <= 0 || value > 65535) {
        throw std::runtime_error("Invalid port: " + text);
    }
    return static_cast<uint16_t>(value);
}

void apply_common_options(const std::vector<std::string>& args, ttrans::TransferOptions& options) {
    if (has_arg(args, "--chunk")) {
        options.chunk_size = static_cast<std::size_t>(std::stoul(value_after(args, "--chunk")));
        if (options.chunk_size < 128 || options.chunk_size > 1200) {
            throw std::runtime_error("--chunk must be between 128 and 1200");
        }
    }
    if (has_arg(args, "--timeout")) {
        options.timeout_ms = std::stoi(value_after(args, "--timeout"));
    }
    if (has_arg(args, "--retries")) {
        options.retries = std::stoi(value_after(args, "--retries"));
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_help();
        return args.empty() ? 1 : 0;
    }

    ttrans::TransferOptions options;
    auto log = [](const std::string& line) {
        std::cout << line << '\n';
    };

    try {
        apply_common_options(args, options);
        const auto command = args[0];
        if (command == "send") {
            const auto host = value_after(args, "--host");
            const auto file = value_after(args, "--file");
            const auto port = parse_port(value_after(args, "--port"), 44777);
            if (host.empty() || file.empty()) {
                print_help();
                return 1;
            }
            return ttrans::send_file(host, port, file, options, log) ? 0 : 2;
        }
        if (command == "receive") {
            const auto port = parse_port(value_after(args, "--port"), 44777);
            const auto out = value_after(args, "--out", "downloads");
            return ttrans::receive_once(port, out, options, log) ? 0 : 2;
        }
        if (command == "gui") {
            const auto port = parse_port(value_after(args, "--port"), 44777);
            const auto out = value_after(args, "--out", "downloads");
            return ttrans::run_imgui_gui(port, out, options);
        }
        print_help();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
