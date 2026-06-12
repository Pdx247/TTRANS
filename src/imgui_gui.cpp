#include "ttrans.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#ifndef SDL_HINT_WINDOWS_DPI_AWARENESS
#define SDL_HINT_WINDOWS_DPI_AWARENESS "SDL_WINDOWS_DPI_AWARENESS"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ttrans {
namespace {

constexpr uint16_t kFixedUdpPort = 44777;
constexpr const char* kGithubUrl = "https://github.com/Pdx247/TTRANS";

ImFont* g_icon_solid = nullptr;
ImFont* g_icon_brands = nullptr;
bool g_dark_mode = false;

struct SelectedFile {
    std::string path;
};

struct PeerInfo {
    std::string ip;
    std::string label;
    uint64_t last_seen_ms = 0;
};

struct LocalNetwork {
    std::string ip;
    std::string subnet_mask;
    std::string broadcast;
};

struct ChatMessage {
    std::string ip;
    std::string text;
    bool outgoing = false;
};

struct SharedLog {
    std::mutex mutex;
    std::vector<std::string> lines;

    void add(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        if (lines.size() > 400) {
            lines.erase(lines.begin(), lines.begin() + 100);
        }
        lines.push_back(line);
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }
};

struct ReceivePrompt {
    std::mutex mutex;
    std::condition_variable cv;
    IncomingFile file;
    bool active = false;
    bool decided = false;
    bool accepted = false;
};

struct GuiState {
    char output_dir[1024] = "downloads";
    char chat_input[512] = "";
    int selected_peer = 0;
    int udp_port = kFixedUdpPort;
    std::atomic_bool sending{false};
    std::atomic_bool listening{false};
    std::atomic_bool discovering{false};
    std::atomic_bool speed_testing{false};
    std::atomic_bool stop_listener{false};
    std::atomic_bool stop_discovery{false};
    std::atomic<float> transfer_progress{0.0f};
    bool dark_mode = false;
    std::vector<SelectedFile> files;
    std::vector<std::string> local_ips;
    std::vector<LocalNetwork> local_networks;
    std::vector<PeerInfo> peers;
    std::vector<ChatMessage> chat;
    std::string transfer_status;
    std::string speed_status;
    std::mutex peers_mutex;
    std::mutex chat_mutex;
    std::mutex status_mutex;
    SharedLog log;
    ReceivePrompt prompt;
    std::thread listener;
    std::thread discovery;
};

ImU32 u32(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

ImVec4 rgba(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

ImVec4 theme_vec4(int lr, int lg, int lb, int dr, int dg, int db, int a = 255) {
    return g_dark_mode ? rgba(dr, dg, db, a) : rgba(lr, lg, lb, a);
}

ImU32 theme_u32(int lr, int lg, int lb, int dr, int dg, int db, int a = 255) {
    return g_dark_mode ? u32(dr, dg, db, a) : u32(lr, lg, lb, a);
}

std::string human_size(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

uint64_t now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string read_command_all(const char* command) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        return {};
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process {};
    std::string command_line = std::string("cmd.exe /C ") + command;
    const BOOL ok = CreateProcessA(nullptr,
                                   &command_line[0],
                                   nullptr,
                                   nullptr,
                                   TRUE,
                                   CREATE_NO_WINDOW,
                                   nullptr,
                                   nullptr,
                                   &startup,
                                   &process);
    CloseHandle(write_pipe);
    if (!ok) {
        CloseHandle(read_pipe);
        return {};
    }
    std::string result;
    std::array<char, 1024> buffer {};
    DWORD read_bytes = 0;
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, nullptr) && read_bytes > 0) {
        result.append(buffer.data(), buffer.data() + read_bytes);
    }
    WaitForSingleObject(process.hProcess, 1500);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);
    return result;
#else
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return {};
    }
    std::array<char, 1024> buffer {};
    std::string result;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
#endif
}

bool valid_ipv4(const std::string& text) {
    int parts = 0;
    int value = 0;
    bool has_digit = false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char ch = i < text.size() ? text[i] : '.';
        if (ch >= '0' && ch <= '9') {
            has_digit = true;
            value = value * 10 + (ch - '0');
            if (value > 255) {
                return false;
            }
        } else if (ch == '.') {
            if (!has_digit) {
                return false;
            }
            ++parts;
            value = 0;
            has_digit = false;
        } else {
            return false;
        }
    }
    return parts == 4;
}

std::vector<std::string> extract_ipv4_addresses(const std::string& text) {
    std::vector<std::string> ips;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) {
            ++end;
        }
        const auto candidate = text.substr(i, end - i);
        if (valid_ipv4(candidate) &&
            candidate != "0.0.0.0" &&
            candidate != "255.255.255.255" &&
            candidate.find("169.254.") != 0 &&
            std::find(ips.begin(), ips.end(), candidate) == ips.end()) {
            ips.push_back(candidate);
        }
        i = end;
    }
    return ips;
}

bool ipv4_to_u32(const std::string& text, uint32_t& value) {
    if (!valid_ipv4(text)) {
        return false;
    }
    uint32_t result = 0;
    uint32_t part = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char ch = i < text.size() ? text[i] : '.';
        if (ch >= '0' && ch <= '9') {
            part = part * 10 + static_cast<uint32_t>(ch - '0');
        } else if (ch == '.') {
            result = (result << 8) | part;
            part = 0;
        }
    }
    value = result;
    return true;
}

std::string u32_to_ipv4(uint32_t value) {
    std::ostringstream out;
    out << ((value >> 24) & 0xff) << '.'
        << ((value >> 16) & 0xff) << '.'
        << ((value >> 8) & 0xff) << '.'
        << (value & 0xff);
    return out.str();
}

