cmake_minimum_required(VERSION 3.16)
project(ward-gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets WebSockets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CBOR REQUIRED IMPORTED_TARGET libcbor)

# ---- ward's wire, compiled into this client --------------------------------
#
# The frames, the CBOR codec and the two path helpers are the same C files the
# daemon and the CLI use, so this client cannot drift from the protocol by
# re-implementing it. Only the three translation units the client needs are
# built; nothing from the daemon, store, wasm or net layers comes with them.
set(WARD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_library(wardwire STATIC
    "${WARD_SRC}/proto/frame.c"
    "${WARD_SRC}/proto/codec.c"
    "${WARD_SRC}/core/util.c"
)
target_include_directories(wardwire PUBLIC "${WARD_SRC}")
target_compile_definitions(wardwire PRIVATE _GNU_SOURCE)
target_link_libraries(wardwire PUBLIC PkgConfig::CBOR)

# ---- the shell --------------------------------------------------------------
#
# One window, and it is made of QWidgets: Qt Widgets is a hard dependency of the
# product, and Qt Quick is not one.
if(NOT TARGET Qt6::Widgets)
    message(FATAL_ERROR
        "dsh-native needs Qt6 Widgets: the shell is a QWidget top-level. Install "
        "qtbase[widgets] and re-run cmake.")
endif()

message(STATUS "shell: Qt6 Widgets ENABLED")

# ---- the web-UI trapdoor (optional, OFF by default) -------------------------
#
# The trapdoor shows the real DSH web UI in this window through Servo 0.7 and a
# Rust C shim (~/var/work/servo, libservo_webview). It is the rung below the two
# transports, not part of the client: the default build compiles a stub that says
# it has no web engine, links nothing from Servo and does not even ask for
# Qt6::OpenGL. Turn it on only with an explicit shim path:
#
#   cmake -DDSH_SERVO=ON \
#         -DDSH_SERVO_VIEW_LIB=$HOME/var/work/servo/target/debug/libservo_webview.so
#
# Packaging consequence, stated here because it must not be discovered later: the
# shim .so has NO SONAME, so it cannot be a normal versioned dependency, and it is
# 174 MB of uninstalled build output. The installed binary gets DT_NEEDED
# "libservo_webview.so" and no RPATH — it will need that library on the loader
# path. Do not put this in the ebuild until the shim is packaged properly.
option(DSH_SERVO "Build the Servo web-UI trapdoor (requires libservo_webview)" OFF)
set(DSH_SERVO_VIEW_LIB "" CACHE FILEPATH
    "Path to libservo_webview.so (or .a) built by ~/var/work/servo")
# Where the installed binary should look for the shim at run time.
#
# The shim has no SONAME, so DT_NEEDED records its bare file name and the loader
# has only this run path to find it. Empty means "the build tree only" (the
# default for a developer build); a package sets it to the directory it installs
# the library into.
set(DSH_SERVO_VIEW_RPATH "" CACHE STRING
    "Run path the installed binary uses to find libservo_webview.so")

if(DSH_SERVO)
    if(NOT DSH_SERVO_VIEW_LIB OR NOT EXISTS "${DSH_SERVO_VIEW_LIB}")
        message(FATAL_ERROR
            "DSH_SERVO=ON needs the shim: -DDSH_SERVO_VIEW_LIB=/path/to/libservo_webview.so")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS OpenGL)
    get_filename_component(DSH_SERVO_VIEW_DIR "${DSH_SERVO_VIEW_LIB}" DIRECTORY)
    get_filename_component(DSH_SERVO_VIEW_FILE "${DSH_SERVO_VIEW_LIB}" NAME)
    message(STATUS "web trapdoor: Servo ENABLED, shim ${DSH_SERVO_VIEW_LIB}")
else()
    message(STATUS "web trapdoor: Servo NOT built (the default; the pane offers the browser)")
endif()

# The terminal emulator is libvterm: a parser and a screen grid with no PTY of
# its own, which is exactly the seam this client needs — the PTY is on the host,
# and this side must not be able to open one by accident.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)

qt_standard_project_setup()

