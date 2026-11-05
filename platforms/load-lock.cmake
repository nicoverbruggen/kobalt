# Keeps the performance evaluation apart from other evaluations and from Kobo
# builds; see doc/release-evaluation.md#machine-load. The locks live in the Git
# common directory, so all worktrees of a repository share them. The kernel
# releases a lock when its process ends, so a crashed job leaves no lock behind.
#
# - evaluation.lock: run.cmake holds it for the whole evaluation.
# - builds/<id>.lock: each running build holds its own file. A build creates and
#   locks it while it holds evaluation.lock, so it cannot register while an
#   evaluation runs, and an evaluation sees every build that registered before it.
#
# A build starts this script as a helper that holds its registration:
#
#   exec 3< <(cmake -DROOT=<checkout> -DOWNER=<build PID> -P platforms/load-lock.cmake)
#   read -r registered <&3
#
# The helper prints "registered" on its standard output when it holds the
# registration, and keeps it until the owner process ends. The build stays the
# parent of its own container, so its cleanup does not depend on the helper.
#
# run.cmake sets KOBALT_LOAD_LOCK_HELD for its child processes, so a nested
# build, such as an evaluation's device package, does not wait for its own
# parent.
cmake_minimum_required(VERSION 3.22)

function(kobalt_load_directory root output)
    set(directory "${root}/build/kobalt-load")
    execute_process(COMMAND git -C "${root}" rev-parse --git-common-dir
        RESULT_VARIABLE status OUTPUT_VARIABLE common OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(status EQUAL 0)
        get_filename_component(directory "${common}/kobalt-load" ABSOLUTE BASE_DIR "${root}")
    endif()
    # build.loadLock passes a scratch folder, so the test never takes or waits
    # for the locks that the repository shares.
    if(KOBALT_LOAD_DIRECTORY)
        set(directory "${KOBALT_LOAD_DIRECTORY}")
    endif()
    # A tree without Git, such as a source bundle, locks only itself.
    file(MAKE_DIRECTORY "${directory}/builds")
    set(${output} "${directory}" PARENT_SCOPE)
endfunction()

# Takes evaluation.lock until this process ends.
function(kobalt_lock_evaluation root)
    kobalt_load_directory("${root}" directory)
    file(LOCK "${directory}/evaluation.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE locked)
    if(NOT locked STREQUAL "0")
        message(STATUS "Waiting for another evaluation to finish (${directory}/evaluation.lock)")
        file(LOCK "${directory}/evaluation.lock" GUARD PROCESS)
    endif()
    set(ENV{KOBALT_LOAD_LOCK_HELD} 1)
endfunction()

# Returns when no registered build runs. Call it while holding evaluation.lock.
function(kobalt_wait_for_builds root)
    kobalt_load_directory("${root}" directory)
    file(GLOB registrations "${directory}/builds/*.lock")
    foreach(registration IN LISTS registrations)
        file(LOCK "${registration}" GUARD FUNCTION TIMEOUT 0 RESULT_VARIABLE locked)
        if(NOT locked STREQUAL "0")
            message(STATUS "Waiting for a Kobo build to finish before the measured steps (${registration})")
            file(LOCK "${registration}" GUARD FUNCTION)
        endif()
        # The build ended or crashed; its file is no longer needed. A release
        # creates the file again, so remove it afterwards.
        file(LOCK "${registration}" RELEASE)
        file(REMOVE "${registration}")
    endforeach()
endfunction()

if(NOT CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    return()
endif()
if(NOT ROOT OR NOT OWNER MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "Usage: cmake -DROOT=<checkout> -DOWNER=<process ID> -P load-lock.cmake")
endif()
kobalt_load_directory("${ROOT}" directory)
string(RANDOM LENGTH 16 id)
set(registration "${directory}/builds/${id}.lock")
# Standard output carries only the "registered" line, so messages go to stderr.
file(LOCK "${directory}/evaluation.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE locked)
if(NOT locked STREQUAL "0")
    message(NOTICE "Waiting for the performance evaluation to finish before this build starts (${directory}/evaluation.lock)")
    file(LOCK "${directory}/evaluation.lock" GUARD PROCESS)
endif()
file(LOCK "${registration}" GUARD PROCESS)
file(LOCK "${directory}/evaluation.lock" RELEASE)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo registered)
# CMake cannot wait for a process that is not its child, so check once a second.
execute_process(COMMAND sh -c "while kill -0 \"$1\" 2> /dev/null; do sleep 1; done" sh "${OWNER}")
file(REMOVE "${registration}")
