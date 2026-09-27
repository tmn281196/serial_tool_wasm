#include "app.h"
#include "serial.h"

#include "imgui.h"
#include "imgui_stdlib.h"
#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

namespace app {
namespace {

// ---------------------------------------------------------------- palette (same as the web version)
ImU32 C(unsigned rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
ImVec4 V(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
constexpr unsigned BG = 0x0a0e16, PANEL = 0x111a2b, PANEL2 = 0x0d1524, BORDER = 0x24324a,
                   TEXT = 0xd6deec, DIM = 0x7c8aa5, TXC = 0x39d353, RXC = 0x38bdf8, ERR = 0xf2555a,
                   ACCENT = 0x2b6cff, TS = 0x55627d, NP = 0xc68b3a, RX_PAY = 0xcfe8f6, TX_PAY = 0xcdeecf;

// ---------------------------------------------------------------- settings
const int BAUDS[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
constexpr int NBAUD = IM_ARRAYSIZE(BAUDS);   // index NBAUD = "Custom…"
const char* PARITY[] = {"none", "even", "odd"};
const char* FLOW[] = {"none", "hardware"};
const char* EOLS[] = {"none", "CR", "LF", "CRLF"};
const char* EOL_BYTES[] = {"", "\r", "\n", "\r\n"};

struct Settings {
    int baudIdx = 4, baudCustom = 115200, dataBits = 8, parity = 0, stopBits = 1, flow = 0;
    bool dtr = true, rts = true;
    bool hex = false, timestamps = true, autoscroll = true, echo = true;
    int bpl = 64, eol = 3;
    bool sendHex = false;
} S;

std::string serializeSettings() {
    char b[512];
    snprintf(b, sizeof b,
             "baudIdx=%d\nbaudCustom=%d\ndataBits=%d\nparity=%d\nstopBits=%d\nflow=%d\ndtr=%d\nrts=%d\n"
             "hex=%d\ntimestamps=%d\nautoscroll=%d\necho=%d\nbpl=%d\neol=%d\nsendHex=%d\n",
             S.baudIdx, S.baudCustom, S.dataBits, S.parity, S.stopBits, S.flow, S.dtr, S.rts, S.hex,
             S.timestamps, S.autoscroll, S.echo, S.bpl, S.eol, S.sendHex);
    return b;
}

void loadSettings() {
    std::istringstream in(serial::loadSettings());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        int v = atoi(line.c_str() + eq + 1);
        if (k == "baudIdx") S.baudIdx = std::clamp(v, 0, NBAUD);
        else if (k == "baudCustom") S.baudCustom = std::max(1, v);
        else if (k == "dataBits") S.dataBits = v == 7 ? 7 : 8;
        else if (k == "parity") S.parity = std::clamp(v, 0, 2);
        else if (k == "stopBits") S.stopBits = v == 2 ? 2 : 1;
        else if (k == "flow") S.flow = std::clamp(v, 0, 1);
        else if (k == "dtr") S.dtr = v;
        else if (k == "rts") S.rts = v;
        else if (k == "hex") S.hex = v;
        else if (k == "timestamps") S.timestamps = v;
        else if (k == "autoscroll") S.autoscroll = v;
        else if (k == "echo") S.echo = v;
        else if (k == "bpl") S.bpl = std::clamp(v, 0, 4096);
        else if (k == "eol") S.eol = std::clamp(v, 0, 3);
        else if (k == "sendHex") S.sendHex = v;
    }
}

int currentBaud() { return S.baudIdx < NBAUD ? BAUDS[S.baudIdx] : std::max(1, S.baudCustom); }

// ---------------------------------------------------------------- line model
enum Dir : uint8_t { RX, TX, SYS };
struct Line {
    Dir dir;
    bool closed;
    double ts;          // ms since epoch
    std::string data;   // raw bytes (RX/TX) or text (SYS)
};
constexpr size_t MAX_LINES = 5000;

std::deque<Line> lines;
bool dirty = true;          // log changed since the filtered view was built
bool scrollToEnd = true;
std::vector<int> view;      // indices into `lines` that pass the filter
std::string filter;

uint64_t rxTotal = 0, txTotal = 0, rxMark = 0, txMark = 0;
double rateT = 0, rxRate = 0, txRate = 0;

std::string statusMsg;
serial::MsgKind statusKind = serial::MsgKind::None;
double statusT = 0;

std::string sendText;
std::vector<std::string> history;
int histPos = -1;
std::string histDraft;
bool focusSend = true;

int selA = -1, selB = -1;   // selected line range (indices into `lines`)

ImFont* fontMono = nullptr;

double nowMs() { return emscripten_date_now(); }

void trim() {
    if (lines.size() <= MAX_LINES) return;
    size_t drop = lines.size() - MAX_LINES;
    lines.erase(lines.begin(), lines.begin() + drop);
    selA = selB = -1;
}

void appendBytes(Dir dir, const uint8_t* p, size_t n) {
    size_t bpl = (size_t)std::max(0, S.bpl);
    for (size_t i = 0; i < n; i++) {
        if (lines.empty() || lines.back().closed || lines.back().dir != dir)
            lines.push_back({dir, false, nowMs(), {}});
        Line& l = lines.back();
        l.data.push_back((char)p[i]);
        if (p[i] == 0x0A || (bpl > 0 && l.data.size() >= bpl)) l.closed = true;
    }
    trim();
    dirty = true;
}

void sysLine(const std::string& text) {
    lines.push_back({SYS, true, nowMs(), text});
    trim();
    dirty = true;
}

void setStatus(const std::string& text, serial::MsgKind kind) {
    statusMsg = text;
    statusKind = kind;
    statusT = nowMs();
}

std::string stamp(double ms) {
    time_t t = (time_t)(ms / 1000.0);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)fmod(ms, 1000.0));
    return b;
}

bool printable(uint8_t b) { return b >= 0x20 && b <= 0x7E; }

// Text view drops a trailing LF (and the CR just before it); other non-printables become \xNN.
size_t textEnd(const std::string& d) {
    size_t end = d.size();
    if (end && d[end - 1] == '\n') { end--; if (end && d[end - 1] == '\r') end--; }
    return end;
}

std::string hexOf(const std::string& d) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < d.size(); i++) {
        snprintf(b, sizeof b, i + 1 < d.size() ? "%02X " : "%02X", (uint8_t)d[i]);
        s += b;
    }
    return s;
}

