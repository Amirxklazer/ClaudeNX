#include "net.hpp"
#include <switch.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "json.hpp"

using json = nlohmann::json;

namespace net {

static const char* CA_PATH = "sdmc:/switch/ClaudeNX/cacert.pem";
static const char* PUTER_AI_URL = "https://api.puter.com/ai/chat";

bool init() {
    if (R_FAILED(socketInitializeDefault())) return false;
    nifmInitialize(NifmServiceType_User);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

void shutdown() {
    stopServer();
    curl_global_cleanup();
    nifmExit();
    socketExit();
}

std::string localIp() {
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) return "0.0.0.0";
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return b;
}

static const char* PAGE = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ClaudeNX Auth</title>
<style>
* { box-sizing: border-box; }
body {
  margin: 0; padding: 20px;
  min-height: 100vh;
  display: flex; align-items: center; justify-content: center;
  background: #0b0e17;
  color: #e8eaf0;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  font-size: 16px;
}
.card {
  width: 100%; max-width: 420px;
  padding: 40px 28px;
  border-radius: 24px;
  background: #121624;
  border: 1px solid #2a3050;
  text-align: center;
}
h1 {
  margin: 0 0 8px 0;
  font-size: 42px;
  font-weight: 600;
  letter-spacing: -0.5px;
}
.nx { color: #d97757; }
.icon {
  width: 80px; height: 80px;
  margin: 24px auto;
  border-radius: 50%;
  background: rgba(217, 119, 87, 0.1);
  display: flex;
  align-items: center; justify-content: center;
  font-size: 44px;
}
h2 {
  margin: 20px 0 8px 0;
  font-size: 20px;
  font-weight: 600;
}
p {
  margin: 0 0 24px 0;
  color: #9aa0b4;
  line-height: 1.5;
}
button {
  width: 100%;
  padding: 14px 24px;
  margin-top: 8px;
  border: 0;
  border-radius: 12px;
  background: #d97757;
  color: #fff;
  font-size: 16px;
  font-weight: 600;
  cursor: pointer;
  transition: opacity 0.2s;
}
button:hover:not(:disabled) { opacity: 0.9; }
button:disabled { opacity: 0.5; cursor: not-allowed; }
.ok { color: #4ade80; }
.err { color: #ef4444; }
.spinner {
  display: inline-block;
  width: 4px; height: 4px;
  background: #d97757;
  border-radius: 50%;
  animation: spin 1s infinite;
}
@keyframes spin {
  0%, 100% { opacity: 0.3; }
  50% { opacity: 1; }
}
</style></head><body>
<div class="card">
  <h1>Claude<span class="nx">NX</span></h1>
  <div class="icon" id="icon">🔐</div>
  <h2 id="title">Sign in to Puter</h2>
  <p id="msg">You'll need a Puter account. Click below to sign in.</p>
  <button id="btn" onclick="signIn()">Open Puter Sign In</button>
</div>
<script>
const k = new URLSearchParams(location.search).get('k') || '';
const $ = (id) => document.getElementById(id);

function safe_uuid() {
  try {
    if (crypto.randomUUID) return crypto.randomUUID();
  } catch(e) {}
  var b = new Uint8Array(16);
  crypto.getRandomValues(b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  var h = '';
  for (var i = 0; i < 16; i++) {
    h += ('0' + b[i].toString(16)).slice(-2);
  }
  return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
}

async function signIn() {
  $('btn').disabled = true;
  $('title').textContent = 'Opening Puter...';
  $('msg').textContent = 'Please sign in with your Puter account.';
  $('icon').innerHTML = '<span class="spinner"></span>';
  
  try {
    await puter.auth.signIn();
    const token = puter.authToken;
    
    if (!token) {
      throw new Error('No auth token received from Puter');
    }
    
    $('title').textContent = 'Sending token...';
    const resp = await fetch('/token?k=' + encodeURIComponent(k), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token: token })
    });
    
    if (!resp.ok) {
      throw new Error('Switch rejected the token. Rescan the QR code.');
    }
    
    $('icon').textContent = '✓';
    $('title').textContent = 'Success!';
    $('title').className = 'ok';
    $('msg').textContent = 'Token sent to Switch. App is starting...';
    $('btn').style.display = 'none';
  } catch (e) {
    $('icon').textContent = '✗';
    $('title').textContent = 'Error';
    $('title').className = 'err';
    $('msg').textContent = (e && e.message) ? e.message : String(e);
    $('btn').disabled = false;
    $('btn').textContent = 'Try again';
  }
}
</script>
<script src="https://js.puter.com/v2/"></script>
</body></html>)HTML";

static std::atomic<bool> g_run{false};
static int g_srv = -1;
static Thread g_thr;
static bool g_thrValid = false;
static std::string g_key;
static std::function<void(const std::string&)> g_cb;

static void sendAll(int fd, const std::string& s) {
    size_t o = 0;
    while (o < s.size()) {
        ssize_t n = send(fd, s.data() + o, s.size() - o, 0);
        if (n <= 0) break;
        o += (size_t)n;
    }
}

static void reply(int fd, int code, const char* ctype, const std::string& body) {
    std::string status = (code == 200) ? "OK" : "Error";
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status +
                    "\r\nContent-Type: " + ctype +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    sendAll(fd, h + body);
}

static std::string queryParam(const std::string& q, const std::string& name) {
    std::string pat = name + "=";
    size_t p = 0;
    while ((p = q.find(pat, p)) != std::string::npos) {
        if (p == 0 || q[p - 1] == '&') {
            size_t e = q.find('&', p);
            return q.substr(p + pat.size(), e == std::string::npos ? std::string::npos : e - p - pat.size());
        }
        p++;
    }
    return "";
}

static void handle(int fd) {
    std::string req;
    char buf[2048];
    size_t hdrEnd = std::string::npos, need = 0;
    while (req.size() < 65536) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        if (hdrEnd == std::string::npos) {
            hdrEnd = req.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                std::string low = req.substr(0, hdrEnd);
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                size_t p = low.find("content-length:"), cl = 0;
                if (p != std::string::npos) cl = strtoul(low.c_str() + p + 15, nullptr, 10);
                need = hdrEnd + 4 + cl;
            }
        }
        if (hdrEnd != std::string::npos && req.size() >= need) break;
    }
    if (hdrEnd == std::string::npos) return;

    size_t s1 = req.find(' '), s2 = req.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return;
    std::string method = req.substr(0, s1), target = req.substr(s1 + 1, s2 - s1 - 1);
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }
    std::string body = req.substr(hdrEnd + 4);

    if (method == "GET" && path == "/") {
        reply(fd, 200, "text/html; charset=utf-8", PAGE);
        return;
    }

    if (method == "POST" && path == "/token") {
        if (queryParam(query, "k") != g_key) {
            reply(fd, 403, "application/json", "{\"ok\":false,\"error\":\"key mismatch\"}");
            return;
        }
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("token") || !j["token"].is_string()) {
            reply(fd, 400, "application/json", "{\"ok\":false,\"error\":\"no token\"}");
            return;
        }
        std::string tok = j["token"].get<std::string>();
        if (tok.empty()) {
            reply(fd, 400, "application/json", "{\"ok\":false,\"error\":\"empty token\"}");
            return;
        }
        if (g_cb) g_cb(tok);
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    reply(fd, 404, "text/plain", "not found");
}

