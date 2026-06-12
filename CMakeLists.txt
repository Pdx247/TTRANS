cmake_minimum_required(VERSION 3.16)
project(TTrans VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(OpenGL REQUIRED)
find_package(SDL2 REQUIRED)

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
    target_link_libraries(ttrans PRIVATE ws2_32)
endif()

install(TARGETS ttrans RUNTIME DESTINATION bin BUNDLE DESTINATION .)

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
