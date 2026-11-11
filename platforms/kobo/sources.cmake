# What a Kobo build compiles: source lists, compile options and definitions.
# platforms/kobo/CMakeLists.txt includes this file for its SDK, and
# platforms/kobo/syntax.cmake includes it once for each SDK, so the syntax
# check reads the same lists as the build.
#
# Inputs: KOBALT_ROOT, KOBALT_QT_MAJOR (5 or 6), KOBALT_VERSION and
# kobalt_shared_definitions of platforms/shared-inputs.cmake.
#
# This file only sets variables, all named kobo_*. It defines no targets and
# no options, so it also runs in script mode (cmake -P).
set(kobo_directory "${KOBALT_ROOT}/platforms/kobo")
set(kobo_source "${KOBALT_ROOT}/src")

# A build system reruns CMake when a glob finds other files. Script mode has
# no build system and rejects the keyword.
set(kobo_glob_options CONFIGURE_DEPENDS)
if(CMAKE_SCRIPT_MODE_FILE)
    set(kobo_glob_options "")
endif()

# Shared application sources and headers. The scenario player is built only
# into evaluation packages, and the Library belongs to the standalone apps.
file(GLOB_RECURSE kobo_shared_sources ${kobo_glob_options} "${kobo_source}/*.cpp" "${kobo_source}/*.h")
list(FILTER kobo_shared_sources EXCLUDE REGEX "/src/scenario/")
list(FILTER kobo_shared_sources EXCLUDE REGEX "/(library|app/library)/")
# The adapters of this SDK's firmware.
file(GLOB kobo_adapter_sources ${kobo_glob_options}
    "${kobo_directory}/common/*.cpp" "${kobo_directory}/common/*.h"
    "${kobo_directory}/nickel-qt${KOBALT_QT_MAJOR}/*.cpp" "${kobo_directory}/nickel-qt${KOBALT_QT_MAJOR}/*.h"
    "${kobo_directory}/display/*.cpp")
set(kobo_c_sources "${kobo_directory}/common/install.c" "${kobo_directory}/common/startup_recovery.c")
# NickelHook's loader, linked into the mod library only.
set(kobo_hooks "${kobo_directory}/toolchains/nickelhook-qt6")
set(kobo_hook_sources "${kobo_hooks}/nh.c" "${kobo_hooks}/resources.c")
# An evaluation package (KOBALT_EVAL_PACKAGE) adds the scenario player and
# this definition.
set(kobo_eval_sources "${kobo_source}/scenario/player.cpp")
set(kobo_eval_definitions KOBALT_SCENARIO_PLAYER)

# Options of the application objects and the mod library, on top of the
# warnings that the tests also use.
set(kobo_warning_options -Wall -Wextra -Werror)
set(kobo_code_options -Wno-missing-field-initializers -fvisibility=hidden)
if(KOBALT_QT_MAJOR EQUAL 5)
    set(kobo_system_font_directory "/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/fonts")
else()
    set(kobo_system_font_directory "/usr/share/fonts")
endif()
set(kobo_definitions ${kobalt_shared_definitions} NH_VERSION="${KOBALT_VERSION}"
    KOBALT_CONFIG_DIR="/mnt/onboard/.adds/kobalt"
    KOBALT_SYSTEM_FONT_DIRECTORY="${kobo_system_font_directory}")
# Set while the KOBALT_PAGE_ANIMATIONS option is on, which is its default.
set(kobo_animation_definitions KOBALT_PAGE_ANIMATIONS)

# A definition of one source. The build adds it while the
# KOBALT_VERBOSE_DIAGNOSTICS option is on, which is its default.
# platforms/shared-inputs.cmake has the definition of the annotation store.
set(kobo_diagnostics_source "${kobo_source}/support/diagnostics.cpp")
set(kobo_diagnostics_definitions KOBALT_VERBOSE_DIAGNOSTICS=1)

# The Kobo adapter tests (KOBALT_BUILD_TESTS). Pewter is the fake Nickel: the
# shared functions and those of this SDK's firmware.
set(kobo_pewter "${KOBALT_ROOT}/tests/pewter")
set(kobo_pewter_sources "${kobo_pewter}/pewter.cpp" "${kobo_pewter}/nickel.cpp")
set(kobo_pewter_sdk_sources "${kobo_pewter}/nickel_qt${KOBALT_QT_MAJOR}.cpp")
set(kobo_pewter_headers "${kobo_pewter}/pewter.h" "${kobo_pewter}/nickel.h")
# One program for each test source.
set(kobo_progress_test_sources "${kobo_pewter}/progress.cpp")
# The progress test uses the device build's view of the adapter headers.
set(kobo_progress_test_definitions KOBALT_READER_HARDWARE)
set(kobo_keyboard_test_sources "${kobo_pewter}/keyboard.cpp")
set(kobo_native_ui_test_sources "${kobo_pewter}/native_ui.cpp")
set(kobo_tile_cover_test_sources "${kobo_pewter}/tile_cover.cpp")
set(kobo_event_history_test_sources "${kobo_pewter}/event_history.cpp")
set(kobo_test_sources ${kobo_progress_test_sources} ${kobo_keyboard_test_sources}
    ${kobo_native_ui_test_sources} ${kobo_tile_cover_test_sources} ${kobo_event_history_test_sources})
# The Qt5 refresh test fakes the display driver call itself. It compiles its
# own copy of two adapter sources and does not use pewter's Nickel.
set(kobo_sdk_test_sources "")
set(kobo_refresh_test_adapter_sources "")
if(KOBALT_QT_MAJOR EQUAL 5)
    set(kobo_sdk_test_sources "${kobo_pewter}/refresh_qt5.cpp")
    set(kobo_refresh_test_adapter_sources "${kobo_directory}/nickel-qt5/refresh.cpp"
        "${kobo_directory}/display/page_animation.cpp")
endif()
