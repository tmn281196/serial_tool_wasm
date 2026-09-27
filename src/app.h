#pragma once
struct ImFont;

namespace app {

void init(ImFont* uiFont, ImFont* monoFont);
void frame();   // pump serial I/O and draw the whole UI (one full-window ImGui window)

} // namespace app
