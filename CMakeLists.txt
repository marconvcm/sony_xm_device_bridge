cmake_minimum_required(VERSION 3.14)

# Qt 6.10 supports macOS 13 and up. Info.plist.in stamps this in as
# LSMinimumSystemVersion, so an older Mac gets a clear refusal from Finder
# instead of a dyld crash. Must precede project().
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(SonyDeviceCenter VERSION 0.1.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_SANITIZERS "Enable ASan and UBSan compiler instrumentation" OFF)
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include(CTest)
find_package(Threads REQUIRED)

add_subdirectory(libs/sony-transport)
add_subdirectory(libs/sony-protocol)
add_subdirectory(libs/sony-core)

add_subdirectory(apps/sonyd)
add_subdirectory(apps/sonyctl)
add_subdirectory(apps/device-center)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# The legacy ImGui client builds from CMake on Linux and Windows only; on
# macOS its entry point and ImGui backend live in the Xcode project, so the
# CMake target has no main() and cannot link. See docs/architecture-current.md.
if(NOT APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Client/CMakeLists.txt")
    add_subdirectory(Client)
endif()

include(GNUInstallDirs)

if(WIN32)
    # The binaries link the MSVC runtime dynamically. Without these DLLs beside
    # them the installed .exe dies before main() with "VCRUNTIME140_1.dll was
    # not found" on any machine lacking the VC++ redistributable.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP FALSE)
    include(InstallRequiredSystemLibraries)
endif()

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/sony-device-center.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/sony-device-center.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(FILES packaging/linux/sony-device-center.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
    install(FILES packaging/linux/com.github.sonybridge.sony-device-center.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    # systemd only searches lib/systemd/user, never the arch-specific
    # CMAKE_INSTALL_LIBDIR (lib64 on Fedora, lib/<triplet> on Debian with /usr).
    set(SONY_SYSTEMD_USER_UNIT_DIR "lib/systemd/user" CACHE STRING
        "Install directory for the sonyd systemd user unit, relative to the prefix")
    install(FILES packaging/linux/sonyd.service
            DESTINATION ${SONY_SYSTEMD_USER_UNIT_DIR} OPTIONAL)
endif()

# Packaging via CPack
set(CPACK_PACKAGE_NAME "sony-device-center")
set(CPACK_PACKAGE_VENDOR "SonyBridge")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cross-platform Sony audio device companion SDK, daemon, CLI, and GUI")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "https://github.com/Plutoberth/SonyHeadphonesClient")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SonyBridge Team")
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libbluetooth3, libdbus-1-3")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP;WIX")
    # WiX rejects a licence file without a .txt/.rtf extension, and the
    # repository keeps its licence extensionless.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                   "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
    # Stable across releases - changing it breaks in-place upgrades.
    set(CPACK_WIX_UPGRADE_GUID "45B55BE7-8A9A-4442-8F1E-90109B402CB1")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/sony-device-center.ico")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Sony Device Center")
    set(CPACK_PACKAGE_EXECUTABLES "sony-device-center" "Sony Device Center")
elseif(APPLE)
    # The shipped artifact is the DMG from the `dmg` target in
    # apps/device-center, which runs macdeployqt first. CPack's DragNDrop
    # would wrap a bundle that still points at the build machine's Qt.
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
