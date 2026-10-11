cmake_minimum_required(VERSION 3.20)
project(krosspoint_desktop VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(KS_BUILD_TESTS "Build the unit tests" ON)
option(KS_BUILD_APP   "Build the ImGui app (needs SDL2 + OpenGL)" ON)

set(TP ${PROJECT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------- system libs
find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- vendored libs
add_library(ks_miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(ks_miniz PUBLIC ${TP}/miniz)
target_compile_definitions(ks_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_library(ks_md4c STATIC ${TP}/md4c/md4c.c ${TP}/md4c/md4c-html.c ${TP}/md4c/entity.c)
target_include_directories(ks_md4c PUBLIC ${TP}/md4c)

add_library(ks_stb STATIC ${PROJECT_SOURCE_DIR}/src/core/stb_impl.cpp)
target_include_directories(ks_stb PUBLIC ${TP}/stb)

add_library(ks_json INTERFACE)
target_include_directories(ks_json INTERFACE ${TP}/json)

add_library(ks_httplib INTERFACE)
target_include_directories(ks_httplib INTERFACE ${TP}/httplib)
if(WIN32)
    target_link_libraries(ks_httplib INTERFACE ws2_32 crypt32)
endif()

# ---------------------------------------------------------------- core (no UI)
# Each module owner adds their sources to the matching variable in
# src/<module>/sources.cmake. The lead wires the files here.
include(${PROJECT_SOURCE_DIR}/src/extract/sources.cmake)   # Agent E
include(${PROJECT_SOURCE_DIR}/src/output/sources.cmake)    # Agent F
include(${PROJECT_SOURCE_DIR}/src/net/sources.cmake)       # Agent G
include(${PROJECT_SOURCE_DIR}/src/spider/sources.cmake)    # Agent N

add_library(ks_core STATIC
    ${PROJECT_SOURCE_DIR}/src/core/util.cpp
    ${KS_EXTRACT_SOURCES}
    ${KS_OUTPUT_SOURCES}
    ${KS_NET_SOURCES}
    ${KS_SPIDER_SOURCES}
)
target_include_directories(ks_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(ks_core PUBLIC
    CURL::libcurl LibXml2::LibXml2 ZLIB::ZLIB Threads::Threads
    ks_miniz ks_md4c ks_stb ks_json ks_httplib)
if(WIN32)
    target_link_libraries(ks_core PUBLIC ws2_32 iphlpapi ${KS_NET_LIBS})
    target_compile_definitions(ks_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ks_core PRIVATE /W3 /utf-8)
else()
    target_compile_options(ks_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- app
if(KS_BUILD_APP)
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    add_library(ks_imgui STATIC
        ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
        ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_demo.cpp
        ${TP}/imgui/backends/imgui_impl_sdl2.cpp ${TP}/imgui/backends/imgui_impl_opengl3.cpp)
    target_include_directories(ks_imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ks_imgui PUBLIC SDL2::SDL2 OpenGL::GL)
    else()
        target_include_directories(ks_imgui PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(ks_imgui PUBLIC ${SDL2_LIBRARIES} OpenGL::GL)
    endif()

    include(${PROJECT_SOURCE_DIR}/src/app/sources.cmake)    # Agent H
    add_executable(krosspoint_sender WIN32 ${KS_APP_SOURCES})
    target_link_libraries(krosspoint_sender PRIVATE ks_core ks_imgui)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(krosspoint_sender PRIVATE SDL2::SDL2main)
    endif()
    if(WIN32)
        # GetOpenFileNameW / GetSaveFileNameW (dialogs.cpp, "send a file").
        target_link_libraries(krosspoint_sender PRIVATE comdlg32)
    endif()
    if(WIN32 AND MINGW)
        target_link_options(krosspoint_sender PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(NOT WIN32)
        # No WIN32 flag on other platforms; keep a console entry point.
        set_target_properties(krosspoint_sender PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
endif()

# ---------------------------------------------------------------- tests
if(KS_BUILD_TESTS)
    enable_testing()
    include(${PROJECT_SOURCE_DIR}/tests/tests.cmake)
endif()
