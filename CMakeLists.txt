cmake_minimum_required(VERSION 3.16)
project(hb-download-manager-linux VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)

# Ana GUI Uygulaması
set(SOURCES
    src/main.cpp
    src/core/DownloadItem.cpp
    src/core/SegmentedDownloader.cpp
    src/core/VideoDownloader.cpp
    src/core/DownloadManager.cpp
    src/server/LocalServer.cpp
    src/utils/ClipboardWatcher.cpp
    src/ui/NewDownloadDialog.cpp
    src/ui/DownloadDetailDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/TrayManager.cpp
    resources/resources.qrc
)

add_executable(hb-download-manager ${SOURCES})
target_link_libraries(hb-download-manager PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
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
    Qt6::Core
)
