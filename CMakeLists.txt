cmake_minimum_required(VERSION 3.10)

project(airan-desk VERSION 0.1.0)

include(GNUInstallDirs)

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

if(MSVC)
    add_definitions(-DUNICODE -D_UNICODE)
    add_compile_options(/utf-8)
endif()

set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Enable automoc, autouic, and autorcc for Qt project
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Detect 64-bit vs 32-bit
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_64 TRUE)
else()
    set(ARCH_64 FALSE)
endif()

# Dependencies
include(cmake/deps/Qt5.cmake)
include(cmake/deps/OpenSSL.cmake)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Third-party dependencies
include(cmake/deps/Spdlog.cmake)
include(cmake/deps/LibDataChannel.cmake)
include(cmake/deps/LibVTerm.cmake)
include(cmake/deps/FFmpeg.cmake)
include(cmake/ProjectSources.cmake)

set(QM_FILES)
if(Qt5LinguistTools_FOUND)
    qt5_add_translation(QM_FILES ${TS_FILES})
    add_custom_target(translations ALL DEPENDS ${QM_FILES})
else()
    message(WARNING "Qt5 LinguistTools not found; application translation .qm files will not be generated automatically")
endif()

if(WIN32)
    list(APPEND EXTRA_LIBS
        advapi32 bcrypt ole32 shell32 psapi opengl32 gdi32 secur32 crypt32 user32 wtsapi32 winmm
    )
    message(STATUS "Windows capture backends are built as runtime-loaded plugins")
    list(APPEND HDR_FILES
        src/desktop/desktop_grab_wgc.h
    )
    list(APPEND SRC_FILES
        src/codec/h264_encoder_zerocopy_encode_win.cpp
        src/codec/h264_encoder_zerocopy_frame_win.cpp
        src/codec/h264_encoder_zerocopy_session_win.cpp
        src/util/input_util_events_win.cpp
    )
elseif(APPLE)
    list(APPEND EXTRA_LIBS
        "-framework Carbon" "-framework CoreGraphics"
        "-framework AudioToolbox" "-framework VideoToolbox" "-framework CoreVideo"
        "-framework CoreMedia" "-framework AVFoundation" "-framework CoreFoundation"
        "-framework Security" "-framework QuartzCore" "-framework OpenGL"
        util
    )
    list(APPEND SRC_FILES
        src/util/input_util_events_macos.cpp
    )
elseif(UNIX)
    # 可选：在 Linux 下静态链接 libstdc++/libgcc，默认关闭
    option(STATIC_LINK_LIBSTDCPP "Link libstdc++ and libgcc statically on Linux" ON)

    # 注意：input_util.cpp 已改用 uinput（内核虚拟设备），不再依赖 Xtst（XTest）
    list(APPEND EXTRA_LIBS
        pthread dl rt m X11 Xext Xfixes Xv z asound util
    )
    list(APPEND SRC_FILES
        src/util/input_util_events_linux.cpp
    )

    # Wayland 截屏：xdg-desktop-portal + PipeWire
    # 仅在系统提供 libpipewire-0.3 与 Qt5::DBus 时启用；缺失则降级到 X11/Qt 截屏
    find_package(PkgConfig QUIET)
    set(AIRAN_PIPEWIRE_FOUND FALSE)
    if(PkgConfig_FOUND AND TARGET Qt5::DBus)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
        if(PIPEWIRE_FOUND)
            set(AIRAN_PIPEWIRE_FOUND TRUE)
            message(STATUS "PipeWire screen-capture backend enabled (libpipewire-0.3 v${PIPEWIRE_VERSION})")
            list(APPEND SRC_FILES
                src/desktop/desktop_grab_pipewire.cpp
            )
            list(APPEND HDR_FILES
                src/desktop/desktop_grab_pipewire.h
            )
            # 优先使用 *_LINK_LIBRARIES（绝对路径），避免依赖 target_link_directories（需 CMake 3.13+）
            if(PIPEWIRE_LINK_LIBRARIES)
                list(APPEND EXTRA_LIBS ${PIPEWIRE_LINK_LIBRARIES})
            else()
                list(APPEND EXTRA_LIBS ${PIPEWIRE_LIBRARIES})
            endif()
        else()
            message(STATUS "PipeWire screen-capture backend disabled (libpipewire-0.3 not found)")
        endif()
    else()
        message(STATUS "PipeWire screen-capture backend disabled (PkgConfig or Qt5::DBus missing)")
    endif()
