# Checks which paths platforms/cleanup.cmake removes and which it refuses.
#
#   cmake -DTEST_ROOT=<scratch> -DCLEANUP=<platforms/cleanup.cmake> -P test-cleanup.cmake
#
# The script decides by the checkout that holds it, so the test runs a copy in
# a fake checkout under TEST_ROOT, with TMPDIR set to a folder outside that
# checkout's build/. Every path the test passes is inside TEST_ROOT.
cmake_minimum_required(VERSION 3.22)

foreach(variable IN ITEMS TEST_ROOT CLEANUP)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Pass -D${variable}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
set(checkout "${TEST_ROOT}/checkout")
set(tmp "${TEST_ROOT}/tmp")
set(project "${tmp}/claude-test/project")
set(scratchpad "${project}/session/scratchpad")
file(MAKE_DIRECTORY "${checkout}/platforms" "${checkout}/doc" "${checkout}/build/out"
    "${checkout}/build/dependency-cache/entry" "${scratchpad}/work" "${project}/other-session/scratchpad")
file(COPY "${CLEANUP}" DESTINATION "${checkout}/platforms")
file(TOUCH "${checkout}/doc/keep.md" "${scratchpad}/work/file" "${checkout}/build/out/file")
file(CREATE_LINK "${checkout}/doc" "${scratchpad}/doc-link" SYMBOLIC)
file(CREATE_LINK "${checkout}/build/out" "${scratchpad}/out-link" SYMBOLIC)
get_filename_component(tmp_real "${tmp}" REALPATH)

set(failures "")
# Runs the cleanup script on one path and checks its exit status.
function(cleanup path expect)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${tmp_real}"
        "${CMAKE_COMMAND}" "-DPATHS=${path}" -P "${checkout}/platforms/cleanup.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if((expect STREQUAL "removed" AND NOT status EQUAL 0) OR (expect STREQUAL "refused" AND status EQUAL 0))
        set(failures "${failures}\n${path}: expected ${expect}, exit ${status}\n${output}" PARENT_SCOPE)
    endif()
endfunction()
function(present path)
    if(NOT EXISTS "${path}")
        set(failures "${failures}\n${path} was removed" PARENT_SCOPE)
    endif()
endfunction()

# Refused: the folder of all projects, a project, a session, a scratchpad
# itself, a `..` path that leads to a project, the checkout outside build/,
# and a symlink that leads outside.
cleanup("${tmp}/claude-test" refused)
cleanup("${project}" refused)
cleanup("${project}/session" refused)
cleanup("${scratchpad}" refused)
cleanup("${scratchpad}/work/../../.." refused)
cleanup("${checkout}/build" refused)
cleanup("${checkout}/build/dependency-cache/entry" refused)
cleanup("${checkout}/doc" refused)
cleanup("${checkout}/build/../doc" refused)
cleanup("${scratchpad}/doc-link" refused)
present("${scratchpad}/work/file")
present("${project}/other-session/scratchpad")
present("${checkout}/build/dependency-cache/entry")
present("${checkout}/doc/keep.md")

# Removed: work inside one scratchpad, a symlink that leads inside, as a link,
# and build output.
cleanup("${scratchpad}/work" removed)
cleanup("${scratchpad}/out-link" removed)
present("${checkout}/build/out/file")
cleanup("${checkout}/build/out" removed)
foreach(path IN ITEMS "${scratchpad}/work" "${scratchpad}/out-link" "${checkout}/build/out")
    if(EXISTS "${path}" OR IS_SYMLINK "${path}")
        set(failures "${failures}\n${path} is still present")
    endif()
endforeach()
present("${checkout}/doc/keep.md")

file(REMOVE_RECURSE "${TEST_ROOT}")
if(failures)
    message(FATAL_ERROR "cleanup.cmake check failed:${failures}")
endif()
message(STATUS "cleanup.cmake removes and refuses the expected paths")
