#include "ttrans.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
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
    ImGui::TextUnformatted("Send");
    ImGui::Separator();
    ImGui::InputText("Target IP", state.target_host, sizeof(state.target_host));
    ImGui::InputInt("UDP Port", &state.udp_port);
    state.udp_port = std::max(1, std::min(65535, state.udp_port));
    ImGui::InputText("File path", state.file_path, sizeof(state.file_path));
    ImGui::TextDisabled("Tip: drag a file onto this window.");

    const bool can_send = state.file_path[0] != '\0' && state.target_host[0] != '\0' && !state.sending.load();
    if (!can_send) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send File", ImVec2(-1, 0))) {
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

    ImGui::Spacing();
    ImGui::TextUnformatted("Preview");
    ImGui::BeginChild("preview", ImVec2(0, 160), true);
    if (state.file_path[0]) {
        const std::string path = state.file_path;
        ImGui::TextWrapped("Name: %s", file_name_only(path).c_str());
        ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(file_size_or_zero(path)));
        const auto preview = text_preview(path);
        if (!preview.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(preview.c_str());
        }
    } else {
        ImGui::TextDisabled("No file selected.");
    }
    ImGui::EndChild();
}

void draw_receive_panel(GuiState& state, const TransferOptions& options) {
    ImGui::TextUnformatted("Receive");
    ImGui::Separator();
    ImGui::InputText("Output dir", state.output_dir, sizeof(state.output_dir));
    ImGui::Text("Status: %s", state.listening.load() ? "listening" : "stopped");
    if (ImGui::Button("Restart Listener", ImVec2(-1, 0))) {
        start_listener(state, options);
    }
    ImGui::TextWrapped("The app keeps listening while this window is open.");

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
        ImGui::TextWrapped("Accept this file?");
        ImGui::Separator();
        ImGui::Text("Name: %s", pending.filename.c_str());
        ImGui::Text("From: %s:%u", pending.peer_host.c_str(), pending.peer_port);
        ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(pending.file_size));
        ImGui::Text("Checksum: %s", pending.checksum.c_str());
        if (ImGui::Button("Accept", ImVec2(120, 0))) {
            {
                std::lock_guard<std::mutex> lock(state.prompt.mutex);
                state.prompt.accepted = true;
                state.prompt.decided = true;
            }
            state.prompt.cv.notify_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject", ImVec2(120, 0))) {
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
    ImGui::TextUnformatted("Log");
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const auto lines = state.log.snapshot();
    for (const auto& line : lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void apply_style() {
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 7);
}

} // namespace

int run_imgui_gui(uint16_t udp_port, const std::string& output_dir, const TransferOptions& options) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("TTrans",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          780,
                                          520,
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

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

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

        ImGui::Columns(2, "main_columns", true);
        draw_send_panel(state, options);
        ImGui::NextColumn();
        draw_receive_panel(state, options);
        ImGui::Spacing();
        draw_log_panel(state);
        ImGui::Columns(1);
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.94f, 0.93f, 0.89f, 1.0f);
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
