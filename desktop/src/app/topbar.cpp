// The right side of the menu bar: device combo (or "add device"), the send
// button, the "send a file..." button (send_file.cpp) and the transfer pill. The device has no panel of its own; the
// devices window (win_devices.cpp) adds, tests and edits devices.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ks::app {

namespace {

std::string profileLabel(const DeviceProfile& p) {
    return p.name + "  " + (p.method == TransferMethod::Ble ? deviceWhere(p) : p.host);
}

ImVec4 pillColor(TransferState s) {
    switch (s) {
        case TransferState::Complete: return theme::Live;
        case TransferState::Failed: return theme::Error;
        case TransferState::Idle: return theme::Dim;
        default: return theme::Amber;
    }
}

// The short pill text: "idle", "uploading 42%", "complete", "failed".
std::string pillText(const App& app) {
    const TransferProgress& t = app.transfer;
    std::string s = toLower(stateLabel(t.state));
    if (t.state == TransferState::Uploading && t.fraction >= 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, " %d%%", static_cast<int>(t.fraction * 100.0 + 0.5));
        s += buf;
    }
    return s;
}

float pillWidth(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x + 16.f; }

// The pill, with a thin progress bar under it while a transfer runs.
void drawPill(App& app, const std::string& text) {
    ImVec4 c = pillColor(app.transfer.state);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    float h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + ts.x + 16.f, p.y + h - 2.f);
    p.y += 2.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, ImGui::GetColorU32(theme::alpha(c, 0.22f)), 20.f);
    dl->AddRect(p, q, ImGui::GetColorU32(c), 20.f);
    dl->AddText(ImVec2(p.x + 8.f, p.y + (q.y - p.y - ts.y) * 0.5f), ImGui::GetColorU32(c), text.c_str());
    if (app.busy(JobKind::Send)) {
        float f = app.transfer.fraction >= 0 ? static_cast<float>(app.transfer.fraction) : -1.f;
        float y = q.y + 1.f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(q.x, y), ImGui::GetColorU32(theme::Muted), 2.f);
        if (f >= 0) {
            dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + (q.x - p.x) * f, y), ImGui::GetColorU32(theme::Amber), 2.f);
        } else {
            // Unknown size: a short segment moves along the bar.
            float w = (q.x - p.x) * 0.3f;
            float t = static_cast<float>(ImGui::GetTime() * 0.8 - static_cast<long>(ImGui::GetTime() * 0.8));
            float x0 = p.x + (q.x - p.x - w) * t;
            dl->AddLine(ImVec2(x0, y), ImVec2(x0 + w, y), ImGui::GetColorU32(theme::Amber), 2.f);
        }
    }
    ImGui::InvisibleButton("##pill", ImVec2(q.x - p.x, h));
    if (ui::g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        std::string tip;
        if (app.transfer.state == TransferState::Failed && !app.lastExplain.empty()) {
            tip = app.lastExplain;   // title, message, reasons, fallbacks
        } else {
            tip = stateLabel(app.transfer.state);
            if (!app.transfer.message.empty()) tip += ": " + app.transfer.message;
            if (app.transfer.bytesTotal)
                tip += "\n" + humanBytes(app.transfer.bytesSent) + " / " + humanBytes(app.transfer.bytesTotal);
            if (!app.sendStatus.empty()) tip += "\n" + app.sendStatus;
        }
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
        ImGui::TextUnformatted(tip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void selectProfile(App& app, const DeviceProfile& p) {
    app.profileId = p.id;
    app.method = p.method;
    app.settings.defaultDeviceId = p.id;
    app.markSettingsChanged();
    app.testResult.reset();
}

} // namespace

void openDevicesWindow(App& app) {
    app.showDevices = true;
    if (app.dev.view != DeviceView::Form) app.dev.autoDiscovered = false;
}

void drawDeviceBar(App& app) {
    const ImGuiStyle& st = ImGui::GetStyle();
    bool none = app.profiles.empty();
    std::string cur = "(choose a device)";
    for (auto& p : app.profiles)
        if (p.id == app.profileId) cur = profileLabel(p);
    std::string pill = pillText(app);

    float comboW = none ? ui::ButtonWidth("add device")
                        : std::clamp(ImGui::CalcTextSize(cur.c_str()).x + ImGui::GetFrameHeight() + st.FramePadding.x * 3.f,
                                     ImGui::GetFontSize() * 10.f, ImGui::GetFontSize() * 18.f);
    float sendW = ui::ButtonWidth("send") + ImGui::GetFontSize();
    float fileW = ui::ButtonWidth("send a file...");
    float pillW = std::max(pillWidth(pill), pillWidth("uploading 100%"));
    float total = comboW + sendW + fileW + pillW + st.ItemSpacing.x * 4.f;
    float x = ImGui::GetWindowContentRegionMax().x - total;
    if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);

    if (none) {
        if (ImGui::Button("add device")) openDevicesWindow(app);
        ui::Tip("find the reader on the network and add it");
    } else {
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##device", cur.c_str())) {
            for (auto& p : app.profiles) {
                std::string label = profileLabel(p) + (p.lastConnectedAt.empty() ? "  (unconfirmed)" : "") + "##" + p.id;
                if (ImGui::Selectable(label.c_str(), p.id == app.profileId)) selectProfile(app, p);
            }
            ImGui::Separator();
            if (ImGui::Selectable("add device...")) {
                openDevicesWindow(app);
                app.dev.view = DeviceView::List;
            }
            if (ImGui::Selectable("manage devices...")) openDevicesWindow(app);
            ImGui::EndCombo();
        }
        ui::Tip("the device that gets the file");
    }

    ImGui::SameLine();
    bool sending = app.busy(JobKind::Send);
    bool pack = app.packMode && app.pk.pack;
    bool haveFile = pack ? app.pk.file.has_value() : app.article.has_value();
    bool canSend = !none && !app.profileId.empty() && haveFile && !sending;
    ImGui::BeginDisabled(!canSend);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Amber);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Bg);
    if (ImGui::Button("send", ImVec2(sendW, 0))) {
        if (pack) jobPackSend(app);
        else jobSend(app);
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    if (ui::g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
        const char* why = none ? "add a device first"
                          : app.profileId.empty() ? "choose a device first"
                          : !haveFile ? (pack ? "build the pack first (pack tab, step 4)" : "load an article first")
                          : sending ? "a transfer runs"
                          : pack    ? "send the pack epub to the device"
                                    : "build the file and send it to the device";
        ImGui::SetTooltip("%s", why);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sending);
    if (ImGui::Button("send a file...", ImVec2(fileW, 0))) chooseFilesToSend(app);
    ImGui::EndDisabled();
    if (ui::g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", sending ? "a transfer runs"
                                        : "send any file from disk as it is (for example firmware.bin to / for the "
                                          "reader's SD Card Firmware Update)");
    ImGui::SameLine();
    drawPill(app, pill);
}

} // namespace ks::app
