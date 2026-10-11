// UI test hook. When the environment variable KS_UI_TEST names a file, the
// app reads one command from it per frame and then removes the file. A test
// script can drive the window this way without the global mouse or the
// keyboard focus. Commands:
//   move <x> <y>         move the mouse (window pixels)
//   click <x> <y>        move the mouse and click
//   double <x> <y>       double click
//   drag <x0> <y0> <x1>  press at x0,y0, move to x1 in steps, release
//   action <id>          run a keybind action, for example toggle_left_panel
//   type <text>          type the text into the focused input box
//   key <name>           press one key: enter, tab, escape, backspace,
//                        ctrl+a (select all)
//   shot <file.png>      write a screenshot of the window after the next frame
// Without KS_UI_TEST this file does nothing.
#include "app/ui.h"

#include <SDL_opengl.h>
#include <imgui.h>

#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

namespace ks::app {

namespace {

struct Step {
    float x = 0, y = 0;
    int button = -1;   // -1: move only
    bool down = false;
    bool keyStep = false;   // a key or a text step, no mouse
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    std::string text;
};

std::deque<Step> g_steps;
std::string g_shotPath;   // written after the next render

void queueMouse(float x, float y, int button, bool down) {
    Step s;
    s.x = x;
    s.y = y;
    s.button = button;
    s.down = down;
    g_steps.push_back(s);
}

void queueClick(float x, float y, int times) {
    queueMouse(x, y, -1, false);   // hover first: ImGui needs one frame to see the item
    for (int i = 0; i < times; ++i) {
        queueMouse(x, y, 0, true);
        queueMouse(x, y, 0, false);
    }
}

void queueDrag(float x0, float y0, float x1) {
    queueMouse(x0, y0, -1, false);
    queueMouse(x0, y0, 0, true);
    for (int i = 1; i <= 10; ++i) queueMouse(x0 + (x1 - x0) * static_cast<float>(i) / 10.f, y0, -1, false);
    queueMouse(x1, y0, 0, false);
}

void queueKey(ImGuiKey k, bool ctrl) {
    Step s;
    s.keyStep = true;
    s.key = k;
    s.ctrl = ctrl;
    s.down = true;
    g_steps.push_back(s);
    s.down = false;
    g_steps.push_back(s);
}

void queueText(const std::string& text) {
    Step s;
    s.keyStep = true;
    s.text = text;
    g_steps.push_back(s);
}

ImGuiKey keyFrom(const std::string& name, bool& ctrl) {
    ctrl = false;
    if (name == "enter") return ImGuiKey_Enter;
    if (name == "tab") return ImGuiKey_Tab;
    if (name == "escape") return ImGuiKey_Escape;
    if (name == "backspace") return ImGuiKey_Backspace;
    if (name == "ctrl+a") {
        ctrl = true;
        return ImGuiKey_A;
    }
    return ImGuiKey_None;
}

void runCommand(App& app, const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    float x = 0, y = 0, x1 = 0;
    if (cmd == "move" && (in >> x >> y)) queueMouse(x, y, -1, false);
    else if (cmd == "click" && (in >> x >> y)) queueClick(x, y, 1);
    else if (cmd == "double" && (in >> x >> y)) queueClick(x, y, 2);
    else if (cmd == "drag" && (in >> x >> y >> x1)) queueDrag(x, y, x1);
    else if (cmd == "type") {
        std::string rest;
        std::getline(in, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        queueText(rest);
    } else if (cmd == "key") {
        std::string name;
        in >> name;
        bool ctrl = false;
        ImGuiKey k = keyFrom(name, ctrl);
        if (k != ImGuiKey_None) queueKey(k, ctrl);
    } else if (cmd == "shot") {
        std::string rest;
        std::getline(in, rest);
        g_shotPath = trim(rest);
    } else if (cmd == "action") {
        std::string id;
        in >> id;
        bool quit = false;
        for (int i = 0; i < kActionCount; ++i)
            if (id == actionId(static_cast<Action>(i))) runAction(app, static_cast<Action>(i), quit);
    }
    app.addLog("UI test: " + line);
}

} // namespace

bool uiTestPoll(App& app) {
    static const char* path = std::getenv("KS_UI_TEST");
    if (!path || !*path) return false;
    ImGuiIO& io = ImGui::GetIO();
    // One step for each frame, so that ImGui sees the hover before the press.
    if (!g_steps.empty()) {
        Step s = g_steps.front();
        g_steps.pop_front();
        if (s.keyStep) {
            if (!s.text.empty()) {
                io.AddInputCharactersUTF8(s.text.c_str());
            } else {
                if (s.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, s.down);
                io.AddKeyEvent(s.key, s.down);
            }
        } else {
            io.AddMousePosEvent(s.x, s.y);
            if (s.button >= 0) io.AddMouseButtonEvent(s.button, s.down);
        }
        return true;
    }
    if (!g_shotPath.empty()) return true;   // keep frames coming until the shot is written
    bool ok = false;
    std::string text = readFile(path, &ok);
    if (!ok) return false;
    std::remove(path);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (!line.empty()) runCommand(app, line);
    }
    return true;
}

void uiTestAfterRender(int width, int height) {
    if (g_shotPath.empty() || !g_steps.empty() || width <= 0 || height <= 0) return;
    std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // opaque
    stbi_flip_vertically_on_write(1);
    stbi_write_png(g_shotPath.c_str(), width, height, 4, px.data(), width * 4);
    stbi_flip_vertically_on_write(0);
    g_shotPath.clear();
}

} // namespace ks::app
