# Build inputs that the desktop, Kobo and Android projects share. A platform's
# CMakeLists.txt includes this file once, after it knows KOBALT_VERSION and
# has run kobalt_dependencies().
#
# Inputs: KOBALT_ROOT, KOBALT_VERSION and, for the generated headers,
# CRENGINE_SOURCE.
#
# This file only runs steps and sets variables. It defines no targets, and
# each platform keeps its own target_compile_definitions() and resource code.
#
# The Kobo syntax check includes it in script mode (cmake -P) with
# placeholders for KOBALT_VERSION, layout_policy and rendering_policy. Script
# mode stops after the variables, before the steps that need a build tree.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    # Sets layout_policy and rendering_policy. Parsed books stay valid across
    # app updates while the rendering dependencies are compatible.
    include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
endif()

# Definitions of every Kobalt target that compiles shared sources.
set(kobalt_shared_definitions KOBALT_READER_HARDWARE KOBALT_VERSION="${KOBALT_VERSION}"
    KOBALT_LAYOUT_POLICY="${layout_policy}" KOBALT_RENDERING_POLICY="${rendering_policy}")

# The annotation store reads its default from a definition of its own source.
set(kobalt_annotations_source "${KOBALT_ROOT}/src/storage/annotation_database.cpp")
set(kobalt_annotations_definition KOBALT_SQLITE_ANNOTATIONS_DEFAULT)

# Directory discovery must accept 64-bit inode numbers in a 32-bit process.
# A compiler's forced includes can read system headers before the source.
set(kobalt_custom_pins_source "${KOBALT_ROOT}/src/storage/custom_pins.cpp")
set(kobalt_custom_pins_definition _FILE_OFFSET_BITS=64)

# Files for the "/kobalt" resource prefix: the acknowledgments, the license
# notices and the sample texts of the settings previews. Paths below
# kobalt_notice_base are the resource aliases.
set(kobalt_notice_base "${KOBALT_ROOT}/assets")
set(kobalt_notice_samples "${kobalt_notice_base}/samples/words-and-letters.html"
    "${kobalt_notice_base}/samples/layout.html" "${kobalt_notice_base}/samples/hanging-punctuation.html")

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

file(GLOB kobalt_notice_files CONFIGURE_DEPENDS "${kobalt_notice_base}/notices/*")
set(kobalt_notice_resources "${kobalt_notice_base}/acknowledgments.json" ${kobalt_notice_files}
    ${kobalt_notice_samples})

option(KOBALT_SQLITE_ANNOTATIONS_DEFAULT "Use SQLite annotation storage by default" ON)
set(kobalt_annotations_default 0)
if(KOBALT_SQLITE_ANNOTATIONS_DEFAULT)
    set(kobalt_annotations_default 1)
endif()
set_source_files_properties("${kobalt_annotations_source}" PROPERTIES
    COMPILE_DEFINITIONS ${kobalt_annotations_definition}=${kobalt_annotations_default})
set_source_files_properties("${kobalt_custom_pins_source}" PROPERTIES
    COMPILE_DEFINITIONS ${kobalt_custom_pins_definition})

# reader_styles.h holds the engine's default style sheet.
file(READ "${CRENGINE_SOURCE}/cr3gui/data/html5.css" READER_HTML_CSS)
configure_file("${KOBALT_ROOT}/src/engine/crengine/reader_styles.h.in" reader_styles.h @ONLY)
# reader_hyphenation.h lists the engine's hyphenation patterns.
file(GLOB kobalt_patterns RELATIVE "${CRENGINE_SOURCE}/cr3gui/data/hyph" "${CRENGINE_SOURCE}/cr3gui/data/hyph/*.pattern")
list(SORT kobalt_patterns)
list(TRANSFORM kobalt_patterns PREPEND "\"")
list(TRANSFORM kobalt_patterns APPEND "\"")
list(JOIN kobalt_patterns ", " READER_HYPHENATION_PATTERNS)
configure_file("${KOBALT_ROOT}/src/engine/crengine/reader_hyphenation.h.in" reader_hyphenation.h @ONLY)
