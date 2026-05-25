find_package(Qt5 REQUIRED COMPONENTS Core Gui Svg Widgets WebSockets Network)
find_package(Qt5 COMPONENTS LinguistTools QUIET)
# DBus 仅在 Linux/Wayland 屏幕捕获后端 (xdg-desktop-portal) 需要
if(UNIX AND NOT APPLE)
    find_package(Qt5 COMPONENTS DBus QUIET)
endif()
