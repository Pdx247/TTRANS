#include "ttrans.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::string value_after(const std::vector<std::string>& args, const std::string& key, const std::string& fallback = {}) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) {
            return args[i + 1];
        }
    }
    return fallback;
}

uint16_t parse_port(const std::string& text, uint16_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    const auto value = std::stoi(text);
    if (value <= 0 || value > 65535) {
        return fallback;
    }
    return static_cast<uint16_t>(value);
}

int run_gui_args(const std::vector<std::string>& args) {
    ttrans::TransferOptions options;
    const auto port = parse_port(value_after(args, "--port"), 44777);
    const auto out = value_after(args, "--out", "downloads");
    return ttrans::run_imgui_gui(port, out, options);
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* text) {
    if (!text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    return out;
}
#endif

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(wide_to_utf8(argv_w[i]));
    }
    if (argv_w) {
        LocalFree(argv_w);
    }
    return run_gui_args(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return run_gui_args(args);
}
#endif
