# Checks the resource gate of the comparator: a pass, one profile write in one page turn, a
# resource metric beyond its band, a resource scenario missing from the target and a run
# without resource scenarios.
cmake_minimum_required(VERSION 3.22)
set(root "${CMAKE_CURRENT_BINARY_DIR}/resource-gate")
file(REMOVE_RECURSE "${root}")
set(meta [=[{
    "subject":{"label":"fixture","sha":"0000000000000000000000000000000000000000"},
    "profile":"quick","parts":["perf"],"host":{},"device_failures":[]
}]=])

# One performance file with a turn scenario. `writes` lists the profile
# transactions of each turn, `cpu` the CPU time of each turn and `freed` the
# heap growth of each turn, which is negative.
function(write_run side writes cpu freed)
    file(MAKE_DIRECTORY "${root}/${side}")
    file(WRITE "${root}/${side}/meta.json" "${meta}")
    file(WRITE "${root}/${side}/perf-1.json" "{\"kind\":\"kobalt-perf\",\"scenarios\":[
        {\"id\":\"resources.turn.forward\",\"kind\":\"e2e\",\"unit\":\"ms\",\"paths\":[\"src/\"],
         \"samples_ms\":[3,3,3,3],\"resources\":true,\"counts\":[\"profile_transactions\"],
         \"expect\":{\"profile_transactions\":0},
         \"stages\":{\"profile_transactions\":[${writes}],\"cpu_ms\":[${cpu}],
                    \"rss_growth_kib\":[0,0,0,0],\"heap_growth_kib\":[${freed}],\"thread_cpu_ms.QThread\":[1,1,1,1]}}]}")
endfunction()

function(compare base target output)
    execute_process(COMMAND "${COMPARE}" --base "${root}/${base}" --target "${root}/${target}"
        --output "${root}/${output}" --config "${CONFIG}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Comparator failed: ${stdout}\n${stderr}")
    endif()
endfunction()

# Fails unless the gate of `output` has `verdict` and the report names `text`.
function(check output verdict text)
    file(READ "${root}/${output}/report.json" report)
    file(READ "${root}/${output}/report.md" markdown)
    string(JSON gate GET "${report}" gate verdict)
    if(NOT gate STREQUAL verdict)
        message(FATAL_ERROR "${output}: expected the gate to ${verdict}, got ${gate}: ${markdown}")
    endif()
    string(FIND "${markdown}" "${text}" found)
    if(found LESS 0)
        message(FATAL_ERROR "${output}: the report omitted \"${text}\": ${markdown}")
    endif()
endfunction()

write_run(base "0,0,0,0" "4,4,4,4" "-50000,-50000,-50000,-50000")
# Freeing 3% less memory than the base is within the band, and so is a
# small growth where the base freed memory in one sample.
write_run(same "0,0,0,0" "4,4,5,4" "-48500,-48500,-48500,-48500")
write_run(reclaimed "0,0,0,0" "4,4,4,4" "-5000,0,0,0")
write_run(small "0,0,0,0" "4,4,4,4" "100,0,0,0")
write_run(write "0,0,1,0" "4,4,4,4" "-50000,-50000,-50000,-50000")
write_run(cpu "0,0,0,0" "9,9,9,9" "-50000,-50000,-50000,-50000")
compare(base same same)
check(same pass "Resource gate: pass.")
# Thread names are not compared.
file(READ "${root}/same/report.json" report)
string(FIND "${report}" "thread_cpu_ms" thread)
if(NOT thread LESS 0)
    message(FATAL_ERROR "A thread's CPU time became a compared metric: ${report}")
endif()
compare(reclaimed small small)
check(small pass "Resource gate: pass.")
compare(base write write)
check(write fail "`resources.turn.forward:profile_transactions` is above 0 in 1 of 4 samples")
compare(base cpu cpu)
check(cpu fail "`resources.turn.forward:cpu_ms` is slower")

file(MAKE_DIRECTORY "${root}/missing")
file(WRITE "${root}/missing/meta.json" "${meta}")
file(WRITE "${root}/missing/perf-1.json" [=[{"kind":"kobalt-perf","scenarios":[
    {"id":"book.open","kind":"e2e","unit":"ms","paths":[],"samples_ms":[30],"stages":{}}]}]=])
compare(base missing missing)
check(missing fail "`resources.turn.forward` was not measured in the target")
compare(missing missing none)
check(none "not measured" "Not checked: neither run has resource scenarios.")