std::string asciiOf(const std::string& d) {
    std::string s;
    for (char c : d) s += printable((uint8_t)c) ? c : '.';
    return s;
}

std::string escapedText(const std::string& d) {
    std::string s;
    char b[8];
    for (size_t i = 0, e = textEnd(d); i < e; i++) {
        uint8_t c = (uint8_t)d[i];
        if (printable(c)) s += (char)c;
        else { snprintf(b, sizeof b, "\\x%02X", c); s += b; }
    }
    return s;
}

const char* tagOf(Dir d) { return d == RX ? "RX" : d == TX ? "TX" : "--"; }

// One line as plain text, used for the filter, copy and "Save log…".
std::string plainLine(const Line& l, bool withTs) {
    std::string payload = l.dir == SYS ? l.data : S.hex ? hexOf(l.data) : escapedText(l.data);
    return (withTs ? stamp(l.ts) + " " : std::string()) + tagOf(l.dir) + " " + payload;
}

bool containsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((uint8_t)a) == tolower((uint8_t)b); });
    return it != hay.end();
}

void rebuildView() {
    view.clear();
    for (int i = 0; i < (int)lines.size(); i++) {
        const Line& l = lines[i];
        if (!filter.empty()) {
            // Same matching as the web version: SYS text, or the payload as shown (hex / ascii).
            std::string hay = l.dir == SYS ? l.data : S.hex ? hexOf(l.data) : asciiOf(l.data);
            if (!containsCI(hay, filter)) continue;
        }
        view.push_back(i);
    }
    dirty = false;
}

