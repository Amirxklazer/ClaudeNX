#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "net.hpp"
#include "qrcodegen.hpp"

static const int W = 1280, H = 720;
static const int SIDEBAR_W = 220, CHAT_X = SIDEBAR_W;
static const int HEAD_H = 60, FOOTER_H = 60, CHAT_H = H - HEAD_H - FOOTER_H;

static SDL_Renderer* R;
static TTF_Font *fBody, *fSmall, *fTitle, *fBig;

static const SDL_Color 
    C_BG{20, 20, 18, 255},
    C_SIDEBAR{28, 28, 26, 255},
    C_PANEL{40, 40, 38, 255},
    C_INPUT{50, 50, 48, 255},
    C_TEXT{235, 235, 230, 255},
    C_DIM{140, 140, 135, 255},
    C_ACC{217, 119, 87, 255},
    C_ERR{231, 76, 60, 255},
    C_WHITE{255, 255, 255, 255};

static const char* DIR_PATH = "sdmc:/switch/ClaudeNX";
static const char* CFG_PATH = "sdmc:/switch/ClaudeNX/config.txt";
static const char* MODELS_PATH = "sdmc:/switch/ClaudeNX/models.txt";
static const char* SYSTEM_PROMPT =
    "You are Claude, running on a Nintendo Switch. Keep answers concise. "
    "Use plain text and code blocks. No tables or complex markdown.";

static void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(R, &r);
}

static void rrect(int x, int y, int w, int h, int r, SDL_Color c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    for (int i = 0; i < r; i++) {
        double dy = r - i - 0.5;
        int inset = r - (int)std::lround(std::sqrt((double)r * r - dy * dy));
        SDL_Rect a{x + inset, y + i, w - 2 * inset, 1};
        SDL_Rect b{x + inset, y + h - 1 - i, w - 2 * inset, 1};
        SDL_RenderFillRect(R, &a);
        SDL_RenderFillRect(R, &b);
    }
    SDL_Rect m{x, y + r, w, h - 2 * r};
    SDL_RenderFillRect(R, &m);
}

static void circle(int cx, int cy, int r, SDL_Color c) { 
    rrect(cx - r, cy - r, 2 * r, 2 * r, r, c); 
}

static SDL_Texture* mkText(TTF_Font* f, const std::string& s, SDL_Color c, int wrap, int* w, int* h) {
    if (s.empty()) { *w = *h = 0; return nullptr; }
    SDL_Surface* sf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), c, wrap)
                               : TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!sf) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(R, sf);
    *w = sf->w;
    *h = sf->h;
    SDL_FreeSurface(sf);
    return t;
}

static int text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int align = 0) {
    if (s.empty()) return 0;
    int w, h;
    SDL_Texture* t = mkText(f, s, c, 0, &w, &h);
    if (!t) return 0;
    SDL_Rect d{align == 1 ? x - w / 2 : x, y, w, h};
    SDL_RenderCopy(R, t, nullptr, &d);
    SDL_DestroyTexture(t);
    return w;
}

struct Block {
    bool code = false;
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

struct Msg {
    std::string role, text;
    std::vector<Block> blocks;
    bool built = false;
    int h = 0;
};

static void freeBlocks(Msg& m) {
    for (auto& b : m.blocks) if (b.tex) SDL_DestroyTexture(b.tex);
    m.blocks.clear();
    m.built = false;
}

static std::string stripMd(std::string s) {
    size_t p;
    while ((p = s.find("**")) != std::string::npos) s.erase(p, 2);
    while ((p = s.find("##")) != std::string::npos) s.erase(p, 2);
    s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
    return s;
}

static void buildBlocks(Msg& m) {
    freeBlocks(m);
    bool user = m.role == "user";
    int wrapW = W - CHAT_X - 80;
    SDL_Color col = m.role == "error" ? C_ERR : C_TEXT;
    bool inCode = false;
    std::string cur;

    auto addBlock = [&](const std::string& s, bool code) {
        if (s.empty()) return;
        Block b;
        b.code = code;
        int w, h;
        b.tex = mkText(fBody, s, code ? C_ACC : col, code ? W - CHAT_X - 40 : wrapW, &w, &h);
        if (b.tex) { b.w = w; b.h = h; m.blocks.push_back(b); }
    };
    auto flush = [&]() {
        while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) cur.pop_back();
        if (!cur.empty()) addBlock(inCode ? cur : stripMd(cur), inCode);
        cur.clear();
    };

    std::istringstream is(m.text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("```", 0) == 0) {
            flush();
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            cur += line + "\n";
        } else {
            if (line.empty()) { flush(); }
            else { cur += line + " "; }
        }
    }
    flush();
    if (m.blocks.empty()) addBlock(" ", false);
    m.built = true;
}

