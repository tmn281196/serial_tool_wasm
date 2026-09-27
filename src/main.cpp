// Serial Tool (WASM) — Dear ImGui on GLFW (emscripten-glfw port) + WebGL2.
// The serial port itself is driven by the browser's Web Serial API, see serial_bridge.js.
#include "app.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <GLFW/emscripten_glfw3.h>
#include <emscripten.h>
#include <cstdio>

static GLFWwindow* window = nullptr;

static void frame() {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    app::frame();

    ImGui::Render();
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.039f, 0.055f, 0.086f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

int main() {
    if (!glfwInit()) { printf("glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);   // render at devicePixelRatio (crisp on HiDPI)
    window = glfwCreateWindow(1280, 720, "Serial Tool", nullptr, nullptr);
    if (!window) {
        EM_ASM({ document.getElementById('loading').textContent =
                     'WebGL 2 is not available in this browser (or it was disabled after a GPU crash; restart the browser).'; });
        return 1;
    }
    EM_ASM({ document.getElementById('loading').remove(); });
    glfwMakeContextCurrent(window);
    // Focus through GLFW (not canvas.focus() in the page): the port only routes keys to a window
    // it saw gain focus.
    glfwFocusWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFont* ui = io.Fonts->AddFontFromFileTTF("assets/Roboto-Medium.ttf", 15.0f);
    ImFont* mono = io.Fonts->AddFontFromFileTTF("assets/Cousine-Regular.ttf", 14.0f);
    if (!ui) ui = io.Fonts->AddFontDefault();
    if (!mono) mono = ui;
    io.FontDefault = ui;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");   // canvas follows the browser window
    ImGui_ImplOpenGL3_Init("#version 300 es");

    app::init(ui, mono);
    emscripten_set_main_loop(frame, 0, false);
    return 0;
}
