#include "serial.h"
#include <emscripten.h>
#include <cstdlib>

// Thin EM_JS wrappers over the ST object defined in serial_bridge.js.

EM_JS(int, st_supported, (), { return ST.supported() ? 1 : 0; });

EM_JS(void, st_connect, (int baud, int dataBits, int stopBits, int parity, int hwFlow, int dtr, int rts), {
    ST.connect({
        open: { baudRate: baud, dataBits: dataBits, stopBits: stopBits,
                parity: ['none', 'even', 'odd'][parity] || 'none',
                flowControl: hwFlow ? 'hardware' : 'none', bufferSize: 8192 },
        dtr: !!dtr, rts: !!rts,
    });
});

EM_JS(void, st_disconnect, (), { ST.disconnect(); });
EM_JS(int, st_state, (), { return ST.state; });
EM_JS(int, st_signals, (), { return ST.signals; });
EM_JS(void, st_set_signals, (int dtr, int rts), { ST.setSignals({ dataTerminalReady: !!dtr, requestToSend: !!rts }); });
EM_JS(void, st_break, (), { ST.sendBreak(); });
EM_JS(void, st_write, (const uint8_t* p, int n), { ST.write(HEAPU8.slice(p, p + n)); });

EM_JS(int, st_read, (uint8_t* dst, int cap), {
    let n = 0;
    while (n < cap && ST.rx.length) {
        const chunk = ST.rx[0];
        const take = Math.min(cap - n, chunk.length - ST.rxHead);
        HEAPU8.set(chunk.subarray(ST.rxHead, ST.rxHead + take), dst + n);
        n += take;
        ST.rxHead += take;
        if (ST.rxHead >= chunk.length) { ST.rx.shift(); ST.rxHead = 0; }
    }
    return n;
});

// Returns the kind and a malloc'd UTF-8 copy of the text (caller frees).
EM_JS(int, st_next_msg, (char** out), {
    const m = ST.msgs.shift();
    if (!m) return 0;
    HEAPU32[out >> 2] = stringToNewUTF8(m[1]);
    return m[0];
});

EM_JS(void, st_pick_file, (), { ST.pickFile(); });
EM_JS(int, st_file_size, (), { return ST.file ? ST.file.length : -1; });
EM_JS(char*, st_take_file, (uint8_t* dst), {
    HEAPU8.set(ST.file, dst);
    const name = stringToNewUTF8(ST.fileName);
    ST.file = null; ST.fileName = "";
    return name;
});

EM_JS(void, st_download, (const char* name, const char* p, int n), {
    ST.download(UTF8ToString(name), HEAPU8.slice(p, p + n));
});

EM_JS(char*, st_load_settings, (), {
    let s = "";
    try { s = localStorage.getItem('serial_tool_wasm.settings') || ""; } catch (e) {}
    return stringToNewUTF8(s);
});
EM_JS(void, st_save_settings, (const char* s), {
    try { localStorage.setItem('serial_tool_wasm.settings', UTF8ToString(s)); } catch (e) {}
});

namespace serial {

bool supported() { return st_supported() != 0; }

void connect(const Options& o) {
    st_connect(o.baud, o.dataBits, o.stopBits, o.parity, o.hwFlow, o.dtr, o.rts);
}

void disconnect() { st_disconnect(); }
State state() { return (State)st_state(); }
int signals() { return st_signals(); }
void setSignals(bool dtr, bool rts) { st_set_signals(dtr, rts); }
void sendBreak() { st_break(); }
void write(const uint8_t* data, size_t len) { if (len) st_write(data, (int)len); }

size_t read(uint8_t* dst, size_t cap) { return (size_t)st_read(dst, (int)cap); }

MsgKind nextMessage(std::string& text) {
    char* p = nullptr;
    int kind = st_next_msg(&p);
    if (!kind) return MsgKind::None;
    text = p;
    free(p);
    return (MsgKind)kind;
}

void pickFile() { st_pick_file(); }

bool takeFile(std::vector<uint8_t>& data, std::string& name) {
    int n = st_file_size();
    if (n < 0) return false;
    data.resize((size_t)n);
    char* p = st_take_file(data.data());
    name = p;
    free(p);
    return true;
}

void download(const char* filename, const std::string& content) {
    st_download(filename, content.data(), (int)content.size());
}

std::string loadSettings() {
    char* p = st_load_settings();
    std::string s = p;
    free(p);
    return s;
}

void saveSettings(const std::string& s) { st_save_settings(s.c_str()); }

} // namespace serial