else()
    list(APPEND SRC_FILES
        src/util/input_util_events_fallback.cpp
    )
endif()

# Windows UAC manifest（requireAdministrator）—— 嵌入资源文件以避免 UIPI 阻断 SendInput
if(WIN32)
    set(RC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resource/windows/main.rc")
    # RC 编译时需要能找到 uac.manifest，指定资源目录作为 include 路径
    if(MSVC)
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}/resource/windows\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resource/icons/windows\"")
    else()
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I\"${CMAKE_CURRENT_SOURCE_DIR}/resource/windows\" -I\"${CMAKE_CURRENT_SOURCE_DIR}/resource/icons/windows\"")
    endif()
endif()

# Add executable
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RESOURCE_FILES}
        ${RC_FILE}
    )
elseif(APPLE)
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RESOURCE_FILES}
        ${RC_FILE}
    )
else()
    add_executable(${PROJECT_NAME}
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RESOURCE_FILES}
        ${RC_FILE}
    )
endif()

if(TARGET translations)
    add_dependencies(${PROJECT_NAME} translations)
endif()

if(MSVC)
    # main.rc 已嵌入 MANIFEST，关闭链接器自动生成以避免 CVT1100 重复资源
    set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS " /MANIFEST:NO")
endif()

# 立即为主可执行文件设置输出目录，确保不会落到 third_party/bin
if(UNIX AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
    )
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
    )
endif()

