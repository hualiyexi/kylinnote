cmake_minimum_required(VERSION 3.16)

project(kylinnote
        VERSION 1.0.2
        DESCRIPTION "Windows 7 风格桌面便笺 (银河麒麟 V10 SP1 ARM64)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(POLICY CMP0071)
    cmake_policy(SET CMP0071 NEW)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Qt 5.12.8：Core / Gui / Widgets / Sql / DBus
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Sql DBus)

message(STATUS "Qt5 版本: ${Qt5_VERSION}")
message(STATUS "Qt5 目录: ${Qt5_DIR}")

add_executable(kylinnote
    src/main.cpp
    src/Common.cpp
    src/app/KylinNoteApp.cpp
    src/app/SingleInstanceGuard.cpp
    src/app/DbusAdapter.cpp
    src/app/NoteManager.cpp
    src/app/TrayController.cpp
    src/app/Settings.cpp
    src/db/Database.cpp
    src/db/NoteDao.cpp
    src/ui/NoteWidget.cpp
    src/ui/NoteEditor.cpp
    src/ui/TitleBar.cpp
    src/ui/ResizeGrip.cpp
    src/ui/ColorTheme.cpp
    src/ui/ConfirmDeleteDialog.cpp
    src/platform/Autostart.cpp
    src/platform/ScreenFit.cpp
    src/platform/GlobalHotkey.cpp
)

target_include_directories(kylinnote PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(kylinnote PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::DBus
)

# X11 全局热键（Ctrl+N / Ctrl+D / F12），用 Xlib 的 XGrabKey 实现。
# 拿不到 libX11 时程序仍可编译运行，只是快捷键退化为"便签窗口激活时生效"。
find_path(X11_INCLUDE_DIR X11/Xlib.h)
find_library(X11_LIBRARY NAMES X11)
if(X11_INCLUDE_DIR AND X11_LIBRARY)
    target_include_directories(kylinnote PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(kylinnote PRIVATE ${X11_LIBRARY})
    target_compile_definitions(kylinnote PRIVATE KYLINNOTE_HAVE_X11)
    message(STATUS "X11 全局热键: 已启用 (${X11_LIBRARY})")
else()
    message(WARNING "未找到 libX11 头文件/库，快捷键退化为窗口内生效")
endif()

# 自包含模式：Qt 运行库与插件随包携带，用于内网/离线部署
option(KYLINNOTE_BUNDLE_QT "随包携带 Qt 运行库（内网离线部署用）" OFF)

if(KYLINNOTE_BUNDLE_QT)
    # 真身与 Qt 库同目录，用 $ORIGIN 定位；/usr/bin/kylinnote 是启动脚本
    set_target_properties(kylinnote PROPERTIES
        INSTALL_RPATH "\$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    install(TARGETS kylinnote RUNTIME DESTINATION lib/kylinnote)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/kylinnote-wrapper.sh
            DESTINATION bin RENAME kylinnote)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/qt.conf
            DESTINATION lib/kylinnote)
    # 开机自启要用 /usr/bin 下的启动脚本，否则拿不到随包库路径
    target_compile_definitions(kylinnote PRIVATE
        KYLINNOTE_AUTOSTART_EXEC="/usr/bin/kylinnote")
    message(STATUS "自包含模式：Qt 运行库将随包携带（由 packaging/build-deb.sh 收集）")
else()
    install(TARGETS kylinnote RUNTIME DESTINATION bin)
endif()

# 安装规则：packaging/build-deb.sh 通过 DESTDIR 收集到打包暂存目录
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/KylinNote.desktop
        DESTINATION share/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons/kylinnote.svg
        DESTINATION share/icons/hicolor/scalable/apps)