static void blit(SDL_Texture* t, int x, int y, int w, int h) {
    SDL_Rect d{x, y, w, h};
    SDL_RenderCopy(R, t, nullptr, &d);
}

static int drawMsg(Msg& m, int y, bool draw) {
    if (!m.built) buildBlocks(m);
    int h = 0;
    if (m.role == "user") {
        int bw = 0, bh = 0;
        for (auto& b : m.blocks) { bw = std::max(bw, b.w); bh += b.h + 6; }
        if (bh > 0) bh -= 6;
        int w = std::min(bw + 32, W - CHAT_X - 40);
        int x = W - w - 20;
        h = bh + 24;
        if (draw) {
            rrect(x, y, w, h, 16, C_ACC);
            int yy = y + 12;
            for (auto& b : m.blocks) { 
                if (b.tex) { blit(b.tex, x + 16, yy, b.w, b.h); yy += b.h + 6; }
            }
        }
        return h + 12;
    }
    // Assistant message
    int yy = y;
    if (draw && !m.blocks.empty()) circle(CHAT_X + 20, y + 10, 6, C_ACC);
    for (auto& b : m.blocks) {
        if (b.code) {
            int bh = b.h + 16;
            if (draw) {
                rrect(CHAT_X + 20, yy, W - CHAT_X - 40, bh, 8, C_PANEL);
                if (b.tex) blit(b.tex, CHAT_X + 30, yy + 8, b.w, b.h);
            }
            yy += bh + 8;
        } else {
            if (draw && b.tex) blit(b.tex, CHAT_X + 20, yy, b.w, b.h);
            yy += b.h + 6;
        }
    }
    return yy - y + 8;
}

enum Screen { PAIR, CHAT };
static Screen screen = PAIR;
static std::vector<Msg> msgs;
static std::string token, model = "claude-3-5-sonnet";
static std::vector<std::string> models{"claude-3-5-sonnet", "claude-3-opus", "gpt-4"};
static int mi = 0;
static float scrollY = 0;
static bool stick = true;

static std::mutex mu;
static bool tokenReady = false, replyReady = false, replyOk = false;
static std::string pendTok, pendReply;
static std::atomic<bool> busy{false};
static Thread chatThr;
static bool chatThrValid = false;

static std::string pairUrl;
static std::vector<std::vector<bool>> qr;

static void saveCfg() {
    mkdir(DIR_PATH, 0777);
    FILE* f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "token=%s\nmodel=%s\n", token.c_str(), model.c_str());
    fclose(f);
}

static void loadCfg() {
    std::ifstream in(MODELS_PATH);
    std::string l;
    std::vector<std::string> ms;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') ms.push_back(l);
    }
    if (!ms.empty()) models = ms;
    std::ifstream cf(CFG_PATH);
    while (std::getline(cf, l)) {
        size_t e = l.find('=');
        if (e == std::string::npos) continue;
        std::string k = l.substr(0, e), v = l.substr(e + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        if (k == "token") token = v;
        else if (k == "model") model = v;
    }
    auto it = std::find(models.begin(), models.end(), model);
    mi = it == models.end() ? 0 : (int)(it - models.begin());
    model = models[mi];
}

