cmake_minimum_required(VERSION 3.16)

project(kylinlist
        VERSION 1.1.0
        DESCRIPTION "KylinList local tasks, calendar and desktop notes"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(MSVC)
    # Keep CMake's per-compiler Release flags; GCC's -O2 is not valid on MSVC.
    add_compile_options(/utf-8)
endif()

include(CTest)
option(KYLINLIST_BUNDLE_QT "Install Linux executable beside bundled Qt libraries" OFF)
option(KYLINLIST_WITH_DBUS "Build the optional Linux D-Bus adapter" ${UNIX})
option(KYLINLIST_REQUIRE_X11 "Fail if Linux global-hotkey support cannot be built" OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Sql Network Svg)
if(UNIX AND NOT APPLE AND KYLINLIST_WITH_DBUS)
    find_package(Qt5 5.12 REQUIRED COMPONENTS DBus)
endif()
message(STATUS "Qt version: ${Qt5Core_VERSION}; Qt directory: ${Qt5_DIR}")

# Explicit source lists: no recursive glob that silently picks up stale UI/tests.
set(KYLINLIST_CORE_SOURCES
    src/Common.cpp
    src/domain/ViewSpec.cpp
    src/domain/InternalLink.cpp
    src/db/Database.cpp
    src/db/TaskRepository.cpp
    src/app/Settings.cpp
    src/app/AttachmentStore.cpp
    src/app/Journal.cpp
    src/app/TaskStore.cpp
    src/app/ReminderScheduler.cpp
    src/app/SingleInstanceGuard.cpp
    src/platform/Autostart.cpp
    src/platform/InstanceChannel.cpp
)
add_library(kylinlist_core STATIC ${KYLINLIST_CORE_SOURCES})
target_include_directories(kylinlist_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Common.cpp uses QTextDocument, so the domain library also needs QtGui.
target_link_libraries(kylinlist_core PUBLIC Qt5::Core Qt5::Gui Qt5::Sql Qt5::Network)
target_compile_definitions(kylinlist_core PUBLIC
    KYLINLIST_VERSION="${PROJECT_VERSION}")

set(KYLINLIST_UI_SOURCES
    src/app/KylinListApp.cpp
    src/app/NoteManager.cpp
    src/app/TrayController.cpp
    src/platform/ScreenFit.cpp
    src/platform/GlobalHotkey.cpp
    src/ui/NoteWidget.cpp
    src/ui/TitleBar.cpp
    src/ui/ResizeGrip.cpp
    src/ui/ColorTheme.cpp
    src/ui/NoteChecklist.cpp
    src/ui/AttachmentWidget.cpp
    src/ui/design/DesignTokens.cpp
    src/ui/design/ThemeManager.cpp
    src/ui/design/TypeSystem.cpp
    src/ui/design/QssBuilder.cpp
    src/ui/AppIcons.cpp
    src/ui/AppStyle.cpp
    src/ui/Markdown.cpp
    src/ui/EntityHoverPreview.cpp
    src/ui/EntityPicker.cpp
    src/ui/ActivityLogDialog.cpp
    src/ui/MarkdownEditor.cpp
    src/ui/TaskListModel.cpp
    src/ui/TaskDelegate.cpp
    src/ui/TaskDetailWidget.cpp
    src/ui/TaskPanel.cpp
    src/ui/ReminderPopup.cpp
    src/ui/EntityCommands.cpp
    src/ui/DatePickerPopup.cpp
    src/ui/FormatToolbar.cpp
)
if(UNIX AND NOT APPLE AND KYLINLIST_WITH_DBUS)
    list(APPEND KYLINLIST_UI_SOURCES src/app/DbusAdapter.cpp)
endif()
add_library(kylinlist_ui STATIC ${KYLINLIST_UI_SOURCES})
target_include_directories(kylinlist_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kylinlist_ui PUBLIC kylinlist_core Qt5::Widgets Qt5::Svg)
if(UNIX AND NOT APPLE AND KYLINLIST_WITH_DBUS)
    target_link_libraries(kylinlist_ui PUBLIC Qt5::DBus)
    target_compile_definitions(kylinlist_ui PUBLIC KYLINLIST_HAVE_DBUS=1)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Markdown.cpp")
    set(MD4C_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c")
    if(EXISTS "${MD4C_ROOT}/md4c.c")
        set(MD4C_CORE "${MD4C_ROOT}")
        set(MD4C_HTML "${MD4C_ROOT}")
    else()
        set(MD4C_CORE "${MD4C_ROOT}/src")
        set(MD4C_HTML "${MD4C_ROOT}/src")
        if(EXISTS "${MD4C_ROOT}/md4c-html/md4c-html.c")
            set(MD4C_HTML "${MD4C_ROOT}/md4c-html")
        endif()
    endif()
    foreach(source IN ITEMS "${MD4C_CORE}/md4c.c" "${MD4C_HTML}/md4c-html.c" "${MD4C_HTML}/entity.c")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Vendored md4c 0.5.2 source is missing: ${source}")
        endif()
    endforeach()
    add_library(kylinlist_md4c STATIC
        "${MD4C_CORE}/md4c.c" "${MD4C_HTML}/md4c-html.c" "${MD4C_HTML}/entity.c")
    set_target_properties(kylinlist_md4c PROPERTIES AUTOMOC OFF C_STANDARD 99)
    target_include_directories(kylinlist_md4c PUBLIC "${MD4C_CORE}" "${MD4C_HTML}")
    target_link_libraries(kylinlist_ui PUBLIC kylinlist_md4c)
endif()

# X11 hotkeys are a Linux capability, never probe/link host X11 on Windows.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        target_include_directories(kylinlist_ui PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(kylinlist_ui PRIVATE ${X11_LIBRARIES})
        target_compile_definitions(kylinlist_ui PRIVATE KYLINLIST_HAVE_X11=1)
        message(STATUS "X11 global hotkeys enabled")
    elseif(KYLINLIST_REQUIRE_X11)
        message(FATAL_ERROR "X11 development headers/libraries are required (install libx11-dev)")
    else()
        message(WARNING "X11 unavailable: shortcuts only work while a note has focus")
    endif()
endif()

if(WIN32)
    # 正式应用绝不创建控制台窗口。打包脚本用独立启动器检查 --smoke-test 退出码。
    add_executable(kylinlist WIN32 src/main.cpp)
else()
    add_executable(kylinlist src/main.cpp)
endif()
target_link_libraries(kylinlist PRIVATE kylinlist_ui)

if(BUILD_TESTING)
    find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
    # Windows 的 offscreen 平台插件不读系统字体目录，会退化成无字形渲染
    # 并刷 QFontDatabase 警告。显式指向系统字体，让截图与文本度量可信。
    set(KYLINLIST_TEST_ENV "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" KYLINLIST_WIN_FONTS)
        list(APPEND KYLINLIST_TEST_ENV "QT_QPA_FONTDIR=${KYLINLIST_WIN_FONTS}")
    endif()
    foreach(test IN ITEMS tasks ui)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_${test}.cpp")
            message(FATAL_ERROR "BUILD_TESTING requires tests/tst_${test}.cpp")
        endif()
        add_executable(test_${test} "tests/tst_${test}.cpp")
        if(test STREQUAL "tasks")
            target_link_libraries(test_${test} PRIVATE kylinlist_core Qt5::Test Qt5::Widgets)
        else()
            target_link_libraries(test_${test} PRIVATE kylinlist_ui Qt5::Test)
        endif()
        add_test(NAME ${test} COMMAND test_${test})
        set_tests_properties(${test} PROPERTIES
            ENVIRONMENT "${KYLINLIST_TEST_ENV}"
            TIMEOUT 90)
    endforeach()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md")
    install(FILES THIRD_PARTY.md DESTINATION share/doc/kylinlist)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c/LICENSE.md")
    install(FILES third_party/md4c/LICENSE.md DESTINATION share/doc/kylinlist)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Qt-LICENSE.LGPLv3.txt")
    install(FILES packaging/windows/Qt-LICENSE.LGPLv3.txt DESTINATION share/doc/kylinlist)
endif()
if(WIN32)
    install(TARGETS kylinlist RUNTIME DESTINATION .)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md")
        install(FILES THIRD_PARTY.md DESTINATION .)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c/LICENSE.md")
        install(FILES third_party/md4c/LICENSE.md DESTINATION . RENAME md4c-LICENSE.txt)
    endif()
elseif(KYLINLIST_BUNDLE_QT)
    set_target_properties(kylinlist PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
    install(TARGETS kylinlist RUNTIME DESTINATION lib/kylinlist)
    install(PROGRAMS packaging/kylinlist-wrapper.sh DESTINATION bin RENAME kylinlist)
    install(FILES packaging/qt.conf DESTINATION lib/kylinlist)
    target_compile_definitions(kylinlist_core PRIVATE
        KYLINLIST_AUTOSTART_EXEC="${CMAKE_INSTALL_PREFIX}/bin/kylinlist")
else()
    install(TARGETS kylinlist RUNTIME DESTINATION bin)
endif()
if(UNIX AND NOT APPLE)
    install(FILES packaging/KylinList.desktop DESTINATION share/applications)
    install(FILES src/resources/icons/kylinlist.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()
