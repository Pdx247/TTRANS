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

struct SelectedFile {
    std::string path;
};

struct PeerInfo {
    std::string ip;
    std::string label;
    uint64_t last_seen_ms = 0;
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
    std::vector<SelectedFile> files;
    std::vector<std::string> local_ips;
    std::vector<PeerInfo> peers;
    std::vector<ChatMessage> chat;
    std::string transfer_status = "选择文件后开始传输";
    std::string speed_status = "未测速";
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

std::vector<std::string> collect_local_ips() {
#ifdef _WIN32
    auto ips = extract_ipv4_addresses(read_command_all("ipconfig"));
#else
    auto ips = extract_ipv4_addresses(read_command_all("hostname -I 2>/dev/null"));
    if (ips.empty()) {
        ips = extract_ipv4_addresses(read_command_all("ifconfig 2>/dev/null"));
    }
#endif
    if (std::find(ips.begin(), ips.end(), "127.0.0.1") == ips.end()) {
        ips.insert(ips.begin(), "127.0.0.1");
    }
    return ips;
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

void draw_feather_icon(FeatherIcon icon, ImVec2 pos, float size, ImU32 color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float s = size / 24.0f;
    auto p = [&](float x, float y) {
        return ImVec2(pos.x + x * s, pos.y + y * s);
    };
    const float t = std::max(1.4f, size / 18.0f);
    const float r = 2.0f * s;
    auto arc = [&](ImVec2 center, float radius, float a_min, float a_max) {
        draw->PathArcTo(center, radius, a_min, a_max, 18);
        draw->PathStroke(color, 0, t);
    };

    if (icon == FeatherIcon::File || icon == FeatherIcon::FileText) {
        draw->AddRect(p(5, 2), p(19, 22), color, r, 0, t);
        draw->AddLine(p(14, 2), p(19, 7), color, t);
        draw->AddLine(p(14, 2), p(14, 7), color, t);
        draw->AddLine(p(14, 7), p(19, 7), color, t);
        if (icon == FeatherIcon::FileText) {
            draw->AddLine(p(8, 12), p(16, 12), color, t);
            draw->AddLine(p(8, 16), p(15, 16), color, t);
        }
        return;
    }
    if (icon == FeatherIcon::Image) {
        draw->AddRect(p(3, 5), p(21, 19), color, r, 0, t);
        draw->AddCircle(p(8, 10), 1.6f * s, color, 16, t);
        draw->AddLine(p(4, 18), p(10, 13), color, t);
        draw->AddLine(p(10, 13), p(14, 16), color, t);
        draw->AddLine(p(14, 16), p(17, 13), color, t);
        draw->AddLine(p(17, 13), p(21, 18), color, t);
        return;
    }
    if (icon == FeatherIcon::Archive) {
        draw->AddRect(p(3, 5), p(21, 8), color, r, 0, t);
        draw->AddRect(p(5, 8), p(19, 21), color, r, 0, t);
        draw->AddLine(p(10, 12), p(14, 12), color, t);
        return;
    }
    if (icon == FeatherIcon::Code) {
        draw->AddLine(p(10, 8), p(6, 12), color, t);
        draw->AddLine(p(6, 12), p(10, 16), color, t);
        draw->AddLine(p(14, 8), p(18, 12), color, t);
        draw->AddLine(p(18, 12), p(14, 16), color, t);
        return;
    }
    if (icon == FeatherIcon::Music) {
        draw->AddLine(p(9, 18), p(9, 5), color, t);
        draw->AddLine(p(9, 5), p(19, 3), color, t);
        draw->AddLine(p(19, 3), p(19, 15), color, t);
        draw->AddCircle(p(7, 18), 2.4f * s, color, 18, t);
        draw->AddCircle(p(17, 15), 2.4f * s, color, 18, t);
        return;
    }
    if (icon == FeatherIcon::Video) {
        draw->AddRect(p(3, 7), p(15, 17), color, r, 0, t);
        draw->AddLine(p(15, 11), p(21, 7), color, t);
        draw->AddLine(p(21, 7), p(21, 17), color, t);
        draw->AddLine(p(21, 17), p(15, 13), color, t);
        return;
    }
    if (icon == FeatherIcon::Database) {
        arc(p(12, 6), 6.0f * s, 0.0f, 6.28318f);
        draw->AddLine(p(5, 5), p(5, 18), color, t);
        draw->AddLine(p(19, 5), p(19, 18), color, t);
        arc(p(12, 18), 6.0f * s, 0.0f, 6.28318f);
        draw->AddLine(p(6, 12), p(18, 12), color, t);
        return;
    }
    if (icon == FeatherIcon::Message) {
        draw->AddRect(p(3, 5), p(21, 17), color, r, 0, t);
        draw->AddLine(p(8, 17), p(5, 21), color, t);
        draw->AddLine(p(8, 17), p(12, 17), color, t);
        return;
    }
    if (icon == FeatherIcon::Github) {
        draw->AddCircle(p(12, 12), 8.5f * s, color, 28, t);
        draw->AddLine(p(8, 16), p(8, 20), color, t);
        draw->AddLine(p(16, 16), p(16, 20), color, t);
        draw->AddLine(p(9, 9), p(7, 6), color, t);
        draw->AddLine(p(15, 9), p(17, 6), color, t);
        return;
    }
    if (icon == FeatherIcon::Wifi) {
        arc(p(12, 15), 9.0f * s, 3.75f, 5.67f);
        arc(p(12, 15), 5.5f * s, 3.75f, 5.67f);
        draw->AddCircleFilled(p(12, 18), 1.6f * s, color);
        return;
    }
    if (icon == FeatherIcon::Zap) {
        draw->AddLine(p(13, 2), p(5, 14), color, t);
        draw->AddLine(p(5, 14), p(13, 14), color, t);
        draw->AddLine(p(13, 14), p(11, 22), color, t);
        draw->AddLine(p(11, 22), p(19, 10), color, t);
        draw->AddLine(p(19, 10), p(11, 10), color, t);
        draw->AddLine(p(11, 10), p(13, 2), color, t);
        return;
    }
    if (icon == FeatherIcon::Send) {
        draw->AddLine(p(3, 11), p(21, 3), color, t);
        draw->AddLine(p(21, 3), p(14, 21), color, t);
        draw->AddLine(p(14, 21), p(11, 13), color, t);
        draw->AddLine(p(11, 13), p(3, 11), color, t);
        draw->AddLine(p(11, 13), p(21, 3), color, t);
        return;
    }
    if (icon == FeatherIcon::Users) {
        draw->AddCircle(p(9, 8), 3.0f * s, color, 18, t);
        draw->AddCircle(p(17, 9), 2.4f * s, color, 18, t);
        arc(p(9, 19), 6.0f * s, 3.55f, 5.87f);
        arc(p(17, 19), 4.5f * s, 3.55f, 5.87f);
    }
}

void section_label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.16f, 0.25f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const float x = ImGui::GetCursorPosX();
    const float y = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.55f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(ImGui::GetWindowPos().x + x + 8.0f, y),
                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 18.0f, y),
                  u32(219, 226, 236), 1.0f);
    ImGui::NewLine();
}

