cmake_minimum_required(VERSION 3.16)

# One version for everything: the VERSION file (checked against the extension by scripts/check_version.sh)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HBDM_VERSION_RAW)
string(STRIP "${HBDM_VERSION_RAW}" HBDM_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(hb-download-manager-linux VERSION ${HBDM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)

find_package(Qt6 COMPONENTS Core Gui Widgets Network QUIET)
if (NOT Qt6_FOUND)
    if (Qt6_DIR)
        # QUIET hides the reason; the usual one on Ubuntu/Debian is Qt6Gui needing the OpenGL headers
        message(WARNING "Qt6 was found at ${Qt6_DIR} but could not be loaded. "
                        "If the reason is OpenGL, install it: sudo apt install libgl-dev. Trying Qt5 instead.")
    endif()
    find_package(Qt5 COMPONENTS Core Gui Widgets Network REQUIRED)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()
# A Qt outside the system library path (e.g. ~/Qt/<ver>/gcc_64) must be found at run time from
# the build tree too; CMake did not add it to the build RPATH on its own
get_target_property(HBDM_QT_CORE_LOCATION Qt${QT_VERSION_MAJOR}::Core LOCATION)
if (HBDM_QT_CORE_LOCATION)
    get_filename_component(HBDM_QT_LIB_DIR "${HBDM_QT_CORE_LOCATION}" DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH "${HBDM_QT_LIB_DIR}")
endif()

# lrelease / lupdate (optional: without it the app simply has no English catalog)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS LinguistTools QUIET)

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
    src/utils/MediaHosts.cpp
    src/utils/ClipboardWatcher.cpp
    src/ui/NewDownloadDialog.cpp
    src/ui/DownloadDetailDialog.cpp
    src/ui/SettingsDialog.cpp
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
target_compile_definitions(hb-download-manager PRIVATE HBDM_VERSION="${HBDM_VERSION}")

# Translations: Turkish is the source language; translations/hbdm_en.ts is the English catalog.
set(HBDM_TS_FILES translations/hbdm_en.ts)
set(HBDM_QM_FILES "")
if (Qt${QT_VERSION_MAJOR}LinguistTools_FOUND)
    if (QT_VERSION_MAJOR EQUAL 6 AND COMMAND qt_add_translations)
        # Embedded in the binary under :/i18n/ (Qt 6.2 or newer)
        qt_add_translations(hb-download-manager
            TS_FILES ${HBDM_TS_FILES}
            RESOURCE_PREFIX /i18n
        )
        # Embedded: nothing to install (QM_FILES_OUTPUT_VARIABLE cannot be combined with RESOURCE_PREFIX)
    else()
        # Qt 5 / early Qt 6: build the .qm next to the executable (main.cpp looks in
        # <app dir>/translations) and install it
        if (QT_VERSION_MAJOR EQUAL 6)
            qt6_add_translation(HBDM_QM_FILES ${HBDM_TS_FILES})
        else()
            qt5_add_translation(HBDM_QM_FILES ${HBDM_TS_FILES})
        endif()
        add_custom_target(hbdm_translations ALL DEPENDS ${HBDM_QM_FILES})
        add_dependencies(hb-download-manager hbdm_translations)
        add_custom_command(TARGET hb-download-manager POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hb-download-manager>/translations"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HBDM_QM_FILES} "$<TARGET_FILE_DIR:hb-download-manager>/translations/"
            VERBATIM
        )
    endif()
else()
    message(STATUS "Qt LinguistTools not found: building without the English translation")
endif()

# Tarayıcı Native Messaging Host İkilisi
add_executable(hbdm-native-host src/native_host/native_host_main.cpp)
target_link_libraries(hbdm-native-host PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
)
target_include_directories(hbdm-native-host PRIVATE
    src/server
    src/utils
)

# ---------------------------------------------------------------------------------------------
# Tests: every test is registered with CTest (QT_QPA_PLATFORM=offscreen). The binaries and
# test_server.py end up in the same build directory, where test_engine_integration finds it.
# ---------------------------------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test QUIET)
    if(Qt${QT_VERSION_MAJOR}Test_FOUND)
        configure_file(tests/test_server.py "${CMAKE_CURRENT_BINARY_DIR}/test_server.py" COPYONLY)

        # hbdm_add_test(<name> SOURCES <files...> [QT_LIBS <Qt components...>] [VERSION_DEFINE])
        function(hbdm_add_test name)
            cmake_parse_arguments(T "VERSION_DEFINE" "" "SOURCES;QT_LIBS" ${ARGN})
            add_executable(${name} ${T_SOURCES})
            set(libs Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Test)
            foreach(comp ${T_QT_LIBS})
                list(APPEND libs Qt${QT_VERSION_MAJOR}::${comp})
            endforeach()
            target_link_libraries(${name} PRIVATE ${libs})
            target_include_directories(${name} PRIVATE src src/core src/server src/ui src/utils tests)
            if(T_VERSION_DEFINE)
                target_compile_definitions(${name} PRIVATE HBDM_VERSION="${HBDM_VERSION}")
            endif()
            add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
            set_tests_properties(${name} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 900)
        endfunction()

        hbdm_add_test(test_rangeplanner
            SOURCES tests/test_rangeplanner.cpp src/core/RangePlanner.cpp)

        hbdm_add_test(test_range_validation
            SOURCES tests/test_range_validation.cpp
                    src/core/SegmentedDownloader.cpp src/core/RangePlanner.cpp
                    src/core/DownloadItem.cpp src/utils/PathUtils.cpp
            QT_LIBS Network)

        hbdm_add_test(test_pathutils
            SOURCES tests/test_pathutils.cpp src/utils/PathUtils.cpp src/utils/MediaHosts.cpp)

        hbdm_add_test(test_json_roundtrip
            SOURCES tests/test_json_roundtrip.cpp src/core/DownloadItem.cpp src/utils/PathUtils.cpp)

        hbdm_add_test(test_bridge_auth
            SOURCES tests/test_bridge_auth.cpp tests/mock_downloaders.cpp
                    src/utils/PathUtils.cpp src/utils/MediaHosts.cpp
                    src/core/DownloadItem.cpp src/core/DownloadManager.cpp
                    src/server/LocalServer.cpp
                    src/core/SegmentedDownloader.h src/core/VideoDownloader.h
            QT_LIBS Network Widgets
            VERSION_DEFINE)

        hbdm_add_test(test_download_lifecycle
            SOURCES tests/test_download_lifecycle.cpp tests/mock_downloaders.cpp
                    src/utils/PathUtils.cpp src/utils/MediaHosts.cpp
                    src/core/DownloadItem.cpp src/core/DownloadManager.cpp
                    src/core/SegmentedDownloader.h src/core/VideoDownloader.h
            QT_LIBS Network Widgets)

        hbdm_add_test(test_engine_integration
            SOURCES tests/test_engine_integration.cpp
                    src/core/SegmentedDownloader.cpp src/core/RangePlanner.cpp
                    src/core/DownloadItem.cpp src/utils/PathUtils.cpp
            QT_LIBS Network)
    endif()

    # Not Qt tests, but part of the same gate
    find_program(HBDM_PYTHON python3)
    if(HBDM_PYTHON)
        add_test(NAME test_server_selftest
            COMMAND ${HBDM_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server.py" --self-test)
    endif()
    find_program(HBDM_BASH bash)
    if(HBDM_BASH)
        add_test(NAME check_version
            COMMAND ${HBDM_BASH} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_version.sh")
    endif()
    find_program(HBDM_NODE node)
    if(HBDM_NODE)
        file(GLOB HBDM_EXTENSION_JS "${CMAKE_CURRENT_SOURCE_DIR}/extension/*.js")
        foreach(js ${HBDM_EXTENSION_JS})
            get_filename_component(js_name ${js} NAME)
            add_test(NAME node_check_${js_name} COMMAND ${HBDM_NODE} --check "${js}")
        endforeach()
    endif()
endif()

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

# English catalog for installs where it is not embedded (main.cpp also looks here)
if(HBDM_QM_FILES)
    install(FILES ${HBDM_QM_FILES}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/hb-download-manager/translations
    )
endif()
