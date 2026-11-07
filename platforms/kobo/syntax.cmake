# Checks the sources that the Kobo builds compile with the Mac's clang in
# syntax-only mode, against the headers of the prepared Kobo SDK images. See
# doc/building.md#kobo-syntax-check.
#
#   cmake [-DSDK=qt5|qt6|all] [-DEVAL=ON] -P platforms/kobo/syntax.cmake [file...]
#
# Each lane uses the Qt headers, C++ library, system headers, flags and
# definitions of one Kobo build:
#
# - qt5: Qt 5.2, the GCC 4.9 libstdc++ and the sysroot of the Qt5 image.
# - qt6: Qt 6.5, the GCC 11 libstdc++ and the glibc of the Qt6 image.
#
# SDK selects the lanes. The default is all, which checks both. EVAL=ON also
# checks the evaluation package of each selected SDK: every source again with
# KOBALT_SCENARIO_PLAYER, and the scenario player.
#
# Without file arguments it checks every source of the selected lanes. One
# generated Ninja file with clang depfiles holds all lanes and keeps the check
# incremental: a later run checks only the files whose sources, headers or
# flags changed. File arguments check just those sources, in each selected
# lane that compiles them. A header argument checks the whole selected lanes,
# because the depfiles decide which sources include it.
#
# The first run of a lane copies the SDK headers out of its prepared image into
# the shared dependency cache, at <cache>/sdk-headers/<sdk>-<key>/. The key is
# a hash of the pinned image digest in platforms/sdk-image.sh and the copy
# recipe below, so all worktrees share one copy per image, and later runs need
# no container engine until the key changes. The copy is made in a temporary
# folder in the cache and then renamed into place, so a concurrent run sees a
# whole copy or none. The script never pulls or builds an image.
#
# Kobalt's generated headers and the dependency headers come from the
# configured desktop build. Those headers do not depend on the target, except
# HAVE_OFF64_T in crsetup.h, which macOS leaves undefined. The check defines it
# the way the Kobo build does.
#
# This check finds compile errors early. It does not replace
# `mac-kobo-checks`, which compiles and links with the real SDKs.
cmake_minimum_required(VERSION 3.22)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${root}" root)
include("${root}/dependencies/cache.cmake")
set(work "${root}/build/kobo-syntax")
set(engine "$ENV{CONTAINER_ENGINE}")
if(NOT engine)
    set(engine docker)
endif()

if(NOT DEFINED SDK OR SDK STREQUAL "all")
    set(sdks qt5 qt6)
elseif(SDK MATCHES "^qt[56]$")
    set(sdks "${SDK}")
else()
    message(FATAL_ERROR "SDK must be qt5, qt6 or all")
endif()
set(lanes ${sdks})
if(EVAL)
    foreach(sdk IN LISTS sdks)
        list(APPEND lanes "${sdk}-eval")
    endforeach()
endif()

# Escapes a path for a Ninja build line.
function(ninja_path output path)
    string(REPLACE "$" "$$" path "${path}")
    string(REPLACE " " "$ " path "${path}")
    string(REPLACE ":" "$:" path "${path}")
    set(${output} "${path}" PARENT_SCOPE)
endfunction()

# Writes a file only when its content changes, so Ninja keeps its stamps.
function(write_if_changed path content)
    file(WRITE "${path}.new" "${content}")
    file(COPY_FILE "${path}.new" "${path}" ONLY_IF_DIFFERENT)
    file(REMOVE "${path}.new")
endfunction()

# Writes clang arguments as a response file, one quoted argument per line.
function(write_response_file path)
    set(content "")
    foreach(argument IN LISTS ARGN)
        string(REPLACE "\\" "\\\\" argument "${argument}")
        string(REPLACE "\"" "\\\"" argument "${argument}")
        string(APPEND content "\"${argument}\"\n")
    endforeach()
    write_if_changed("${path}" "${content}")
endfunction()

