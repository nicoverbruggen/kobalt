# Source checks that keep known regressions out of the reader. Each check
# compares what it finds with an allowlist next to this script, and fails on a
# new match or on an allowlist entry that no longer matches.
#
#   storage-io.allow         app and UI code that writes storage itself
#   blocking-reasons.allow   reasons named by mainthread::AllowBlocking scopes
#   blocking-forwarders.allow  scopes whose reason comes from a caller
#   reader-properties.allow  "reader_*" names with their meaning
#   short-timers.allow       app and UI timers shorter than 100 ms
#
# A change to an allowlist is a decision for the user. Run:
#
#   cmake -P platforms/checks/source-checks.cmake
#
# ROOT selects another source tree and LISTS another allowlist folder; the
# self-test in source-checks-test.cmake uses both.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/source-scan.cmake")
if(NOT DEFINED ROOT)
    get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
if(NOT DEFINED LISTS)
    set(LISTS "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(problems)
kobalt_list_sources("${ROOT}" "src/app;src/ui" interface_sources)
kobalt_list_sources("${ROOT}" "src;platforms" product_sources)
kobalt_list_sources("${ROOT}" "src;platforms;tests" all_sources)

# Storage writes belong behind the storage helpers, which report themselves to
# the main-thread guard and run on a writer thread. Preference saves have
# their own check, preference-writes.cmake.
kobalt_scan("${ROOT}" "${interface_sources}"
    "(writeJson[ \t]*\\(|QSaveFile|QSettings|QSql[A-Z][A-Za-z]*|storage::checkpointProfile[ \t]*\\(|\\.open[ \t]*\\([ \t]*(QIODevice|QFile)::(WriteOnly|Append|ReadWrite))"
    hits)
kobalt_check_sites("writes storage in app or UI code" "${hits}" "${LISTS}/storage-io.allow" problems)

# Each AllowBlocking scope names why the main thread may wait. A literal reason
# must be listed. A scope that takes its reason from a caller must be listed as
# a forwarder, and the reasons its callers pass must be listed too.
kobalt_scan("${ROOT}" "${product_sources}"
    "AllowBlocking[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[({][ \t]*([^)}]*[^ \t)}])" hits)
set(literal_hits)
set(forwarding_hits)
foreach(hit IN LISTS hits)
    if(hit MATCHES "^(.*)\\|\"([^\"]*)\"$")
        list(APPEND literal_hits "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}")
    else()
        list(APPEND forwarding_hits "${hit}")
    endif()
endforeach()
kobalt_check_sites("blocks with a reason from its caller" "${forwarding_hits}"
    "${LISTS}/blocking-forwarders.allow" problems)
kobalt_read_allowlist("${LISTS}/blocking-reasons.allow" reasons)
foreach(hit IN LISTS literal_hits)
    string(REGEX MATCH "^([^|]+)\\|([^|]+)\\|([0-9]+)\\|(.*)$" ignored "${hit}")
    if(NOT CMAKE_MATCH_4 IN_LIST reasons)
        list(APPEND problems "unlisted AllowBlocking reason: `${CMAKE_MATCH_4}` at ${CMAKE_MATCH_1}:${CMAKE_MATCH_3}")
    endif()
endforeach()
# A forwarded reason appears only as a string literal at its caller.
set(product_text "")
foreach(source IN LISTS product_sources)
    file(READ "${ROOT}/${source}" text)
    string(APPEND product_text "${text}")
endforeach()
foreach(reason IN LISTS reasons)
    string(FIND "${product_text}" "\"${reason}\"" position)
    if(position EQUAL -1)
        list(APPEND problems "AllowBlocking reason without a use, remove it: ${reason}")
    endif()
endforeach()

# "reader_*" names pass refresh intent and roles between the reader, the
# platform adapters and the tests through Qt properties. A typo reads as false
# and fails silently, so every name must be listed.
kobalt_scan("${ROOT}" "${all_sources}" "\"(reader_[A-Za-z0-9_]+)\"" hits)
kobalt_check_names("unlisted reader_ name" "${hits}" "${LISTS}/reader-properties.allow" problems MEANINGS)

# Timers shorter than 100 ms wake the device often. 0 ms, which defers work
# to the next event loop pass, is allowed.
kobalt_scan("${ROOT}" "${interface_sources}"
    "(singleShot|setInterval|[.>]start)[ \t]*\\([ \t]*([1-9][0-9]?)[ \t]*[,)]" hits)
kobalt_check_sites("timer shorter than 100 ms" "${hits}" "${LISTS}/short-timers.allow" problems)

if(problems)
    list(REMOVE_DUPLICATES problems)
    set(report "")
    foreach(problem IN LISTS problems)
        string(APPEND report "\n  ${problem}")
    endforeach()
    message(FATAL_ERROR "Source checks:${report}\n"
        "Fix the code, or ask the user before adding an allowlist entry in ${LISTS}. "
        "See doc/validation.md#source-checks.")
endif()
message(STATUS "Source checks: only allowlisted matches")
