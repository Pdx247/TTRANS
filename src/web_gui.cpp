#include "ttrans.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <thread>
#endif

namespace ttrans {
namespace {

#ifdef _WIN32
using TcpSocket = SOCKET;
constexpr TcpSocket kInvalidTcpSocket = INVALID_SOCKET;
#else
using TcpSocket = int;
constexpr TcpSocket kInvalidTcpSocket = -1;
#endif

std::vector<std::string> g_logs;
std::atomic_bool g_busy{false};

#ifdef _WIN32
CRITICAL_SECTION& log_section() {
    static CRITICAL_SECTION section;
    static bool initialized = [] {
        InitializeCriticalSection(&section);
        return true;
    }();
    (void)initialized;
    return section;
}

struct LogGuard {
    LogGuard() { EnterCriticalSection(&log_section()); }
    ~LogGuard() { LeaveCriticalSection(&log_section()); }
};

template <typename T>
void spawn_detached(T* task, DWORD (WINAPI *entry)(LPVOID)) {
    HANDLE handle = CreateThread(nullptr, 0, entry, task, 0, nullptr);
    if (handle) {
        CloseHandle(handle);
    } else {
        delete task;
        g_busy = false;
    }
}
#else
std::mutex g_log_mutex;

struct LogGuard {
    std::lock_guard<std::mutex> lock;
    LogGuard() : lock(g_log_mutex) {}
};
#endif

void close_tcp(TcpSocket sock) {
    if (sock == kInvalidTcpSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

void gui_log(const std::string& line) {
    LogGuard lock;
    if (g_logs.size() > 300) {
        g_logs.erase(g_logs.begin(), g_logs.begin() + 80);
    }
    g_logs.push_back(line);
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string logs_json() {
    LogGuard lock;
    std::string out = "{\"busy\":";
    out += g_busy ? "true" : "false";
    out += ",\"logs\":[";
    for (std::size_t i = 0; i < g_logs.size(); ++i) {
        if (i) {
            out += ',';
        }
        out += '"' + json_escape(g_logs[i]) + '"';
    }
    out += "]}";
    return out;
}

std::string lower(std::string text) {
    for (auto& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

bool recv_request(TcpSocket client, HttpRequest& request) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const auto n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            return false;
        }
        data.append(buffer, buffer + n);
        if (data.size() > 1024 * 1024) {
            return false;
        }
    }

    std::istringstream head(data.substr(0, header_end));
    std::string line;
    if (!std::getline(head, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream first(line);
    first >> request.method >> request.path;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
        request.headers[key] = value;
    }

    std::size_t content_length = 0;
    auto it = request.headers.find("content-length");
    if (it != request.headers.end()) {
        content_length = static_cast<std::size_t>(std::stoull(it->second));
    }
    request.body = data.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const auto n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            return false;
        }
        request.body.append(buffer, buffer + n);
    }
    if (request.body.size() > content_length) {
        request.body.resize(content_length);
    }
    return true;
}

bool send_all(TcpSocket client, const std::string& data) {
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const auto n = send(client, p, static_cast<int>(left), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

void respond(TcpSocket client, const std::string& body, const std::string& type = "text/plain", int code = 200) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error") << "\r\n"
        << "Content-Type: " << type << "; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    send_all(client, out.str());
}

std::string url_decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+' ) {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            const auto hex = text.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_form(const std::string& body) {
    std::map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto amp = body.find('&', start);
        const auto item = body.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const auto eq = item.find('=');
        if (eq != std::string::npos) {
            fields[url_decode(item.substr(0, eq))] = url_decode(item.substr(eq + 1));
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return fields;
}

std::string header_attr(const std::string& header, const std::string& name) {
    const auto needle = name + "=\"";
    const auto start = header.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + needle.size();
    const auto end = header.find('"', value_start);
    if (end == std::string::npos) {
        return {};
    }
    return header.substr(value_start, end - value_start);
}

struct MultipartData {
    std::map<std::string, std::string> fields;
    std::string filename;
    std::string file_bytes;
};

bool parse_multipart(const HttpRequest& request, MultipartData& result) {
    auto type_it = request.headers.find("content-type");
    if (type_it == request.headers.end()) {
        return false;
    }
    const auto boundary_pos = type_it->second.find("boundary=");
    if (boundary_pos == std::string::npos) {
        return false;
    }
    const std::string marker = "--" + type_it->second.substr(boundary_pos + 9);
    std::size_t pos = 0;
    while ((pos = request.body.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        if (request.body.compare(pos, 2, "--") == 0) {
            break;
        }
        if (request.body.compare(pos, 2, "\r\n") == 0) {
            pos += 2;
        }
        const auto header_end = request.body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) {
            break;
        }
        const auto headers = request.body.substr(pos, header_end - pos);
        auto content_start = header_end + 4;
        auto next = request.body.find(marker, content_start);
        if (next == std::string::npos) {
            break;
        }
        auto content_end = next;
        if (content_end >= 2 && request.body.compare(content_end - 2, 2, "\r\n") == 0) {
            content_end -= 2;
        }
        const auto name = header_attr(headers, "name");
        const auto filename = header_attr(headers, "filename");
        const auto content = request.body.substr(content_start, content_end - content_start);
        if (!filename.empty()) {
            result.filename = filename;
            result.file_bytes = content;
        } else if (!name.empty()) {
            result.fields[name] = content;
        }
        pos = next;
    }
    return !result.filename.empty() && !result.file_bytes.empty();
}

std::string safe_name(std::string name) {
    if (name.empty()) {
        name = "upload.bin";
    }
    for (auto& ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return name;
}

std::string temp_dir() {
#ifdef _WIN32
    const char* value = std::getenv("TEMP");
    if (!value || !*value) {
        value = std::getenv("TMP");
    }
    return value && *value ? value : ".";
#else
    const char* value = std::getenv("TMPDIR");
    return value && *value ? value : "/tmp";
#endif
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

std::string page_html() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TTrans</title>
<style>
:root { --paper:#f6f3eb; --ink:#171a1f; --line:#c8c1b4; --panel:#fffdf7; --cyan:#007f92; --amber:#c77900; --red:#b0322b; }
* { box-sizing:border-box; }
body { margin:0; min-height:100vh; background:var(--paper); color:var(--ink); font-family:Cambria, Georgia, serif; display:grid; place-items:center; padding:22px; }
.shell { width:min(780px, 100%); border:1px solid var(--ink); background:var(--panel); box-shadow:8px 8px 0 rgba(23,26,31,.12); }
header { display:flex; align-items:center; justify-content:space-between; border-bottom:1px solid var(--ink); padding:10px 12px; }
h1 { font-size:20px; margin:0; letter-spacing:0; }
.state { font:12px Consolas, monospace; color:var(--cyan); }
main { display:grid; grid-template-columns:1fr 250px; gap:0; }
section { padding:12px; }
section + section { border-left:1px solid var(--ink); }
.row { display:grid; grid-template-columns:1fr 92px; gap:8px; margin-bottom:8px; }
label { display:block; font:12px Consolas, monospace; margin-bottom:4px; color:#343941; }
input { width:100%; min-height:34px; border:1px solid var(--line); background:#fff; padding:7px 8px; color:var(--ink); font:14px Consolas, monospace; }
button { min-height:34px; border:1px solid var(--ink); background:var(--ink); color:#fff; padding:7px 10px; font:700 13px Consolas, monospace; cursor:pointer; }
button.alt { background:#fff; color:var(--ink); }
button:disabled { opacity:.45; cursor:wait; }
.actions { display:flex; gap:8px; margin:10px 0; }
.preview { height:188px; border:1px solid var(--line); background:#fff; display:grid; place-items:center; overflow:auto; padding:8px; }
.preview pre { margin:0; width:100%; height:100%; white-space:pre-wrap; word-break:break-word; font:12px Consolas, monospace; }
.preview img { max-width:100%; max-height:170px; display:block; }
.log { height:352px; overflow:auto; background:#171a1f; color:#dfead2; padding:9px; font:12px Consolas, monospace; white-space:pre-wrap; }
.hint { color:var(--amber); font:12px Consolas, monospace; margin-top:8px; min-height:16px; }
@media (max-width:720px) { main { grid-template-columns:1fr; } section + section { border-left:0; border-top:1px solid var(--ink); } .row { grid-template-columns:1fr; } }
</style>
</head>
<body>
<div class="shell">
  <header><h1>TTrans</h1><div class="state" id="state">idle</div></header>
  <main>
    <section>
      <div class="row">
        <div><label>Target IP</label><input id="host" value="127.0.0.1"></div>
        <div><label>UDP Port</label><input id="port" value="44777"></div>
      </div>
      <label>File</label><input id="file" type="file">
      <div class="actions"><button id="send">SEND</button><button class="alt" id="receive">RECEIVE</button></div>
      <div class="row">
        <div><label>Output Dir</label><input id="out" value="downloads"></div>
        <div><label>HTTP Port</label><input value="local" disabled></div>
      </div>
      <div class="preview" id="preview"><span>No file selected</span></div>
      <div class="hint" id="hint"></div>
    </section>
    <section><div class="log" id="log"></div></section>
  </main>
</div>
<script>
const $ = id => document.getElementById(id);
let busy = false;
$('file').addEventListener('change', () => {
  const file = $('file').files[0], box = $('preview');
  box.innerHTML = '';
  if (!file) { box.textContent = 'No file selected'; return; }
  if (file.type.startsWith('image/')) {
    const img = document.createElement('img'); img.src = URL.createObjectURL(file); box.appendChild(img);
  } else if (file.type.startsWith('text/') || /\.(txt|md|cpp|c|h|hpp|json|log|csv)$/i.test(file.name)) {
    const pre = document.createElement('pre'); box.appendChild(pre);
    const reader = new FileReader();
    reader.onload = () => pre.textContent = String(reader.result).slice(0, 8000);
    reader.readAsText(file);
  } else {
    box.innerHTML = `<pre>${file.name}\n${file.size} bytes\n${file.type || 'application/octet-stream'}</pre>`;
  }
});
$('send').onclick = async () => {
  const file = $('file').files[0]; if (!file || busy) return;
  const fd = new FormData(); fd.append('host', $('host').value); fd.append('port', $('port').value); fd.append('file', file);
  $('hint').textContent = 'queued send';
  await fetch('/api/send', { method:'POST', body:fd });
  poll();
};
$('receive').onclick = async () => {
  if (busy) return;
  $('hint').textContent = 'receiver armed';
  await fetch('/api/receive', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:new URLSearchParams({port:$('port').value, out:$('out').value}) });
  poll();
};
async function poll() {
  const r = await fetch('/api/logs'), j = await r.json();
  busy = j.busy; $('state').textContent = busy ? 'busy' : 'idle';
  $('send').disabled = $('receive').disabled = busy;
  $('log').textContent = j.logs.join('\n');
  $('log').scrollTop = $('log').scrollHeight;
}
setInterval(poll, 800); poll();
</script>
</body>
</html>)HTML";
}

struct SendTask {
    std::string host;
    uint16_t port;
    std::string temp;
};

struct ReceiveTask {
    uint16_t port;
    std::string out;
    TransferOptions options;
};

#ifdef _WIN32
DWORD WINAPI send_task_proc(LPVOID raw) {
    auto* task = static_cast<SendTask*>(raw);
    TransferOptions options;
    send_file(task->host, task->port, task->temp, options, gui_log);
    std::remove(task->temp.c_str());
    delete task;
    g_busy = false;
    return 0;
}

DWORD WINAPI receive_task_proc(LPVOID raw) {
    auto* task = static_cast<ReceiveTask*>(raw);
    receive_once(task->port, task->out, task->options, gui_log);
    delete task;
    g_busy = false;
    return 0;
}
#endif

void handle_send(const HttpRequest& request) {
    if (g_busy.exchange(true)) {
        gui_log("Another transfer is already running.");
        return;
    }
    MultipartData data;
    if (!parse_multipart(request, data)) {
        gui_log("Unable to parse upload.");
        g_busy = false;
        return;
    }
    const auto host = data.fields["host"].empty() ? "127.0.0.1" : data.fields["host"];
    const auto port = static_cast<uint16_t>(std::stoi(data.fields["port"].empty() ? "44777" : data.fields["port"]));
    const auto filename = safe_name(data.filename);
    auto temp = join_path(temp_dir(), "ttrans_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + filename);
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(data.file_bytes.data(), static_cast<std::streamsize>(data.file_bytes.size()));
    }
    auto* task = new SendTask{host, port, temp};
#ifdef _WIN32
    spawn_detached(task, send_task_proc);
#else
    std::thread([task] {
        TransferOptions options;
        send_file(task->host, task->port, task->temp, options, gui_log);
        std::remove(task->temp.c_str());
        delete task;
        g_busy = false;
    }).detach();
#endif
}

void handle_receive(const HttpRequest& request, const TransferOptions& options) {
    if (g_busy.exchange(true)) {
        gui_log("Another transfer is already running.");
        return;
    }
    const auto fields = parse_form(request.body);
    const auto port_it = fields.find("port");
    const auto out_it = fields.find("out");
    const auto port = static_cast<uint16_t>(std::stoi(port_it == fields.end() ? "44777" : port_it->second));
    const auto out = out_it == fields.end() || out_it->second.empty() ? std::string("downloads") : out_it->second;
    auto* task = new ReceiveTask{port, out, options};
#ifdef _WIN32
    spawn_detached(task, receive_task_proc);
#else
    std::thread([task] {
        receive_once(task->port, task->out, task->options, gui_log);
        delete task;
        g_busy = false;
    }).detach();
#endif
}

void handle_client(TcpSocket client, const TransferOptions& options) {
    HttpRequest request;
    if (!recv_request(client, request)) {
        close_tcp(client);
        return;
    }
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html")) {
        respond(client, page_html(), "text/html");
    } else if (request.method == "GET" && request.path == "/api/logs") {
        respond(client, logs_json(), "application/json");
    } else if (request.method == "POST" && request.path == "/api/send") {
        handle_send(request);
        respond(client, "{\"ok\":true}", "application/json");
    } else if (request.method == "POST" && request.path == "/api/receive") {
        handle_receive(request, options);
        respond(client, "{\"ok\":true}", "application/json");
    } else {
        respond(client, "Not found", "text/plain", 404);
    }
    close_tcp(client);
}

#ifdef _WIN32
struct ClientTask {
    TcpSocket client;
    TransferOptions options;
};

DWORD WINAPI client_task_proc(LPVOID raw) {
    auto* task = static_cast<ClientTask*>(raw);
    handle_client(task->client, task->options);
    delete task;
    return 0;
}
#endif

} // namespace

int run_web_gui(uint16_t http_port, const TransferOptions& options) {
    if (!ensure_socket_runtime()) {
        std::cerr << "Socket runtime unavailable.\n";
        return 2;
    }

    const auto server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == kInvalidTcpSocket) {
        std::cerr << "Unable to create HTTP socket.\n";
        return 2;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(http_port);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(server, 16) != 0) {
        close_tcp(server);
        std::cerr << "Unable to listen on HTTP port " << http_port << ".\n";
        return 2;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(http_port);
    gui_log("GUI ready at " + url);
    std::cout << "TTrans GUI: " << url << "\n";
    std::cout << "Press Ctrl+C to stop.\n";

    while (true) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif
        const auto client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client == kInvalidTcpSocket) {
            continue;
        }
#ifdef _WIN32
        auto* task = new ClientTask{client, options};
        HANDLE handle = CreateThread(nullptr, 0, client_task_proc, task, 0, nullptr);
        if (handle) {
            CloseHandle(handle);
        } else {
            handle_client(client, options);
        }
#else
        std::thread(handle_client, client, options).detach();
#endif
    }
}

} // namespace ttrans
