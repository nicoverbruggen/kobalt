cmake_minimum_required(VERSION 3.22)
project(KobaltKobo LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(KOBALT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(retired KOBALT_DEPENDENCIES KOBALT_DEPENDENCY_CACHE KOBALT_ASSETS)
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

add_subdirectory("${KOBALT_ROOT}/dependencies" "${CMAKE_CURRENT_BINARY_DIR}/dependencies")
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

file(GLOB_RECURSE shared CONFIGURE_DEPENDS "${source}/*.cpp" "${source}/*.h")
# The scenario player is built only into evaluation packages.
list(FILTER shared EXCLUDE REGEX "/src/scenario/")
list(FILTER shared EXCLUDE REGEX "/(library|interfaces/library)/")
file(GLOB adapters CONFIGURE_DEPENDS "common/*.cpp" "common/*.h" "qt${KOBALT_QT_MAJOR}/*.cpp" "qt${KOBALT_QT_MAJOR}/*.h" "display/*.cpp")
add_library(kobalt MODULE ${shared} ${adapters} common/install.c common/startup_recovery.c "${hooks}/nh.c" "${hooks}/resources.c"
    "${CMAKE_CURRENT_BINARY_DIR}/nhplugin.h")
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
target_include_directories(kobalt PRIVATE "${hooks}" "${CMAKE_CURRENT_BINARY_DIR}"
    "${source}/interfaces/reader/ui/controls")
target_include_directories(kobalt SYSTEM PRIVATE "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${prefix}/include" "${prefix}/include/harfbuzz" "${prefix}/include/freetype2" "${prefix}/include/fribidi")
target_compile_options(kobalt PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
    "-iquote${KOBALT_ROOT}" "-iquote${source}")
option(KOBALT_PAGE_ANIMATIONS "Enable page animations on compatible display backends" ON)
target_compile_definitions(kobalt PRIVATE KOBALT_READER_HARDWARE
    $<$<BOOL:${KOBALT_PAGE_ANIMATIONS}>:KOBALT_PAGE_ANIMATIONS>
    KOBALT_VERSION="${KOBALT_VERSION}" NH_VERSION="${KOBALT_VERSION}"
    KOBALT_CONFIG_DIR="/mnt/onboard/.adds/kobalt" KOBALT_SYSTEM_FONT_DIRECTORY="${KOBALT_SYSTEM_FONT_DIRECTORY}")
# Keep parsed books across app updates when the rendering dependencies are compatible.
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
target_compile_definitions(kobalt PRIVATE KOBALT_LAYOUT_POLICY="${layout_policy}")

set(engine "${CMAKE_CURRENT_BINARY_DIR}/libcrengine-reader.a")
add_custom_command(OUTPUT "${engine}"
    COMMAND "${CMAKE_OBJCOPY}" --redefine-sym FT_Load_Glyph=kobalt_font_load_glyph
        --redefine-sym FT_Render_Glyph=kobalt_font_render_glyph "${CRENGINE_BUILD}/libcrengine.a" "${engine}"
    DEPENDS "${CRENGINE_BUILD}/libcrengine.a" VERBATIM)
add_custom_target(reader-engine DEPENDS "${engine}")
add_dependencies(reader-engine kobalt-dependencies)
add_dependencies(kobalt reader-engine)
target_link_libraries(kobalt PRIVATE Qt${KOBALT_QT_MAJOR}::Widgets Qt${KOBALT_QT_MAJOR}::GuiPrivate
    Qt${KOBALT_QT_MAJOR}::Network Qt${KOBALT_QT_MAJOR}::Sensors Qt${KOBALT_QT_MAJOR}::Sql Qt${KOBALT_QT_MAJOR}::DBus
    "${engine}" "${prefix}/lib/libharfbuzz.a" "${prefix}/lib/libxxhash.a" "${prefix}/lib/libutf8proc.a"
    "${prefix}/lib/libjpeg.a" "${prefix}/lib/liblunasvg.a" "${prefix}/lib/libfreetype.a"
    "${prefix}/lib/libfribidi.a" "${prefix}/lib/libunibreak.a" "${KOBALT_TEST_PNG}" z dl pthread)
if(KOBALT_QT_MAJOR EQUAL 5)
    target_link_libraries(kobalt PRIVATE Qt5::WidgetsPrivate)
endif()
# An evaluation package adds the scenario player, its scenario file, the test
# books and the diagnostics switch. Release packages never contain them.
option(KOBALT_VERBOSE_DIAGNOSTICS "Enable verbose diagnostics without a profile marker" OFF)
if(KOBALT_VERBOSE_DIAGNOSTICS)
    set_property(SOURCE "${source}/support/diagnostics.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS KOBALT_VERBOSE_DIAGNOSTICS=1)
    set(package_suffix "-diagnostic")
endif()
option(KOBALT_EVAL_PACKAGE "Build the evaluation package" OFF)
if(KOBALT_EVAL_PACKAGE)
    target_sources(kobalt PRIVATE "${source}/scenario/player.cpp")
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
        "${resources}/samples/words-and-letters.html" "${resources}/samples/layout.html")
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
