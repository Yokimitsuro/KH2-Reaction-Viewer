cmake_minimum_required(VERSION 3.28)
project(KH2ReactionViewer VERSION 0.8.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KH2 Reaction Viewer requires a Windows x64 MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz
    URL_HASH SHA256=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(SDL3 imgui)

add_library(viewer-imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(viewer-imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(viewer-imgui PUBLIC SDL3::SDL3-static)

set(days "${CMAKE_CURRENT_SOURCE_DIR}/third_party/khdays-port")
set(assets "${days}/assets")
add_executable(kh2-rc-viewer WIN32
    src/main.cpp src/cache.cpp src/catalog.cpp src/game_data.cpp src/game_formats.cpp src/reader.cpp src/viewport.cpp
    "${assets}/src/kh2_bar.cpp" "${assets}/src/kh2_mdlx.cpp" "${assets}/src/kh2_motion.cpp"
    "${assets}/src/kh2_map.cpp" "${assets}/src/dds.cpp" "${assets}/src/image.cpp")
target_include_directories(kh2-rc-viewer PRIVATE
    "${assets}/include" "${days}/port/runtime/mods" third_party/stb)
target_compile_options(kh2-rc-viewer PRIVATE /utf-8 /permissive-)
target_compile_definitions(kh2-rc-viewer PRIVATE KH2RC_VERSION="${PROJECT_VERSION}")
target_link_libraries(kh2-rc-viewer PRIVATE viewer-imgui opengl32 bcrypt)
set_target_properties(kh2-rc-viewer PROPERTIES OUTPUT_NAME "KH2 RC Viewer")
add_custom_command(TARGET kh2-rc-viewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ReactionCatalog.tsv" "$<TARGET_FILE_DIR:kh2-rc-viewer>"
    VERBATIM)

install(TARGETS kh2-rc-viewer RUNTIME DESTINATION .)
install(FILES LICENSE THIRD_PARTY_NOTICES.txt README.md ReactionCatalog.tsv
    DESTINATION .)
install(FILES Licenses/OPENKH_LICENSE.txt Licenses/OPENKH_NOTICE.txt DESTINATION Licenses)
install(FILES third_party/khdays-port/LICENSE DESTINATION Licenses RENAME KHDAYS_PORT_LICENSE.txt)
install(FILES third_party/stb/LICENSE DESTINATION Licenses RENAME STB_LICENSE.txt)
