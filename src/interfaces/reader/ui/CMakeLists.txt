cmake_minimum_required(VERSION 3.16)
project(reader_widgets LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT DEFINED KOBALT_QT_MAJOR)
    message(FATAL_ERROR "Build reader-widgets through the selected Kobalt application target")
endif()
add_library(reader-widgets STATIC
    style/theme.cpp
    style/font_data.cpp
    style/fonts.cpp
    style/private_text.cpp
    controls/buttons.cpp
    controls/icons.cpp
    controls/text.cpp
    controls/rows.cpp
    controls/options.cpp
    controls/sliders.cpp
    controls/paging.cpp
    controls/panels.cpp
    controls/paged_text.cpp
    dialogs/modals.cpp)
set_target_properties(reader-widgets PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(reader-widgets PUBLIC Qt${KOBALT_QT_MAJOR}::Widgets reader-fonts)
target_include_directories(reader-widgets SYSTEM PRIVATE
    "${KOBALT_DEPENDENCY_PREFIX}/include/freetype2"
    "${KOBALT_DEPENDENCY_PREFIX}/include/harfbuzz"
    "${KOBALT_DEPENDENCY_PREFIX}/include/fribidi")
target_link_libraries(reader-widgets PRIVATE
    "${KOBALT_DEPENDENCY_PREFIX}/lib/libharfbuzz.a"
    "${KOBALT_DEPENDENCY_PREFIX}/lib/libfreetype.a"
    "${KOBALT_DEPENDENCY_PREFIX}/lib/libfribidi.a" z)
if(KOBALT_TEST_PNG)
    target_link_libraries(reader-widgets PRIVATE "${KOBALT_TEST_PNG}" z)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake/EngineSources.cmake")
kobalt_link_private_freetype(reader-widgets "${KOBALT_DEPENDENCY_PREFIX}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(reader-widgets INTERFACE
        "LINKER:--exclude-libs,libharfbuzz.a:libfribidi.a")
endif()
target_compile_options(reader-widgets PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
get_filename_component(KOBALT_ICON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
file(GLOB READER_ICON_SVGS CONFIGURE_DEPENDS "${KOBALT_ICON_ROOT}/resources/icons/phosphor/*.svg")
set(READER_ICON_GEOMETRY "${CMAKE_CURRENT_BINARY_DIR}/reader_icon_vectors.h")
add_custom_command(OUTPUT "${READER_ICON_GEOMETRY}"
    COMMAND "${Python3_EXECUTABLE}" "${KOBALT_ICON_ROOT}/tools/ui_icon_vectors.py"
        "${KOBALT_ICON_ROOT}/resources/icons/phosphor" "${READER_ICON_GEOMETRY}"
    DEPENDS "${KOBALT_ICON_ROOT}/tools/ui_icon_vectors.py"
        "${KOBALT_ICON_ROOT}/resources/icons/phosphor/manifest.json" ${READER_ICON_SVGS}
    VERBATIM)
target_sources(reader-widgets PRIVATE "${READER_ICON_GEOMETRY}")
target_include_directories(reader-widgets PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