// ---------------------------------------------------------------- actions
void doConnect() {
    serial::Options o;
    o.baud = currentBaud();
    o.dataBits = S.dataBits;
    o.stopBits = S.stopBits;
    o.parity = S.parity;
    o.hwFlow = S.flow == 1;
    o.dtr = S.dtr;
    o.rts = S.rts;
    serial::connect(o);
    serial::saveSettings(serializeSettings());
}

void writeBytes(const uint8_t* p, size_t n) {
    serial::write(p, n);
    txTotal += n;
    if (S.echo) { appendBytes(TX, p, n); scrollToEnd = true; }
}

bool parseHex(const std::string& in, std::vector<uint8_t>& out, std::string& err) {
    std::string digits;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '0' && i + 1 < in.size() && (in[i + 1] == 'x' || in[i + 1] == 'X')) { i++; continue; }
        if (isxdigit((uint8_t)in[i])) digits += in[i];
    }
    if (digits.size() % 2) { err = "odd number of hex digits"; return false; }
    out.clear();
    for (size_t i = 0; i < digits.size(); i += 2)
        out.push_back((uint8_t)strtol(digits.substr(i, 2).c_str(), nullptr, 16));
    return true;
}

void sendCurrent() {
    if (serial::state() != serial::State::Open) return;
    bool blank = sendText.find_first_not_of(" \t") == std::string::npos;
    std::vector<uint8_t> bytes;
    if (S.sendHex) {
        if (blank) return;
        std::string err;
        if (!parseHex(sendText, bytes, err)) { setStatus("Invalid hex: " + err, serial::MsgKind::Err); return; }
    } else {
        std::string s = sendText + EOL_BYTES[S.eol];
        bytes.assign(s.begin(), s.end());
    }
    writeBytes(bytes.data(), bytes.size());
    if (!blank) {
        history.push_back(sendText);
        if (history.size() > 200) history.erase(history.begin());
    }
    histPos = -1;
    histDraft.clear();
    sendText.clear();
}

void exportLog() {
    std::string out;
    for (const Line& l : lines) {
        std::string payload = l.dir == SYS ? l.data : S.hex ? hexOf(l.data) : escapedText(l.data);
        out += (S.timestamps ? stamp(l.ts) + " " : std::string()) + tagOf(l.dir) + " " + payload + "\r\n";
    }
    std::string ts = stamp(nowMs());
    ts.erase(std::remove_if(ts.begin(), ts.end(), [](char c) { return c == ':' || c == '.'; }), ts.end());
    serial::download(("serial_log_" + ts + ".txt").c_str(), out);
}

void copySelection() {
    if (selA < 0) return;
    int a = std::min(selA, selB), b = std::max(selA, selB);
    std::string out;
    for (int i = a; i <= b && i < (int)lines.size(); i++) out += plainLine(lines[i], S.timestamps) + "\n";
    ImGui::SetClipboardText(out.c_str());
    setStatus("Copied " + std::to_string(b - a + 1) + " line(s).", serial::MsgKind::Ok);
}

void copyVisible() {
    std::string out;
    for (int i : view) out += plainLine(lines[i], S.timestamps) + "\n";
    ImGui::SetClipboardText(out.c_str());
    setStatus("Copied " + std::to_string(view.size()) + " line(s).", serial::MsgKind::Ok);
}

// ---------------------------------------------------------------- pump (called every frame)
void pump() {
    static uint8_t buf[64 * 1024];
    size_t n;
    while ((n = serial::read(buf, sizeof buf)) > 0) {
        rxTotal += n;
        appendBytes(RX, buf, n);
        scrollToEnd = true;
    }

    std::string text;
    for (serial::MsgKind k; (k = serial::nextMessage(text)) != serial::MsgKind::None;) {
        if (k == serial::MsgKind::Log) { sysLine(text); scrollToEnd = true; }
        else setStatus(text, k);
    }

    std::vector<uint8_t> file;
    std::string name;
    if (serial::takeFile(file, name)) {
        if (serial::state() == serial::State::Open) {
            writeBytes(file.data(), file.size());
            setStatus("Sent file " + name + " (" + std::to_string(file.size()) + " B).", serial::MsgKind::Ok);
        }
    }

    double t = nowMs();
    if (t - rateT >= 1000) {
        double dt = (t - rateT) / 1000.0;
        if (rateT > 0) { rxRate = (rxTotal - rxMark) / dt; txRate = (txTotal - txMark) / dt; }
        rxMark = rxTotal; txMark = txTotal; rateT = t;
    }
    if (!statusMsg.empty() && t - statusT > 6000) statusMsg.clear();
}

