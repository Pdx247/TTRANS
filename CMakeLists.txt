cmake_minimum_required(VERSION 3.16)
project(TTrans VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(OpenGL REQUIRED)
find_package(SDL2 QUIET)

if(NOT SDL2_FOUND)
    set(_sdl_prefix "$ENV{CMAKE_PREFIX_PATH}")
    if(WIN32 AND NOT _sdl_prefix)
        set(_sdl_prefix "C:/msys64/mingw64")
    endif()
    find_path(SDL2_INCLUDE_DIRS
        NAMES SDL.h
        PATHS "${_sdl_prefix}/include/SDL2" "${_sdl_prefix}/include"
        NO_DEFAULT_PATH
    )
    find_library(SDL2_LIBRARIES
        NAMES SDL2
        PATHS "${_sdl_prefix}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT SDL2_INCLUDE_DIRS OR NOT SDL2_LIBRARIES)
        message(FATAL_ERROR "SDL2 was not found. Install SDL2 development files or set CMAKE_PREFIX_PATH.")
    endif()
endif()

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.9.zip
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)

if(TARGET SDL2::SDL2)
    target_link_libraries(imgui PUBLIC SDL2::SDL2)
else()
    target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES})
endif()

add_executable(ttrans
    src/main.cpp
    src/protocol.cpp
    src/socket.cpp
    src/transfer.cpp
    src/imgui_gui.cpp
)

target_include_directories(ttrans PRIVATE include)
target_link_libraries(ttrans PRIVATE imgui OpenGL::GL)

if(WIN32)
    set_source_files_properties(assets/ttrans.rc PROPERTIES COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/assets")
    target_sources(ttrans PRIVATE assets/ttrans.rc)
    target_link_libraries(ttrans PRIVATE ws2_32)
endif()

add_executable(ttrans-gui
    src/gui_main.cpp
    src/protocol.cpp
    src/socket.cpp
    src/transfer.cpp
    src/imgui_gui.cpp
)

target_include_directories(ttrans-gui PRIVATE include)
target_link_libraries(ttrans-gui PRIVATE imgui OpenGL::GL)

if(WIN32)
    set_target_properties(ttrans-gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(ttrans-gui PRIVATE assets/ttrans.rc)
    target_link_libraries(ttrans-gui PRIVATE ws2_32 shell32)
endif()

install(TARGETS ttrans ttrans-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)

set(CPACK_PACKAGE_NAME "TTrans")
set(CPACK_PACKAGE_VENDOR "Pdx247")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight UDP LAN file transfer with CLI and compact Dear ImGui GUI")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
