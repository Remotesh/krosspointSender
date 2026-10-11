// Devices window: the only place that adds, tests, edits and deletes
// devices. The list view shows the found and the saved devices. The form
// view is the add / edit flow: 1 test, 2 confirm (device_setup.cpp).
#include "app/ui.h"

#include <imgui.h>

namespace ks::app {

void drawDevicesWindow(App& app) {
    if (!app.showDevices) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("devices", &app.showDevices)) {
        ImGui::End();
        return;
    }
    if (app.dev.view == DeviceView::Form) drawDeviceForm(app);
    else drawDeviceList(app);
    ImGui::End();
}

} // namespace ks::app