static void serverThread(void*) {
    while (g_run) {
        sockaddr_in ca;
        socklen_t l = sizeof ca;
        int c = accept(g_srv, (sockaddr*)&ca, &l);
        if (c < 0) { svcSleepThread(50000000ULL); continue; }
        int fl = fcntl(c, F_GETFL, 0);
        fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
        timeval tv{3, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c);
        close(c);
    }
}

bool startServer(const std::string& key, std::function<void(const std::string&)> cb) {
    if (g_run) { g_key = key; g_cb = cb; return true; }
    g_key = key; g_cb = cb;
    g_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (g_srv < 0) return false;
    int one = 1;
    setsockopt(g_srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(8080);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_srv, (sockaddr*)&a, sizeof a) < 0 || listen(g_srv, 4) < 0) {
        close(g_srv); g_srv = -1; return false;
    }
    fcntl(g_srv, F_SETFL, fcntl(g_srv, F_GETFL, 0) | O_NONBLOCK);
    g_run = true;
    if (R_FAILED(threadCreate(&g_thr, serverThread, nullptr, nullptr, 0x80000, 0x2B, -2)) ||
        R_FAILED(threadStart(&g_thr))) {
        g_run = false; close(g_srv); g_srv = -1; return false;
    }
    g_thrValid = true;
    return true;
}

void stopServer() {
    if (!g_run) return;
    g_run = false;
    if (g_thrValid) { threadWaitForExit(&g_thr); threadClose(&g_thr); g_thrValid = false; }
    if (g_srv >= 0) { close(g_srv); g_srv = -1; }
}

bool serverRunning() { return g_run; }

static size_t writeCb(char* p, size_t s, size_t n, void* u) {
    ((std::string*)u)->append(p, s * n);
    return s * n;
}

bool chat(const std::string& token, const std::string& model, const std::string& system,
          const std::vector<ChatMsg>& hist, std::string& out, std::string& err) {
    json msgs = json::array();
    msgs.push_back({{"role", "system"}, {"content", system}});
    for (auto& m : hist) {
        msgs.push_back({{"role", m.role}, {"content", m.text}});
    }
    
    json req = {{"messages", msgs}};
    std::string body = req.dump();

    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return false; }
    std::string resp;
    curl_slist* h = nullptr;
    h = curl_slist_append(h, ("Authorization: Bearer " + token).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    
    curl_easy_setopt(c, CURLOPT_URL, PUTER_AI_URL);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    
    if (FILE* f = fopen(CA_PATH, "rb")) {
        fclose(f);
        curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    
    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        err = std::string("Network: ") + curl_easy_strerror(rc);
        return false;
    }

    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) {
        err = "Invalid JSON response";
        return false;
    }
    
    // Try different response formats
    if (j.contains("result") && j["result"].is_string()) {
        out = j["result"].get<std::string>();
        return true;
    }
    if (j.contains("response") && j["response"].is_string()) {
        out = j["response"].get<std::string>();
        return true;
    }
    if (j.contains("message") && j["message"].is_string()) {
        out = j["message"].get<std::string>();
        return true;
    }
    if (j.contains("choices") && j["choices"].is_array() && j["choices"].size() > 0) {
        auto& choice = j["choices"][0];
        if (choice.contains("message") && choice["message"].is_object()) {
            auto& m = choice["message"];
            if (m.contains("content") && m["content"].is_string()) {
                out = m["content"].get<std::string>();
                return true;
            }
        }
        if (choice.contains("text") && choice["text"].is_string()) {
            out = choice["text"].get<std::string>();
            return true;
        }
    }
    
    // Error handling
    if (j.contains("error")) {
        auto& e = j["error"];
        if (e.is_string()) err = e.get<std::string>();
        else if (e.is_object() && e.contains("message")) err = e["message"].dump();
    }
    
    if (err.empty()) {
        err = "HTTP " + std::to_string(http) + ": ";
        if (resp.size() > 200) err += resp.substr(0, 200) + "...";
        else err += resp;
    }
    return false;
}

}  // namespace net