void status_badge(const char* text, bool active) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.85f, 0.97f, 0.91f, 1.0f) : ImVec4(0.94f, 0.95f, 0.97f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ImVec4(0.78f, 0.94f, 0.86f, 1.0f) : ImVec4(0.91f, 0.93f, 0.96f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? ImVec4(0.72f, 0.90f, 0.81f, 1.0f) : ImVec4(0.88f, 0.91f, 0.95f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.04f, 0.42f, 0.24f, 1.0f) : ImVec4(0.37f, 0.43f, 0.52f, 1.0f));
    ImGui::Button(text, ImVec2(94, 0));
    ImGui::PopStyleColor(4);
}

bool primary_button(const char* text, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.34f, 0.74f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.06f, 0.41f, 0.86f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.04f, 0.27f, 0.62f, 1.0f));
    const bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool secondary_button(const char* text, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.94f, 0.96f, 0.99f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.89f, 0.93f, 0.99f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.89f, 0.97f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.11f, 0.17f, 0.26f, 1.0f));
    const bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(4);
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
    set_status(state, "已选择 " + std::to_string(state.files.size()) + " 个文件");
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

std::vector<std::string> broadcast_targets(const std::vector<std::string>& local_ips) {
    std::vector<std::string> targets;
    targets.push_back("127.0.0.1");
    targets.push_back("255.255.255.255");
    for (const auto& ip : local_ips) {
        const auto dot = ip.find_last_of('.');
        if (dot == std::string::npos || ip == "127.0.0.1") {
            continue;
        }
        const auto broadcast = ip.substr(0, dot + 1) + "255";
        if (std::find(targets.begin(), targets.end(), broadcast) == targets.end()) {
            targets.push_back(broadcast);
        }
    }
    return targets;
}

