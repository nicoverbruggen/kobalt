# Checks platforms/load-lock.cmake without Docker, a Kobo SDK or a real
# evaluation. The test starts itself again as child processes: an "evaluation"
# child calls the lock functions in the order of platforms/eval/run.cmake, and
# an "owner" child stands in for the build process that a registration belongs
# to. Every child uses the scratch lock folder in KOBALT_LOAD_DIRECTORY, never
# the locks that the repository shares.
cmake_minimum_required(VERSION 3.22)

# A child keeps what it holds until the test writes <MARKS>.release or the test
# process ends.
function(hold)
    while(NOT EXISTS "${MARKS}.release")
        execute_process(COMMAND sh -c [=[kill -0 "$1" 2> /dev/null && sleep 0.05]=] sh "${PARENT}"
            RESULT_VARIABLE alive)
        if(NOT alive EQUAL 0)
            return()
        endif()
    endwhile()
endfunction()

if(ROLE STREQUAL "owner")
    hold()
    return()
elseif(ROLE STREQUAL "evaluation")
    include("${LOAD_LOCK}")
    kobalt_lock_evaluation("${TEST_ROOT}")
    file(WRITE "${MARKS}.locked" "")
    kobalt_wait_for_builds("${TEST_ROOT}")
    file(WRITE "${MARKS}.measured" "")
    if(HOLD)
        hold()
    endif()
    return()
endif()

# A launcher run owns this root so cancellation can remove only its scratch state.
if(NOT "$ENV{KOBALT_TEST_TMP_ROOT}" STREQUAL "")
    set(TEST_ROOT "$ENV{KOBALT_TEST_TMP_ROOT}/load-lock")
endif()
if(NOT TEST_ROOT OR NOT EXISTS "${LOAD_LOCK}")
    message(FATAL_ERROR "Usage: cmake -DTEST_ROOT=<scratch folder> -DLOAD_LOCK=<load-lock.cmake> -P test-load-lock.cmake")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(locks "${TEST_ROOT}/kobalt-load")
# The children watch this process, so a test that CTest stops leaves none behind.
execute_process(COMMAND sh -c [=[echo $PPID]=] OUTPUT_VARIABLE parent OUTPUT_STRIP_TRAILING_WHITESPACE)

# Stops the remaining children, removes the scratch folder and reports the
# children's output.
function(fail text)
    get_property(pids GLOBAL PROPERTY load_lock_children)
    if(pids)
        execute_process(COMMAND kill ${pids} OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND sh -c [=[for p; do while kill -0 "$p" 2> /dev/null; do sleep 0.05; done; done]=]
            sh ${pids} TIMEOUT 2)
    endif()
    file(GLOB logs "${TEST_ROOT}/*.log")
    set(details "")
    foreach(log IN LISTS logs)
        file(READ "${log}" content)
        string(APPEND details "\n--- ${log}\n${content}")
    endforeach()
    file(REMOVE_RECURSE "${TEST_ROOT}")
    message(FATAL_ERROR "${text}${details}")
endfunction()

# Starts a command in the background with its output in <name>.log and returns
# its process ID.
function(start name output)
    execute_process(COMMAND sh -c [=[log=$1; shift; "$@" < /dev/null > "$log" 2>&1 & echo $!]=]
        sh "${TEST_ROOT}/${name}.log" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE pid OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status EQUAL 0 OR NOT pid MATCHES "^[1-9][0-9]*$")
        fail("Cannot start ${name}")
    endif()
    set_property(GLOBAL APPEND PROPERTY load_lock_children ${pid})
    set(${output} ${pid} PARENT_SCOPE)
endfunction()

# Returns the command for a child of this script. Its marker files start with
# <TEST_ROOT>/<name>.
function(child_command name role output)
    set(${output} "${CMAKE_COMMAND}" "-DROLE=${role}" "-DPARENT=${parent}" "-DTEST_ROOT=${TEST_ROOT}"
        "-DLOAD_LOCK=${LOAD_LOCK}" "-DKOBALT_LOAD_DIRECTORY=${locks}" "-DMARKS=${TEST_ROOT}/${name}" ${ARGN}
        -P "${CMAKE_SCRIPT_MODE_FILE}" PARENT_SCOPE)
endfunction()

# Starts the registration helper that platforms/kobo/build.sh starts for a build.
function(register name owner output)
    start(${name} pid "${CMAKE_COMMAND}" "-DROOT=${TEST_ROOT}" "-DOWNER=${owner}"
        "-DKOBALT_LOAD_DIRECTORY=${locks}" -P "${LOAD_LOCK}")
    wait_for("the build registers" LOG ${name} "registered")
    set(${output} ${pid} PARENT_SCOPE)
