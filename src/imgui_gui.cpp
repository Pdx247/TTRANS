#include "ttrans.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#ifndef SDL_HINT_WINDOWS_DPI_AWARENESS
#define SDL_HINT_WINDOWS_DPI_AWARENESS "SDL_WINDOWS_DPI_AWARENESS"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ttrans {
namespace {

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
    char target_host[128] = "127.0.0.1";
    char file_path[1024] = "";
    char output_dir[1024] = "downloads";
    int udp_port = 44777;
    std::atomic_bool sending{false};
    std::atomic_bool listening{false};
    std::atomic_bool stop_listener{false};
    SharedLog log;
    ReceivePrompt prompt;
    std::thread listener;
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

std::string file_name_only(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

uint64_t file_size_or_zero(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return 0;
    }
    in.seekg(0, std::ios::end);
    return static_cast<uint64_t>(in.tellg());
}

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
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string data(4096, '\0');
    in.read(&data[0], static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));
    for (char& ch : data) {
        if (ch == '\0') {
            ch = ' ';
        }
    }
    return data;
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
        receive_forever(port, output_dir, options, accept, stop, log);
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

void draw_send_panel(GuiState& state, const TransferOptions& options) {
    section_label("Send");
    ImGui::InputText("Target IP", state.target_host, sizeof(state.target_host));
    ImGui::InputInt("UDP Port", &state.udp_port);
    state.udp_port = std::max(1, std::min(65535, state.udp_port));
    ImGui::InputText("File path", state.file_path, sizeof(state.file_path));

    const bool can_send = state.file_path[0] != '\0' && state.target_host[0] != '\0' && !state.sending.load();
    if (!can_send) {
        ImGui::BeginDisabled();
    }
    if (primary_button(state.sending.load() ? "Sending" : "Send File", ImVec2(-1, 36))) {
        const std::string host = state.target_host;
        const std::string file = state.file_path;
        const uint16_t port = static_cast<uint16_t>(state.udp_port);
        state.sending = true;
        std::thread([&state, host, file, port, options] {
            auto log = [&state](const std::string& line) {
                state.log.add(line);
            };
            send_file(host, port, file, options, log);
            state.sending = false;
        }).detach();
    }
    if (!can_send) {
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(1, 8));
    section_label("Preview");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.98f, 0.99f, 1.0f, 1.0f));
    ImGui::BeginChild("preview", ImVec2(0, 176), true);
    if (state.file_path[0]) {
        const std::string path = state.file_path;
        ImGui::TextWrapped("%s", file_name_only(path).c_str());
        ImGui::TextDisabled("%s", human_size(file_size_or_zero(path)).c_str());
        const auto preview = text_preview(path);
        if (!preview.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(preview.c_str());
        }
    } else {
        ImGui::TextDisabled("Drop a file here or paste a path.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_receive_panel(GuiState& state, const TransferOptions& options) {
    section_label("Receive");
    ImGui::InputText("Output dir", state.output_dir, sizeof(state.output_dir));
    ImGui::TextUnformatted("Listener");
    ImGui::SameLine();
    status_badge(state.listening.load() ? "ACTIVE" : "STOPPED", state.listening.load());
    if (secondary_button("Restart Listener", ImVec2(-1, 34))) {
        start_listener(state, options);
    }

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

void draw_log_panel(GuiState& state) {
    section_label("Activity");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.88f, 0.96f, 1.0f));
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const auto lines = state.log.snapshot();
    for (const auto& line : lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
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
    const char* font_path = "C:/Windows/Fonts/segoeui.ttf";
#elif __APPLE__
    const char* font_path = "/System/Library/Fonts/SFNS.ttf";
#else
    const char* font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    ImFontConfig config;
    config.OversampleH = 3;
    config.OversampleV = 2;
    config.PixelSnapH = true;
    if (!io.Fonts->AddFontFromFileTTF(font_path, size, &config)) {
        io.Fonts->AddFontDefault();
    }
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
                                          980,
                                          620,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }

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
    state.udp_port = udp_port;
    copy_to_buffer(state.output_dir, sizeof(state.output_dir), output_dir.empty() ? "downloads" : output_dir);
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
                copy_to_buffer(state.file_path, sizeof(state.file_path), event.drop.file);
                state.log.add("Selected " + std::string(event.drop.file));
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

        const float full_width = ImGui::GetContentRegionAvail().x;
        const float full_height = ImGui::GetContentRegionAvail().y;
        const float gap = 12.0f * dpi_scale;
        const float left_width = std::max(300.0f * dpi_scale, full_width * 0.34f);
        const float right_width = std::max(280.0f * dpi_scale, full_width * 0.30f);
        const float mid_width = std::max(240.0f * dpi_scale, full_width - left_width - right_width - gap * 2.0f);

        ImGui::BeginChild("send_panel", ImVec2(left_width, full_height), true);
        draw_send_panel(state, options);
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("receive_panel", ImVec2(mid_width, full_height), true);
        draw_receive_panel(state, options);
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("activity_panel", ImVec2(0, full_height), true);
        draw_log_panel(state);
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
