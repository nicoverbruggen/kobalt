# Host tests for the portable reader: models, storage, publication, engine
# adapter and the reader window. They build against a stock Qt 6 for the host
# CPU and need no Kobo device, firmware or Nickel. Run them with tools/run_tests.py.
#
# The first tier uses Qt and the pinned text libraries. KOBALT_TEST_ENGINE adds the tests that open
# real books through CREngine; tools/run_tests.py builds CREngine and its pinned
# dependencies for the host and passes their locations.
cmake_minimum_required(VERSION 3.19)
project(kobalt_tests LANGUAGES C CXX)
option(KOBALT_MINI "Test the mini Kobo asset bundle" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
get_filename_component(KOBALT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(KOBALT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}")
option(KOBALT_TEST_ENGINE "Build the tests that need CREngine" OFF)

# Qt 6.4 is enough without the engine; Ubuntu 24.04 ships it.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Test)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(KOBALT_QT_MAJOR 6)
include("${KOBALT_ROOT}/cmake/Strings.cmake")
enable_testing()
add_test(NAME screenshot-runner COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_native_screenshots.py")
add_test(NAME screenshot-review COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_ui_review.py")
add_test(NAME ui-architecture COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_ui_architecture.py")
add_test(NAME ui-correctness-inventory COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_ui_correctness.py")
add_test(NAME ui-icon-vectors COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_ui_icon_vectors.py")
add_test(NAME bundled-fonts COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_bundled_fonts.py")
add_test(NAME mini-build COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_mini_build.py")
add_test(NAME dependency-extraction COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_build.py")
add_test(NAME host-dependency-cache COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_run_tests.py")
add_test(NAME dependency-artifacts COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_dependency_cache.py")
add_test(NAME install-receipt COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${KOBALT_TESTS}/tooling" -p test_install_receipt.py)
add_test(NAME kobo-build-script COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${KOBALT_TESTS}/tooling" -p test_kobo_build.py)
add_test(NAME coverage-report COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_test_coverage.py")
add_test(NAME variable-font-fixture COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_variable_font.py")
add_test(NAME cpp-formatter-tool COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_format_cpp.py")
add_test(NAME release-version COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_version.py")
add_test(NAME release-artifacts COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_release_artifacts.py")
add_test(NAME android-ci-artifacts COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/tooling/test_ci_android.py")

execute_process(COMMAND "${Python3_EXECUTABLE}" -B -c
    "import sys; sys.path.insert(0, sys.argv[1]); from version import release_version; print(release_version())"
    "${KOBALT_ROOT}/tools"
    OUTPUT_VARIABLE KOBALT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)

# The reader and component tests read the same verified asset bundle as a
# device installation.
execute_process(COMMAND "${Python3_EXECUTABLE}" -B -c
    "import sys; sys.path.insert(0, sys.argv[1]); import assets; from pathlib import Path; assets.stage(Path(sys.argv[2]), mini=sys.argv[3] == 'ON')"
    "${KOBALT_ROOT}/tools" "${CMAKE_BINARY_DIR}/assets" "${KOBALT_MINI}"
    RESULT_VARIABLE asset_result)
if(NOT asset_result EQUAL 0)
    message(FATAL_ERROR "Could not stage the test assets")
endif()
set(KOBALT_TEST_ASSETS "${CMAKE_BINARY_DIR}/assets/install/.adds/kobalt/assets")
set(KOBALT_TEST_FIXTURES "${CMAKE_BINARY_DIR}/fixtures")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/fixtures/generate.py" "${KOBALT_TEST_FIXTURES}"
    RESULT_VARIABLE fixture_result)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the test books")
endif()

# macOS has no offscreen platform with the reader's FreeType font engine.
set(KOBALT_TEST_PLATFORM "offscreen")
if(APPLE)
    set(KOBALT_TEST_PLATFORM "minimal:enable_fonts:freetype")
endif()

# Every target, including the widget kit's subdirectory, resolves quoted
# includes from src/ and the test support headers. -iquote keeps
# src/support/strings.h from shadowing the C library's <strings.h>.
add_compile_options("-iquote${KOBALT_ROOT}/src" "-iquote${KOBALT_TESTS}/support")
if(APPLE)
    # Darwin has fsync but no fdatasync. The device builds keep fdatasync.
    add_compile_definitions(fdatasync=fsync)
endif()

function(kobalt_test_options target)
    target_compile_definitions(${target} PRIVATE KOBALT_VERSION="${KOBALT_VERSION}"
        KOBALT_TEST_ASSET_ROOT="${KOBALT_TEST_ASSETS}")
endfunction()

# kobalt_check(NAME SOURCES files... LIBRARIES targets... [DEFINITIONS defs...] [ARGUMENTS args...])
# Builds a plain check program from paths relative to the repository root and
# registers it. A check exits with a non-zero status on the first failure.
function(kobalt_check name)
    cmake_parse_arguments(check "" "" "SOURCES;LIBRARIES;DEFINITIONS;ARGUMENTS" ${ARGN})
    list(TRANSFORM check_SOURCES PREPEND "${KOBALT_ROOT}/")
    add_executable(${name}-check ${check_SOURCES})
    target_link_libraries(${name}-check PRIVATE ${check_LIBRARIES})
    target_compile_definitions(${name}-check PRIVATE ${check_DEFINITIONS})
    target_compile_options(${name}-check PRIVATE -Wall -Wextra -Werror)
    kobalt_test_options(${name}-check)
    set(work "${CMAKE_CURRENT_BINARY_DIR}/work/${name}")
    file(MAKE_DIRECTORY "${work}")
    add_test(NAME ${name} COMMAND ${name}-check ${check_ARGUMENTS} WORKING_DIRECTORY "${work}")
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

# Models, storage and publication code. No engine and no widgets.
kobalt_check(touch-rotation SOURCES tests/model/touch_rotation.cpp src/platform/nickel/qt5/touch_rotation.cpp LIBRARIES Qt6::Gui)
kobalt_check(usb-removal SOURCES tests/model/usb_removal.cpp LIBRARIES Qt6::Core)
kobalt_check(nickel-session-calls SOURCES tests/model/nickel_session_calls.cpp LIBRARIES Qt6::Core)

kobalt_check(progress SOURCES tests/model/reading_progress.cpp src/reading/reading_progress.cpp LIBRARIES Qt6::Core)
kobalt_check(storage-assets SOURCES tests/model/storage_assets.cpp src/assets/assets.cpp src/storage/reader_storage.cpp
    LIBRARIES Qt6::Core)
kobalt_check(layout-compatibility SOURCES tests/model/layout_compatibility.cpp src/publication/layout_compatibility.cpp
    src/publication/resource_cache.cpp LIBRARIES Qt6::Core)
kobalt_check(layout-cache SOURCES tests/model/layout_cache.cpp src/storage/layout_cache.cpp
    src/publication/layout_compatibility.cpp src/publication/resource_cache.cpp LIBRARIES Qt6::Core)
kobalt_check(resource-cache SOURCES tests/model/resource_cache.cpp src/publication/resource_cache.cpp LIBRARIES Qt6::Core)
kobalt_check(local-resources SOURCES tests/model/local_resources.cpp src/publication/local_resources.cpp
    src/publication/resource_cache.cpp src/publication/image_policy.cpp src/publication/publication.cpp
    src/publication/zip_resource.cpp LIBRARIES Qt6::Core ZLIB::ZLIB Threads::Threads)
kobalt_check(zip-resource SOURCES tests/model/zip_resource.cpp src/publication/zip_resource.cpp LIBRARIES Qt6::Core ZLIB::ZLIB)
kobalt_check(diagnostics SOURCES tests/model/diagnostics.cpp src/support/diagnostics.cpp LIBRARIES Qt6::Core)
kobalt_check(engine-activity SOURCES tests/model/engine_activity.cpp LIBRARIES Threads::Threads)
kobalt_check(brightness-gesture SOURCES tests/model/brightness_gesture.cpp LIBRARIES Qt6::Core)
kobalt_check(logical-word-counter SOURCES tests/model/logical_word_counter.cpp LIBRARIES Qt6::Core)
kobalt_check(reading-pace SOURCES tests/model/reading_pace.cpp LIBRARIES Qt6::Core)
kobalt_check(screen-refresh SOURCES tests/model/screen_refresh.cpp LIBRARIES Qt6::Core)
kobalt_check(font-registration SOURCES tests/model/font_registration.cpp LIBRARIES Qt6::Gui
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/font.ttf")
kobalt_check(page-refresh-regions SOURCES tests/model/page_refresh_regions.cpp src/model/page_refresh_regions.cpp LIBRARIES Qt6::Gui)
kobalt_check(bookmark-store SOURCES tests/model/bookmark_store.cpp src/storage/bookmark_store.cpp src/storage/reader_storage.cpp
    LIBRARIES Qt6::Core)
kobalt_check(image-damage SOURCES tests/model/image_damage.cpp LIBRARIES Qt6::Gui)
kobalt_check(page-pixels SOURCES tests/model/page_pixels.cpp LIBRARIES Qt6::Gui)
kobalt_check(qt-compat SOURCES tests/model/qt_compat.cpp LIBRARIES Qt6::Core)
kobalt_check(appearance-settings SOURCES tests/model/appearance_settings.cpp src/reading/appearance_settings.cpp LIBRARIES Qt6::Core)
kobalt_check(font-data SOURCES tests/model/font_data.cpp src/interfaces/reader/ui/style/font_data.cpp LIBRARIES Qt6::Gui
    DEFINITIONS KOBALT_FONT_DIRECTORY="${KOBALT_ROOT}/resources/fonts")
kobalt_check(image-policy SOURCES tests/model/image_policy.cpp src/publication/image_policy.cpp
    src/publication/native_resources.cpp src/publication/resource_cache.cpp src/support/diagnostics.cpp
    LIBRARIES Qt6::Network Threads::Threads)
kobalt_check(plain-archive SOURCES tests/model/plain_archive.cpp src/publication/plain_archive.cpp
    src/publication/publication.cpp src/publication/zip_resource.cpp src/publication/layout_compatibility.cpp
    src/publication/resource_cache.cpp LIBRARIES Qt6::Network ZLIB::ZLIB)
# The loopback setup is Linux code for the device; other hosts use a stub.
set(loopback src/publication/loopback.cpp)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(loopback tests/support/host_loopback.cpp)
endif()
kobalt_check(resource-provider SOURCES tests/model/resource_provider.cpp src/publication/resource_server.cpp
    src/publication/zip_resource.cpp ${loopback} src/publication/publication.cpp
    src/publication/image_policy.cpp src/publication/native_resources.cpp src/publication/resource_cache.cpp
    src/support/diagnostics.cpp LIBRARIES Qt6::Gui Qt6::Network Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})

# Reader widgets without an engine.
set(KOBALT_DEPENDENCY_PREFIX "${KOBALT_TEST_DEPENDENCIES}")
add_executable(font-outlines-check "${KOBALT_TESTS}/support/font_outlines.cpp")
target_compile_options(font-outlines-check PRIVATE -Wall -Wextra -Werror)
target_include_directories(font-outlines-check PRIVATE "${KOBALT_TEST_DEPENDENCIES}/include/freetype2")
target_link_libraries(font-outlines-check PRIVATE "${KOBALT_TEST_DEPENDENCIES}/lib/libfreetype.a" ZLIB::ZLIB ${KOBALT_TEST_PNG})
set_tests_properties(mini-build PROPERTIES ENVIRONMENT "KOBALT_TEST_OUTLINE_CHECK=$<TARGET_FILE:font-outlines-check>")
add_library(reader-fonts STATIC "${KOBALT_ROOT}/src/assets/assets.cpp" "${KOBALT_ROOT}/src/assets/bundled_fonts.cpp"
    "${KOBALT_ROOT}/src/assets/system_fonts.cpp")
target_link_libraries(reader-fonts PUBLIC Qt6::Gui PRIVATE ZLIB::ZLIB)
kobalt_check(system-fonts SOURCES tests/model/system_fonts.cpp LIBRARIES reader-fonts
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/font.ttf")
include("${KOBALT_ROOT}/cmake/EngineSources.cmake")
kobalt_test_options(reader-fonts)
include("${KOBALT_ROOT}/cmake/ReaderComponents.cmake")
kobalt_publication_library(test-publication)
add_subdirectory("${KOBALT_ROOT}/src/interfaces/reader/ui" reader-widgets)

# kobalt_widget_test(NAME SOURCES files... LIBRARIES targets...)
# Builds a widget test and runs it on the headless platform.
function(kobalt_widget_test name)
    cmake_parse_arguments(widget "SHARDED" "" "SOURCES;LIBRARIES;ARGUMENTS;ENVIRONMENT" ${ARGN})
    add_executable(${name}-test ${widget_SOURCES})
    target_link_libraries(${name}-test PRIVATE ${widget_LIBRARIES} Qt6::Test)
    kobalt_test_options(${name}-test)
    if(NOT widget_SHARDED)
        add_test(NAME ${name} COMMAND ${name}-test ${widget_ARGUMENTS})
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM};QT_STYLE_OVERRIDE=Fusion;${widget_ENVIRONMENT}")
    endif()
endfunction()

set(component_sources "${KOBALT_TESTS}/model/ui.cpp")
foreach(source interfaces/reader/views/opening interfaces/reader/panels/dictionary_panel interfaces/reader/settings/dictionaries_page
        interfaces/reader/panels/link_panel interfaces/reader/panels/chapter_graph
        interfaces/reader/panels/chapter_contents interfaces/reader/panels/choice_screen
        storage/reader_storage)
    list(APPEND component_sources "${KOBALT_ROOT}/src/${source}.cpp")
endforeach()
kobalt_widget_test(reader-ui-components SOURCES ${component_sources} LIBRARIES reader-widgets)

file(GLOB notices "${KOBALT_ROOT}/resources/notices/*")
kobalt_widget_test(acknowledgments-components SOURCES "${KOBALT_TESTS}/model/acknowledgments.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/settings/acknowledgments.cpp" LIBRARIES reader-widgets)
qt_add_resources(acknowledgments-components-test component_notices PREFIX "/kobalt"
    BASE "${KOBALT_ROOT}/resources" FILES "${KOBALT_ROOT}/resources/acknowledgments.json" ${notices})

kobalt_widget_test(reader-canvas-input SOURCES "${KOBALT_TESTS}/native-ui/reader_canvas_test.cpp" LIBRARIES Qt6::Widgets)
kobalt_widget_test(test-shards SOURCES "${KOBALT_TESTS}/native-ui/test_shards_test.cpp" LIBRARIES Qt6::Core)

kobalt_widget_test(native-ui-smoke SOURCES "${KOBALT_TESTS}/native-ui/main.cpp" "${KOBALT_TESTS}/native-ui/host_display.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/settings/settings_panel.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/settings/dictionaries_page.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/settings/appearance_panel.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/views/opening.cpp"
    "${KOBALT_ROOT}/src/storage/reader_storage.cpp"
    LIBRARIES reader-widgets ARGUMENTS --smoke)
target_compile_definitions(native-ui-smoke-test PRIVATE KOBALT_ASSET_ROOT="${KOBALT_TEST_ASSETS}")

kobalt_widget_test(page-transition-native SOURCES "${KOBALT_TESTS}/model/page_transition_native.cpp" LIBRARIES Qt6::Widgets)
kobalt_widget_test(nickel-browser-handoff SOURCES "${KOBALT_TESTS}/nickel/browser_handoff_test.cpp"
    LIBRARIES Qt6::Widgets)
kobalt_widget_test(nickel-window-presentation SOURCES "${KOBALT_TESTS}/nickel/window_presentation_test.cpp"
    "${KOBALT_ROOT}/src/platform/nickel/common/window_presentation.cpp" LIBRARIES Qt6::Widgets)
kobalt_widget_test(nickel-usb-dialog-overlay SOURCES "${KOBALT_TESTS}/nickel/usb_dialog_overlay_test.cpp"
    LIBRARIES Qt6::Widgets)
kobalt_widget_test(nickel-usb-transition SOURCES "${KOBALT_TESTS}/nickel/usb_transition_test.cpp"
    LIBRARIES Qt6::Widgets)
find_package(Qt6 QUIET COMPONENTS Sql)
if(TARGET Qt6::Sql)
    kobalt_check(nickel-database-wait SOURCES tests/nickel/native_database_wait.cpp LIBRARIES Qt6::Sql)
endif()
kobalt_widget_test(nickel-lifecycle SOURCES "${KOBALT_TESTS}/nickel/native_lifecycle_test.cpp"
    "${KOBALT_ROOT}/src/platform/nickel/common/native_lifecycle.cpp"
    "${KOBALT_ROOT}/src/platform/nickel/common/native_lifecycle.h"
    "${KOBALT_ROOT}/src/support/diagnostics.cpp" LIBRARIES Qt6::Widgets Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(nickel-lifecycle-test PRIVATE KOBALT_READER_HARDWARE
    KOBALT_CONFIG_DIR="/unused" KOBALT_SYSTEM_FONT_DIRECTORY="/unused")
set_target_properties(nickel-lifecycle-test PROPERTIES ENABLE_EXPORTS TRUE)

kobalt_widget_test(nickel-light-panel SOURCES "${KOBALT_TESTS}/nickel/light_panel_test.cpp"
    "${KOBALT_ROOT}/src/interfaces/reader/panels/light_panel.cpp" LIBRARIES reader-widgets)

kobalt_widget_test(nickel-light SOURCES "${KOBALT_TESTS}/nickel/native_light_test.cpp"
    "${KOBALT_ROOT}/src/platform/nickel/qt5/native_light.cpp"
    "${KOBALT_ROOT}/src/platform/nickel/common/native_light.h"
    "${KOBALT_ROOT}/src/support/diagnostics.cpp" LIBRARIES Qt6::Widgets Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(nickel-light-test PRIVATE KOBALT_READER_HARDWARE)
set_target_properties(nickel-light-test PROPERTIES ENABLE_EXPORTS TRUE)

# The ioctl adapter requires Linux framebuffer headers.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    kobalt_check(page-animation-native SOURCES tests/model/page_animation_native.cpp
        src/platform/nickel/display/page_animation.cpp src/support/diagnostics.cpp LIBRARIES Qt6::Core Qt6::Gui)
endif()

if(NOT KOBALT_TEST_ENGINE)
    return()
endif()

if(NOT DEFINED KOBALT_TEST_VARIABLE_FONT_SOURCE)
    message(FATAL_ERROR "KOBALT_TEST_VARIABLE_FONT_SOURCE is required. Use tools/run_tests.py to prepare it.")
endif()
set(KOBALT_TEST_VARIABLE_FONTS "${KOBALT_TEST_FIXTURES}/variable-fonts")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${KOBALT_TESTS}/fixtures/variable_font.py"
    "${KOBALT_TEST_VARIABLE_FONT_SOURCE}" "${KOBALT_TEST_VARIABLE_FONTS}"
    RESULT_VARIABLE variable_font_result)
if(NOT variable_font_result EQUAL 0)
    message(FATAL_ERROR "Could not stage the verified variable-font fixture")
endif()

# Tests that open real books. They need a host CREngine build.
foreach(input CRENGINE_SOURCE CRENGINE_BUILD KOBALT_TEST_DEPENDENCIES KOBALT_TEST_PNG KOBALT_TEST_PNG_INCLUDE)
    if(NOT DEFINED ${input})
        message(FATAL_ERROR "${input} is required with KOBALT_TEST_ENGINE. Use tools/run_tests.py to prepare it.")
    endif()
endforeach()
find_package(Qt6 6.5 REQUIRED COMPONENTS Sensors)
kobalt_check(reader-orientation SOURCES tests/model/reader_orientation.cpp src/platform/api/reader_orientation.cpp LIBRARIES Qt6::Sensors Qt6::Gui)
file(READ "${CRENGINE_SOURCE}/cr3gui/data/html5.css" READER_HTML_CSS)
configure_file("${KOBALT_ROOT}/src/engine/crengine/reader_styles.h.in" reader_styles.h @ONLY)
file(GLOB READER_HYPHENATION_PATTERNS RELATIVE "${CRENGINE_SOURCE}/cr3gui/data/hyph" "${CRENGINE_SOURCE}/cr3gui/data/hyph/*.pattern")
list(SORT READER_HYPHENATION_PATTERNS)
list(TRANSFORM READER_HYPHENATION_PATTERNS PREPEND "\"")
list(TRANSFORM READER_HYPHENATION_PATTERNS APPEND "\"")
list(JOIN READER_HYPHENATION_PATTERNS ", " READER_HYPHENATION_PATTERNS)
configure_file("${KOBALT_ROOT}/src/engine/crengine/reader_hyphenation.h.in" reader_hyphenation.h @ONLY)
include("${KOBALT_ROOT}/cmake/EngineSources.cmake")
set(engine_headers "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${KOBALT_TEST_DEPENDENCIES}/include/harfbuzz" "${KOBALT_TEST_DEPENDENCIES}/include"
    "${KOBALT_TEST_DEPENDENCIES}/include/freetype2" "${KOBALT_TEST_PNG_INCLUDE}")
add_library(test-reader-model STATIC ${KOBALT_ENGINE_SOURCES})
target_include_directories(test-reader-model PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(test-reader-model SYSTEM PRIVATE ${engine_headers})
kobalt_test_options(test-reader-model)
target_link_libraries(test-reader-model PUBLIC test-publication reader-fonts Qt6::Gui "${CRENGINE_BUILD}/libcrengine.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libharfbuzz.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libxxhash.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libutf8proc.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libjpeg.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/liblunasvg.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libfreetype.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libfribidi.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libunibreak.a"
    "${KOBALT_TEST_PNG}" z)
kobalt_link_private_freetype(test-reader-model "${KOBALT_TEST_DEPENDENCIES}")
kobalt_nickel_position_library(test-positions test-reader-model)
kobalt_reading_library(test-reading test-reader-model test-publication "native-public-v1")

# Engine checks: the adapter against the synthetic books.
function(kobalt_engine_check name)
    cmake_parse_arguments(engine "" "" "ARGUMENTS;LIBRARIES" ${ARGN})
    string(REPLACE "-" "_" source "${name}")
    kobalt_check(${name} SOURCES tests/model/${source}.cpp LIBRARIES test-reader-model ${engine_LIBRARIES}
        ARGUMENTS ${engine_ARGUMENTS})
    target_include_directories(${name}-check SYSTEM PRIVATE ${engine_headers})
endfunction()
kobalt_check(dictionary-store SOURCES tests/model/dictionary_store.cpp LIBRARIES test-reading)
kobalt_engine_check(font-sources ARGUMENTS "${KOBALT_TEST_FIXTURES}/font.ttf" "${KOBALT_TEST_FIXTURES}/text.epub")
kobalt_engine_check(reader LIBRARIES test-positions
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/text.epub" "${KOBALT_TEST_FIXTURES}/color.epub")
kobalt_engine_check(search ARGUMENTS "${KOBALT_TEST_FIXTURES}/dictionary.epub")
kobalt_engine_check(word-selection ARGUMENTS "${KOBALT_TEST_FIXTURES}/dictionary.epub")
kobalt_engine_check(document-word-count ARGUMENTS "${KOBALT_TEST_FIXTURES}/word-counts.epub"
    "${KOBALT_TEST_FIXTURES}/word-count-many.epub")
add_executable(word-count-benchmark "${KOBALT_ROOT}/tests/model/word_count_benchmark.cpp")
target_link_libraries(word-count-benchmark PRIVATE test-reader-model)
kobalt_test_options(word-count-benchmark)
kobalt_engine_check(alignment)
kobalt_engine_check(vertical-writing)
kobalt_engine_check(mathml)
kobalt_engine_check(line-breaking)
kobalt_engine_check(whitespace)
kobalt_engine_check(svg-limits)
target_compile_definitions(whitespace-check PRIVATE KOBALT_FONT_DIRECTORY="${KOBALT_ROOT}/resources/fonts")
kobalt_engine_check(multilingual ARGUMENTS "${KOBALT_TEST_FIXTURES}/multilingual-ar.epub"
    "${KOBALT_TEST_FIXTURES}/multilingual-he.epub")
kobalt_engine_check(links ARGUMENTS "${KOBALT_TEST_FIXTURES}/links.epub")
kobalt_engine_check(publisher-markup ARGUMENTS "${KOBALT_TEST_FIXTURES}/publisher.epub"
    "${KOBALT_TEST_FIXTURES}/publisher-de.epub" "${KOBALT_TEST_FIXTURES}/publisher-ru.epub")

# The reader window with host services instead of a device.
include("${KOBALT_ROOT}/src/interfaces/reader/Sources.cmake")
# About shows the commit, as in the device build. Empty when unknown.
set(KOBALT_COMMIT "" CACHE STRING "Commit of the sources, empty when unknown")
option(KOBALT_COMMIT_MODIFIED "The sources had uncommitted changes" OFF)
configure_file("${KOBALT_ROOT}/src/support/build_info.h.in" build_info.h)
# Two copies of the same runtime. The tests use test-state/, which they erase
# before each run, and fake device capabilities. The interactive reader keeps
# its settings and bookmarks in state/ and never sees the test fixtures.
foreach(runtime test-reader-runtime interactive-reader-runtime)
    add_library(${runtime} STATIC
        "${KOBALT_TESTS}/native-ui/reader_host.cpp"
        "${KOBALT_TESTS}/native-ui/host_services.cpp" "${KOBALT_TESTS}/native-ui/host_display.cpp"
        "${KOBALT_TESTS}/support/host_loopback.cpp" "${KOBALT_TESTS}/native-ui/host_progress.cpp"
        ${KOBALT_READER_INTERFACE_SOURCES}
        "${KOBALT_ROOT}/src/publication/resource_server.cpp" "${KOBALT_ROOT}/src/publication/plain_archive.cpp")
    foreach(source platform/api/reader_orientation support/diagnostics publication/native_resources)
        target_sources(${runtime} PRIVATE "${KOBALT_ROOT}/src/${source}.cpp")
    endforeach()
    kobalt_test_options(${runtime})
    target_include_directories(${runtime} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    if(runtime STREQUAL "test-reader-runtime")
        set(state_root "${CMAKE_BINARY_DIR}/test-state")
        target_compile_definitions(${runtime} PUBLIC KOBALT_NATIVE_TEST_FIXTURES KOBALT_PAGE_ANIMATIONS)
    else()
        set(state_root "${CMAKE_BINARY_DIR}/state")
    endif()
    target_compile_definitions(${runtime} PUBLIC KOBALT_READER_HARDWARE
        KOBALT_CONFIG_DIR="${state_root}" KOBALT_STORAGE_ROOT="${state_root}"
        KOBALT_ASSET_ROOT="${KOBALT_TEST_ASSETS}" KOBALT_LAYOUT_POLICY="native-public-v1")
    target_link_libraries(${runtime} PUBLIC reader-widgets test-reading test-reader-model Qt6::Sensors Qt6::Network)
endforeach()

kobalt_check(reading-book SOURCES tests/model/reading_book.cpp LIBRARIES test-reader-runtime
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/text.epub")
kobalt_check(reader-session SOURCES tests/model/reader_session.cpp LIBRARIES test-reader-runtime
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/text.epub" "${KOBALT_TEST_FIXTURES}/publisher-non-linear.epub")
kobalt_check(local-reader SOURCES tests/model/local_reader.cpp src/support/diagnostics.cpp LIBRARIES test-reading
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/local-cache.epub" "${KOBALT_TEST_FIXTURES}/color.epub")
set_tests_properties(local-reader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM}")
kobalt_check(bookmark-session SOURCES tests/model/bookmark_session.cpp LIBRARIES test-reader-runtime
    ARGUMENTS "${KOBALT_TEST_FIXTURES}/text.epub")
kobalt_check(engine-resource-cache SOURCES tests/model/resource_cache.cpp LIBRARIES test-reader-runtime)
kobalt_check(engine-image-policy SOURCES tests/model/image_policy.cpp LIBRARIES test-reader-runtime)

kobalt_widget_test(android-ui SOURCES "${KOBALT_TESTS}/android/android_ui_test.cpp"
    "${KOBALT_ROOT}/src/platform/android/start_screen.cpp" LIBRARIES test-reader-runtime)
kobalt_widget_test(android-reader-lifecycle SOURCES "${KOBALT_TESTS}/android/android_reader_test.cpp"
    "${KOBALT_ROOT}/src/platform/android/reader.cpp" LIBRARIES test-reader-runtime
    ENVIRONMENT "KOBALT_FIXTURES=${KOBALT_TEST_FIXTURES}")
kobalt_widget_test(desktop-window SHARDED SOURCES "${KOBALT_TESTS}/desktop/window_test.cpp"
    "${KOBALT_ROOT}/src/platform/desktop/window.cpp" "${KOBALT_ROOT}/src/platform/desktop/services.cpp"
    "${KOBALT_ROOT}/src/platform/android/reader.cpp" "${KOBALT_ROOT}/src/platform/android/start_screen.cpp"
    LIBRARIES interactive-reader-runtime)
target_include_directories(desktop-window-test SYSTEM PRIVATE "${KOBALT_TEST_DEPENDENCIES}/include")
qt_add_resources(desktop-window-test desktop_artwork PREFIX "/desktop" BASE "${KOBALT_ROOT}/docs/assets"
    FILES "${KOBALT_ROOT}/docs/assets/kobalt-android.svg")
target_compile_definitions(desktop-window-test PRIVATE KOBALT_FIXTURES="${KOBALT_TEST_FIXTURES}")
# The Android tests drive the same host services as the reader window.
foreach(target android-ui-test android-reader-lifecycle-test)
    target_compile_options(${target} PRIVATE "-iquote${KOBALT_TESTS}/native-ui")
endforeach()

add_executable(reader-test "${KOBALT_TESTS}/native-ui/reader_test.cpp")
# The position codec lets the progress tests place platform positions.
target_link_libraries(reader-test PRIVATE test-reader-runtime test-positions Qt6::Test)
kobalt_test_options(reader-test)
qt_add_resources(reader-test reader-test-notices PREFIX "/kobalt" BASE "${KOBALT_ROOT}/resources"
    FILES "${KOBALT_ROOT}/resources/acknowledgments.json" ${notices})
# Each reader process owns its state and capture directory.
add_test(NAME reader-window-device COMMAND reader-test deviceCapabilities overflowClosesCurrentPanel tabsCloseBookmarks)
set(device_work "${CMAKE_CURRENT_BINARY_DIR}/work/reader-window-device")
file(MAKE_DIRECTORY "${device_work}")
set_tests_properties(reader-window-device PROPERTIES WORKING_DIRECTORY "${device_work}"
    ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM};KOBALT_FIXTURES=${KOBALT_TEST_FIXTURES};KOBALT_TEST_DEVICE_FIXTURE=libra-colour;KOBALT_TEST_STATE=${device_work}/test-state")
foreach(shard RANGE 0 11)
    set(work "${CMAKE_CURRENT_BINARY_DIR}/work/reader-window-${shard}")
    file(MAKE_DIRECTORY "${work}")
    add_test(NAME reader-window-${shard} COMMAND reader-test)
    set_tests_properties(reader-window-${shard} PROPERTIES WORKING_DIRECTORY "${work}" COST 10
        ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM};KOBALT_FIXTURES=${KOBALT_TEST_FIXTURES};KOBALT_TEST_SHARD=${shard}/12;KOBALT_TEST_STATE=${work}/test-state")
endforeach()
foreach(shard RANGE 0 7)
    set(work "${CMAKE_CURRENT_BINARY_DIR}/work/desktop-window-${shard}")
    file(MAKE_DIRECTORY "${work}")
    add_test(NAME desktop-window-${shard} COMMAND desktop-window-test)
    set_tests_properties(desktop-window-${shard} PROPERTIES WORKING_DIRECTORY "${work}" COST 10
        ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM};QT_STYLE_OVERRIDE=Fusion;KOBALT_TEST_SHARD=${shard}/8;KOBALT_TEST_EXCLUDE=canvasBookLifecycle")
endforeach()
foreach(shard RANGE 0 4)
    set(work "${CMAKE_CURRENT_BINARY_DIR}/work/desktop-window-books-${shard}")
    file(MAKE_DIRECTORY "${work}")
    add_test(NAME desktop-window-books-${shard} COMMAND desktop-window-test canvasBookLifecycle)
    set_tests_properties(desktop-window-books-${shard} PROPERTIES WORKING_DIRECTORY "${work}" COST 14
        ENVIRONMENT "QT_QPA_PLATFORM=${KOBALT_TEST_PLATFORM};QT_STYLE_OVERRIDE=Fusion;KOBALT_TEST_DATA_SHARD=${shard}/5")
endforeach()
set_tests_properties(desktop-window-6 PROPERTIES COST 16)
set_tests_properties(reader PROPERTIES COST 25)

# The interactive reader for manual checks. Not a test.
add_executable(kobalt-test-reader "${KOBALT_TESTS}/native-ui/reader_main.cpp")
target_link_libraries(kobalt-test-reader PRIVATE interactive-reader-runtime)
kobalt_test_options(kobalt-test-reader)
qt_add_resources(kobalt-test-reader kobalt-test-reader-notices PREFIX "/kobalt" BASE "${KOBALT_ROOT}/resources"
    FILES "${KOBALT_ROOT}/resources/acknowledgments.json" ${notices})

# Deterministic screenshot tour, run explicitly by tools/native_screenshots.py.
add_executable(reader-screenshot-tour "${KOBALT_TESTS}/native-ui/screenshot_tour.cpp"
    "${KOBALT_ROOT}/src/platform/android/start_screen.cpp")
target_link_libraries(reader-screenshot-tour PRIVATE test-reader-runtime Qt6::Test)
kobalt_test_options(reader-screenshot-tour)
qt_add_resources(reader-screenshot-tour screenshot-notices PREFIX "/kobalt" BASE "${KOBALT_ROOT}/resources"
    FILES "${KOBALT_ROOT}/resources/acknowledgments.json" ${notices})

get_property(native_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS native_tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "KOBALT_TEST_VARIABLE_FONTS=${KOBALT_TEST_VARIABLE_FONTS}")
endforeach()