void start_discovery(GuiState& state) {
    state.stop_discovery = false;
    state.discovering = true;
    upsert_peer(state, "127.0.0.1", "本机");
    state.discovery = std::thread([&state] {
        const auto targets = broadcast_targets(state.local_ips);
        while (!state.stop_discovery.load()) {
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
            for (int i = 0; i < 12 && !state.stop_discovery.load(); ++i) {
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
    set_speed_status(state, "测速中...");
    std::thread([&state, host] {
        UdpSocket sock;
        if (!sock.valid() || !sock.set_timeout(120)) {
            set_speed_status(state, "测速失败");
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
    set_status(state, "准备发送 " + std::to_string(files.size()) + " 个文件");
    std::thread([&state, files, host, options] {
        const std::size_t total_files = files.size();
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto path = files[i].path;
            set_status(state, "发送中: " + file_name_only(path));
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
                set_status(state, "发送失败: " + file_name_only(path));
                state.sending = false;
                return;
            }
            state.transfer_progress.store(static_cast<float>(i + 1) / static_cast<float>(total_files));
        }
        set_status(state, "发送完成");
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
    draw_feather_icon(icon, ImVec2(pos.x, pos.y + 1.0f), 18.0f, u32(38, 64, 102));
    ImGui::Dummy(ImVec2(24.0f, 20.0f));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.14f, 0.22f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

std::string peer_combo_label(const PeerInfo& peer) {
    if (peer.label.empty()) {
        return peer.ip;
    }
    return peer.ip + "  " + peer.label;
}

void draw_left_panel(GuiState& state) {
    panel_title(FeatherIcon::Wifi, "局域网主机");
    const auto peers = peer_snapshot(state);
    const int peer_count = static_cast<int>(peers.size());
    if (state.selected_peer >= peer_count) {
        state.selected_peer = std::max(0, peer_count - 1);
    }
    const std::string current = peers.empty() ? "127.0.0.1  本机" : peer_combo_label(peers[static_cast<std::size_t>(state.selected_peer)]);
    ImGui::TextDisabled("选择 IP");
    if (ImGui::BeginCombo("##peer_combo", current.c_str())) {
        for (int i = 0; i < peer_count; ++i) {
            const bool selected = state.selected_peer == i;
            const auto label = peer_combo_label(peers[static_cast<std::size_t>(i)]);
            if (ImGui::Selectable(label.c_str(), selected)) {
                state.selected_peer = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("固定端口  %u/udp", static_cast<unsigned>(kFixedUdpPort));
    ImGui::TextDisabled("%s", state.discovering.load() ? "正在自动发现 TTrans 主机" : "发现服务已停止");

    ImGui::Dummy(ImVec2(1, 10));
    panel_title(FeatherIcon::Users, "本机 IP");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
    ImGui::BeginChild("local_ips", ImVec2(0, 104), true);
    for (const auto& ip : state.local_ips) {
        ImGui::BulletText("%s", ip.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(1, 10));
    panel_title(FeatherIcon::Zap, "UDP 传输测速");
    ImGui::TextDisabled("目标 %s", selected_peer_ip(state).c_str());
    if (state.speed_testing.load()) {
        ImGui::BeginDisabled();
    }
    if (secondary_button("开始测速", ImVec2(-1, 34))) {
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
        section_label("Incoming file");
        ImGui::TextWrapped("%s", pending.filename.c_str());
        ImGui::TextDisabled("%s from %s:%u", human_size(pending.file_size).c_str(), pending.peer_host.c_str(), pending.peer_port);
        ImGui::TextDisabled("Checksum %s", pending.checksum.c_str());
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
    const ImVec2 row_pos = ImGui::GetCursorScreenPos();
    draw_feather_icon(icon_for_file(path), ImVec2(row_pos.x + 6.0f, row_pos.y + 7.0f), 28.0f, u32(42, 105, 185));
    ImGui::Dummy(ImVec2(42.0f, 42.0f));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", name.c_str());
    const auto ext = file_extension(path);
    if (ext.empty()) {
        ImGui::TextDisabled("%s", human_size(size).c_str());
    } else {
        ImGui::TextDisabled(".%s  %s", ext.c_str(), human_size(size).c_str());
    }
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() - 66.0f);
    ImGui::PushID(static_cast<int>(index));
    if (secondary_button("移除", ImVec2(48, 28))) {
        state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(index));
        set_status(state, "已选择 " + std::to_string(state.files.size()) + " 个文件");
    }
    ImGui::PopID();
    ImGui::Separator();
}

void draw_center_panel(GuiState& state, const TransferOptions& options) {
    panel_title(FeatherIcon::FileText, "文件传输");
    if (secondary_button("选择文件", ImVec2(112, 34))) {
        add_files(state, open_files_dialog());
    }
    ImGui::SameLine();
    if (secondary_button("清空", ImVec2(76, 34))) {
        state.files.clear();
        state.transfer_progress.store(0.0f);
        set_status(state, "选择文件后开始传输");
    }

    ImGui::Dummy(ImVec2(1, 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, state.files.empty() ? ImVec4(0.93f, 0.97f, 1.0f, 1.0f) : ImVec4(1, 1, 1, 1));
    ImGui::BeginChild("file_queue", ImVec2(0, 328), true);
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(pos.x + ImGui::GetWindowWidth(), pos.y + ImGui::GetWindowHeight());
    ImGui::GetWindowDrawList()->AddRect(pos, max, state.files.empty() ? u32(64, 128, 210) : u32(218, 226, 236), 8.0f, 0, state.files.empty() ? 2.0f : 1.0f);
    if (state.files.empty()) {
        const ImVec2 center = ImVec2(pos.x + ImGui::GetWindowWidth() * 0.5f - 14.0f, pos.y + 74.0f);
        draw_feather_icon(FeatherIcon::File, center, 40.0f, u32(64, 128, 210));
        ImGui::SetCursorPosY(128.0f);
        ImGui::SetCursorPosX(18.0f);
        ImGui::TextWrapped("把文件拖到这里，或点击上方选择文件。支持多个文件，列表固定高度并可滚动。");
    } else {
        for (std::size_t i = 0; i < state.files.size();) {
            const std::size_t before = state.files.size();
            draw_file_row(state, i);
            if (state.files.size() == before) {
                ++i;
            }
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
    if (primary_button(state.sending.load() ? "发送中" : "发送文件", ImVec2(-1, 38))) {
        send_selected_files(state, options);
    }
    if (!can_send) {
        ImGui::EndDisabled();
    }
}

void draw_right_panel(GuiState& state) {
    panel_title(FeatherIcon::Message, "聊天对话");
    const auto peer_ip = selected_peer_ip(state);
    ImGui::TextDisabled("当前对话 %s", peer_ip.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.98f, 0.99f, 1.0f, 1.0f));
    ImGui::BeginChild("chat_history", ImVec2(0, 332), true);
    const auto messages = chat_snapshot(state);
    for (const auto& message : messages) {
        if (message.ip != peer_ip) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, message.outgoing ? ImVec4(0.04f, 0.30f, 0.66f, 1.0f) : ImVec4(0.10f, 0.15f, 0.23f, 1.0f));
        ImGui::TextWrapped("%s  %s", message.outgoing ? "我" : message.ip.c_str(), message.text.c_str());
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
    const bool clicked = secondary_button("发送", ImVec2(72, 32));
    if ((enter || clicked) && state.chat_input[0]) {
        const std::string text = state.chat_input;
        if (send_gui_packet(peer_ip, PacketType::Chat, text)) {
            add_chat(state, peer_ip, text, true);
            state.chat_input[0] = '\0';
        }
    }

    ImGui::Dummy(ImVec2(1, 12));
    panel_title(FeatherIcon::Github, "项目仓库");
    ImGui::TextWrapped("%s", kGithubUrl);
    if (secondary_button("打开 GitHub", ImVec2(-1, 34))) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        SDL_OpenURL(kGithubUrl);
#endif
    }
}

void apply_style() {
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 5.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 9);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 12.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.10f, 0.15f, 0.23f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.51f, 0.60f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.97f, 0.99f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.84f, 0.88f, 0.93f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.97f, 0.98f, 1.00f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.92f, 0.95f, 0.99f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.88f, 0.92f, 0.98f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.90f, 0.94f, 0.99f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.84f, 0.90f, 0.98f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.77f, 0.86f, 0.97f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.05f, 0.34f, 0.74f, 1.0f);
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
            return;
        }
    }
    io.Fonts->AddFontDefault();
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

void draw_header(GuiState& state) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + 58.0f), u32(20, 45, 75), 8.0f);
    draw->AddCircleFilled(ImVec2(pos.x + 28.0f, pos.y + 29.0f), 13.0f, u32(56, 210, 219));
    draw->AddText(ImVec2(pos.x + 52.0f, pos.y + 12.0f), u32(245, 248, 252), "TTrans");
    draw->AddText(ImVec2(pos.x + 52.0f, pos.y + 33.0f), u32(164, 183, 205), "LAN file transfer");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 116.0f, pos.y + 17.0f));
    status_badge(state.listening.load() ? "LISTENING" : "STOPPED", state.listening.load());
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 70.0f));
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
                                          1120,
                                          700,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_SetWindowMinimumSize(window, 980, 620);
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
    state.local_ips = collect_local_ips();
    for (const auto& ip : state.local_ips) {
        upsert_peer(state, ip, ip == "127.0.0.1" ? "本机" : "本机 IP");
    }
    state.log.add("TTrans native GUI ready.");
    start_listener(state, options);
    start_discovery(state);

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
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("TTrans", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);

        draw_header(state);
        draw_incoming_popup(state);

        const float full_width = ImGui::GetContentRegionAvail().x;
        const float full_height = ImGui::GetContentRegionAvail().y;
        const float gap = 12.0f * dpi_scale;
        const float left_width = std::min(280.0f * dpi_scale, full_width * 0.27f);
        const float right_width = std::min(330.0f * dpi_scale, full_width * 0.32f);
        const float mid_width = std::max(320.0f * dpi_scale, full_width - left_width - right_width - gap * 2.0f);

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
        glClearColor(0.95f, 0.97f, 0.99f, 1.0f);
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
