# Agent-owned source list. Add .cpp files here.
set(KS_NET_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/webdav.cpp
    ${CMAKE_CURRENT_LIST_DIR}/crosspoint_http.cpp
    ${CMAKE_CURRENT_LIST_DIR}/crosspoint_ws.cpp
    ${CMAKE_CURRENT_LIST_DIR}/folder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ble_protocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ble_transport.cpp
    ${CMAKE_CURRENT_LIST_DIR}/explain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/discovery.cpp
    ${CMAKE_CURRENT_LIST_DIR}/storage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/store_json.cpp
    ${CMAKE_CURRENT_LIST_DIR}/packs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/remote_folder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/helper_server.cpp
    ${CMAKE_CURRENT_LIST_DIR}/file_send.cpp
)

# Bluetooth LE link: C++/WinRT on Windows (needs the MSYS2 package
# mingw-w64-x86_64-cppwinrt; this one file compiles as C++20), a stub elsewhere.
if(WIN32)
    list(APPEND KS_NET_SOURCES ${CMAKE_CURRENT_LIST_DIR}/ble_win.cpp)
    if(NOT MSVC)
        set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ble_win.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
    else()
        set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ble_win.cpp PROPERTIES COMPILE_OPTIONS "/std:c++20")
    endif()
    set(KS_NET_LIBS runtimeobject ole32 oleaut32)
else()
    list(APPEND KS_NET_SOURCES ${CMAKE_CURRENT_LIST_DIR}/ble_stub.cpp)
    set(KS_NET_LIBS)
endif()