// ---------------------------------------------------------------- widgets
// Tiny flow layout: before each group, wrap to a new row if the group (measured
// last frame) no longer fits beside the previous one.
struct Flow {
    float widths[32] = {};
    int i = 0;
    bool first = true;
    void begin() { i = 0; first = true; }
    void item() {
        if (!first) {
            float need = widths[i] + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
        }
        first = false;
        ImGui::BeginGroup();
    }
    void end() { ImGui::EndGroup(); widths[i++] = ImGui::GetItemRectSize().x; }
};
Flow flowConn, flowView;

void label(const char* s) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(V(DIM), "%s", s);
    ImGui::SameLine(0, 5);
}

void separatorV() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 3, p.y + 2), ImVec2(p.x + 3, p.y + h - 2), C(BORDER));
    ImGui::Dummy(ImVec2(7, h));
}

bool coloredButton(const char* text, unsigned bg, unsigned hover, unsigned border, unsigned fg) {
    ImGui::PushStyleColor(ImGuiCol_Button, V(bg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V(hover));
    ImGui::PushStyleColor(ImGuiCol_Border, V(border));
    ImGui::PushStyleColor(ImGuiCol_Text, V(fg));
    bool r = ImGui::Button(text);
    ImGui::PopStyleColor(5);
    return r;
}

void lamp(const char* name, bool on) {
    ImVec2 ts = ImGui::CalcTextSize(name);
    ImVec2 sz(ts.x + 12, ImGui::GetFrameHeight());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), on ? C(TXC) : C(PANEL), 5);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), on ? C(TXC) : C(BORDER), 5);
    dl->AddText(ImVec2(p.x + 6, p.y + (sz.y - ts.y) / 2), on ? C(0x06210d) : C(DIM), name);
    ImGui::Dummy(sz);
}

void drawLogo(ImVec2 p, float s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float k = s / 64.f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    dl->AddRectFilled(P(2, 2), P(62, 62), C(0x0f1a30), 13 * k);
    dl->AddRect(P(2, 2), P(62, 62), C(0x36527a), 13 * k, 0, 2 * k);
    float fs = 19 * k;
    ImFont* f = ImGui::GetFont();
    dl->AddText(f, fs, P(9, 11), C(TXC), "TX");
    dl->AddTriangleFilled(P(40, 15), P(50, 21), P(40, 27), C(TXC));
    dl->AddText(f, fs, P(33, 36), C(RXC), "RX");
    dl->AddTriangleFilled(P(24, 41), P(14, 47), P(24, 53), C(RXC));
}

