cmake_minimum_required(VERSION 3.24)
project(SerialToolWasm LANGUAGES C CXX)

# Serial Tool — Dear ImGui 1.92 on GLFW + WebGL2, built to WebAssembly with Emscripten.
# Configure/build through the presets:  cmake --preset web  &&  cmake --build --preset web

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Web build only (Emscripten). Use: cmake --preset web")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(GLFW_PORT --use-port=contrib.glfw3)   # GLFW 3.4 for the browser: clipboard, HiDPI, resizable canvas

# ---------------------------------------------------------------- Dear ImGui + GLFW/OpenGL3 backends
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PUBLIC ${GLFW_PORT})

# ---------------------------------------------------------------- the app
add_executable(serialtool src/main.cpp src/app.cpp src/serial.cpp)
target_link_libraries(serialtool PRIVATE imgui)
target_compile_options(serialtool PRIVATE -Wall)
target_link_options(serialtool PRIVATE
    ${GLFW_PORT} -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1mb
    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$stringToNewUTF8','$UTF8ToString']"
    "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/src/serial_bridge.js"
    "SHELL:--embed-file ${CMAKE_SOURCE_DIR}/assets@assets"
    "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/shell.html")
set_target_properties(serialtool PROPERTIES
    OUTPUT_NAME index
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/web
    LINK_DEPENDS "${CMAKE_SOURCE_DIR}/shell.html;${CMAKE_SOURCE_DIR}/src/serial_bridge.js")
