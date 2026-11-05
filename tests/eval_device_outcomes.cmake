# Checks failed, skipped and missing device scenarios in both result formats, and a run without a device part.
cmake_minimum_required(VERSION 3.22)
set(root "${CMAKE_CURRENT_BINARY_DIR}/device-outcomes")
set(base "${root}/base")
set(target "${root}/target")
foreach(side base target)
    file(MAKE_DIRECTORY "${root}/${side}/device-qt5")
    file(WRITE "${root}/${side}/meta.json" [=[{
        "subject":{"label":"fixture","sha":"0000000000000000000000000000000000000000"},
        "profile":"quick","parts":["device"],"host":{},"device_failures":[]
    }]=])
endforeach()
set(header "{\"kind\":\"kobalt-device-run\",\"target\":\"qt5\"}\n")
set(healthy [=[{"id":"healthy","status":"completed","kind":"e2e","samples":[10,10]}]=])
file(WRITE "${base}/device-qt5/scenarios.jsonl" "${header}${healthy}\n")
foreach(id failed skipped missing)
    file(APPEND "${base}/device-qt5/scenarios.jsonl"
        "{\"id\":\"${id}\",\"status\":\"completed\",\"kind\":\"e2e\",\"samples\":[20,20]}\n")
endforeach()
set(failed [=[{"id":"failed","status":"failed","samples":[],"reason":"no panel-open event"}]=])
set(skipped [=[{"id":"skipped","status":"skipped","samples":[],"reason":"no dictionary"}]=])
file(WRITE "${target}/device-qt5/scenarios.jsonl" "${header}${healthy}\n${failed}\n${skipped}\n")

function(compare output)
    execute_process(COMMAND "${COMPARE}" --base "${base}" --target "${target}" --output "${root}/${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Comparator failed: ${stdout}\n${stderr}")
    endif()
endfunction()

function(check_outcomes output)
    file(READ "${root}/${output}/report.json" report)
    file(READ "${root}/${output}/report.md" markdown)
    string(JSON failed_status GET "${report}" device_scenarios target device.qt5.failed status)
    string(JSON skipped_status GET "${report}" device_scenarios target device.qt5.skipped status)
    string(JSON missing GET "${report}" device_scenarios missing 0)
    string(JSON metrics LENGTH "${report}" device metrics)
    if(NOT failed_status STREQUAL "failed" OR NOT skipped_status STREQUAL "skipped" OR
       NOT missing STREQUAL "device.qt5.missing" OR NOT metrics EQUAL 1)
        message(FATAL_ERROR "Device outcomes or successful timing samples were lost: ${report}")
    endif()
    foreach(text "device.qt5.failed" "no panel-open event" "device.qt5.skipped" "no dictionary"
                 "device.qt5.missing" "Evaluation contains failed or missing results.")
        string(FIND "${markdown}" "${text}" found)
        if(found LESS 0)
            message(FATAL_ERROR "Report omitted ${text}: ${markdown}")
        endif()
    endforeach()
endfunction()

compare(jsonl)
check_outcomes(jsonl)
# The older runner format must retain the same outcomes.
file(REMOVE "${target}/device-qt5/scenarios.jsonl")
string(REPLACE "\"samples\":" "\"samples_ms\":" legacy
    "{\"target\":\"qt5\",\"scenarios\":[${healthy},${failed},${skipped}]}")
file(WRITE "${target}/device-qt5/device.json" "${legacy}")
compare(legacy)
check_outcomes(legacy)
# A deliberately omitted device part is not a failed device run.
file(REMOVE_RECURSE "${target}/device-qt5")
file(WRITE "${target}/meta.json" [=[{
    "subject":{"label":"fixture","sha":"0000000000000000000000000000000000000000"},
    "profile":"quick","parts":["perf"],"host":{},"device_failures":[]
}]=])
compare(no-device)
file(READ "${root}/no-device/report.json" report)
string(JSON failures LENGTH "${report}" failed_parts)
string(JSON missing LENGTH "${report}" device_scenarios missing)
if(NOT failures EQUAL 0 OR NOT missing EQUAL 0)
    message(FATAL_ERROR "An omitted device part was treated as failure: ${report}")
endif()