std::string broadcast_from_ip_mask(const std::string& ip, const std::string& mask) {
    uint32_t ip_value = 0;
    uint32_t mask_value = 0;
    if (!ipv4_to_u32(ip, ip_value) || !ipv4_to_u32(mask, mask_value) || mask_value == 0) {
        return {};
    }
    return u32_to_ipv4((ip_value & mask_value) | (~mask_value));
}

std::string legacy_last_octet_broadcast(const std::string& ip) {
    const auto dot = ip.find_last_of('.');
    if (dot == std::string::npos || ip == "127.0.0.1") {
        return {};
    }
    return ip.substr(0, dot + 1) + "255";
}

void add_unique_string(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void add_unique_network(std::vector<LocalNetwork>& networks, const LocalNetwork& network) {
    if (network.ip.empty()) {
        return;
    }
    for (auto& current : networks) {
        if (current.ip == network.ip) {
            if (!network.subnet_mask.empty()) {
                current.subnet_mask = network.subnet_mask;
            }
            if (!network.broadcast.empty()) {
                current.broadcast = network.broadcast;
            }
            return;
        }
    }
    networks.push_back(network);
}

std::vector<std::string> extract_wlan_ipv4_addresses(const std::string& text) {
    std::vector<std::string> ips;
    std::istringstream input(text);
    std::string line;
    bool in_wlan = false;
    while (std::getline(input, line)) {
        const bool adapter_header =
            !line.empty() &&
            !std::isspace(static_cast<unsigned char>(line[0])) &&
            line.find(':') != std::string::npos;
        if (adapter_header) {
            in_wlan = line.find("WLAN") != std::string::npos ||
                      line.find("Wi-Fi") != std::string::npos ||
                      line.find("WiFi") != std::string::npos;
        }
        if (!in_wlan || line.find("IPv4") == std::string::npos) {
            continue;
        }
        const auto found = extract_ipv4_addresses(line);
        for (const auto& ip : found) {
            if (std::find(ips.begin(), ips.end(), ip) == ips.end()) {
                ips.push_back(ip);
            }
        }
    }
    return ips;
}

std::vector<LocalNetwork> extract_wlan_networks(const std::string& text) {
    std::vector<LocalNetwork> networks;
    std::istringstream input(text);
    std::string line;
    bool in_wlan = false;
    std::string pending_ip;
    while (std::getline(input, line)) {
        const bool adapter_header =
            !line.empty() &&
            !std::isspace(static_cast<unsigned char>(line[0])) &&
            line.find(':') != std::string::npos;
        if (adapter_header) {
            in_wlan = line.find("WLAN") != std::string::npos ||
                      line.find("Wi-Fi") != std::string::npos ||
                      line.find("WiFi") != std::string::npos;
            pending_ip.clear();
        }
        if (!in_wlan) {
            continue;
        }
        if (line.find("IPv4") != std::string::npos) {
            const auto found = extract_ipv4_addresses(line);
            if (!found.empty()) {
                pending_ip = found.front();
            }
            continue;
        }
        if (line.find("Subnet Mask") != std::string::npos && !pending_ip.empty()) {
            const auto found = extract_ipv4_addresses(line);
            if (!found.empty()) {
                const auto broadcast = broadcast_from_ip_mask(pending_ip, found.front());
                add_unique_network(networks, LocalNetwork{pending_ip, found.front(), broadcast});
                pending_ip.clear();
            }
        }
    }
    return networks;
}

std::vector<LocalNetwork> collect_local_networks() {
#ifdef _WIN32
    const auto ipconfig = read_command_all("ipconfig");
    auto networks = extract_wlan_networks(ipconfig);
    if (networks.empty()) {
        for (const auto& ip : extract_wlan_ipv4_addresses(ipconfig)) {
            add_unique_network(networks, LocalNetwork{ip, "", legacy_last_octet_broadcast(ip)});
        }
    }
#else
    std::vector<LocalNetwork> networks;
    auto ips = extract_ipv4_addresses(read_command_all("ip -4 addr show wlan0 2>/dev/null"));
    const auto wifi_ips = extract_ipv4_addresses(read_command_all("ip -4 addr show wlp* 2>/dev/null"));
    for (const auto& ip : wifi_ips) {
        add_unique_string(ips, ip);
    }
    for (const auto& ip : ips) {
        add_unique_network(networks, LocalNetwork{ip, "", legacy_last_octet_broadcast(ip)});
    }
#endif
    return networks;
}

std::string file_name_only(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], needed, nullptr, nullptr);
    return result;
}

std::vector<std::string> open_files_dialog() {
    std::vector<wchar_t> selected(65536, L'\0');
    OPENFILENAMEW ofn {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = selected.data();
    ofn.nMaxFile = static_cast<DWORD>(selected.size());
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }

    std::vector<std::wstring> parts;
    const wchar_t* cursor = selected.data();
    while (*cursor) {
        std::wstring part = cursor;
        parts.push_back(part);
        cursor += part.size() + 1;
    }
    std::vector<std::string> paths;
    if (parts.size() == 1) {
        paths.push_back(wide_to_utf8(parts[0].c_str()));
    } else if (parts.size() > 1) {
        const auto dir = parts[0];
        for (std::size_t i = 1; i < parts.size(); ++i) {
            paths.push_back(wide_to_utf8((dir + L"\\" + parts[i]).c_str()));
        }
    }
    return paths;
}
#else
std::string trim_line(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string read_command_first_line(const char* command) {
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return {};
    }
    std::array<char, 4096> buffer {};
    std::string result;
    if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result = buffer.data();
    }
    pclose(pipe);
    return trim_line(result);
}