# What each SDK copies out of its image: pairs of an image path and a path in
# the cache entry. Ubuntu's cross glibc headers come before the Qt5 sysroot's
# in the image's compiler, so the Qt5 lane copies both and keeps that order.
# The Qt6 compiler reads zlib from the image's /usr/include; the check copies
# only the headers that the Qt6 build reads from there.
set(qt5_copies
    "/tc/qt5/include" "cxx"
    "/tc/qt5/cxx-compat.h" "cxx-compat.h"
    "/usr/arm-linux-gnueabihf/include" "cross-include"
    "/tc/qt5/sysroot/usr/include" "sysroot/usr/include"
    "/tc/qt5/sysroot/usr/mkspecs/linux-arm-nickel-gnueabihf-g++" "sysroot/usr/mkspecs/linux-arm-nickel-gnueabihf-g++"
    "/tc/qt5/sysroot/usr/mkspecs/linux-g++" "sysroot/usr/mkspecs/linux-g++"
    "/tc/qt5/sysroot/usr/mkspecs/common" "sysroot/usr/mkspecs/common")
set(qt6_copies
    "/tc/arm-kobo-linux-gnueabihf/qt6/include" "qt/include"
    "/tc/arm-kobo-linux-gnueabihf/qt6/mkspecs/linux-arm-kobo-gnueabihf-g++" "qt/mkspecs/linux-arm-kobo-gnueabihf-g++"
    "/tc/arm-kobo-linux-gnueabihf/qt6/mkspecs/linux-g++" "qt/mkspecs/linux-g++"
    "/tc/arm-kobo-linux-gnueabihf/qt6/mkspecs/common" "qt/mkspecs/common"
    "/usr/arm-linux-gnueabihf/include" "cross-include"
    "/usr/include/zlib.h" "device-include/zlib.h"
    "/usr/include/zconf.h" "device-include/zconf.h")
# Qt 5.2 tests whether an enum is unsigned with T(-1). Current Apple clang
# rejects that out-of-range enum value as a hard error in every QFlags. The
# Qt5 copy asks the underlying type instead, which gives GCC's answer.
set(qt5_overlay "QtCore/qtypetraits.h")
set(qt5_overlay_old "struct is_unsigned\n    : integral_constant<bool, (T(0) < T(-1))> {};")
set(qt5_overlay_new "struct is_unsigned\n    : integral_constant<bool, std::is_unsigned<typename std::conditional<std::is_enum<T>::value,\n          std::underlying_type<T>, std::common_type<T> >::type::type>::value> {};")
set(qt6_overlay "")

# Removes a container that an interrupted copy left behind.
function(remove_recorded_container cidfile)
    if(NOT EXISTS "${cidfile}")
        return()
    endif()
    file(READ "${cidfile}" container)
    string(STRIP "${container}" container)
    if(container)
        execute_process(COMMAND "${engine}" rm -f "${container}" RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "Cannot remove the copy container; its ID is retained in ${cidfile}")
        endif()
    endif()
    file(REMOVE "${cidfile}")
endfunction()