// ---------------------------------------------------------------- UI sections
void header(bool connected, serial::State st) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 44;
    dl->AddRectFilledMultiColor(ImVec2(p.x - 12, p.y - 8), ImVec2(p.x + w + 12, p.y + h - 8), C(0x131f38),
                                C(0x131f38), C(0x0c1424), C(0x0c1424));
    dl->AddLine(ImVec2(p.x - 12, p.y + h - 8), ImVec2(p.x + w + 12, p.y + h - 8), C(BORDER));

    drawLogo(p, 28);
    ImGui::Dummy(ImVec2(28, 28));
    ImGui::SameLine(0, 10);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::TextUnformatted("Serial Tool");

    const char* text = connected ? "Connected"
                       : st == serial::State::Opening ? "Opening\xe2\x80\xa6"
                       : st == serial::State::Error   ? "Error"
                                                       : "Not connected";
    float tw = ImGui::CalcTextSize(text).x;
    // subtitle only when it fits beside the status
    const char* sub = "WASM \xc2\xb7 Dear ImGui \xc2\xb7 Web Serial API";
    float left = ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x - 6;
    if (left - ImGui::CalcTextSize(sub).x > tw + 40) {
        ImGui::SameLine(0, 6);
        ImGui::TextColored(V(DIM), "%s", sub);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - tw));
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImVec2 dot(c.x - 12, c.y + ImGui::GetTextLineHeight() / 2);
    unsigned col = connected ? TXC : st == serial::State::Error ? ERR : 0x46506a;
    if (connected || st == serial::State::Error) dl->AddCircleFilled(dot, 8, C(col, 60));
    dl->AddCircleFilled(dot, 4.5f, C(col));
    ImGui::TextUnformatted(text);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void connectionBar(bool connected, serial::State st, bool& changed) {
    Flow& f = flowConn;
    f.begin();

    f.item();
    ImGui::BeginDisabled(connected || st == serial::State::Opening || !serial::supported());
    if (coloredButton("Connect\xe2\x80\xa6", 0x16351f, 0x1c4327, 0x2c6a3a, 0xa8f0bb)) doConnect();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected);
    if (coloredButton("Disconnect", 0x37161a, 0x48191f, 0x7a2a30, 0xf6b6b9)) serial::disconnect();
    ImGui::EndDisabled();
    ImGui::SameLine();
    separatorV();
    f.end();

    // Web Serial fixes the framing at open(), so these are locked while connected.
    ImGui::BeginDisabled(connected);
    f.item();
    label("Baud");
    ImGui::SetNextItemWidth(96);
    char preview[16];
    snprintf(preview, sizeof preview, "%d", BAUDS[std::min(S.baudIdx, NBAUD - 1)]);
    if (ImGui::BeginCombo("##baud", S.baudIdx < NBAUD ? preview : "Custom\xe2\x80\xa6")) {
        for (int i = 0; i <= NBAUD; i++) {
            char b[16];
            snprintf(b, sizeof b, "%d", i < NBAUD ? BAUDS[i] : 0);
            if (ImGui::Selectable(i < NBAUD ? b : "Custom\xe2\x80\xa6", S.baudIdx == i)) { S.baudIdx = i; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (S.baudIdx == NBAUD) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("##baudc", &S.baudCustom, 0, 0)) { S.baudCustom = std::max(1, S.baudCustom); changed = true; }
    }
    f.end();

    f.item();
    label("Data");
    ImGui::SetNextItemWidth(48);
    int db = S.dataBits - 7;
    if (ImGui::Combo("##data", &db, "7\0" "8\0")) { S.dataBits = db + 7; changed = true; }
    f.end();

    f.item();
    label("Parity");
    ImGui::SetNextItemWidth(70);
    changed |= ImGui::Combo("##parity", &S.parity, PARITY, 3);
    f.end();

    f.item();
    label("Stop");
    ImGui::SetNextItemWidth(48);
    int sb = S.stopBits - 1;
    if (ImGui::Combo("##stop", &sb, "1\0" "2\0")) { S.stopBits = sb + 1; changed = true; }
    f.end();

    f.item();
    label("Flow");
    ImGui::SetNextItemWidth(92);
    changed |= ImGui::Combo("##flow", &S.flow, FLOW, 2);
    ImGui::SameLine();
    separatorV();
    f.end();
    ImGui::EndDisabled();

    f.item();
    bool sig = false;
    sig |= ImGui::Checkbox("DTR", &S.dtr);
    ImGui::SameLine();
    sig |= ImGui::Checkbox("RTS", &S.rts);
    if (sig) { serial::setSignals(S.dtr, S.rts); changed = true; }
    f.end();

    f.item();
    int s = connected ? serial::signals() : 0;
    lamp("CTS", s & serial::CTS); ImGui::SameLine(0, 6);
    lamp("DSR", s & serial::DSR); ImGui::SameLine(0, 6);
    lamp("DCD", s & serial::DCD); ImGui::SameLine(0, 6);
    lamp("RI", s & serial::RI);
    f.end();
}