# Include directories
target_include_directories(${PROJECT_NAME}
    PRIVATE
    src
    ${FFMPEG_INCLUDE_DIR}
    ${LIBDATACHANNEL_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Svg
    Qt5::Widgets
    Qt5::WebSockets
    spdlog
    datachannel
    vterm
    ${FFMPEG_LINK_LIBRARIES}
    ${EXTRA_LIBS}
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
    AIRAN_DESK_VERSION="${PROJECT_VERSION}"
)

if(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        AIRAN_DESK_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/airan-desk"
    )
    if(AIRAN_PIPEWIRE_FOUND)
        target_compile_definitions(${PROJECT_NAME} PRIVATE AIRAN_HAS_PIPEWIRE)
        target_include_directories(${PROJECT_NAME} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::DBus)
        if(PIPEWIRE_CFLAGS_OTHER)
            target_compile_options(${PROJECT_NAME} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
        endif()
    endif()
endif()

if(WIN32)
    add_library(airan_capture_wgc SHARED
        src/desktop/desktop_grab_wgc.cpp
        src/desktop/desktop_grab_wgc.h
        src/desktop/desktop_grab_wgc_plugin.cpp
        src/desktop/desktop_capture_plugin.h
        src/desktop/desktop_grab.h
        src/common/logger_manager.cpp
        src/common/logger_manager.h
        src/util/config_util.cpp
        src/util/config_util.h
        src/util/convert_util.cpp
        src/util/convert_util.h
        src/util/i18n_util.cpp
        src/util/i18n_util.h
    )
    target_include_directories(airan_capture_wgc PRIVATE
        src
        ${LIBDATACHANNEL_INCLUDE_DIR}
    )
    target_link_libraries(airan_capture_wgc PRIVATE
        Qt5::Core
        Qt5::Gui
        Qt5::Network
        spdlog
        datachannel
        d3d11
        dxgi
        dxguid
        windowsapp
        ole32
    )

    foreach(_capture_plugin airan_capture_wgc)
        set_target_properties(${_capture_plugin} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/release"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/release"
        )
        add_dependencies(${PROJECT_NAME} ${_capture_plugin})
    endforeach()
endif()

if(MSVC AND NOT ARCH_64)
    set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS " /SAFESEH:NO")
elseif(UNIX AND NOT APPLE AND STATIC_LINK_LIBSTDCPP)
    message(STATUS "Static linking libstdc++ and libgcc enabled for ${PROJECT_NAME}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS " -static-libstdc++ -static-libgcc")
    endif()
endif()

set(CMAKE_SKIP_RPATH OFF)

# 设置 RPATH，让程序优先从相对路径查找动态库
if(UNIX AND NOT APPLE)
    # Linux: 设置 RPATH 为 $ORIGIN（可执行文件所在目录）
    # 因为库文件会被拷贝到可执行文件目录，所以只需要 $ORIGIN
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# 设置可执行文件输出目录（兼容单/多配置）
if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/release"
    )
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/debug")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
    endif()
endif()

if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "airan-desk"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.wxalh.airan-desk"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

# Windows: 拷贝 DLL 到输出目录（如果存在）
if(WIN32)
    # 拷贝 FFmpeg DLL（如果存在）
    file(GLOB _FFMPEG_DLLS "${FFMPEG_BINARY_DIR}/*.dll")
    if(_FFMPEG_DLLS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_FFMPEG_DLLS}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endif()

    # 拷贝 OpenSSL DLL（如果存在）
    if(OPENSSL_ROOT_DIR)
        file(GLOB _OPENSSL_DLLS
            "${OPENSSL_ROOT_DIR}/bin/*.dll"
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll"
            "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll"
        )
        if(_OPENSSL_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_OPENSSL_DLLS}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            )
        endif()
    endif()

    # 拷贝 Qt DLL（使用 windeployqt 工具自动部署）
    get_target_property(_qt5_qmake_location Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(_qt5_install_prefix "${_qt5_qmake_location}" DIRECTORY)
    get_filename_component(_qt5_install_prefix "${_qt5_install_prefix}" DIRECTORY)
    set(_qt5_windeployqt "${_qt5_install_prefix}/bin/windeployqt.exe")

    if(EXISTS "${_qt5_windeployqt}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${_qt5_windeployqt}"
            "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "正在使用 windeployqt 部署 Qt 依赖..."
        )
    else()
        message(WARNING "未找到 windeployqt 工具, Qt DLL 需要手动拷贝")
    endif()
endif()

if(APPLE)
    get_target_property(_qt5_qmake_location Qt5::qmake IMPORTED_LOCATION)
    if(_qt5_qmake_location)
        get_filename_component(_qt5_install_prefix "${_qt5_qmake_location}" DIRECTORY)
        get_filename_component(_qt5_install_prefix "${_qt5_install_prefix}" DIRECTORY)
        set(_qt5_macdeployqt "${_qt5_install_prefix}/bin/macdeployqt")

        if(EXISTS "${_qt5_macdeployqt}")
            add_custom_target(mac_bundle
                COMMAND "${_qt5_macdeployqt}" "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>" -verbose=1
                DEPENDS ${PROJECT_NAME}
                COMMENT "正在使用 macdeployqt 部署 macOS app bundle..."
            )
        else()
            message(WARNING "未找到 macdeployqt 工具，macOS .app 依赖需要手动部署")
        endif()
    endif()
endif()

# Copy spdlog and datachannel shared libraries to the executable folder (if built as shared targets)
if(TARGET spdlog AND SPDLOG_BUILD_SHARED)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:spdlog>
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()

if(TARGET datachannel AND BUILD_SHARED_LIBS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:datachannel>
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resource/locale"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/locale"
)

if(QM_FILES)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/locale"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${QM_FILES}
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/locale"
    )
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resource/conf"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/conf"
)

if(UNIX AND NOT APPLE)
    set(UDEV_RULES_INSTALL_DIR "/etc/udev/rules.d" CACHE PATH "Install directory for udev rule files")

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resource/linux/airan-desk.desktop.in"
        "${CMAKE_CURRENT_BINARY_DIR}/airan-desk.desktop"
        @ONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resource/linux/airan-desk-autostart.desktop.in"
        "${CMAKE_CURRENT_BINARY_DIR}/airan-desk-autostart.desktop"
        @ONLY
    )

    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/airan-desk.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/airan-desk-autostart.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/airan-desk
    )
    install(FILES resource/linux/60-airan-desk-uinput.rules
        DESTINATION ${UDEV_RULES_INSTALL_DIR}
    )
    install(DIRECTORY resource/icons/hicolor/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor
        FILES_MATCHING PATTERN "*.png"
    )
    install(DIRECTORY resource/conf/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/airan-desk/conf
        FILES_MATCHING
            PATTERN "*.ini"
            PATTERN "*.qss"
    )
    install(FILES resource/locale/qtbase_zh_CN.qm
        DESTINATION ${CMAKE_INSTALL_DATADIR}/airan-desk/locale
    )
    if(QM_FILES)
        install(FILES ${QM_FILES}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/airan-desk/locale
        )
    endif()
endif()
