# Captures the screens as they were before a feature and records them as the
# screen review's Before set:
#
#   cmake -DCASES="screens;standbySettings" [-DMATCH="zen-menu=menu;zen-chapters=chapters"]
#         [-DBASE=<commit>] [-DQT=<prefix>] -P platforms/review/baseline.cmake
#
# BASE defaults to the commit where this branch left develop. The script
# exports that commit with `git archive` into build/review-base/<commit>/,
# which changes no Git state, captures the gallery cases in CASES there with
# the shared dependency cache, and records their captures with the review
# tool's `baseline` command. CASES names gallery test functions, as
# KOBALT_GALLERY_CASE does. SCENES selects single scenes instead, but only
# after a full gallery run in that export, as KOBALT_GALLERY_SCENE needs one.
# MATCH maps a screen that the feature added or renamed to the base screen it
# replaces. QT defaults to the Qt prefix of this checkout's Mac build. A later
# run of the same commit reuses its build.
# Remove build/review-base/ with platforms/cleanup.cmake when it is no longer
# needed.
cmake_minimum_required(VERSION 3.22)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT CASES AND NOT SCENES)
    message(FATAL_ERROR "Pass -DCASES with the gallery cases to capture at the base, for example -DCASES=\"screens;standbySettings\".")
endif()

function(run)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${root}" RESULT_VARIABLE result
        OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed: ${ARGN}")
    endif()
    set(output "${output}" PARENT_SCOPE)
endfunction()

# A base taken from develop records that ref, so the review can tell when a
# rebase has moved the branch's base.
set(base_ref "")
if(NOT BASE)
    run(git merge-base HEAD develop)
    set(BASE "${output}")
    set(base_ref develop)
endif()
run(git rev-parse --verify "${BASE}^{commit}")
set(commit "${output}")
string(SUBSTRING "${commit}" 0 12 short)
set(export "${root}/build/review-base/${short}")

# The export has no Git directory, so name the main checkout's dependency
# cache explicitly. Otherwise it would rebuild every dependency.
run(git rev-parse --path-format=absolute --git-common-dir)
get_filename_component(checkout "${output}" DIRECTORY)
set(ENV{KOBALT_DEPENDENCY_CACHE} "${checkout}/build/dependency-cache")

if(NOT QT)
    file(STRINGS "${root}/build/desktop-mac/CMakeCache.txt" prefix REGEX "^CMAKE_PREFIX_PATH:")
    string(REGEX REPLACE "^[^=]*=" "" QT "${prefix}")
endif()

if(NOT EXISTS "${export}/platforms/desktop/CMakePresets.json")
    message(STATUS "Exporting ${short} to ${export}")
    file(MAKE_DIRECTORY "${export}")
    execute_process(COMMAND git archive --format=tar "${commit}" COMMAND tar -x -C "${export}"
        WORKING_DIRECTORY "${root}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not export ${commit}")
    endif()
endif()

set(desktop "${export}/platforms/desktop")
set(configure cmake --preset mac)
if(QT)
    list(APPEND configure "-DCMAKE_PREFIX_PATH=${QT}")
endif()
execute_process(COMMAND ${configure} WORKING_DIRECTORY "${desktop}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not configure the base build in ${desktop}")
endif()
if(CASES)
    string(REPLACE ";" "," cases "${CASES}")
    set(ENV{KOBALT_GALLERY_CASE} "${cases}")
    set(captures "${export}/build/screenshots/mac-cases")
else()
    string(REPLACE ";" "," scenes "${SCENES}")
    set(ENV{KOBALT_GALLERY_SCENE} "${scenes}")
    set(captures "${export}/build/screenshots/mac-scenes")
endif()
execute_process(COMMAND cmake --build --preset mac-screenshots WORKING_DIRECTORY "${desktop}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not capture the base gallery in ${desktop}")
endif()

set(record node "${root}/platforms/review/review.mjs" baseline
    --source "${captures}" --commit "${commit}")
if(base_ref)
    list(APPEND record --base-ref "${base_ref}")
endif()
foreach(match IN LISTS MATCH)
    list(APPEND record --match "${match}")
endforeach()
if(DATA)
    list(APPEND record --data "${DATA}")
endif()
execute_process(COMMAND ${record} WORKING_DIRECTORY "${root}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not record the Before set")
endif()