# Copies the SDK headers out of `image` into `partial`. Sets `failure` to an
# error message, or to an empty string when the copy is whole.
function(copy_sdk sdk image partial failure)
    set(${failure} "" PARENT_SCOPE)
    set(cidfile "${work}/${sdk}-copy.cid")
    remove_recorded_container("${cidfile}")
    execute_process(COMMAND "${engine}" create --pull=never --cidfile "${cidfile}" "${image}" true
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        remove_recorded_container("${cidfile}")
        set(${failure} "Cannot create the copy container: ${error}" PARENT_SCOPE)
        return()
    endif()
    file(READ "${cidfile}" container)
    string(STRIP "${container}" container)
    set(copies ${${sdk}_copies})
    set(error_message "")
    while(copies AND NOT error_message)
        list(POP_FRONT copies source destination)
        get_filename_component(parent "${partial}/${destination}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        execute_process(COMMAND "${engine}" cp -q "${container}:${source}" "${partial}/${destination}"
            RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
        if(NOT status EQUAL 0)
            set(error_message "Cannot copy ${source} from the ${sdk} image: ${error}")
        endif()
    endwhile()
    remove_recorded_container("${cidfile}")
    if(error_message)
        set(${failure} "${error_message}" PARENT_SCOPE)
        return()
    endif()
    if(${sdk}_overlay)
        set(header "${partial}/sysroot/usr/include/${${sdk}_overlay}")
        file(READ "${header}" text)
        string(FIND "${text}" "${${sdk}_overlay_old}" found)
        if(found EQUAL -1)
            set(${failure} "${${sdk}_overlay} in the ${sdk} image no longer has the expected text. Update the overlay in platforms/kobo/syntax.cmake." PARENT_SCOPE)
            return()
        endif()
        string(REPLACE "${${sdk}_overlay_old}" "${${sdk}_overlay_new}" text "${text}")
        file(WRITE "${partial}/overlay/${${sdk}_overlay}" "${text}")
    endif()
    file(WRITE "${partial}/image" "${image}\n")
endfunction()

# Publishes the SDK headers of `image` at `entry`, unless they are there.
function(prepare_sdk sdk image entry)
    if(EXISTS "${entry}/image")
        return()
    endif()
    execute_process(COMMAND "${engine}" image inspect --format "{{.Id}}" "${image}"
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        string(STRIP "${error}" error)
        message(FATAL_ERROR "The prepared ${sdk} image is not available locally (${error}).\n"
            "This check never pulls images. Get it yourself with: ${engine} pull ${image}\n"
            "Or check only the other SDK with -DSDK=qt5 or -DSDK=qt6.")
    endif()
    message(STATUS "Copying the ${sdk} SDK headers out of ${image} into ${entry}")
    string(RANDOM LENGTH 16 token)
    set(partial "${entry}.${token}.partial")
    file(MAKE_DIRECTORY "${partial}")
    copy_sdk("${sdk}" "${image}" "${partial}" failure)
    if(failure)
        file(REMOVE_RECURSE "${partial}")
        message(FATAL_ERROR "${failure}")
    endif()
    # A directory rename is atomic and fails when the entry exists, because an
    # entry is never empty. A concurrent copy that finished first keeps its
    # entry. NO_REPLACE fails for directories on macOS.
    file(RENAME "${partial}" "${entry}" RESULT renamed)
    if(NOT renamed STREQUAL "0")
        file(REMOVE_RECURSE "${partial}")
        if(NOT EXISTS "${entry}/image")
            message(FATAL_ERROR "Cannot publish the ${sdk} SDK headers at ${entry}: ${renamed}")
        endif()
    endif()
endfunction()

# Find the SDK header entries of both SDKs. Ninja needs the paths of every
# lane, also of lanes that this run does not check.
kobalt_dependency_cache_root("${root}" "$ENV{KOBALT_DEPENDENCY_CACHE}" cache)
foreach(sdk qt5 qt6)
    execute_process(COMMAND sh "${root}/platforms/sdk-image.sh" ${sdk}
        RESULT_VARIABLE status OUTPUT_VARIABLE image OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status EQUAL 0 OR NOT image MATCHES "@sha256:[0-9a-f]+$")
        message(FATAL_ERROR "Cannot read the pinned ${sdk} image from platforms/sdk-image.sh")
    endif()
    # Change the format when a change to copy_sdk changes the copied files.
    string(SHA256 key "format 1\n${image}\n${${sdk}_copies}\n${${sdk}_overlay}\n${${sdk}_overlay_old}\n${${sdk}_overlay_new}")
    string(SUBSTRING "${key}" 0 16 key)
    set(${sdk}_image "${image}")
    set(${sdk}_sdk "${cache}/sdk-headers/${sdk}-${key}")
endforeach()

file(MAKE_DIRECTORY "${work}" "${cache}/sdk-headers")
# The lock lasts until this process ends. GUARD FILE crashes CMake 4.3 in
# script mode.
file(LOCK "${work}/check.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE locked)
if(NOT locked STREQUAL "0")
    message(FATAL_ERROR "Another Kobo syntax check runs in this worktree")
endif()
# The retired Qt5-only check kept its own copy of the headers here.
file(REMOVE_RECURSE "${root}/build/qt5-syntax")
foreach(sdk IN LISTS sdks)
    prepare_sdk("${sdk}" "${${sdk}_image}" "${${sdk}_sdk}")
endforeach()

# Find the configured desktop build for generated and dependency headers.
set(desktop "")
foreach(candidate desktop-mac desktop-linux)
    if(EXISTS "${root}/build/${candidate}/build.ninja")
        set(desktop "${root}/build/${candidate}")
        break()
    endif()
endforeach()
if(NOT desktop)
    message(FATAL_ERROR "No configured desktop build. Run `cmake --preset mac` in platforms/desktop/ first.")
endif()
file(STRINGS "${desktop}/build.ninja" includes REGEX "^  INCLUDES = .*/crengine/include" LIMIT_COUNT 1)
string(REGEX MATCHALL "-isystem [^ ]+" includes "${includes}")
set(dependency_includes "")
foreach(include IN LISTS includes)
    string(REPLACE "-isystem " "" include "${include}")
    if(include MATCHES "crengine|/install/include")
        list(APPEND dependency_includes "${include}")
    endif()
endforeach()
if(NOT dependency_includes)
    message(FATAL_ERROR "Cannot find the dependency headers in ${desktop}/build.ninja. Build the desktop app once.")
endif()

set(generated "${work}/generated")
file(MAKE_DIRECTORY "${generated}")
foreach(name build_info.h kobalt_strings.inc reader_styles.h reader_hyphenation.h dictionary_catalog.h annotation_export_html.h)
    if(NOT EXISTS "${desktop}/${name}")
        message(FATAL_ERROR "Missing ${desktop}/${name}. Configure the desktop build again.")
    endif()
    file(COPY_FILE "${desktop}/${name}" "${generated}/${name}" ONLY_IF_DIFFERENT)
endforeach()
# The Kobo configure step embeds resource files here. Their content does not
# matter to a syntax check.
write_if_changed("${generated}/embedded_resources.h"
    "#include <resources.h>\nstatic const struct nh_resource nh_embedded_resources[] = {{NULL, NULL, 0}};\n")

# Select the sources the same way as platforms/kobo/CMakeLists.txt. Keep both
# lists the same.
set(kobo "${root}/platforms/kobo")
set(hooks "${kobo}/toolchains/nickelhook-qt6")
file(GLOB_RECURSE shared "${root}/src/*.cpp")
list(FILTER shared EXCLUDE REGEX "/src/scenario/")
list(FILTER shared EXCLUDE REGEX "/(library|app/library)/")
set(c_sources "${kobo}/common/install.c" "${kobo}/common/startup_recovery.c" "${hooks}/nh.c" "${hooks}/resources.c")
foreach(sdk qt5 qt6)
    file(GLOB adapters "${kobo}/common/*.cpp" "${kobo}/nickel-${sdk}/*.cpp" "${kobo}/display/*.cpp")
    set(${sdk}_cxx_sources ${shared} ${adapters})
    list(SORT ${sdk}_cxx_sources)
    set(${sdk}_c_sources ${c_sources})
    # An evaluation package adds the scenario player.
    set(${sdk}-eval_cxx_sources ${${sdk}_cxx_sources} "${root}/src/scenario/player.cpp")
    set(${sdk}-eval_c_sources ${c_sources})
    set(${sdk}-eval_test_sources "")
endforeach()
# KOBALT_BUILD_TESTS adds the pewter tests. The Qt5 lane also has the Qt5
# refresh check.
set(pewter "${root}/tests/pewter")
set(pewter_sources "${pewter}/pewter.cpp" "${pewter}/nickel.cpp" "${pewter}/progress.cpp" "${pewter}/keyboard.cpp" "${pewter}/native_ui.cpp" "${pewter}/tile_cover.cpp")
set(qt5_test_sources ${pewter_sources} "${pewter}/nickel_qt5.cpp" "${pewter}/refresh_qt5.cpp")
set(qt6_test_sources ${pewter_sources} "${pewter}/nickel_qt6.cpp")

# AUTOMOC output is not available here. An empty file is enough for a syntax
# check; -Wno-undefined-internal accepts the missing moc definitions.
set(moc "${work}/moc")
file(MAKE_DIRECTORY "${moc}")
set(moc_sources ${qt5_cxx_sources} ${qt6_cxx_sources} ${qt5_test_sources} ${qt6_test_sources}
    "${root}/src/scenario/player.cpp")
list(REMOVE_DUPLICATES moc_sources)
foreach(source IN LISTS moc_sources)
    file(STRINGS "${source}" lines REGEX "^#include \"([^\"]+\\.moc|moc_[^\"]+\\.cpp)\"")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^#include \"([^\"]+)\".*" "\\1" name "${line}")
        if(NOT EXISTS "${moc}/${name}")
            file(WRITE "${moc}/${name}" "")
        endif()
    endforeach()
endforeach()

# Flags of the Kobo builds: the compiler wrappers of the images (made by
# kobuild) and platforms/kobo/CMakeLists.txt. Keep them the same as those
# files.
find_program(clang clang++ REQUIRED)
execute_process(COMMAND "${clang}" -print-resource-dir
    RESULT_VARIABLE status OUTPUT_VARIABLE resources OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot run ${clang}")
endif()
set(target -target armv7a-unknown-linux-gnueabihf -mfloat-abi=hard -mfpu=neon -fPIC -fsyntax-only -nostdinc -DNDEBUG)
# GCC's -Werror set, without the clang warnings that GCC 11 does not report,
# and with the GCC -Wextra warnings that clang leaves out of -Wextra.
set(warnings -Wall -Wextra -Werror -Wno-unused-private-field -Wno-c++20-extensions
    -Wno-unused-lambda-capture -Wno-inconsistent-missing-override -Wno-overloaded-virtual -Wno-self-assign
    -Wno-undefined-internal -Wno-deprecated-non-prototype -Wimplicit-fallthrough -Wtype-limits -Wtautological-unsigned-zero-compare)
set(qt_defines -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -DQT_NO_DEBUG)
set(kobalt_defines "-DKOBALT_CONFIG_DIR=\"/mnt/onboard/.adds/kobalt\"" "-DKOBALT_LAYOUT_POLICY=\"syntax-check\""
    -DKOBALT_PAGE_ANIMATIONS -DKOBALT_READER_HARDWARE
    "-DKOBALT_VERSION=\"syntax-check\"" "-DNH_VERSION=\"syntax-check\""
    ${qt_defines} -DQT_DBUS_LIB -DQT_NETWORK_LIB -DQT_SENSORS_LIB -DQT_SQL_LIB -Dkobalt_EXPORTS -DHAVE_OFF64_T=1)
set(kobalt_includes "-iquote${root}" "-iquote${root}/src" "-I${hooks}" "-I${generated}" "-I${moc}")
foreach(include IN LISTS dependency_includes)
    list(APPEND kobalt_includes -isystem "${include}")
endforeach()

# Qt5: the image's compiler wrapper /tc/qt5/bin/g++.
set(sdk "${qt5_sdk}")
set(qt "${sdk}/sysroot/usr/include")
set(qt5_target "--sysroot=${sdk}/sysroot")
set(qt5_defines "-DKOBALT_SYSTEM_FONT_DIRECTORY=\"/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/fonts\"")
# Same order as the Qt5 build: the overlay replaces one Qt header.
set(qt5_qt_includes -isystem "${sdk}/overlay")
foreach(module QtWidgets QtGui QtCore QtNetwork QtSensors QtSql QtDBus)
    list(APPEND qt5_qt_includes -isystem "${qt}/${module}")
endforeach()
list(APPEND qt5_qt_includes -isystem "${sdk}/sysroot/usr/mkspecs/linux-arm-nickel-gnueabihf-g++")
foreach(module QtGui QtCore QtWidgets)
    list(APPEND qt5_qt_includes -isystem "${qt}/${module}/5.2.1" -isystem "${qt}/${module}/5.2.1/${module}")
endforeach()
# Same order as `g++ -v` in the image: libstdc++ 4.9, compiler headers, the
# cross glibc 2.35 headers, then the sysroot's glibc 2.19 headers.
set(qt5_cxx_library -std=gnu++17 -fno-sized-deallocation -include "${sdk}/cxx-compat.h"
    -isystem "${sdk}/cxx/c++/4.9.4" -isystem "${sdk}/cxx/c++/4.9.4/arm-nickel-linux-gnueabihf")
set(qt5_system_includes -isystem "${resources}/include" -isystem "${sdk}/cross-include" -isystem "${qt}")

# Qt6: the image's compiler wrapper
# /tc/arm-kobo-linux-gnueabihf/bin/arm-kobo-linux-gnueabihf-g++.
set(sdk "${qt6_sdk}")
set(qt "${sdk}/qt/include")
set(qt6_target -mthumb)
set(qt6_defines "-DKOBALT_SYSTEM_FONT_DIRECTORY=\"/usr/share/fonts\"")
# Same order as the Qt6 build.
set(qt6_qt_includes -isystem "${qt}/QtWidgets" -isystem "${qt}" -isystem "${qt}/QtCore"
    -isystem "${sdk}/qt/mkspecs/linux-arm-kobo-gnueabihf-g++" -isystem "${qt}/QtGui")
foreach(module QtGui QtCore)
    list(APPEND qt6_qt_includes -isystem "${qt}/${module}/6.5.2" -isystem "${qt}/${module}/6.5.2/${module}")
endforeach()
foreach(module QtNetwork QtSensors QtSql QtDBus)
    list(APPEND qt6_qt_includes -isystem "${qt}/${module}")
endforeach()
# Same order as `g++ -v` in the image: libstdc++ 11, compiler headers, the
# cross glibc 2.35 headers, then the device libraries in /usr/include.
set(qt6_cxx_library -std=gnu++17 -isystem "${sdk}/cross-include/c++/11"
    -isystem "${sdk}/cross-include/c++/11/arm-linux-gnueabihf" -isystem "${sdk}/cross-include/c++/11/backward")
set(qt6_system_includes -isystem "${resources}/include" -isystem "${sdk}/cross-include"
    -isystem "${sdk}/device-include")

foreach(lane qt5 qt6 qt5-eval qt6-eval)
    string(REGEX REPLACE "-eval$" "" sdk "${lane}")
    set(lane_defines ${kobalt_defines} ${${sdk}_defines})
    if(lane MATCHES "-eval$")
        list(APPEND lane_defines -DKOBALT_SCENARIO_PLAYER)
    endif()
    set(lane_target ${target} ${${sdk}_target})
    set(lane_qt ${${sdk}_qt_includes})
    file(MAKE_DIRECTORY "${work}/${lane}")
    write_response_file("${work}/${lane}/cxx.rsp" ${lane_target} ${warnings} -Wno-missing-field-initializers
        -fvisibility=hidden ${lane_defines} ${kobalt_includes} ${lane_qt} ${${sdk}_cxx_library} ${${sdk}_system_includes})
    write_response_file("${work}/${lane}/c.rsp" ${lane_target} ${warnings} -Wno-missing-field-initializers
        -fvisibility=hidden ${lane_defines} ${kobalt_includes} ${lane_qt} ${${sdk}_system_includes})
    write_response_file("${work}/${lane}/test.rsp" ${lane_target} ${warnings} ${qt_defines} -DKOBALT_PAGE_ANIMATIONS
        "-I${root}" "-I${root}/src" "-I${root}/tests/support" "-I${moc}" ${lane_qt} ${${sdk}_cxx_library} ${${sdk}_system_includes})
endforeach()

# Per-source definitions of platforms/kobo/CMakeLists.txt with its defaults.
set(defines_reading_timer.cpp -DKOBALT_READING_CLOCK_BOOTTIME=1)
set(defines_annotation_database.cpp -DKOBALT_SQLITE_ANNOTATIONS_DEFAULT=1)
set(defines_diagnostics.cpp -DKOBALT_VERBOSE_DIAGNOSTICS=1)
# The pewter tests use the device build's view of the adapter headers.
set(defines_progress.cpp -DKOBALT_READER_HARDWARE)

# One stamp per lane and source, and one phony target per lane. Ninja reruns
# a source when it, a header in its depfile or its response file changes. The
# file always holds every lane, so a run of one lane keeps the recorded
# dependencies of the others.
set(ninja "ninja_required_version = 1.10\n")
foreach(lane qt5 qt6 qt5-eval qt6-eval)
    set(lane_stamps "")
    foreach(kind cxx c test)
        ninja_path(response "${work}/${lane}/${kind}.rsp")
        set(language "")
        if(kind STREQUAL "c")
            set(language "-x c")
        endif()
        string(APPEND ninja "rule ${lane}_${kind}\n"
            "  command = \"${clang}\" ${language} @${response} $defines -MD -MF $out.d -MT $out $in && \"${CMAKE_COMMAND}\" -E touch $out\n"
            "  depfile = $out.d\n  deps = gcc\n  description = ${lane} syntax $source\n")
        foreach(source IN LISTS ${lane}_${kind}_sources)
            file(RELATIVE_PATH relative "${root}" "${source}")
            get_filename_component(name "${source}" NAME)
            set(stamp "${lane}/stamps/${relative}.stamp")
            set(stamp_${lane}_${relative} "${stamp}")
            ninja_path(stamp_line "${stamp}")
            ninja_path(source_line "${source}")
            string(APPEND ninja "build ${stamp_line}: ${lane}_${kind} ${source_line} | ${response}\n"
                "  source = ${relative}\n  defines = ${defines_${name}}\n")
            list(APPEND lane_stamps "${stamp_line}")
            list(APPEND ${lane}_relative "${relative}")
        endforeach()
    endforeach()
    list(JOIN lane_stamps " " lane_stamps)
    string(APPEND ninja "build check-${lane}: phony ${lane_stamps}\n")
endforeach()
write_if_changed("${work}/build.ninja" "${ninja}")

# Choose the targets from the arguments.
set(targets "")
set(all_targets "")
foreach(lane IN LISTS lanes)
    list(APPEND all_targets "check-${lane}")
endforeach()
# The file arguments follow `-P <script>`; -D options come before it.
set(index 1)
while(index LESS CMAKE_ARGC AND NOT CMAKE_ARGV${index} STREQUAL "-P")
    math(EXPR index "${index} + 1")
endwhile()
math(EXPR index "${index} + 2")
while(index LESS CMAKE_ARGC)
    file(REAL_PATH "${CMAKE_ARGV${index}}" argument)
    file(RELATIVE_PATH relative "${root}" "${argument}")
    if(relative MATCHES "\\.(h|inc)$")
        set(targets "")
        break()
    endif()
    set(found OFF)
    foreach(lane IN LISTS lanes)
        if(relative IN_LIST ${lane}_relative)
            list(APPEND targets "${stamp_${lane}_${relative}}")
            set(found ON)
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "No selected Kobo build compiles ${CMAKE_ARGV${index}} (lanes: ${lanes})")
    endif()
    math(EXPR index "${index} + 1")
endwhile()
if(NOT targets)
    set(targets ${all_targets})
endif()

find_program(ninja_program ninja REQUIRED)
# Keep going after a failure, so one run reports every broken source.
execute_process(COMMAND "${ninja_program}" -C "${work}" -k 0 ${targets} RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "The Kobo syntax check found errors. Fix them before the Kobo build.")
endif()
