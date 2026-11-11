cmake_minimum_required(VERSION 3.22)
project(KobaltKobo LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(KOBALT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(retired KOBALT_DEPENDENCIES KOBALT_ASSETS)
    unset(${retired} CACHE)
endforeach()
set(KOBALT_QT_MAJOR "6" CACHE STRING "Kobo Qt ABI: 5 or 6")
if(NOT KOBALT_QT_MAJOR MATCHES "^[56]$")
    message(FATAL_ERROR "KOBALT_QT_MAJOR must be 5 or 6")
endif()
find_package(Qt${KOBALT_QT_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network Sensors Sql DBus)
if(KOBALT_QT_MAJOR EQUAL 5)
    add_library(Qt5::GuiPrivate INTERFACE IMPORTED)
    set_target_properties(Qt5::GuiPrivate PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Qt5Gui_PRIVATE_INCLUDE_DIRS};${Qt5Core_PRIVATE_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES Qt5::Gui)
    # Reader styling stores style sheets the way Qt 6 does; see theme.cpp.
    add_library(Qt5::WidgetsPrivate INTERFACE IMPORTED)
    set_target_properties(Qt5::WidgetsPrivate PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Qt5Widgets_PRIVATE_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "Qt5::Widgets;Qt5::GuiPrivate")
    set(KOBALT_SYSTEM_FONT_DIRECTORY "/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/fonts")
else()
    set(KOBALT_SYSTEM_FONT_DIRECTORY "/usr/share/fonts")
endif()

include("${KOBALT_ROOT}/dependencies/cache.cmake")
kobalt_dependencies()
add_subdirectory("${KOBALT_ROOT}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets")
set(source "${KOBALT_ROOT}/src")

file(READ "${KOBALT_ASSETS}/manifest.json" assets)
set(asset_inputs "${KOBALT_ASSETS}/manifest.json")
string(JSON KOBALT_VERSION GET "${assets}" version)
string(JSON count LENGTH "${assets}" files)
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    string(JSON name GET "${assets}" files ${index} path)
    if(IS_ABSOLUTE "${name}" OR name MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Invalid runtime asset path: ${name}")
    endif()
    string(JSON expected GET "${assets}" files ${index} sha256)
    list(APPEND asset_inputs "${KOBALT_ASSETS}/${name}")
    file(SHA256 "${KOBALT_ASSETS}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Prepared runtime asset changed: ${name}")
    endif()
endforeach()

file(READ "${CRENGINE_SOURCE}/cr3gui/data/html5.css" READER_HTML_CSS)
configure_file("${source}/engine/crengine/reader_styles.h.in" reader_styles.h @ONLY)
file(GLOB patterns RELATIVE "${CRENGINE_SOURCE}/cr3gui/data/hyph" "${CRENGINE_SOURCE}/cr3gui/data/hyph/*.pattern")
list(SORT patterns)
list(TRANSFORM patterns PREPEND "\"")
list(TRANSFORM patterns APPEND "\"")
list(JOIN patterns ", " READER_HYPHENATION_PATTERNS)
configure_file("${source}/engine/crengine/reader_hyphenation.h.in" reader_hyphenation.h @ONLY)
set(KOBALT_COMMIT "" CACHE STRING "Source commit, passed by the container launcher")
option(KOBALT_COMMIT_MODIFIED "Source has uncommitted changes" OFF)
configure_file("${source}/support/build_info.h.in" build_info.h @ONLY)

set(hooks "${CMAKE_CURRENT_SOURCE_DIR}/toolchains/nickelhook-qt6")
configure_file("${hooks}/nhplugin.h" nhplugin.h COPYONLY)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nhplugin.json" CONTENT "{\"Keys\":[\"nh.kobalt\"]}\n" @ONLY)
set(embedded "#include <resources.h>\n")
foreach(name doc uninstall)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/${name}")
    file(READ "resources/${name}" bytes HEX)
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
    string(APPEND embedded "static const unsigned char resource_${name}[] = {${bytes}};\n")
endforeach()
string(APPEND embedded "static const struct nh_resource nh_embedded_resources[] = {\n")
foreach(name doc uninstall)
    string(APPEND embedded "{\"${name}\", resource_${name}, sizeof(resource_${name})},\n")
endforeach()
string(APPEND embedded "{NULL, NULL, 0}};\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.h" CONTENT "${embedded}" @ONLY)

# syntax.cmake repeats the source selection, compile flags and definitions
# below, also for the evaluation package. Change both files together.
file(GLOB_RECURSE shared CONFIGURE_DEPENDS "${source}/*.cpp" "${source}/*.h")
# The scenario player is built only into evaluation packages.
list(FILTER shared EXCLUDE REGEX "/src/scenario/")
list(FILTER shared EXCLUDE REGEX "/(library|app/library)/")
file(GLOB adapters CONFIGURE_DEPENDS "common/*.cpp" "common/*.h" "nickel-qt${KOBALT_QT_MAJOR}/*.cpp" "nickel-qt${KOBALT_QT_MAJOR}/*.h" "display/*.cpp")
# The application objects are compiled once. The mod library links them, and
# so do the Kobo adapter tests (KOBALT_BUILD_TESTS). NickelHook's loader stays
# in the mod library only.
add_library(kobalt-code OBJECT ${shared} ${adapters} common/install.c common/startup_recovery.c)
add_library(kobalt MODULE "${hooks}/nh.c" "${hooks}/resources.c" "${CMAKE_CURRENT_BINARY_DIR}/nhplugin.h")
set_source_files_properties("${source}/reading/reading_timer.cpp" PROPERTIES
    COMPILE_DEFINITIONS KOBALT_READING_CLOCK_BOOTTIME=1)
option(KOBALT_SQLITE_ANNOTATIONS_DEFAULT "Use SQLite annotation storage by default" ON)
if(KOBALT_SQLITE_ANNOTATIONS_DEFAULT)
    set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
        COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=1)
else()
    set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
        COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=0)
endif()
option(KOBALT_PAGE_ANIMATIONS "Enable page animations on compatible display backends" ON)
# Keep parsed books across app updates when the rendering dependencies are compatible.
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
foreach(target kobalt-code kobalt)
    target_include_directories(${target} PRIVATE "${hooks}" "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(${target} SYSTEM PRIVATE "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
        "${prefix}/include" "${prefix}/include/harfbuzz" "${prefix}/include/freetype2" "${prefix}/include/fribidi")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
        "-iquote${KOBALT_ROOT}" "-iquote${source}")
    target_compile_definitions(${target} PRIVATE KOBALT_READER_HARDWARE
        $<$<BOOL:${KOBALT_PAGE_ANIMATIONS}>:KOBALT_PAGE_ANIMATIONS>
        KOBALT_VERSION="${KOBALT_VERSION}" NH_VERSION="${KOBALT_VERSION}"
        KOBALT_CONFIG_DIR="/mnt/onboard/.adds/kobalt" KOBALT_SYSTEM_FONT_DIRECTORY="${KOBALT_SYSTEM_FONT_DIRECTORY}"
        KOBALT_LAYOUT_POLICY="${layout_policy}" KOBALT_RENDERING_POLICY="${rendering_policy}")
endforeach()

set(engine "${CMAKE_CURRENT_BINARY_DIR}/libcrengine-reader.a")
add_custom_command(OUTPUT "${engine}"
    COMMAND "${CMAKE_OBJCOPY}" --redefine-sym FT_Load_Glyph=kobalt_font_load_glyph
        --redefine-sym FT_Render_Glyph=kobalt_font_render_glyph "${CRENGINE_BUILD}/libcrengine.a" "${engine}"
    DEPENDS "${CRENGINE_BUILD}/libcrengine.a" VERBATIM)
add_custom_target(reader-engine DEPENDS "${engine}")
add_dependencies(kobalt reader-engine)
set(qt_libraries Qt${KOBALT_QT_MAJOR}::Widgets Qt${KOBALT_QT_MAJOR}::GuiPrivate
    Qt${KOBALT_QT_MAJOR}::Network Qt${KOBALT_QT_MAJOR}::Sensors Qt${KOBALT_QT_MAJOR}::Sql Qt${KOBALT_QT_MAJOR}::DBus)
if(KOBALT_QT_MAJOR EQUAL 5)
    list(APPEND qt_libraries Qt5::WidgetsPrivate)
endif()
set(kobalt_libraries ${qt_libraries}
    "${engine}" "${prefix}/lib/libharfbuzz.a" "${prefix}/lib/libxxhash.a" "${prefix}/lib/libutf8proc.a"
    "${prefix}/lib/libjpeg.a" "${prefix}/lib/liblunasvg.a" "${prefix}/lib/libfreetype.a"
    "${prefix}/lib/libfribidi.a" "${prefix}/lib/libunibreak.a" "${KOBALT_TEST_PNG}" z dl pthread)
target_link_libraries(kobalt-code PRIVATE ${qt_libraries})
target_link_libraries(kobalt PRIVATE kobalt-code ${kobalt_libraries})
# An evaluation package adds the scenario player, its scenario file, the test
# books and the diagnostics switch. Release packages never contain them.
option(KOBALT_VERBOSE_DIAGNOSTICS "Enable verbose diagnostics without a profile marker" ON)
if(KOBALT_VERBOSE_DIAGNOSTICS)
    set_property(SOURCE "${source}/support/diagnostics.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS KOBALT_VERBOSE_DIAGNOSTICS=1)
    set(package_suffix "-diagnostic")
endif()
option(KOBALT_EVAL_PACKAGE "Build the evaluation package" OFF)
if(KOBALT_EVAL_PACKAGE)
    target_sources(kobalt-code PRIVATE "${source}/scenario/player.cpp")
    target_compile_definitions(kobalt-code PRIVATE KOBALT_SCENARIO_PLAYER)
    target_compile_definitions(kobalt PRIVATE KOBALT_SCENARIO_PLAYER)
    set(package_suffix "-eval")
endif()
target_link_options(kobalt PRIVATE -Wl,--no-undefined
    "LINKER:--whole-archive,${prefix}/lib/libfreetype.a,--no-whole-archive"
    "LINKER:--exclude-libs,libharfbuzz.a:libjpeg.a:liblunasvg.a:libutf8proc.a:libfribidi.a:libunibreak.a:libfreetype.a")

set(resources "${KOBALT_ROOT}/assets")
file(GLOB notices CONFIGURE_DEPENDS "${resources}/notices/*")
set(qrc "<RCC><qresource prefix=\"/kobalt\">")
foreach(path "${resources}/acknowledgments.json" ${notices}
        "${resources}/samples/words-and-letters.html" "${resources}/samples/layout.html"
        "${resources}/samples/hanging-punctuation.html")
    file(RELATIVE_PATH alias "${resources}" "${path}")
    string(APPEND qrc "<file alias=\"${alias}\">${path}</file>")
endforeach()
string(APPEND qrc "</qresource></RCC>")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/notices.qrc" CONTENT "${qrc}" @ONLY)
if(KOBALT_QT_MAJOR EQUAL 5)
    qt5_add_resources(compiled_resources "${CMAKE_CURRENT_BINARY_DIR}/notices.qrc")
else()
    qt6_add_resources(compiled_resources "${CMAKE_CURRENT_BINARY_DIR}/notices.qrc")
endif()
target_sources(kobalt PRIVATE ${compiled_resources})
set(package_file "${CMAKE_CURRENT_BINARY_DIR}/Kobalt-qt${KOBALT_QT_MAJOR}${package_suffix}.zip")
add_custom_command(OUTPUT "${package_file}"
    COMMAND "${CMAKE_COMMAND}" "-DLIBRARY=$<TARGET_FILE:kobalt>" "-DSTRIP=${CMAKE_STRIP}"
        "-DASSETS=${KOBALT_ASSETS}" "-DQT_MAJOR=${KOBALT_QT_MAJOR}" "-DOUTPUT=${package_file}"
        "-DEVAL=${KOBALT_EVAL_PACKAGE}" "-DROOT=${KOBALT_ROOT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/package.cmake"
    DEPENDS kobalt ${asset_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/package.cmake"
        "${KOBALT_ROOT}/platforms/eval/scenarios.json" VERBATIM)
add_custom_target(package DEPENDS "${package_file}")

add_custom_target(check-package
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-package.sh" "${package_file}" "qt${KOBALT_QT_MAJOR}"
    DEPENDS package VERBATIM)

# The Kobo adapter tests build with this SDK and run under qemu-arm against its
# Qt libraries. `bash platforms/kobo/build.sh qt5 --check` builds and runs them.
option(KOBALT_BUILD_TESTS "Build the Kobo adapter tests" OFF)
if(KOBALT_BUILD_TESTS)
    enable_testing()
    find_program(KOBALT_QEMU qemu-arm REQUIRED)
    # The tests load the firmware's Qt libraries with the image's cross C
    # library. The image's qemu-arm crashes in firmware 4's glibc 2.19 loader.
    get_target_property(qt_core Qt${KOBALT_QT_MAJOR}::Core LOCATION)
    get_filename_component(qt_libraries_directory "${qt_core}" DIRECTORY)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${KOBALT_QEMU};-L;/usr/arm-linux-gnueabihf;-E;LD_LIBRARY_PATH=${qt_libraries_directory}:/usr/lib/arm-linux-gnueabihf")
    get_target_property(sqlite_plugin Qt${KOBALT_QT_MAJOR}::QSQLiteDriverPlugin LOCATION)
    get_filename_component(plugin_directory "${sqlite_plugin}" DIRECTORY)
    get_filename_component(plugin_directory "${plugin_directory}" DIRECTORY)
    set(test_environment "QT_PLUGIN_PATH=${plugin_directory}" KOBALT_STRICT_MAIN_THREAD=1
        QT_QPA_PLATFORM=offscreen)

    # The tests link the same application objects as the mod library. The
    # archive gives each test only the objects it uses.
    add_library(kobalt-test-code STATIC $<TARGET_OBJECTS:kobalt-code>)
    # Pewter, the fake Nickel: the shared functions and those of this SDK's
    # firmware. Nothing calls them directly; the adapter finds them with dlsym,
    # as it finds Nickel's. An object library keeps every one of them linked.
    set(pewter "${KOBALT_ROOT}/tests/pewter")
    add_library(pewter OBJECT "${pewter}/pewter.cpp" "${pewter}/nickel.cpp"
        "${pewter}/nickel_qt${KOBALT_QT_MAJOR}.cpp" "${pewter}/pewter.h" "${pewter}/nickel.h")
    set_target_properties(pewter PROPERTIES AUTOMOC OFF)
    target_compile_options(pewter PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(pewter PUBLIC Qt${KOBALT_QT_MAJOR}::Core Qt${KOBALT_QT_MAJOR}::Sql)
    add_executable(kobalt-pewter-tests "${pewter}/progress.cpp")
    set_target_properties(kobalt-pewter-tests PROPERTIES AUTOMOC OFF ENABLE_EXPORTS ON)
    target_include_directories(kobalt-pewter-tests PRIVATE "${KOBALT_ROOT}" "${source}"
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(kobalt-pewter-tests PRIVATE KOBALT_READER_HARDWARE)
    target_compile_options(kobalt-pewter-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kobalt-pewter-tests PRIVATE pewter kobalt-test-code ${kobalt_libraries})
    add_test(NAME pewter.progress COMMAND kobalt-pewter-tests)
    add_executable(kobalt-tile-cover-tests "${pewter}/tile_cover.cpp")
    set_target_properties(kobalt-tile-cover-tests PROPERTIES ENABLE_EXPORTS ON)
    target_include_directories(kobalt-tile-cover-tests PRIVATE "${KOBALT_ROOT}" "${source}")
    target_compile_options(kobalt-tile-cover-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kobalt-tile-cover-tests PRIVATE kobalt-test-code ${kobalt_libraries})
    add_test(NAME pewter.tile-cover COMMAND kobalt-tile-cover-tests)
    set_tests_properties(pewter.tile-cover PROPERTIES LABELS "pewter;kobo;qt${KOBALT_QT_MAJOR}"
        ENVIRONMENT "${test_environment}" TIMEOUT 120)
    set(tests kobalt-pewter-tests kobalt-tile-cover-tests)
    set_tests_properties(pewter.progress PROPERTIES LABELS "pewter;qt${KOBALT_QT_MAJOR}"
        ENVIRONMENT "${test_environment}" TIMEOUT 120)

    # The Qt5 refresh check fakes the display driver call itself and does not
    # use pewter's Nickel.
    if(KOBALT_QT_MAJOR EQUAL 5)
        add_executable(kobalt-qt5-refresh-tests "${pewter}/refresh_qt5.cpp"
            nickel-qt5/refresh.cpp display/page_animation.cpp)
        target_include_directories(kobalt-qt5-refresh-tests PRIVATE "${KOBALT_ROOT}" "${source}"
            "${KOBALT_ROOT}/tests/support")
        target_compile_definitions(kobalt-qt5-refresh-tests PRIVATE KOBALT_PAGE_ANIMATIONS)
        target_compile_options(kobalt-qt5-refresh-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(kobalt-qt5-refresh-tests PRIVATE Qt5::Widgets ${CMAKE_DL_LIBS} pthread)
        add_test(NAME pewter.qt5-refresh COMMAND kobalt-qt5-refresh-tests)
        set_tests_properties(pewter.qt5-refresh PROPERTIES LABELS "pewter;display;qt5"
            ENVIRONMENT "${test_environment}" TIMEOUT 120)
        list(APPEND tests kobalt-qt5-refresh-tests)
    endif()
    add_executable(kobalt-native-keyboard-tests "${pewter}/keyboard.cpp")
    set_target_properties(kobalt-native-keyboard-tests PROPERTIES ENABLE_EXPORTS ON)
    target_include_directories(kobalt-native-keyboard-tests PRIVATE "${KOBALT_ROOT}" "${source}"
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(kobalt-native-keyboard-tests PRIVATE kobalt-test-code ${kobalt_libraries})
    add_test(NAME pewter.keyboard COMMAND kobalt-native-keyboard-tests)
    set_tests_properties(pewter.keyboard PROPERTIES LABELS "pewter;keyboard;qt${KOBALT_QT_MAJOR}"
        ENVIRONMENT "${test_environment};QT_QPA_FONTDIR=${KOBALT_ASSETS}/fonts" TIMEOUT 120)
    list(APPEND tests kobalt-native-keyboard-tests)
    add_executable(kobalt-native-ui-tests "${pewter}/native_ui.cpp")
    set_target_properties(kobalt-native-ui-tests PROPERTIES AUTOMOC OFF ENABLE_EXPORTS ON)
    target_include_directories(kobalt-native-ui-tests PRIVATE "${KOBALT_ROOT}" "${source}")
    target_link_libraries(kobalt-native-ui-tests PRIVATE pewter kobalt-test-code ${kobalt_libraries})
    add_test(NAME pewter.native-ui COMMAND kobalt-native-ui-tests)
    set_tests_properties(pewter.native-ui PROPERTIES LABELS "pewter;sleep;qt${KOBALT_QT_MAJOR}"
        ENVIRONMENT "${test_environment}" TIMEOUT 120)
    list(APPEND tests kobalt-native-ui-tests)
    add_custom_target(kobo-tests DEPENDS ${tests})
endif()