std::vector<std::string> open_files_dialog() {
#ifdef __APPLE__
    const auto path = read_command_first_line("osascript -e 'POSIX path of (choose file)' 2>/dev/null");
    return path.empty() ? std::vector<std::string>{} : std::vector<std::string>{path};
#else
    const auto output = read_command_first_line(
        "if command -v zenity >/dev/null 2>&1; then "
        "zenity --file-selection --multiple --separator='|' 2>/dev/null; "
        "elif command -v kdialog >/dev/null 2>&1; then "
        "kdialog --getopenfilename . 2>/dev/null; "
        "else printf ''; fi");
    std::vector<std::string> paths;
    std::string current;
    for (char ch : output) {
        if (ch == '|') {
            if (!current.empty()) {
                paths.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        paths.push_back(current);
    }
    return paths;
#endif
}
#endif

bool looks_textual(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    auto ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return ext == "txt" || ext == "md" || ext == "c" || ext == "cpp" || ext == "h" ||
           ext == "hpp" || ext == "json" || ext == "csv" || ext == "log" || ext == "ini";
}

std::string text_preview(const std::string& path) {
    if (!looks_textual(path)) {
        return {};
    }
    std::string data = read_file_head(path, 4096);
    for (char& ch : data) {
        if (ch == '\0') {
            ch = ' ';
        }
    }
    return data;
}

enum class FeatherIcon {
    File,
    FileText,
    Image,
    Archive,
    Code,
    Music,
    Video,
    Database,
    Message,
    Github,
    Wifi,
    Zap,
    Send,
    Users,
};

std::string file_extension(const std::string& path) {
    const auto name = file_name_only(path);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    auto ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return ext;
}

FeatherIcon icon_for_file(const std::string& path) {
    const auto ext = file_extension(path);
    if (ext == "txt" || ext == "md" || ext == "pdf" || ext == "doc" || ext == "docx" || ext == "csv" || ext == "log") {
        return FeatherIcon::FileText;
    }
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp" || ext == "webp" || ext == "svg") {
        return FeatherIcon::Image;
    }
    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" || ext == "gz") {
        return FeatherIcon::Archive;
    }
    if (ext == "c" || ext == "cpp" || ext == "h" || ext == "hpp" || ext == "json" || ext == "xml" || ext == "html" || ext == "css" || ext == "js" || ext == "py") {
        return FeatherIcon::Code;
    }
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac") {
        return FeatherIcon::Music;
    }
    if (ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "avi") {
        return FeatherIcon::Video;
    }
    if (ext == "db" || ext == "sqlite" || ext == "sql") {
        return FeatherIcon::Database;
    }
    return FeatherIcon::File;
}

void draw_icon(FeatherIcon icon, ImVec2 pos, float size, ImU32 color) {
    const char* glyph = u8"\uf15b";
    ImFont* font = g_icon_solid;
    switch (icon) {
    case FeatherIcon::File: glyph = u8"\uf15b"; break;
    case FeatherIcon::FileText: glyph = u8"\uf15c"; break;
    case FeatherIcon::Image: glyph = u8"\uf03e"; break;
    case FeatherIcon::Archive: glyph = u8"\uf1c6"; break;
    case FeatherIcon::Code: glyph = u8"\uf121"; break;
    case FeatherIcon::Music: glyph = u8"\uf001"; break;
    case FeatherIcon::Video: glyph = u8"\uf03d"; break;
    case FeatherIcon::Database: glyph = u8"\uf1c0"; break;
    case FeatherIcon::Message: glyph = u8"\uf086"; break;
    case FeatherIcon::Github:
        glyph = u8"\uf09b";
        font = g_icon_brands ? g_icon_brands : g_icon_solid;
        break;
    case FeatherIcon::Wifi: glyph = u8"\uf1eb"; break;
    case FeatherIcon::Zap: glyph = u8"\uf0e7"; break;
    case FeatherIcon::Send: glyph = u8"\uf1d8"; break;
    case FeatherIcon::Users: glyph = u8"\uf0c0"; break;
    }
    if (font) {
        ImGui::GetWindowDrawList()->AddText(font, size, pos, color, glyph);
    } else {
        ImGui::GetWindowDrawList()->AddText(pos, color, "*");
    }
}

void section_label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(26, 41, 64, 226, 232, 240));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const float x = ImGui::GetCursorPosX();
    const float y = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.55f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(ImGui::GetWindowPos().x + x + 8.0f, y),
                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 18.0f, y),
                  theme_u32(219, 226, 236, 51, 65, 85), 1.0f);
    ImGui::NewLine();
}

ImVec2 centered_button_size(const char* text, const ImVec2& requested) {
    ImVec2 size = requested;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float min_width = text_size.x + style.FramePadding.x * 2.0f + 10.0f;
    const float min_height = text_size.y + style.FramePadding.y * 2.0f + 2.0f;
    if (size.x > 0.0f) {
        size.x = std::max(size.x, min_width);
    }
    if (size.y > 0.0f) {
        size.y = std::max(size.y, min_height);
    }
    return size;
}

bool primary_button(const char* text, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 7));
    ImGui::PushStyleColor(ImGuiCol_Button, theme_vec4(13, 87, 189, 60, 132, 246));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme_vec4(15, 105, 219, 80, 152, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme_vec4(10, 69, 158, 40, 102, 210));
    ImGui::PushStyleColor(ImGuiCol_Text, rgba(255, 255, 255));
    const bool clicked = ImGui::Button(text, centered_button_size(text, size));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