endfunction()

# Checks every 50 ms, for at most five seconds, until the condition holds:
#   FILE <path>          the file exists
#   LOG <name> <text>    the log of child <name> contains the text
#   EXIT <pid>           the process has ended
function(wait_for description kind subject)
    string(TIMESTAMP started "%s")
    while(TRUE)
        if(kind STREQUAL "FILE" AND EXISTS "${subject}")
            return()
        elseif(kind STREQUAL "LOG" AND EXISTS "${TEST_ROOT}/${subject}.log")
            file(READ "${TEST_ROOT}/${subject}.log" content)
            string(FIND "${content}" "${ARGV3}" found)
            if(NOT found EQUAL -1)
                return()
            endif()
        elseif(kind STREQUAL "EXIT")
            execute_process(COMMAND kill -0 ${subject} RESULT_VARIABLE alive OUTPUT_QUIET ERROR_QUIET)
            if(NOT alive EQUAL 0)
                get_property(pids GLOBAL PROPERTY load_lock_children)
                list(REMOVE_ITEM pids ${subject})
                set_property(GLOBAL PROPERTY load_lock_children ${pids})
                return()
            endif()
        endif()
        string(TIMESTAMP now "%s")
        math(EXPR elapsed "${now} - ${started}")
        if(elapsed GREATER 5)
            fail("Timed out waiting until ${description}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
    endwhile()
endfunction()

function(expect_registrations expected description)
    file(GLOB registrations "${locks}/builds/*.lock")
    list(LENGTH registrations count)
    if(NOT count EQUAL expected)
        fail("Expected ${expected} registration files ${description}, found ${count}")
    endif()
endfunction()

# 1. A second evaluation waits while the first holds the lock, and takes it
#    when the first ends.
child_command(first evaluation command -DHOLD=ON)
start(first first ${command})
wait_for("the first evaluation holds the lock" FILE "${TEST_ROOT}/first.measured")
child_command(second evaluation command)
start(second second ${command})
wait_for("the second evaluation waits for the first" LOG second "Waiting for another evaluation to finish")
if(EXISTS "${TEST_ROOT}/second.locked")
    fail("The second evaluation took the lock while the first held it")
endif()
file(WRITE "${TEST_ROOT}/first.release" "")
wait_for("the first evaluation ends" EXIT ${first})
wait_for("the second evaluation finishes after the first ends" FILE "${TEST_ROOT}/second.measured")
wait_for("the second evaluation ends" EXIT ${second})

# 2. An evaluation waits while a build registration is held, and continues
#    when the build ends.
child_command(build owner command)
start(build build ${command})
register(registration ${build} registration)
expect_registrations(1 "while the build runs")
child_command(waiting evaluation command)
start(waiting waiting ${command})
wait_for("the evaluation waits for the build" LOG waiting "Waiting for a Kobo build to finish")
if(NOT EXISTS "${TEST_ROOT}/waiting.locked" OR EXISTS "${TEST_ROOT}/waiting.measured")
    fail("The evaluation did not stop between its lock and the measured steps while a build ran")
endif()
file(WRITE "${TEST_ROOT}/build.release" "")
wait_for("the build ends" EXIT ${build})
wait_for("the evaluation continues after the build ends" FILE "${TEST_ROOT}/waiting.measured")
wait_for("the registration helper ends" EXIT ${registration})
wait_for("the evaluation ends" EXIT ${waiting})
expect_registrations(0 "after the build ended")

# 3. A registration whose process is gone does not block, and the evaluation
#    removes its file.
child_command(crashed owner command)
start(crashed crashed ${command})
register(stale ${crashed} stale)
execute_process(COMMAND kill -9 ${stale})
wait_for("the registration helper is gone" EXIT ${stale})
expect_registrations(1 "after the registration helper crashed")
file(WRITE "${TEST_ROOT}/crashed.release" "")
wait_for("the crashed build ends" EXIT ${crashed})
child_command(unblocked evaluation command)
execute_process(COMMAND ${command} TIMEOUT 5 RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT status EQUAL 0 OR NOT EXISTS "${TEST_ROOT}/unblocked.measured")
    fail("The evaluation did not finish with a stale registration (${status}):\n${output}")
endif()
if(output MATCHES "Waiting for")
    fail("A stale registration made the evaluation wait:\n${output}")
endif()
expect_registrations(0 "after the evaluation saw the stale registration")

file(REMOVE_RECURSE "${TEST_ROOT}")
