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
static const char* PUTER_URL = "https://api.puter.com/puterai/openai/v1/chat/completions";

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
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ClaudeNX</title>
<style>
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#0b0e17;color:#e8eaf0;font-family:system-ui,sans-serif}
.card{width:min(92vw,420px);padding:36px 24px;border-radius:28px;background:#121624;border:1px solid #2a3050;text-align:center}
h1{margin:0;font-size:40px}h1 b{color:#d97757}
.ic{width:84px;height:84px;margin:18px auto;border-radius:50%;background:#2a1a14;display:flex;align-items:center;justify-content:center;font-size:42px;color:#d97757}
h2{margin:8px 0}p{color:#9aa0b4;line-height:1.5}
button{margin-top:14px;padding:14px 28px;border:0;border-radius:14px;background:#d97757;color:#fff;font-size:18px;font-weight:600}
button:disabled{opacity:.5}.ok{color:#2fbf8f}.err{color:#e86e6e}
</style></head><body><div class="card">
<h1>Claude<b>NX</b></h1><div class="ic" id="ic">&#10022;</div>
<h2 id="t">Sign in</h2><p id="m">Connect your Puter account to your Switch.</p>
<button id="b" onclick="go()">Sign in with Puter</button>
</div>
<script src="https://js.puter.com/v2/"></script>
<script>
const k=new URLSearchParams(location.search).get('k')||'';
const $=i=>document.getElementById(i);
async function go(){
  $('b').disabled=true;$('t').className='';$('m').textContent='Opening Puter sign-in...';
  try{
    await puter.auth.signIn();
    const u=await puter.auth.getUser();
    const r=await fetch('/token?k='+encodeURIComponent(k),{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({token:puter.authToken,user:u.username})});
    if(!r.ok) throw new Error('Switch refused - rescan the QR code');
    $('ic').textContent='✓';$('t').textContent='All set';$('t').className='ok';
    $('m').textContent='Your Puter token was sent to your Switch. The app is starting on your screen.';
    $('b').style.display='none';
  }catch(e){
    $('t').textContent='Failed';$('t').className='err';
    $('m').textContent=String((e&&e.message)||e);$('b').disabled=false;
  }
}
</script></body></html>)HTML";

static std::atomic<bool> g_run{false};
static int g_srv = -1;
static Thread g_thr;
static bool g_thrValid = false;
static std::string g_key;
static std::function<void(const std::string&, const std::string&)> g_cb;

static void sendAll(int fd, const std::string& s) {
    size_t o = 0;
    while (o < s.size()) {
        ssize_t n = send(fd, s.data() + o, s.size() - o, 0);
        if (n <= 0) break;
        o += (size_t)n;
    }
}

static void reply(int fd, int code, const char* ctype, const std::string& body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                    "\r\nContent-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
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

    if (method == "GET" && path == "/") { reply(fd, 200, "text/html; charset=utf-8", PAGE); return; }

    if (method == "POST" && path == "/token") {
        if (queryParam(query, "k") != g_key) { reply(fd, 403, "application/json", "{\"ok\":false}"); return; }
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("token") || !j["token"].is_string() || j["token"].get<std::string>().empty()) {
            reply(fd, 400, "application/json", "{\"ok\":false}");
            return;
        }
        std::string user = (j.contains("user") && j["user"].is_string()) ? j["user"].get<std::string>() : "";
        if (g_cb) g_cb(j["token"].get<std::string>(), user);
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

bool startServer(const std::string& key, std::function<void(const std::string&, const std::string&)> cb) {
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
    if (bind(g_srv, (sockaddr*)&a, sizeof a) < 0 || listen(g_srv, 4) < 0) { close(g_srv); g_srv = -1; return false; }
    fcntl(g_srv, F_SETFL, fcntl(g_srv, F_GETFL, 0) | O_NONBLOCK);
    g_run = true;
    if (R_FAILED(threadCreate(&g_thr, serverThread, nullptr, nullptr, 0x80000, 0x2B, -2)) ||
        R_FAILED(threadStart(&g_thr))) { g_run = false; close(g_srv); g_srv = -1; return false; }
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
    for (auto& m : hist) msgs.push_back({{"role", m.role}, {"content", m.text}});
    json req = {{"model", model}, {"messages", msgs}, {"stream", false}};
    std::string body = req.dump(-1, ' ', false, json::error_handler_t::replace);

    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return false; }
    std::string resp;
    curl_slist* h = nullptr;
    h = curl_slist_append(h, ("Authorization: Bearer " + token).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, PUTER_URL);
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

    if (rc != CURLE_OK) { err = std::string("Network error: ") + curl_easy_strerror(rc); return false; }

    json j = json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& m = j["choices"][0]["message"];
            if (m.contains("content")) {
                if (m["content"].is_string()) { out = m["content"].get<std::string>(); return true; }
                if (m["content"].is_array()) {
                    for (auto& part : m["content"])
                        if (part.contains("text") && part["text"].is_string()) out += part["text"].get<std::string>();
                    if (!out.empty()) return true;
                }
            }
        }
        if (j.contains("error")) {
            auto& e = j["error"];
            if (e.is_string()) err = e.get<std::string>();
            else if (e.is_object() && e.contains("message") && e["message"].is_string()) err = e["message"].get<std::string>();
        }
    }
    if (err.empty()) err = "HTTP " + std::to_string(http) + ": " + resp.substr(0, 200);
    return false;
}

}  // namespace net