static void enterPair() {
    u8 rnd[4];
    randomGet(rnd, sizeof rnd);
    char key[16];
    snprintf(key, sizeof key, "%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);
    std::string ip = net::localIp();
    pairUrl = "http://" + ip + ":8080/?k=" + key;
    qr.clear();
    if (ip != "0.0.0.0") {
        auto q = qrcodegen::QrCode::encodeText(pairUrl.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        qr.assign(q.getSize(), std::vector<bool>(q.getSize()));
        for (int y = 0; y < q.getSize(); y++)
            for (int x = 0; x < q.getSize(); x++) qr[y][x] = q.getModule(x, y);
    }
    net::startServer(key, [](const std::string& t) {
        std::lock_guard<std::mutex> g(mu);
        pendTok = t;
        tokenReady = true;
    });
    screen = PAIR;
}

struct ChatJob {
    std::string tok, model;
    std::vector<ChatMsg> hist;
};

static void chatThread(void* arg) {
    ChatJob* j = (ChatJob*)arg;
    std::string out, err;
    bool ok = net::chat(j->tok, j->model, SYSTEM_PROMPT, j->hist, out, err);
    {
        std::lock_guard<std::mutex> g(mu);
        pendReply = ok ? out : ("Error: " + err);
        replyOk = ok;
        replyReady = true;
    }
    delete j;
    busy = false;
}

static void sendMsg(const std::string& t) {
    if (busy || t.empty()) return;
    msgs.push_back({"user", t});
    ChatJob* j = new ChatJob{token, model, {}};
    size_t start = msgs.size() > 30 ? msgs.size() - 30 : 0;
    for (size_t i = start; i < msgs.size(); i++)
        if (msgs[i].role != "error") j->hist.push_back({msgs[i].role, msgs[i].text});
    if (chatThrValid) { threadWaitForExit(&chatThr); threadClose(&chatThr); chatThrValid = false; }
    busy = true;
    if (R_FAILED(threadCreate(&chatThr, chatThread, j, nullptr, 0x100000, 0x2B, -2)) ||
        R_FAILED(threadStart(&chatThr))) {
        delete j;
        busy = false;
        msgs.push_back({"error", "Thread error"});
        return;
    }
    chatThrValid = true;
    stick = true;
}

static bool askText(const char* header, std::string& out) {
    SwkbdConfig k;
    if (R_FAILED(swkbdCreate(&k, 0))) return false;
    swkbdConfigMakePresetDefault(&k);
    swkbdConfigSetHeaderText(&k, header);
    swkbdConfigSetStringLenMax(&k, 500);
    char buf[1024] = {0};
    Result rc = swkbdShow(&k, buf, sizeof buf);
    swkbdClose(&k);
    if (R_FAILED(rc)) return false;
    out = buf;
    return true;
}

static void drawPair(Uint32 tick) {
    fillRect(0, 0, W, H, C_BG);
    rrect(250, 80, 780, 560, 24, C_SIDEBAR);
    
    int w1 = 0, w2 = 0, hh = 0;
    TTF_SizeUTF8(fTitle, "Claude", &w1, &hh);
    TTF_SizeUTF8(fTitle, "NX", &w2, &hh);
    int tx = 640 - (w1 + w2) / 2;
    text(fTitle, "Claude", tx, 120, C_WHITE);
    text(fTitle, "NX", tx + w1, 120, C_ACC);
    text(fSmall, "Scan with your phone to sign in", 640, 180, C_DIM, 1);

    if (qr.empty()) {
        text(fBody, "Waiting for Wi-Fi...", 640, 350, C_ERR, 1);
    } else {
        int n = (int)qr.size(), quiet = 2, sc = std::max(3, 280 / (n + 2 * quiet));
        int size = (n + 2 * quiet) * sc, x0 = 640 - size / 2, y0 = 220;
        rrect(x0, y0, size, size, 8, C_WHITE);
        SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qr[y][x]) {
                    SDL_Rect r{x0 + (x + quiet) * sc, y0 + (y + quiet) * sc, sc, sc};
                    SDL_RenderFillRect(R, &r);
                }
        text(fSmall, pairUrl, 640, y0 + size + 16, C_DIM, 1);
    }
    std::string dots(1 + (tick / 600) % 3, '.');
    text(fBody, "Waiting for phone" + dots, 640, 530, C_ACC, 1);
}

static void drawChat(Uint32 tick) {
    fillRect(0, 0, W, H, C_BG);
    
    // Sidebar
    fillRect(0, 0, SIDEBAR_W, H, C_SIDEBAR);
    text(fTitle, "Claude", 12, 12, C_WHITE);
    text(fSmall, "NX", 90, 18, C_ACC);
    
    rrect(12, 60, SIDEBAR_W - 24, 48, 12, C_PANEL);
    text(fSmall, "New chat", SIDEBAR_W / 2, 78, C_TEXT, 1);
    
    text(fSmall, "Model", 12, 130, C_DIM);
    int mlen = 0, mh = 0;
    TTF_SizeUTF8(fSmall, model.c_str(), &mlen, &mh);
    rrect(12, 152, SIDEBAR_W - 24, 40, 8, C_PANEL);
    text(fSmall, model.size() > 20 ? model.substr(0, 17) + "..." : model, SIDEBAR_W / 2, 166, C_TEXT, 1);
    
    // Header
    fillRect(CHAT_X, 0, W - CHAT_X, HEAD_H, C_BG);
    text(fTitle, "Chat", CHAT_X + 20, 12, C_TEXT);
    fillRect(CHAT_X, HEAD_H - 1, W - CHAT_X, 1, C_PANEL);

    // Messages
    int total = 16;
    for (auto& m : msgs) { m.h = drawMsg(m, 0, false); total += m.h; }
    if (busy) total += 40;
    int view = CHAT_H;
    float maxS = (float)std::max(0, total - view);
    if (stick) scrollY = maxS;
    scrollY = std::max(0.f, std::min(scrollY, maxS));
    if (scrollY >= maxS - 2) stick = true;

    SDL_Rect clip{CHAT_X, HEAD_H, W - CHAT_X, view};
    SDL_RenderSetClipRect(R, &clip);
    int y = HEAD_H + 12 - (int)scrollY;
    
    if (msgs.empty() && !busy) {
        circle(W / 2, HEAD_H + 120, 16, C_ACC);
        text(fBig, "How can I help?", W / 2, HEAD_H + 180, C_TEXT, 1);
    }
    
    for (auto& m : msgs) {
        if (y + m.h > HEAD_H && y < HEAD_H + view) drawMsg(m, y, true);
        y += m.h;
    }
    
    if (busy) {
        circle(CHAT_X + 20, y + 10, 6, C_ACC);
        text(fBody, "Claude is thinking" + std::string(1 + (tick / 500) % 3, '.'), CHAT_X + 20, y + 2, C_DIM);
    }
    SDL_RenderSetClipRect(R, nullptr);

    // Footer
    fillRect(CHAT_X, H - FOOTER_H, W - CHAT_X, FOOTER_H, C_BG);
    fillRect(CHAT_X, H - FOOTER_H, W - CHAT_X, 1, C_PANEL);
    
    rrect(CHAT_X + 12, H - FOOTER_H + 8, W - CHAT_X - 24, 44, 12, C_INPUT);
    text(fBody, busy ? "Waiting..." : "Message Claude...", CHAT_X + 24, H - FOOTER_H + 16, C_DIM);
    
    circle(W - 32, H - FOOTER_H + 30, 18, busy ? C_PANEL : C_ACC);
    text(fSmall, "A", W - 32, H - FOOTER_H + 22, C_WHITE, 1);
    
    text(fSmall, "A Type  Y New  X Pair  ZL/ZR Model  + Quit", CHAT_X + 20, H - 8, C_DIM);
}

int main(int, char**) {
    plInitialize(PlServiceType_User);
    net::init();
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    SDL_Window* win = SDL_CreateWindow("ClaudeNX", 0, 0, W, H, SDL_WINDOW_SHOWN);
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);

    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        auto open = [&](int sz) { return TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, sz); };
        fSmall = open(18);
        fBody = open(24);
        fTitle = open(32);
        fBig = open(44);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    loadCfg();

    if (fBody && fSmall && fTitle && fBig) {
        if (token.empty()) enterPair();
        else screen = CHAT;

        bool wasDown = false, dragged = false, tapInput = false;
        int startY = 0, lastY = 0, prevY = 0;
        while (appletMainLoop()) {
            padUpdate(&pad);
            u64 down = padGetButtonsDown(&pad), held = padGetButtons(&pad);
            if (down & HidNpadButton_Plus) break;
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}

            HidTouchScreenState ts = {0};
            hidGetTouchScreenStates(&ts, 1);
            tapInput = false;
            if (ts.count > 0) {
                int ty = (int)ts.touches[0].y;
                if (!wasDown) { startY = prevY = ty; dragged = false; }
                else {
                    if (std::abs(ty - startY) > 12) dragged = true;
                    if (dragged && screen == CHAT && ty < H - FOOTER_H) { scrollY -= (ty - prevY); stick = false; }
                    prevY = ty;
                }
                lastY = ty;
                wasDown = true;
            } else if (wasDown) {
                if (!dragged && lastY >= H - FOOTER_H && screen == CHAT) tapInput = true;
                wasDown = false;
            }

            {
                std::lock_guard<std::mutex> g(mu);
                if (tokenReady) {
                    token = pendTok;
                    tokenReady = false;
                    saveCfg();
                    screen = CHAT;
                    for (auto& m : msgs) freeBlocks(m);
                    msgs.clear();
                    stick = true;
                }
                if (replyReady) {
                    msgs.push_back({replyOk ? "assistant" : "error", pendReply});
                    replyReady = false;
                    stick = true;
                }
            }
            if (screen == CHAT && net::serverRunning()) net::stopServer();

            if (screen == PAIR) {
                if ((down & HidNpadButton_B) && !token.empty()) { net::stopServer(); screen = CHAT; }
            } else {
                if (((down & HidNpadButton_A) || tapInput) && !busy) {
                    std::string t;
                    if (askText("Your message", t)) sendMsg(t);
                }
                if (down & HidNpadButton_Y) { for (auto& m : msgs) freeBlocks(m); msgs.clear(); }
                if (down & HidNpadButton_X) enterPair();
                if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                    int n = (int)models.size();
                    mi = (mi + ((down & HidNpadButton_ZR) ? 1 : n - 1)) % n;
                    model = models[mi];
                    saveCfg();
                }
                HidAnalogStickState rs = padGetStickPos(&pad, 1);
                if (std::abs(rs.y) > 4000) { scrollY -= rs.y * 16.f / 32767.f; stick = false; }
            }

            Uint32 tick = SDL_GetTicks();
            if (screen == PAIR) drawPair(tick);
            else drawChat(tick);
            SDL_RenderPresent(R);
        }
    }

    for (auto& m : msgs) freeBlocks(m);
    net::shutdown();
    if (R) SDL_DestroyRenderer(R);
    if (win) SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    plExit();
    return 0;
}
