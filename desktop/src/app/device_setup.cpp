// Device list and the inline add flow of the devices window.
// The list shows the devices that discovery found and the saved profiles.
// The add flow has two steps: 1 test the connection, 2 confirm (send the
// test EPUB). Only a confirmed device gets the compact send strip.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ks::app {

namespace {

// How discovery found the device: UDP reply, HTTP probe or mDNS name.
const char* viaOf(const net::Discovered& d) {
    if (d.raw.rfind("http", 0) == 0) return "http";
    if (d.raw == "mdns") return "mdns";
    return "udp";
}

// A new profile for the add form. The host comes from the selected row.
DeviceProfile newDraft(const net::Discovered* d) {
    DeviceProfile p = net::defaultProfile(FirmwareType::CrossPoint);
    p.name = "X4 Pro";
    p.host = d ? d->ip : "crosspoint.local";
    p.port = 80;
    p.method = TransferMethod::WebDav;
    p.targetFolder = "/Books/K-rosspoint/";
    return p;
}

// Title, message and reasons of a failure, without the fallback buttons.
void compactExplain(const TransferError& e) {
    net::Explanation ex = net::explain(e);
    ImGui::TextColored(theme::Error, "%s", ex.title.c_str());
    ui::Hint(e.message.c_str());
    if (!ex.reasons.empty()) ui::Bullets(ex.reasons);
}

void selectProfile(App& app, const DeviceProfile& p) {
    app.profileId = p.id;
    app.method = p.method;
    app.settings.defaultDeviceId = p.id;
    app.markSettingsChanged();
    app.testResult.reset();
    app.lastError.reset();
    app.toast(ToastKind::Info, "device: " + p.name);
}

void deletePopup(App& app) {
    if (!ImGui::BeginPopupModal("delete device?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string name = app.dev.deleteId;
    for (auto& p : app.profiles)
        if (p.id == app.dev.deleteId) name = p.name + " (" + p.host + ")";
    ImGui::Text("delete device %s?", name.c_str());
    if (ImGui::Button("delete")) {
        try {
            net::deleteProfile(app.dev.deleteId);
            app.addLog("Device deleted: " + name);
        } catch (const std::exception& e) {
            app.addLog(std::string("Delete failed: ") + e.what());
        }
        if (app.settings.defaultDeviceId == app.dev.deleteId) {
            app.settings.defaultDeviceId.clear();
            app.markSettingsChanged();
        }
        if (app.profileId == app.dev.deleteId) app.profileId.clear();
        app.dev.deleteId.clear();
        app.reloadProfiles();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("cancel")) {
        app.dev.deleteId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The list box of discovered devices. Each row: ip, hostname, via, add.
void drawFound(App& app, bool busy) {
    DeviceFlow& f = app.dev;
    if (f.discoverSel >= static_cast<int>(app.discovered.size())) f.discoverSel = -1;
    float rowH = ImGui::GetFrameHeightWithSpacing();
    int rows = std::clamp(static_cast<int>(app.discovered.size()), 2, 5);
    if (!ImGui::BeginListBox("##found", ImVec2(-1.f, rowH * static_cast<float>(rows) + 6.f))) return;
    if (busy) {
        ui::Spinner();
        ImGui::SameLine();
        ImGui::TextDisabled("searching...");
    } else if (app.discovered.empty()) {
        ImGui::TextDisabled(app.discoverDone ? "no device replied" : "press discover");
    }
    float addW = ui::ButtonWidth("add");
    for (size_t i = 0; i < app.discovered.size(); ++i) {
        const net::Discovered& d = app.discovered[i];
        ImGui::PushID(static_cast<int>(i));
        float w = ImGui::GetContentRegionAvail().x - addW - ImGui::GetStyle().ItemSpacing.x;
        bool sel = f.discoverSel == static_cast<int>(i);
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, 0))) {
            f.discoverSel = static_cast<int>(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openDeviceForm(app, newDraft(&d), false);
        }
        ui::Tip(("via " + std::string(viaOf(d)) + (d.raw.empty() ? "" : ": " + d.raw)).c_str());
        // Draw the row text: ip, hostname (clipped), via at the right end.
        ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float sp = ImGui::GetStyle().ItemSpacing.x;
        const char* via = viaOf(d);
        float viaX = r1.x - ImGui::CalcTextSize(via).x - sp;
        float ty = r0.y + (r1.y - r0.y - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(viaX, ty), ImGui::GetColorU32(theme::Dim), via);
        dl->PushClipRect(r0, ImVec2(viaX - sp, r1.y), true);
        dl->AddText(ImVec2(r0.x, ty), ImGui::GetColorU32(theme::Fg), d.ip.c_str());
        float hx = r0.x + ImGui::CalcTextSize(d.ip.c_str()).x + sp * 2.f;
        dl->AddText(ImVec2(hx, ty), ImGui::GetColorU32(sel ? theme::Amber : theme::Dim),
                    d.hostname.empty() ? "-" : d.hostname.c_str());
        dl->PopClipRect();
        ImGui::SameLine();
        if (ImGui::SmallButton("add")) openDeviceForm(app, newDraft(&d), false);
        ImGui::PopID();
    }
    ImGui::EndListBox();
}

// The saved profiles. Each row: name, host, state, then use / edit /
// delete and a "..." popup with the tools (test, send test epub).
void drawSaved(App& app) {
    ui::SectionGap();
    ImGui::TextDisabled("saved devices");
    ImGui::Separator();
    if (app.profiles.empty()) {
        ui::Hint("No saved device yet. Add one from the list above.");
        return;
    }
    bool wantDelete = false;
    bool sending = app.busy(JobKind::Send), testing = app.busy(JobKind::Test);
    for (auto& p : app.profiles) {
        ImGui::PushID(p.id.c_str());
        bool cur = p.id == app.profileId;
        bool ok = !p.lastConnectedAt.empty();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(cur ? theme::Amber : theme::Fg, "%s", p.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", p.host.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ok ? theme::Live : theme::Dim, "%s", ok ? "confirmed" : "unconfirmed");
        ui::RightAlign(ui::ButtonsWidth({"use", "edit", "delete", "..."}, true));
        ImGui::BeginDisabled(cur && ok);
        if (ImGui::SmallButton("use")) {
            if (ok) selectProfile(app, p);
            else openDeviceForm(app, p, true);   // an unconfirmed device needs 1 test, 2 confirm
        }
        ImGui::EndDisabled();
        ui::Tip(ok ? "send to this device" : "test and confirm this device");
        ImGui::SameLine();
        if (ImGui::SmallButton("edit")) openDeviceForm(app, p, true);
        ImGui::SameLine();
        if (ImGui::SmallButton("delete")) {
            app.dev.deleteId = p.id;
            wantDelete = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) ImGui::OpenPopup("tools");
        ui::Tip("tools");
        if (ImGui::BeginPopup("tools")) {
            if (ImGui::MenuItem("test connection", nullptr, false, !testing)) jobTest(app, p, TestTarget::Panel);
            if (ImGui::MenuItem("send test epub", nullptr, false, !sending)) jobSendFileTo(app, p, output::testEpub());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ui::JobRow(app, JobKind::Test);
    ui::JobRow(app, JobKind::Send);
    if (app.testResult && !testing) {
        if (app.testResult->ok) ImGui::TextColored(theme::Live, "connected (%d ms)", app.testResult->latencyMs);
        else if (app.testResult->error) compactExplain(*app.testResult->error);
    }
    if (wantDelete) ImGui::OpenPopup("delete device?");
}

} // namespace

void openDeviceForm(App& app, const DeviceProfile& p, bool editing) {
    app.showDevices = true;
    app.dev.view = DeviceView::Form;
    app.dev.draft = p;
    app.dev.editing = editing;
    app.dev.problems.clear();
    app.dev.test.reset();
    app.dev.testedKey.clear();
    app.dev.browseOpen = false;
    app.dev.newFolderOpen = false;
    app.dev.browseEntries.clear();
    app.dev.browseError.clear();
    app.lastError.reset();
}

void drawDeviceList(App& app) {
    DeviceFlow& f = app.dev;
    bool busy = app.busy(JobKind::Discover);
    // Look for devices once, the first time the list shows with no results.
    if (!f.autoDiscovered && app.discovered.empty() && !busy) {
        f.autoDiscovered = true;
        jobDiscover(app);
        busy = true;
    }
    ui::HeadingText("found on the network");
    char label[32];
    std::snprintf(label, sizeof label, busy ? "discover %c###disc" : "discover###disc", ui::SpinnerChar());
    ui::RightAlign(ui::ButtonWidth("discover |"));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(label, ImVec2(ui::ButtonWidth("discover |"), 0))) jobDiscover(app);
    ImGui::EndDisabled();
    ui::Tip("Find CrossPoint devices on the LAN (UDP port 8134).");
    ImGui::Separator();

    drawFound(app, busy);
    if (app.discoverDone && app.discovered.empty() && !busy)
        ui::Hint("Check that the reader is on Wi-Fi and its file transfer screen is open. "
                 "You can also type the address after add.");

    ui::SectionGap();
    const net::Discovered* sel =
        f.discoverSel >= 0 ? &app.discovered[static_cast<size_t>(f.discoverSel)] : nullptr;
    if (ui::PrimaryButton(sel ? "add the selected device" : "add a device by address", ImGui::GetFrameHeight() * 1.4f))
        openDeviceForm(app, newDraft(sel), false);
    drawSaved(app);
    deletePopup(app);
}

void drawDeviceForm(App& app) {
    DeviceFlow& f = app.dev;
    DeviceProfile& d = f.draft;
    ui::HeadingText(f.editing ? "edit device" : "add device");
    ImGui::Separator();

    bool testing = app.busy(JobKind::Test);
    bool sending = app.busy(JobKind::Send);
    bool testOk = f.test && f.test->ok && f.testedKey == testKey(d);
    ImGui::TextColored(testOk ? theme::Live : theme::Amber, "1 test");
    ImGui::SameLine();
    ImGui::TextColored(testOk ? theme::Amber : theme::Dim, " 2 confirm");

    ImGui::PushItemWidth(-ImGui::GetFontSize() * 4.5f);
    ui::InputStr("name", d.name);
    bool folder = d.method == TransferMethod::Folder;
    ui::InputStrHint(folder ? "drive" : "host", folder ? "E:/" : "crosspoint.local or 192.168.x.x", d.host);
    if (!folder) ImGui::InputInt("port", &d.port, 0, 0);
    ui::EnumCombo("method", d.method, ui::methodItems());
    // CrossPoint HTTP and WebDAV get the folder browser below. The other
    // methods cannot list folders: a text field only.
    bool browse = net::canBrowseRemote(d.method);
    if (!browse) ui::InputStr("folder", d.targetFolder);
    FirmwareType fw = d.firmware;
    if (ui::EnumCombo("firmware", fw, ui::firmwareItems())) {
        DeviceProfile def = net::defaultProfile(fw);
        d.firmware = fw;
        d.port = def.port;
        d.method = def.method;
    }
    if (ImGui::TreeNode("login and more")) {
        ui::InputStr("user name", d.username);
        ui::InputStr("password", d.password, ImGuiInputTextFlags_Password);
        ui::EnumCombo("default format", d.defaultFormat, ui::formatItems());
        ui::InputStrMultiline("notes", d.notes, ImVec2(-ImGui::GetFontSize() * 4.5f, ImGui::GetTextLineHeight() * 3));
        ImGui::TreePop();
    }
    ImGui::PopItemWidth();
    if (folder && ImGui::SmallButton("browse...")) {
        if (auto dir = pickFolderDialog("Choose the reader drive")) d.host = *dir;
    }
    if (browse) drawFolderBrowser(app, d);

    ImGui::Spacing();
    ImGui::BeginDisabled(testing || sending);
    if (ImGui::Button("test")) {
        f.problems = net::validateProfile(d);
        if (f.problems.empty()) {
            // Test a copy without the id: only confirm may mark the device.
            DeviceProfile probe = d;
            probe.id.clear();
            jobTest(app, probe, TestTarget::Form);
        }
    }
    ImGui::EndDisabled();
    ui::Tip("step 1: test the connection");
    ImGui::SameLine();
    ImGui::BeginDisabled(!testOk || sending);
    ImGui::PushStyleColor(ImGuiCol_Button, testOk ? theme::Amber : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_Text, testOk ? theme::Bg : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    if (ImGui::Button("confirm")) {
        f.problems = net::validateProfile(d);
        if (f.problems.empty()) jobConfirmDevice(app, d);
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ui::g_showTooltips)
        ImGui::SetTooltip(testOk ? "step 2: send the test epub and save the device" : "test the connection first");
    ui::RightAlign(ui::ButtonWidth("cancel"));
    if (ImGui::Button("cancel")) {
        f.view = DeviceView::List;
        f.test.reset();
        f.problems.clear();
    }

    ui::JobRow(app, JobKind::Test);
    if (f.test && !testing) {
        if (f.testedKey != testKey(d)) ImGui::TextDisabled("the address changed. test again.");
        else if (f.test->ok) ImGui::TextColored(theme::Live, "connected (%d ms). now confirm.", f.test->latencyMs);
        else if (f.test->error) compactExplain(*f.test->error);
        else ImGui::TextColored(theme::Error, "test failed");
    }
    if (sending) {
        ui::StatePill(app.transfer.state);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", app.transfer.message.c_str());
        ui::JobRow(app, JobKind::Send);
    } else if (app.lastError && testOk) {
        ImGui::TextColored(theme::Error, "the test epub did not arrive");
        compactExplain(*app.lastError);
    }
    if (!f.problems.empty()) ui::Bullets(f.problems, &theme::Error);
}

} // namespace ks::app