void viewBar(bool& changed) {
    Flow& f = flowView;
    f.begin();
    f.item();
    bool v = false;
    v |= ImGui::Checkbox("Hex", &S.hex); ImGui::SameLine();
    v |= ImGui::Checkbox("Timestamps", &S.timestamps); ImGui::SameLine();
    v |= ImGui::Checkbox("Autoscroll", &S.autoscroll); ImGui::SameLine();
    changed |= ImGui::Checkbox("Echo TX", &S.echo);
    f.end();

    f.item();
    label("Bytes/line");
    ImGui::SetNextItemWidth(64);
    if (ImGui::InputInt("##bpl", &S.bpl, 0, 0)) { S.bpl = std::clamp(S.bpl, 0, 4096); changed = true; }
    ImGui::SameLine();
    separatorV();
    f.end();

    f.item();
    label("Filter");
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputTextWithHint("##filter", "filter lines containing\xe2\x80\xa6", &filter)) dirty = true;
    ImGui::SameLine();
    separatorV();
    f.end();

    f.item();
    if (ImGui::Button("Clear")) { lines.clear(); selA = selB = -1; dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Save log\xe2\x80\xa6")) exportLog();
    f.end();

    if (v) { dirty = true; if (S.autoscroll) scrollToEnd = true; changed = true; }
}

// Draws text runs on one row; SameLine(0,0) keeps them glued together.
void run(const char* b, const char* e, ImU32 col) {
    if (b == e) return;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(b, e);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
}
void run(const std::string& s, ImU32 col) { run(s.data(), s.data() + s.size(), col); }

void drawPayload(const Line& l) {
    if (l.dir == SYS) { run(l.data, C(DIM)); return; }
    ImU32 pay = l.dir == RX ? C(RX_PAY) : C(TX_PAY);
    const std::string& d = l.data;
    if (S.hex) {
        run(hexOf(d), pay);
        if (!d.empty()) run("  |" + asciiOf(d) + "|", C(TS));
        return;
    }
    size_t e = textEnd(d), start = 0;
    char esc[8];
    for (size_t i = 0; i < e; i++) {
        uint8_t c = (uint8_t)d[i];
        if (printable(c)) continue;
        run(d.data() + start, d.data() + i, pay);
        snprintf(esc, sizeof esc, "\\x%02X", c);
        run(esc, esc + 4, C(NP));
        start = i + 1;
    }
    run(d.data() + start, d.data() + e, pay);
}

float sendBarHeight();

void logView() {
    if (dirty) rebuildView();
    // leave room for the send bar and footer below (heights match sendBar()/footer())
    float sendH = sendBarHeight(), footH = ImGui::GetTextLineHeight() + 12;
    ImVec2 size(0, -(ImGui::GetStyle().ItemSpacing.y + sendH + footH));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, V(BG));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("log", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fontMono, 0);

    float lh = ImGui::GetTextLineHeight() + 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowSize().x;

    ImGuiListClipper clip;
    clip.Begin((int)view.size(), lh);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            int idx = view[r];
            const Line& l = lines[idx];
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (r & 1) dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + lh), C(0x0b0f19));

            ImGui::PushID(idx);
            bool sel = selA >= 0 && idx >= std::min(selA, selB) && idx <= std::max(selA, selB);
            ImGui::PushStyleColor(ImGuiCol_Header, V(ACCENT, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V(0xffffff, 0.04f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, V(ACCENT, 0.35f));
            if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, lh))) {
                if (ImGui::GetIO().KeyShift && selA >= 0) selB = idx;
                else if (sel && selA == selB) selA = selB = -1;
                else selA = selB = idx;
            }
            ImGui::PopStyleColor(3);
            if (ImGui::BeginPopupContextItem("ctx")) {
                if (!sel) selA = selB = idx;
                if (ImGui::MenuItem("Copy selected", "Ctrl+C")) copySelection();
                if (ImGui::MenuItem("Copy all visible")) copyVisible();
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 1));
            if (S.timestamps) run(stamp(l.ts) + " ", C(TS));
            run(tagOf(l.dir), l.dir == RX ? C(RXC) : l.dir == TX ? C(TXC) : C(DIM));
            run(" ", 0);
            drawPayload(l);
            ImGui::NewLine();
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lh));
        }
    }
    clip.End();

    if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) copySelection();

    // Follow the tail when autoscroll is on (turn it off to read back).
    if (S.autoscroll && scrollToEnd) ImGui::SetScrollHereY(1.0f);
    scrollToEnd = false;

    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Below this width the send bar puts EOL/Send/Send file/Break on a second row.
