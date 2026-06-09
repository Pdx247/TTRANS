cmake_minimum_required(VERSION 3.16)
project(TTrans VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ttrans
    src/main.cpp
    src/protocol.cpp
    src/socket.cpp
    src/transfer.cpp
    src/web_gui.cpp
)

target_include_directories(ttrans PRIVATE include)

if(WIN32)
    target_link_libraries(ttrans PRIVATE ws2_32)
endif()

install(TARGETS ttrans RUNTIME DESTINATION bin BUNDLE DESTINATION .)

set(CPACK_PACKAGE_NAME "TTrans")
set(CPACK_PACKAGE_VENDOR "Pdx247")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight UDP LAN file transfer with CLI and compact web GUI")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