bool secondary_button(const char* text, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 7));
    ImGui::PushStyleColor(ImGuiCol_Button, theme_vec4(240, 245, 252, 30, 41, 59));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme_vec4(226, 237, 252, 42, 57, 80));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme_vec4(214, 227, 247, 50, 68, 96));
    ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(28, 43, 66, 226, 232, 240));
    const bool clicked = ImGui::Button(text, centered_button_size(text, size));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

void copy_to_buffer(char* buffer, std::size_t size, const std::string& value) {
    if (size == 0) {
        return;
    }
    std::snprintf(buffer, size, "%s", value.c_str());
}

void set_status(GuiState& state, const std::string& text) {
    std::lock_guard<std::mutex> lock(state.status_mutex);
    state.transfer_status = text;
}

void set_speed_status(GuiState& state, const std::string& text) {
    std::lock_guard<std::mutex> lock(state.status_mutex);
    state.speed_status = text;
}

std::string transfer_status(GuiState& state) {
    std::lock_guard<std::mutex> lock(state.status_mutex);
    return state.transfer_status;
}

std::string speed_status(GuiState& state) {
    std::lock_guard<std::mutex> lock(state.status_mutex);
    return state.speed_status;
}

void upsert_peer(GuiState& state, const std::string& ip, const std::string& label) {
    if (ip.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.peers_mutex);
    for (auto& peer : state.peers) {
        if (peer.ip == ip) {
            peer.last_seen_ms = now_ms();
            if (!label.empty()) {
                peer.label = label;
            }
            return;
        }
    }
    PeerInfo peer;
    peer.ip = ip;
    peer.label = label.empty() ? "TTrans" : label;
    peer.last_seen_ms = now_ms();
    state.peers.push_back(peer);
}

void refresh_local_networks(GuiState& state) {
    state.local_networks = collect_local_networks();
    state.local_ips.clear();
    add_unique_string(state.local_ips, "127.0.0.1");
    for (const auto& network : state.local_networks) {
        add_unique_string(state.local_ips, network.ip);
    }
    for (const auto& ip : state.local_ips) {
        upsert_peer(state, ip, ip == "127.0.0.1" ? "Loopback" : "WLAN");
    }
}

std::vector<PeerInfo> peer_snapshot(GuiState& state) {
    std::lock_guard<std::mutex> lock(state.peers_mutex);
    return state.peers;
}

void add_chat(GuiState& state, const std::string& ip, const std::string& text, bool outgoing) {
    if (text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.chat_mutex);
    if (state.chat.size() > 200) {
        state.chat.erase(state.chat.begin(), state.chat.begin() + 60);
    }
    state.chat.push_back(ChatMessage{ip, text, outgoing});
}

std::vector<ChatMessage> chat_snapshot(GuiState& state) {
    std::lock_guard<std::mutex> lock(state.chat_mutex);
    return state.chat;
}

uint32_t next_gui_session() {
    static std::atomic<uint32_t> session{10000};
    return session.fetch_add(1);
}

bool send_gui_packet(const std::string& host, PacketType type, const std::string& payload = {}) {
    UdpSocket sock;
    if (!sock.valid()) {
        return false;
    }
    Packet packet;
    packet.type = type;
    packet.session = next_gui_session();
    packet.seq = 1;
    packet.total = 1;
    packet.payload.assign(payload.begin(), payload.end());
    const auto bytes = encode_packet(packet);
    return sock.send_bytes(host, kFixedUdpPort, bytes.data(), bytes.size());
}

bool has_file(const GuiState& state, const std::string& path) {
    for (const auto& file : state.files) {
        if (file.path == path) {
            return true;
        }
    }
    return false;
}

void add_file(GuiState& state, const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (!path_is_regular_file(path)) {
        state.log.add("Choose a file, not a folder: " + path);
        return;
    }
    if (!has_file(state, path)) {
        state.files.push_back(SelectedFile{path});
    }
    set_status(state, "");
}

void add_files(GuiState& state, const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        add_file(state, path);
    }
}

std::string selected_peer_ip(GuiState& state) {
    const auto peers = peer_snapshot(state);
    if (peers.empty()) {
        return "127.0.0.1";
    }
    const int index = std::max(0, std::min(state.selected_peer, static_cast<int>(peers.size()) - 1));
    return peers[static_cast<std::size_t>(index)].ip;
}

std::vector<std::string> broadcast_targets(const std::vector<LocalNetwork>& networks) {
    std::vector<std::string> targets;
    add_unique_string(targets, "127.0.0.1");
    add_unique_string(targets, "255.255.255.255");
    for (const auto& network : networks) {
        add_unique_string(targets, network.broadcast);
        add_unique_string(targets, legacy_last_octet_broadcast(network.ip));
    }
    return targets;
}

void start_discovery(GuiState& state) {
    if (state.discovering.load()) {
        return;
    }
    if (state.discovery.joinable()) {
        state.discovery.join();
    }
    state.stop_discovery = false;
    state.discovering = true;
    upsert_peer(state, "127.0.0.1", "Loopback");
    const auto targets = broadcast_targets(state.local_networks);
    state.discovery = std::thread([&state, targets] {
        for (int round = 0; round < 4 && !state.stop_discovery.load(); ++round) {
            UdpSocket sock;
            if (sock.valid()) {
                sock.set_timeout(160);
                sock.set_broadcast(true);
                Packet ping;
                ping.type = PacketType::DiscoveryPing;
                ping.session = next_gui_session();
                ping.seq = 1;
                ping.total = 1;
                const std::string payload = "TTrans";
                ping.payload.assign(payload.begin(), payload.end());
                const auto bytes = encode_packet(ping);
                for (const auto& target : targets) {
                    sock.send_bytes(target, kFixedUdpPort, bytes.data(), bytes.size());
                }

                const auto deadline = now_ms() + 500;
                while (now_ms() < deadline && !state.stop_discovery.load()) {
                    Endpoint from;
                    std::vector<uint8_t> raw;
                    if (!sock.recv_bytes(raw, from, 1400)) {
                        continue;
                    }
                    Packet response;
                    if (decode_packet(raw.data(), raw.size(), response) && response.type == PacketType::DiscoveryPong) {
                        const std::string label(response.payload.begin(), response.payload.end());
                        upsert_peer(state, from.host, label.empty() ? "TTrans" : label);
                    }
                }
            }
            for (int i = 0; i < 2 && !state.stop_discovery.load(); ++i) {
                SDL_Delay(100);
            }
        }
        state.discovering = false;
    });
}