constexpr float SEND_ONE_ROW_MIN = 640;
float sendBarHeight() {
    float h = ImGui::GetFrameHeight() + 16;
    if (ImGui::GetContentRegionAvail().x < SEND_ONE_ROW_MIN) h += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    return h;
}

int historyCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || history.empty()) return 0;
    if (d->EventKey == ImGuiKey_UpArrow) {
        if (histPos == -1) { histDraft.assign(d->Buf, d->BufTextLen); histPos = (int)history.size(); }
        if (histPos > 0) histPos--;
    } else if (d->EventKey == ImGuiKey_DownArrow) {
        if (histPos == -1) return 0;
        histPos++;
    }
    const std::string& s = histPos >= 0 && histPos < (int)history.size() ? history[histPos] : histDraft;
    if (histPos >= (int)history.size()) histPos = -1;
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, s.c_str());
    return 0;
}

void sendBar(bool connected, bool& changed) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = sendBarHeight();
    bool narrow = w < SEND_ONE_ROW_MIN;
    dl->AddRectFilled(ImVec2(p.x - 12, p.y), ImVec2(p.x + w + 12, p.y + h), C(PANEL2));
    dl->AddLine(ImVec2(p.x - 12, p.y), ImVec2(p.x + w + 12, p.y), C(BORDER));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 8));

    changed |= ImGui::Checkbox("Hex##send", &S.sendHex);
    ImGui::SameLine();

    // right-hand controls measured from their labels so the input fills the rest
    ImGuiStyle& st = ImGui::GetStyle();
    auto bw = [&](const char* s) { return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2 + st.ItemSpacing.x; };
    float right = ImGui::CalcTextSize("EOL").x + 5 + 76 + st.ItemSpacing.x + bw("Send") + bw("Send file\xe2\x80\xa6") + bw("Break");
    ImGui::SetNextItemWidth(narrow ? -FLT_MIN : std::max(120.f, ImGui::GetContentRegionAvail().x - right));

    if (focusSend) { ImGui::SetKeyboardFocusHere(); focusSend = false; }
    ImGui::PushFont(fontMono, 0);
    if (S.sendHex) ImGui::PushStyleColor(ImGuiCol_Text, V(TXC));
    bool enter = ImGui::InputTextWithHint("##send", "Type and press Enter to send...   (Up/Down: history)",
                                          &sendText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                          historyCallback);
    if (S.sendHex) ImGui::PopStyleColor();
    ImGui::PopFont();
    if (enter) { sendCurrent(); focusSend = true; }

    if (!narrow) ImGui::SameLine();
    label("EOL");
    ImGui::SetNextItemWidth(76);
    changed |= ImGui::Combo("##eol", &S.eol, EOLS, 4);
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected);
    if (ImGui::Button("Send")) { sendCurrent(); focusSend = true; }
    ImGui::SameLine();
    if (ImGui::Button("Send file\xe2\x80\xa6")) serial::pickFile();
    ImGui::SameLine();
    if (ImGui::Button("Break")) serial::sendBreak();
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

std::string thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