add_executable(ward-gui
    src/main.cpp
    src/AcpClient.cpp
    src/HeadlessClient.cpp
    src/HeadlessPage.h
    src/HeadlessPage.cpp
    src/AcpPage.h
    src/AcpPage.cpp
    src/AcpCompat.h
    src/AcpCompat.cpp
    src/ClientFacade.h
    src/ClientFacade.cpp
    src/DshClient.cpp
    src/WardClient.cpp
    src/BrowserAuth.cpp
    src/TerminalEmulator.cpp
    src/TerminalPaint.cpp
    src/TerminalWidget.cpp
    src/Theme.cpp
    src/Markdown.cpp
    src/Shell.h
    src/Shell.cpp
    src/ShellChrome.h
    src/ShellChrome.cpp
    src/ShellHeader.h
    src/ShellHeader.cpp
    src/StatusStrip.h
    src/StatusStrip.cpp
    # The rail's rows, computed with no view attached: stage 3's port of
    # SessionSidebar.qml's `rows` binding (plan §5 stage 3).
    src/SessionRailModel.h
    src/SessionRailModel.cpp
    # The rail's view: a real QWidget over that model, with its own filter,
    # delegate, keyboard and the two menus the shell owns (plan §5, stage 3).
    src/SessionRail.h
    src/SessionRail.cpp
    src/Composer.h
    src/Composer.cpp
    src/MentionPopup.h
    src/MentionPopup.cpp
    src/GoalRow.h
    src/GoalRow.cpp
    src/FilesDock.h
    src/FilesDock.cpp
    src/PendingBand.h
    src/PendingBand.cpp
    src/TranscriptView.h
    src/TranscriptView.cpp
    src/TrajectoryPage.h
    src/TrajectoryPage.cpp
    src/TerminalPanel.h
    src/TerminalPanel.cpp
    src/WebPage.h
    src/WebPage.cpp
    src/Lightbox.h
    src/Lightbox.cpp
    src/ManagementPage.h
    src/ManagementPage.cpp
    src/SettingsDialog.h
    src/SettingsDialog.cpp
    src/ChangesDialog.h
    src/ChangesDialog.cpp
    src/GoalEditDialog.h
    src/GoalEditDialog.cpp
)

target_link_libraries(ward-gui PRIVATE
    wardwire
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::WebSockets
    PkgConfig::VTERM
)

add_executable(ward-gui-probe
    tools/ward_probe.cpp
    src/WardClient.cpp
    src/AcpClient.cpp
    src/AcpCompat.cpp
)
target_link_libraries(ward-gui-probe PRIVATE wardwire Qt6::Core Qt6::Gui Qt6::Network)
target_include_directories(ward-gui-probe PRIVATE src)

# Where the installed data lives: the icon set the rail and the transcript load,
# and the cursor themes main.cpp prepends to XCURSOR_PATH. Resolved at run time
# from this prefix, never baked as a build-tree path.
target_compile_definitions(ward-gui PRIVATE
    DSH_NATIVE_DATA_SUBDIR="/usr/share/ward-gui"
)

# The trapdoor's link-time half. `-l:` rather than the absolute path on purpose:
# given the absolute path, the linker records the WHOLE build-tree path as
# DT_NEEDED (measured — the .so has no SONAME), which would freeze
# /home/<owner>/var/work/servo into every installed binary. `-l:` records the bare
# file name, and BUILD_RPATH makes it run from the build tree.
if(DSH_SERVO)
    target_compile_definitions(dsh-native PRIVATE DSH_SERVO=1)
    # The item itself is added to the target HERE, not to add_executable above:
    # with the option off there is no ServoView.cpp in the build at all, so the
    # default binary links and contains nothing from this file.
    target_sources(ward-gui PRIVATE src/ServoWidget.cpp)
    target_link_directories(ward-gui PRIVATE "${DSH_SERVO_VIEW_DIR}")
    target_link_libraries(ward-gui PRIVATE
        Qt6::OpenGL
        "-l:${DSH_SERVO_VIEW_FILE}"
    )
    set_target_properties(ward-gui PROPERTIES
        BUILD_RPATH "${DSH_SERVO_VIEW_DIR}"
    )
    # A package installs the shim somewhere the build tree does not exist, so the
    # installed binary needs its own run path — without one it links, installs
    # and then fails to start with "cannot open shared object file".
    #
    # Written as a link option rather than INSTALL_RPATH on purpose: the property
    # is applied by CMake's install-time patching, which produced an installed
    # binary with no DT_RUNPATH at all here (measured: `readelf -d` showed none
    # and `ldd` said "not found"). An explicit `-Wl,-rpath` is in the binary from
    # the link, so what is built is what is installed.
    if(DSH_SERVO_VIEW_RPATH)
        # One run path, not two: CMake would otherwise also record the build
        # tree's shim directory, baking a developer's home into a package.
        set_target_properties(ward-gui PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "${DSH_SERVO_VIEW_RPATH}"
        )
        target_link_options(ward-gui PRIVATE
            "-Wl,-rpath,${DSH_SERVO_VIEW_RPATH}"
        )
        message(STATUS "web trapdoor: shim run path ${DSH_SERVO_VIEW_RPATH}")
    endif()
endif()