void stop_discovery(GuiState& state) {
    state.stop_discovery = true;
    if (state.discovery.joinable()) {
        state.discovery.join();
    }
}

void run_speed_test(GuiState& state, const std::string& host) {
    if (state.speed_testing.load() || host.empty()) {
        return;
    }
    state.speed_testing = true;
    set_speed_status(state, "...");
    std::thread([&state, host] {
        UdpSocket sock;
        if (!sock.valid() || !sock.set_timeout(120)) {
            set_speed_status(state, "Fail");
            state.speed_testing = false;
            return;
        }
        constexpr int count = 96;
        constexpr int payload_size = 1024;
        const uint32_t session = next_gui_session();
        std::string payload(static_cast<std::size_t>(payload_size), 'x');
        Packet probe;
        probe.type = PacketType::SpeedProbe;
        probe.session = session;
        probe.total = count;
        probe.payload.assign(payload.begin(), payload.end());

        const auto start = std::chrono::steady_clock::now();
        for (int i = 1; i <= count; ++i) {
            probe.seq = static_cast<uint32_t>(i);
            const auto bytes = encode_packet(probe);
            sock.send_bytes(host, kFixedUdpPort, bytes.data(), bytes.size());
            SDL_Delay(4);
        }

        int replies = 0;
        const auto deadline = now_ms() + 1700;
        while (now_ms() < deadline && replies < count) {
            Endpoint from;
            std::vector<uint8_t> raw;
            if (!sock.recv_bytes(raw, from, 1400)) {
                continue;
            }
            Packet pong;
            if (decode_packet(raw.data(), raw.size(), pong) &&
                pong.type == PacketType::SpeedPong &&
                pong.session == session) {
                ++replies;
            }
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::ostringstream out;
        if (replies == 0 || elapsed <= 0.0) {
            out << "无响应";
        } else {
            const double mbps = (static_cast<double>(replies) * payload_size * 2.0 * 8.0) / elapsed / 1000000.0;
            out << std::fixed << std::setprecision(2) << mbps << " Mbps  " << replies << "/" << count;
        }
        set_speed_status(state, out.str());
        state.speed_testing = false;
    }).detach();
}

void send_selected_files(GuiState& state, const TransferOptions& options) {
    if (state.sending.load() || state.files.empty()) {
        return;
    }
    const auto files = state.files;
    const auto host = selected_peer_ip(state);
    state.sending = true;
    state.transfer_progress.store(0.0f);
    set_status(state, "");
    std::thread([&state, files, host, options] {
        const std::size_t total_files = files.size();
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto path = files[i].path;
            set_status(state, file_name_only(path));
            auto log = [&state, i, total_files](const std::string& line) {
                const std::string prefix = "Progress ";
                if (line.find(prefix) == 0) {
                    const auto slash = line.find('/');
                    if (slash != std::string::npos) {
                        const int done = std::atoi(line.substr(prefix.size(), slash - prefix.size()).c_str());
                        const int total = std::atoi(line.substr(slash + 1).c_str());
                        if (total > 0 && total_files > 0) {
                            const float current = static_cast<float>(done) / static_cast<float>(total);
                            const float all = (static_cast<float>(i) + current) / static_cast<float>(total_files);
                            state.transfer_progress.store(std::max(0.0f, std::min(1.0f, all)));
                        }
                    }
                }
            };
            const bool ok = send_file(host, kFixedUdpPort, path, options, log);
            if (!ok) {
                set_status(state, file_name_only(path));
                state.sending = false;
                return;
            }
            state.transfer_progress.store(static_cast<float>(i + 1) / static_cast<float>(total_files));
        }
        set_status(state, "");
        state.sending = false;
    }).detach();
}

void start_listener(GuiState& state, const TransferOptions& options) {
    if (state.listener.joinable()) {
        state.stop_listener = true;
        {
            std::lock_guard<std::mutex> lock(state.prompt.mutex);
            state.prompt.decided = true;
            state.prompt.accepted = false;
        }
        state.prompt.cv.notify_all();
        state.listener.join();
    }

    const auto port = static_cast<uint16_t>(std::max(1, std::min(65535, state.udp_port)));
    const std::string output_dir = state.output_dir[0] ? state.output_dir : "downloads";
    state.stop_listener = false;
    state.listening = true;
    state.log.add("Listening on UDP port " + std::to_string(port));

    state.listener = std::thread([&state, port, output_dir, options] {
        auto accept = [&state](const IncomingFile& file) {
            std::unique_lock<std::mutex> lock(state.prompt.mutex);
            state.prompt.file = file;
            state.prompt.active = true;
            state.prompt.decided = false;
            state.prompt.accepted = false;
            state.prompt.cv.wait(lock, [&state] {
                return state.prompt.decided || state.stop_listener.load();
            });
            const bool accepted = state.prompt.accepted && !state.stop_listener.load();
            state.prompt.active = false;
            state.prompt.decided = false;
            state.prompt.accepted = false;
            return accepted;
        };
        auto stop = [&state] {
            return state.stop_listener.load();
        };
        auto log = [&state](const std::string& line) {
            state.log.add(line);
        };
        auto peer_event = [&state](const Endpoint& peer, const Packet& packet) {
            upsert_peer(state, peer.host, "TTrans");
            if (packet.type == PacketType::Chat) {
                const std::string text(packet.payload.begin(), packet.payload.end());
                add_chat(state, peer.host, text, false);
            }
        };
        receive_forever(port, output_dir, options, accept, stop, log, peer_event);
        state.listening = false;
    });
}

