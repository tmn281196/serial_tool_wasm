#pragma once
// C++ view of the Web Serial bridge (src/serial_bridge.js). All calls return
// immediately; results arrive through the poll functions on later frames.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace serial {

enum class State { Closed = 0, Opening = 1, Open = 2, Error = 3 };
enum class MsgKind { None = 0, Log = 1, Ok = 2, Err = 3, Info = 4 };
enum Signal { CTS = 1, DSR = 2, DCD = 4, RI = 8 };

struct Options {
    int baud = 115200;
    int dataBits = 8;
    int stopBits = 1;
    int parity = 0;    // 0 none, 1 even, 2 odd
    bool hwFlow = false;
    bool dtr = true, rts = true;
};

bool supported();
void connect(const Options& o);   // opens the browser's port picker
void disconnect();
State state();
int signals();                    // Signal bitmask
void setSignals(bool dtr, bool rts);
void sendBreak();
void write(const uint8_t* data, size_t len);

size_t read(uint8_t* dst, size_t cap);        // drains queued RX bytes
MsgKind nextMessage(std::string& text);       // one queued message per call

void pickFile();                              // opens the browser file dialog
bool takeFile(std::vector<uint8_t>& data, std::string& name);

void download(const char* filename, const std::string& content);
std::string loadSettings();
void saveSettings(const std::string& s);

} // namespace serial
