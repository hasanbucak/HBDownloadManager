cmake_minimum_required(VERSION 3.16)
project(hb-download-manager-linux VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)

find_package(Qt6 COMPONENTS Core Gui Widgets Network QUIET)
if (NOT Qt6_FOUND)
    find_package(Qt5 COMPONENTS Core Gui Widgets Network REQUIRED)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()

# Ana GUI Uygulaması
set(SOURCES
    src/main.cpp
    src/core/DownloadItem.cpp
    src/core/RangePlanner.cpp
    src/core/SegmentedDownloader.cpp
    src/core/VideoDownloader.cpp
    src/core/DownloadManager.cpp
    src/server/LocalServer.cpp
    src/utils/PathUtils.cpp
    src/utils/ClipboardWatcher.cpp
    src/ui/NewDownloadDialog.cpp
    src/ui/DownloadDetailDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/TrayManager.cpp
    resources/resources.qrc
)

add_executable(hb-download-manager ${SOURCES})
target_link_libraries(hb-download-manager PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

target_include_directories(hb-download-manager PRIVATE
    src
    src/core
    src/server
    src/ui
    src/utils
)

# Tarayıcı Native Messaging Host İkilisi
add_executable(hbdm-native-host src/native_host/native_host_main.cpp)
target_link_libraries(hbdm-native-host PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
)

# Kurulum Kuralları (Install rules)
install(TARGETS hb-download-manager hbdm-native-host
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES hb-download-manager.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

install(FILES resources/app_icon.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
    RENAME hb-download-manager.png
)
