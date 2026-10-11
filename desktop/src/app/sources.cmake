# Agent H / Agent I / Agent J: the ImGui app.
set(KS_APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app_state.cpp
    ${CMAKE_CURRENT_LIST_DIR}/jobs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui.cpp
    ${CMAKE_CURRENT_LIST_DIR}/theme.cpp
    ${CMAKE_CURRENT_LIST_DIR}/fonts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/prefs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keybinds.cpp
    ${CMAKE_CURRENT_LIST_DIR}/chrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/panel_source.cpp
    ${CMAKE_CURRENT_LIST_DIR}/panel_preview.cpp
    ${CMAKE_CURRENT_LIST_DIR}/panel_right.cpp
    ${CMAKE_CURRENT_LIST_DIR}/topbar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/uitest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device_setup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/layout.cpp
    ${CMAKE_CURRENT_LIST_DIR}/win_library.cpp
    ${CMAKE_CURRENT_LIST_DIR}/win_settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/settings_appearance.cpp
    ${CMAKE_CURRENT_LIST_DIR}/win_devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/win_log.cpp
    ${CMAKE_CURRENT_LIST_DIR}/clipboard_win.cpp
    ${CMAKE_CURRENT_LIST_DIR}/dialogs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/cli.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pack_jobs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pack_wizard.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pack_select.cpp
    ${CMAKE_CURRENT_LIST_DIR}/panel_pack.cpp
    ${CMAKE_CURRENT_LIST_DIR}/win_library_packs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/folder_browser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/send_file.cpp
)

# The app target has no warning flags of its own. Turn them on for these
# files only, so a warning in the app shows in every build.
if(NOT MSVC)
    set_source_files_properties(${KS_APP_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
endif()
