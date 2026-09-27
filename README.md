# Serial Tool (WASM)

Bản WebAssembly của [serial_tool](https://github.com/tmn281196/serial_tool): giao diện viết lại
bằng **C++ / Dear ImGui 1.92** (GLFW + WebGL2), cổng COM vẫn dùng **Web Serial API** của trình duyệt.

```
serial_tool_wasm/
├── src/
│   ├── main.cpp           GLFW + ImGui + vòng lặp
│   ├── app.cpp / app.h    mô hình log (RX/TX/SYS) và toàn bộ giao diện
│   ├── serial.cpp / .h    lớp C++ gọi sang JS (EM_JS)
│   └── serial_bridge.js   Web Serial (async) — nạp bằng --pre-js
├── shell.html             trang HTML bọc canvas
├── assets/                Roboto-Medium, Cousine-Regular (nhúng vào .wasm)
├── third_party/imgui/     Dear ImGui 1.92.9b + backend glfw/opengl3 + imgui_stdlib
└── web/                   kết quả build (index.html/.js/.wasm) — đưa thẳng lên GitHub Pages được
```

## Yêu cầu

- Chrome / Edge / trình duyệt nhân Chromium (Web Serial không có trên Firefox/Safari), có WebGL 2.
- Trang phải chạy qua `https://` hoặc `http://localhost` (Web Serial cần secure context).

## Build

Dùng emsdk + CMake + Ninja trong `tools\` của project tien-len (đặt biến `TOOLS` nếu ở chỗ khác):

```bat
build.bat          :: Release  -> web\
build.bat debug    :: Debug
build.bat clean    :: xoá build\ rồi build lại
run.bat            :: chạy http://localhost:8771/
```

Lần build đầu Emscripten tải port `contrib.glfw3` (GLFW 3.4 cho web: clipboard, HiDPI, canvas tự co giãn).

## Tính năng (giống bản web gốc)

- Connect/Disconnect, Baud (preset + Custom), data bits, parity, stop bits, flow control; DTR/RTS; đèn CTS/DSR/DCD/RI; Break.
- Log RX/TX/SYS có timestamp ms; Hex ⇄ text (byte không in được hiện `\xNN`), cột ASCII ở chế độ hex;
  Bytes/line, Filter, Autoscroll, Echo TX, Clear, Save log… (.txt).
- Gửi text với EOL none/CR/LF/CRLF hoặc hex (`01 03 00 6B`), Enter để gửi, ↑/↓ lịch sử, Send file….
- Đếm byte và tốc độ RX/TX. Cài đặt lưu trong localStorage.
- Thêm so với bản gốc: click chọn dòng (Shift+click chọn nhiều), Ctrl+C / chuột phải để copy; danh sách log ảo hoá (ImGuiListClipper).

## Cách hoạt động

JS giữ `SerialPort`, reader và writer. Dữ liệu RX được đưa vào hàng đợi, C++ lấy ra mỗi frame qua `st_read()`.
Lệnh gửi đi được `ST.write()` xếp nối tiếp nhau (promise chain) nên giữ đúng thứ tự. Không dùng Asyncify,
C++ không bao giờ phải chờ.
