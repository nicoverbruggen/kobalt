# Checks source-checks.cmake on this tree and on a small planted tree.
cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED TEST_TMP_ROOT)
    message(FATAL_ERROR "TEST_TMP_ROOT is required")
endif()
set(check "${CMAKE_CURRENT_LIST_DIR}/source-checks.cmake")

function(run_check root lists result_variable output_variable)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${root}" "-DLISTS=${lists}" -P "${check}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

get_filename_component(tree "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
run_check("${tree}" "${CMAKE_CURRENT_LIST_DIR}" result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The tree fails its own source checks:\n${output}")
endif()

set(scratch "${TEST_TMP_ROOT}/source-checks")
file(REMOVE_RECURSE "${scratch}")
# Each planted line follows lines that a naive line split would merge or group.
file(WRITE "${scratch}/src/app/planted.cpp" [=[#define JOIN(a, b) \
    a##b
int values[2] = { 1, 2 };

void Planted::save()
{
    // QSaveFile in a comment does not count.
    QSaveFile file(path);
    timer.setInterval(16);
    timer.start(0);
    widget->setProperty("reader_planted", true);
}

void Planted::wait(const char *reason)
{
    mainthread::AllowBlocking allow("planted wait");
    mainthread::AllowBlocking forwarded(reason);
}
]=])
file(WRITE "${scratch}/src/ui/clean.cpp" "void Clean::run() { QTimer::singleShot(250, this, [] {}); }\n")
set(empty "${scratch}/empty")
foreach(list IN ITEMS storage-io blocking-reasons blocking-forwarders reader-properties short-timers)
    file(WRITE "${empty}/${list}.allow" "# Nothing is allowed.\n")
endforeach()
run_check("${scratch}" "${empty}" result output)
foreach(expected IN ITEMS
        "writes storage in app or UI code: src/app/planted.cpp:8 in Planted::save"
        "timer shorter than 100 ms: src/app/planted.cpp:9 in Planted::save"
        "unlisted reader_ name: `reader_planted` at src/app/planted.cpp:11"
        "unlisted AllowBlocking reason: `planted wait` at src/app/planted.cpp:16"
        "blocks with a reason from its caller: src/app/planted.cpp:17 in Planted::wait")
    string(FIND "${output}" "${expected}" position)
    if(result EQUAL 0 OR position EQUAL -1)
        message(FATAL_ERROR "Planted match not reported as `${expected}`:\n${output}")
    endif()
endforeach()
if(output MATCHES "planted.cpp:(7|10)[^0-9]" OR output MATCHES "clean.cpp")
    message(FATAL_ERROR "A comment, a 0 ms timer or a 250 ms timer was reported:\n${output}")
endif()

set(lists "${scratch}/lists")
file(WRITE "${lists}/storage-io.allow" "src/app/planted.cpp|Planted::save|Fixture.\n")
file(WRITE "${lists}/short-timers.allow" "src/app/planted.cpp|Planted::save|Fixture.\n")
file(WRITE "${lists}/blocking-forwarders.allow" "src/app/planted.cpp|Planted::wait|Fixture.\n")
file(WRITE "${lists}/blocking-reasons.allow" "planted wait\n")
file(WRITE "${lists}/reader-properties.allow" "reader_planted|Fixture.\n")
run_check("${scratch}" "${lists}" result output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Allowlisted matches failed:\n${output}")
endif()

file(APPEND "${lists}/storage-io.allow" "src/app/planted.cpp|Planted::gone|Fixture.\n")
file(APPEND "${lists}/blocking-reasons.allow" "gone wait\n")
file(APPEND "${lists}/reader-properties.allow" "reader_gone|Fixture.\n")
run_check("${scratch}" "${lists}" result output)
foreach(expected IN ITEMS "remove it: src/app/planted.cpp|Planted::gone" "without a use, remove it: gone wait"
        "remove it: reader_gone")
    string(FIND "${output}" "${expected}" position)
    if(result EQUAL 0 OR position EQUAL -1)
        message(FATAL_ERROR "Stale allowlist entry not reported as `${expected}`:\n${output}")
    endif()
endforeach()
file(WRITE "${lists}/reader-properties.allow" "reader_planted\n")
run_check("${scratch}" "${lists}" result output)
if(result EQUAL 0 OR NOT output MATCHES "expected `name\\|meaning`: reader_planted")
    message(FATAL_ERROR "A reader_ name without a meaning passed:\n${output}")
endif()
file(REMOVE_RECURSE "${scratch}")
message(STATUS "Source checks report planted and stale matches")
