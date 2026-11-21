# Checks failed, skipped and missing device scenarios in both result formats, device checks, and a run without a device part.
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
# Device checks list each check scenario and the saves that collection read
# from the event log. A failed save is a failed part.
set(passing [=[{"id":"check.passing","kind":"check","status":"completed","samples":[],"checks":[{"check":"Opens","pass":true}]}]=])
set(failing [=[{"id":"check.failing","kind":"check","status":"failed","samples":[],"reason":"step 4: Restores | defaults","checks":[{"check":"Restores defaults","pass":false}]}]=])
file(WRITE "${target}/device-qt5/scenarios.jsonl" "${header}${healthy}\n${passing}\n${failing}\n")
file(WRITE "${target}/device-qt5/manual-checks.json" [=[{"usb_save": "passed", "power_off_save": "failed"}]=])
compare(checks)
file(READ "${root}/checks/report.json" report)
file(READ "${root}/checks/report.md" markdown)
string(JSON passing_status GET "${report}" device_checks device.qt5.check.passing status)
string(JSON missed GET "${report}" device_checks device.qt5.check.failing failed_checks 0)
string(JSON usb GET "${report}" device_checks device.qt5.usb_save status)
string(JSON parts GET "${report}" failed_parts)
if(NOT passing_status STREQUAL "completed" OR NOT missed STREQUAL "Restores defaults"
   OR NOT usb STREQUAL "passed" OR NOT parts MATCHES "device.qt5.power_off_save failed")
    message(FATAL_ERROR "Device checks were lost: ${report}")
endif()
foreach(text "## Device checks" "| `device.qt5.check.passing` | pass | 1 check |"
             "| `device.qt5.check.failing` | fail | step 4: Restores / defaults |"
             "| `device.qt5.power_off_save` | failed |")
    string(FIND "${markdown}" "${text}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Report omitted ${text}: ${markdown}")
    endif()
endforeach()
file(REMOVE "${target}/device-qt5/manual-checks.json")
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