void stop_listener(GuiState& state) {
    state.stop_listener = true;
    {
        std::lock_guard<std::mutex> lock(state.prompt.mutex);
        state.prompt.decided = true;
        state.prompt.accepted = false;
    }
    state.prompt.cv.notify_all();
    if (state.listener.joinable()) {
        state.listener.join();
    }
}

void panel_title(FeatherIcon icon, const char* title) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    draw_icon(icon, ImVec2(pos.x, pos.y + 1.0f), 18.0f, theme_u32(38, 64, 102, 148, 163, 184));
    ImGui::Dummy(ImVec2(24.0f, 20.0f));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme_vec4(20, 36, 56, 226, 232, 240));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void draw_left_panel(GuiState& state) {
    panel_title(FeatherIcon::Wifi, "IP");
    const auto peers = peer_snapshot(state);
    const int peer_count = static_cast<int>(peers.size());
    if (state.selected_peer >= peer_count) {
        state.selected_peer = std::max(0, peer_count - 1);
    }
    const std::string current = peers.empty() ? "127.0.0.1" : peers[static_cast<std::size_t>(state.selected_peer)].ip;
    if (ImGui::BeginCombo("##peer_combo", current.c_str())) {
        for (int i = 0; i < peer_count; ++i) {
            const bool selected = state.selected_peer == i;
            const auto label = peers[static_cast<std::size_t>(i)].ip;
            if (ImGui::Selectable(label.c_str(), selected)) {
                state.selected_peer = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(1, 8));
    if (state.discovering.load()) {
        ImGui::BeginDisabled();
    }
    if (secondary_button(state.discovering.load() ? "Searching" : "Search", ImVec2(-1, 34))) {
        refresh_local_networks(state);
        start_discovery(state);
    }
    if (state.discovering.load()) {
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(1, 6));
    if (secondary_button(state.dark_mode ? "Day" : "Night", ImVec2(-1, 34))) {
        state.dark_mode = !state.dark_mode;
    }

    ImGui::Dummy(ImVec2(1, 10));
    panel_title(FeatherIcon::Users, "WLAN");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme_vec4(247, 250, 255, 15, 23, 42));
    ImGui::BeginChild("local_ips", ImVec2(0, 104), true);
    for (const auto& ip : state.local_ips) {
        ImGui::BulletText("%s", ip.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(1, 10));
    panel_title(FeatherIcon::Zap, "Speed");
    if (state.speed_testing.load()) {
        ImGui::BeginDisabled();
    }
    if (secondary_button("Start", ImVec2(-1, 34))) {
        run_speed_test(state, selected_peer_ip(state));
    }
    if (state.speed_testing.load()) {
        ImGui::EndDisabled();
    }
    ImGui::TextWrapped("%s", speed_status(state).c_str());
}

void draw_incoming_popup(GuiState& state) {
    IncomingFile pending;
    bool has_pending = false;
    {
        std::lock_guard<std::mutex> lock(state.prompt.mutex);
        if (state.prompt.active && !state.prompt.decided) {
            pending = state.prompt.file;
            has_pending = true;
        }
    }
    if (has_pending) {
        ImGui::OpenPopup("Incoming File");
    }
    if (ImGui::BeginPopupModal("Incoming File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        section_label("File");
        ImGui::TextWrapped("%s", pending.filename.c_str());
        ImGui::TextDisabled("%s  %s:%u", human_size(pending.file_size).c_str(), pending.peer_host.c_str(), pending.peer_port);
        ImGui::TextDisabled("%s", pending.checksum.c_str());
        ImGui::Dummy(ImVec2(1, 8));
        if (primary_button("Accept", ImVec2(128, 34))) {
            {
                std::lock_guard<std::mutex> lock(state.prompt.mutex);
                state.prompt.accepted = true;
                state.prompt.decided = true;
            }
            state.prompt.cv.notify_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (secondary_button("Reject", ImVec2(128, 34))) {
            {
                std::lock_guard<std::mutex> lock(state.prompt.mutex);
                state.prompt.accepted = false;
                state.prompt.decided = true;
            }
            state.prompt.cv.notify_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_file_row(GuiState& state, std::size_t index) {
    const auto path = state.files[index].path;
    const auto name = file_name_only(path);
    const auto size = path_file_size(path);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
    draw_icon(icon_for_file(path), ImVec2(icon_pos.x + 4.0f, icon_pos.y + 7.0f), 28.0f, theme_u32(42, 105, 185, 96, 165, 250));
    ImGui::Dummy(ImVec2(38.0f, 42.0f));

    ImGui::TableSetColumnIndex(1);
    ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopTextWrapPos();
    const auto ext = file_extension(path);
    if (ext.empty()) {
        ImGui::TextDisabled("%s", human_size(size).c_str());
    } else {
        ImGui::TextDisabled(".%s  %s", ext.c_str(), human_size(size).c_str());
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID(static_cast<int>(index));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
    if (secondary_button("Remove", ImVec2(76, 30))) {
        state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(index));
        set_status(state, "");
    }
    ImGui::PopID();
}

void draw_center_panel(GuiState& state, const TransferOptions& options) {
    panel_title(FeatherIcon::FileText, "Files");
    if (secondary_button("Add", ImVec2(82, 34))) {
        add_files(state, open_files_dialog());
    }
    ImGui::SameLine();
    if (secondary_button("Clear", ImVec2(82, 34))) {
        state.files.clear();
        state.transfer_progress.store(0.0f);
        set_status(state, "");
    }

    ImGui::Dummy(ImVec2(1, 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          state.files.empty() ? theme_vec4(238, 247, 255, 15, 23, 42)
                                              : theme_vec4(255, 255, 255, 11, 18, 32));
    ImGui::BeginChild("file_queue", ImVec2(0, 328), true);
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(pos.x + ImGui::GetWindowWidth(), pos.y + ImGui::GetWindowHeight());
    ImGui::GetWindowDrawList()->AddRect(pos,
                                        max,
                                        state.files.empty() ? theme_u32(64, 128, 210, 96, 165, 250)
                                                            : theme_u32(218, 226, 236, 51, 65, 85),
                                        8.0f,
                                        0,
                                        state.files.empty() ? 2.0f : 1.0f);
    if (state.files.empty()) {
        const ImVec2 center = ImVec2(pos.x + ImGui::GetWindowWidth() * 0.5f - 14.0f, pos.y + 74.0f);
        draw_icon(FeatherIcon::File, center, 40.0f, theme_u32(64, 128, 210, 96, 165, 250));
    } else {
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_NoPadOuterX;
        if (ImGui::BeginTable("file_rows", 3, flags)) {
            ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            for (std::size_t i = 0; i < state.files.size();) {
                const std::size_t before = state.files.size();
                draw_file_row(state, i);
                if (state.files.size() == before) {
                    ++i;
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(1, 8));
    ImGui::TextDisabled("%s", transfer_status(state).c_str());
    ImGui::ProgressBar(state.transfer_progress.load(), ImVec2(-1, 18), "");
    const bool can_send = !state.files.empty() && !state.sending.load() && !selected_peer_ip(state).empty();
    if (!can_send) {
        ImGui::BeginDisabled();
    }
    if (primary_button(state.sending.load() ? "Sending" : "Send", ImVec2(-1, 38))) {
        send_selected_files(state, options);
    }
    if (!can_send) {
        ImGui::EndDisabled();
    }
}

void draw_right_panel(GuiState& state) {
    panel_title(FeatherIcon::Message, "Chat");
    const auto peer_ip = selected_peer_ip(state);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme_vec4(250, 252, 255, 15, 23, 42));
    ImGui::BeginChild("chat_history", ImVec2(0, 332), true);
    const auto messages = chat_snapshot(state);
    for (const auto& message : messages) {
        if (message.ip != peer_ip) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text,
                              message.outgoing ? theme_vec4(10, 77, 168, 147, 197, 253)
                                               : theme_vec4(26, 38, 59, 226, 232, 240));
        ImGui::TextWrapped("%s  %s", message.outgoing ? "Me" : message.ip.c_str(), message.text.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::PushItemWidth(-84.0f);
    const bool enter = ImGui::InputText("##chat_input", state.chat_input, sizeof(state.chat_input), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    const bool clicked = secondary_button("Send", ImVec2(72, 32));
    if ((enter || clicked) && state.chat_input[0]) {
        const std::string text = state.chat_input;
        if (send_gui_packet(peer_ip, PacketType::Chat, text)) {
            add_chat(state, peer_ip, text, true);
            state.chat_input[0] = '\0';
        }
    }

    ImGui::Dummy(ImVec2(1, 12));
    panel_title(FeatherIcon::Github, "GitHub");
    ImGui::TextWrapped("%s", kGithubUrl);
    if (secondary_button("Open", ImVec2(-1, 34))) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        SDL_OpenURL(kGithubUrl);
#endif
    }
}

void apply_palette(bool dark_mode) {
    g_dark_mode = dark_mode;
    ImGuiStyle& style = ImGui::GetStyle();
    if (dark_mode) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = theme_vec4(26, 38, 59, 226, 232, 240);
    colors[ImGuiCol_TextDisabled] = theme_vec4(104, 117, 138, 148, 163, 184);
    colors[ImGuiCol_WindowBg] = theme_vec4(242, 247, 252, 7, 12, 22);
    colors[ImGuiCol_ChildBg] = theme_vec4(255, 255, 255, 15, 23, 42);
    colors[ImGuiCol_PopupBg] = theme_vec4(255, 255, 255, 15, 23, 42);
    colors[ImGuiCol_Border] = theme_vec4(214, 224, 238, 51, 65, 85);
    colors[ImGuiCol_FrameBg] = theme_vec4(247, 250, 255, 17, 24, 39);
    colors[ImGuiCol_FrameBgHovered] = theme_vec4(235, 242, 252, 30, 41, 59);
    colors[ImGuiCol_FrameBgActive] = theme_vec4(224, 235, 249, 42, 57, 80);
    colors[ImGuiCol_Header] = theme_vec4(230, 239, 252, 30, 41, 59);
    colors[ImGuiCol_HeaderHovered] = theme_vec4(214, 229, 250, 42, 57, 80);
    colors[ImGuiCol_HeaderActive] = theme_vec4(196, 217, 247, 51, 65, 85);
    colors[ImGuiCol_CheckMark] = theme_vec4(13, 87, 189, 96, 165, 250);
    colors[ImGuiCol_ScrollbarBg] = theme_vec4(238, 244, 251, 11, 18, 32);
    colors[ImGuiCol_ScrollbarGrab] = theme_vec4(194, 207, 226, 51, 65, 85);
    colors[ImGuiCol_ScrollbarGrabHovered] = theme_vec4(164, 183, 211, 71, 85, 105);
    colors[ImGuiCol_TableRowBg] = theme_vec4(255, 255, 255, 11, 18, 32);
    colors[ImGuiCol_TableRowBgAlt] = theme_vec4(247, 250, 255, 15, 23, 42);
    colors[ImGuiCol_PlotHistogram] = theme_vec4(13, 87, 189, 96, 165, 250);
    colors[ImGuiCol_PlotHistogramHovered] = theme_vec4(15, 105, 219, 147, 197, 253);
}

void apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 5.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 7);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.ItemSpacing = ImVec2(10, 9);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 12.0f;
    apply_palette(false);
}

void load_crisp_font(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const float size = 15.5f * scale;
#ifdef _WIN32
    const char* font_paths[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        nullptr,
    };
#elif __APPLE__
    const char* font_paths[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        nullptr,
    };
#else
    const char* font_paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        nullptr,
    };
#endif
    ImFontConfig config;
    config.OversampleH = 3;
    config.OversampleV = 2;
    config.PixelSnapH = true;
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    for (int i = 0; font_paths[i]; ++i) {
        if (io.Fonts->AddFontFromFileTTF(font_paths[i], size, &config, ranges)) {
            break;
        }
    }
    if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefault();
    }

    auto load_icon_font = [&io](const std::string& file_name, float font_size) {
        std::vector<std::string> candidates;
        candidates.push_back(file_name);
        candidates.push_back(std::string("assets/") + file_name);
        char* base = SDL_GetBasePath();
        if (base) {
            candidates.push_back(std::string(base) + file_name);
            candidates.push_back(std::string(base) + "assets/" + file_name);
            candidates.push_back(std::string(base) + "../Resources/" + file_name);
            candidates.push_back(std::string(base) + "../Resources/assets/" + file_name);
            SDL_free(base);
        }
        static const ImWchar icon_ranges[] = {0xf000, 0xf8ff, 0};
        ImFontConfig icon_config;
        icon_config.OversampleH = 3;
        icon_config.OversampleV = 2;
        icon_config.PixelSnapH = true;
        for (const auto& path : candidates) {
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), font_size, &icon_config, icon_ranges)) {
                return font;
            }
        }
        return static_cast<ImFont*>(nullptr);
    };

    g_icon_solid = load_icon_font("fa-solid-900.ttf", 16.5f * scale);
    g_icon_brands = load_icon_font("fa-brands-400.ttf", 16.5f * scale);
}

float dpi_scale_for_window(SDL_Window* window) {
    float ddpi = 96.0f;
    const int display = SDL_GetWindowDisplayIndex(window);
    if (display >= 0) {
        SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr);
    }
    if (ddpi <= 0.0f) {
        ddpi = 96.0f;
    }
    return std::max(1.0f, std::min(1.75f, ddpi / 96.0f));
}

} // namespace

int run_imgui_gui(uint16_t udp_port, const std::string& output_dir, const TransferOptions& options) {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }

    const char* glsl_version = "#version 130";
#ifdef __APPLE__
    glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("TTrans",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          1040,
                                          640,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_SetWindowMinimumSize(window, 920, 560);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_style();
    const float dpi_scale = dpi_scale_for_window(window);
    load_crisp_font(dpi_scale);
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    GuiState state;
    (void)udp_port;
    state.udp_port = kFixedUdpPort;
    copy_to_buffer(state.output_dir, sizeof(state.output_dir), output_dir.empty() ? "downloads" : output_dir);
    refresh_local_networks(state);
    state.log.add("TTrans native GUI ready.");
    start_listener(state, options);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_DROPFILE && event.drop.file) {
                add_file(state, event.drop.file);
                SDL_free(event.drop.file);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        apply_palette(state.dark_mode);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("TTrans", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);

        draw_incoming_popup(state);

        const float full_width = ImGui::GetContentRegionAvail().x;
        const float full_height = ImGui::GetContentRegionAvail().y;
        const float gap = 12.0f * dpi_scale;
        const float panel_width = std::max(0.0f, full_width - gap * 2.0f);
        float left_width = std::min(240.0f, panel_width * 0.23f);
        float right_width = std::min(290.0f, panel_width * 0.27f);
        float mid_width = panel_width - left_width - right_width;
        const float desired_mid = std::min(520.0f, panel_width * 0.52f);
        if (mid_width < desired_mid) {
            float shortage = desired_mid - mid_width;
            const float right_min = std::min(240.0f, panel_width * 0.27f);
            const float left_min = std::min(200.0f, panel_width * 0.22f);
            const float right_cut = std::min(shortage * 0.65f, std::max(0.0f, right_width - right_min));
            right_width -= right_cut;
            shortage -= right_cut;
            const float left_cut = std::min(shortage, std::max(0.0f, left_width - left_min));
            left_width -= left_cut;
            mid_width = panel_width - left_width - right_width;
        }

        ImGui::BeginChild("left_panel", ImVec2(left_width, full_height), true);
        draw_left_panel(state);
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("center_panel", ImVec2(mid_width, full_height), true);
        draw_center_panel(state, options);
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("right_panel", ImVec2(0, full_height), true);
        draw_right_panel(state);
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        SDL_GL_GetDrawableSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        if (state.dark_mode) {
            glClearColor(0.03f, 0.05f, 0.09f, 1.0f);
        } else {
            glClearColor(0.95f, 0.97f, 0.99f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    stop_discovery(state);
    stop_listener(state);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace ttrans