void footer() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeight() + 12;
    dl->AddRectFilled(ImVec2(p.x - 12, p.y), ImVec2(p.x + w + 12, p.y + h + 12), C(0x0b1120));
    dl->AddLine(ImVec2(p.x - 12, p.y), ImVec2(p.x + w + 12, p.y), C(0x1a2436));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 6));

    auto stat = [](const char* k, const std::string& v) {
        ImGui::TextColored(V(0x90a0bd), "%s", k);
        ImGui::SameLine(0, 5);
        ImGui::TextColored(V(DIM), "%s", v.c_str());
        ImGui::SameLine(0, 18);
    };
    stat("RX", thousands(rxTotal) + " B \xc2\xb7 " + std::to_string((long long)llround(rxRate)) + " B/s");
    stat("TX", thousands(txTotal) + " B \xc2\xb7 " + std::to_string((long long)llround(txRate)) + " B/s");
    stat("Lines", std::to_string(view.size()));

    if (!statusMsg.empty()) {
        float tw = ImGui::CalcTextSize(statusMsg.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - tw));
        unsigned col = statusKind == serial::MsgKind::Err ? ERR : statusKind == serial::MsgKind::Ok ? TXC : DIM;
        ImGui::TextColored(V(col), "%s", statusMsg.c_str());
    } else {
        ImGui::NewLine();
    }
}

void noSupportModal() {
    static bool opened = false;
    if (!opened) { ImGui::OpenPopup("Browser does not support Web Serial"); opened = true; }
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    if (ImGui::BeginPopupModal("Browser does not support Web Serial", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("This app uses the Web Serial API, available only in Chromium-based browsers: "
                               "Google Chrome, Microsoft Edge, Opera\xe2\x80\xa6 (not Firefox/Safari).");
        ImGui::Spacing();
        ImGui::TextUnformatted("Open this page in Chrome or Edge, over https:// or http://localhost.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(12, 8);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 8);
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.ScrollbarRounding = 6;
    s.WindowRounding = 0;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.WindowBorderSize = 0;
    s.ScrollbarSize = 12;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = V(TEXT);
    c[ImGuiCol_TextDisabled] = V(DIM);
    c[ImGuiCol_WindowBg] = V(PANEL2);
    c[ImGuiCol_ChildBg] = V(BG);
    c[ImGuiCol_PopupBg] = V(PANEL);
    c[ImGuiCol_Border] = V(BORDER);
    c[ImGuiCol_FrameBg] = V(PANEL);
    c[ImGuiCol_FrameBgHovered] = V(0x1a2740);
    c[ImGuiCol_FrameBgActive] = V(0x1a2740);
    c[ImGuiCol_Button] = V(PANEL);
    c[ImGuiCol_ButtonHovered] = V(0x1a2740);
    c[ImGuiCol_ButtonActive] = V(0x22324f);
    c[ImGuiCol_Header] = V(ACCENT, 0.35f);
    c[ImGuiCol_HeaderHovered] = V(ACCENT, 0.5f);
    c[ImGuiCol_HeaderActive] = V(ACCENT, 0.6f);
    c[ImGuiCol_CheckMark] = V(0x7aa2ff);
    c[ImGuiCol_SliderGrab] = V(ACCENT);
    c[ImGuiCol_ScrollbarBg] = V(BG);
    c[ImGuiCol_ScrollbarGrab] = V(BORDER);
    c[ImGuiCol_ScrollbarGrabHovered] = V(0x33476a);
    c[ImGuiCol_ScrollbarGrabActive] = V(0x3d5580);
    c[ImGuiCol_TextSelectedBg] = V(ACCENT, 0.45f);
    c[ImGuiCol_NavCursor] = V(ACCENT);
    c[ImGuiCol_ModalWindowDimBg] = V(0x060910, 0.85f);
    c[ImGuiCol_TitleBgActive] = V(0x131f38);
    c[ImGuiCol_TitleBg] = V(0x131f38);
}

} // namespace

// ---------------------------------------------------------------- public
void init(ImFont*, ImFont* monoFont) {
    fontMono = monoFont;
    applyStyle();
    loadSettings();
    sysLine("Serial Tool (WASM) ready. Click \"Connect...\" and pick a COM port.");
}

void frame() {
    pump();

    serial::State st = serial::state();
    bool connected = st == serial::State::Open;
    bool changed = false;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Serial Tool", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    header(connected, st);
    connectionBar(connected, st, changed);
    ImGui::Separator();
    viewBar(changed);
    logView();
    sendBar(connected, changed);
    footer();

    if (!serial::supported()) noSupportModal();
    ImGui::End();

    if (changed) serial::saveSettings(serializeSettings());
}

} // namespace app
