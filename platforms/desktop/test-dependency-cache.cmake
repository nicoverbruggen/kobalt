# Checks the shared dependency cache in dependencies/cache.cmake: where it
# lives, that one build of a key runs at a time, that only a finished build is
# published, and that downloads are verified and published by rename.

# A launcher run owns this root so cancellation can remove only its scratch state.
if(NOT "$ENV{KOBALT_TEST_TMP_ROOT}" STREQUAL "")
    set(TEST_ROOT "$ENV{KOBALT_TEST_TMP_ROOT}/dependency-cache")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
include("${CACHE_SCRIPT}")

# Conditions name variables rather than expanding text that may hold quotes.
function(expect condition message)
    cmake_language(EVAL CODE "if(NOT (${condition}))\nmessage(FATAL_ERROR [==[${message}]==])\nendif()")
endfunction()

# Cache location: a linked worktree shares the main checkout's cache, a tree
# without its own .git uses its own, even inside a checkout, and an explicit
# path wins.
set(repository "${TEST_ROOT}/repository")
file(MAKE_DIRECTORY "${repository}")
file(WRITE "${repository}/README" "fixture\n")
foreach(command "init -q" "add README" "-c user.name=Fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false commit -q -m fixture"
        "worktree add -q -b feature/fixture worktrees/fixture")
    separate_arguments(arguments UNIX_COMMAND "${command}")
    execute_process(COMMAND git ${arguments} WORKING_DIRECTORY "${repository}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    expect("result EQUAL 0" "Cannot prepare the Git fixture: git ${command}")
endforeach()
kobalt_dependency_cache_root("${repository}/worktrees/fixture" "" cache)
expect("cache STREQUAL \"${repository}/build/dependency-cache\"" "A linked worktree did not use the main checkout's cache: ${cache}")
kobalt_dependency_cache_root("${repository}" "" cache)
expect("cache STREQUAL \"${repository}/build/dependency-cache\"" "The main checkout did not use its own cache: ${cache}")
file(MAKE_DIRECTORY "${TEST_ROOT}/export")
kobalt_dependency_cache_root("${TEST_ROOT}/export" "" cache)
expect("cache STREQUAL \"${TEST_ROOT}/export/build/dependency-cache\"" "A tree without Git did not use its own cache: ${cache}")
kobalt_dependency_cache_root("${repository}" "/elsewhere/cache" cache)
expect("cache STREQUAL \"/elsewhere/cache\"" "An explicit cache path was ignored")

# A fixture dependency project. Each build counts itself, holds the lock for a
# moment and fails while the FAIL file exists.
set(source "${TEST_ROOT}/recipes")
set(cache "${TEST_ROOT}/cache")
set(count "${TEST_ROOT}/builds")
set(fail "${TEST_ROOT}/FAIL")
file(WRITE "${source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.22)
project(FixtureDependencies NONE)
add_custom_target(fixture ALL COMMAND "${CMAKE_COMMAND}" "-DENTRY=${KOBALT_DEPENDENCY_ENTRY}"
    "-DCOUNT=${FIXTURE_COUNT}" "-DFAIL=${FIXTURE_FAIL}" -P "${CMAKE_CURRENT_SOURCE_DIR}/step.cmake")
]=])
file(WRITE "${source}/step.cmake" [=[
file(APPEND "${COUNT}" "build\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
if(EXISTS "${FAIL}")
    message(FATAL_ERROR "Fixture build failed")
endif()
file(WRITE "${ENTRY}/install/lib/libfixture.a" "fixture")
]=])
function(settings key)
    file(WRITE "${TEST_ROOT}/settings-${key}.cmake"
        "set(KOBALT_DEPENDENCY_ENTRY [==[${cache}/${key}]==] CACHE STRING \"\" FORCE)\n"
        "set(FIXTURE_COUNT [==[${count}]==] CACHE STRING \"\" FORCE)\n"
        "set(FIXTURE_FAIL [==[${fail}]==] CACHE STRING \"\" FORCE)\n"
        "set(CMAKE_MAKE_PROGRAM [==[${MAKE_PROGRAM}]==] CACHE FILEPATH \"\" FORCE)\n")
endfunction()
file(WRITE "${TEST_ROOT}/build.cmake" "
include([==[${CACHE_SCRIPT}]==])
kobalt_dependency_set([==[${cache}]==] \"\${KEY}\" [==[${source}]==] \"${TEST_ROOT}/settings-\${KEY}.cmake\")
")
function(build key output)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DKEY=${key} -P "${TEST_ROOT}/build.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE log)
    set(${output} "${result}" PARENT_SCOPE)
    set(${output}_log "${log}" PARENT_SCOPE)
endfunction()
function(builds output)
    file(STRINGS "${count}" lines)
    list(LENGTH lines total)
    set(${output} ${total} PARENT_SCOPE)
endfunction()

# Two processes that need the same missing key: one builds, the other waits
# and reuses the published result.
settings(shared)
execute_process(COMMAND sh -c [=[
    "$1" -DKEY=shared -P "$2/build.cmake" > "$2/first.log" 2>&1 & first=$!
    sleep 0.5
    "$1" -DKEY=shared -P "$2/build.cmake" > "$2/second.log" 2>&1 & second=$!
    wait $first || exit 1
    wait $second || exit 1
]=] fixture "${CMAKE_COMMAND}" "${TEST_ROOT}" RESULT_VARIABLE result)
expect("result EQUAL 0" "A concurrent dependency build failed")
builds(total)
expect("total EQUAL 1" "Two processes built the same dependency set: ${total} builds")
file(READ "${TEST_ROOT}/first.log" first)
file(READ "${TEST_ROOT}/second.log" second)
set(logs "${first}${second}")
expect("logs MATCHES \"Waiting for another build of dependency set shared\"" "The second process did not wait for the lock")
expect("EXISTS \"${cache}/shared/complete\"" "A finished dependency set was not published")
expect("EXISTS \"${cache}/shared/install/lib/libfixture.a\"" "The published dependency set is incomplete")
build(shared result)
builds(total)
expect("result EQUAL 0 AND total EQUAL 1" "A published dependency set was built again")

# A failed build is never published, and the next attempt builds it again.
settings(failing)
file(WRITE "${fail}" "")
build(failing result)
expect("NOT result EQUAL 0" "A failed dependency build reported success")
expect("result_log MATCHES \"was not published\"" "A failed dependency build gave no reason: ${result_log}")
expect("NOT EXISTS \"${cache}/failing/complete\"" "A failed dependency build was published")
file(REMOVE "${fail}")
file(WRITE "${cache}/failing/sentinel" "partial")
build(failing result)
expect("result EQUAL 0 AND EXISTS \"${cache}/failing/complete\"" "A failed dependency set was not rebuilt")
expect("EXISTS \"${cache}/failing/sentinel\"" "A partial build from the same recipes was not resumed")

# A partial build from another recipe directory starts again.
settings(moved)
file(MAKE_DIRECTORY "${cache}/moved/project")
file(WRITE "${cache}/moved/project/CMakeCache.txt" "CMAKE_HOME_DIRECTORY:INTERNAL=${TEST_ROOT}/other\n")
file(WRITE "${cache}/moved/sentinel" "foreign")
build(moved result)
expect("result EQUAL 0 AND EXISTS \"${cache}/moved/complete\"" "A partial build from another checkout was not rebuilt")
expect("NOT EXISTS \"${cache}/moved/sentinel\"" "A partial build from another checkout was reused")

# Downloads are verified before they are published, and on every use.
set(downloads "${cache}/downloads")
file(WRITE "${TEST_ROOT}/good.archive" "archive")
file(SHA256 "${TEST_ROOT}/good.archive" good)
kobalt_dependency_archive("${downloads}" fixture "${TEST_ROOT}/good.archive" "${good}" archive)
expect("archive STREQUAL \"${downloads}/fixture-${good}.tar\" AND EXISTS \"${archive}\"" "A verified archive was not published")
file(WRITE "${archive}" "damaged")
kobalt_dependency_archive("${downloads}" fixture "${TEST_ROOT}/good.archive" "${good}" archive)
file(SHA256 "${archive}" actual)
expect("actual STREQUAL good" "A damaged cached archive was used")
file(WRITE "${TEST_ROOT}/script.cmake" "
include([==[${CACHE_SCRIPT}]==])
kobalt_dependency_archive([==[${downloads}]==] fixture \"\${URL}\" \"\${HASH}\" archive)
")
string(SHA256 wrong "different")
foreach(case "${TEST_ROOT}/good.archive|${wrong}|Bundled source checksum mismatch: fixture"
        "${TEST_ROOT}/missing.archive|${wrong}|Missing bundled source archive: fixture")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 url)
    list(GET case 1 hash)
    list(GET case 2 expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DURL=${url}" "-DHASH=${hash}" -P "${TEST_ROOT}/script.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE log)
    expect("NOT result EQUAL 0" "An unverified archive was accepted: ${expected}")
    expect("log MATCHES \"${expected}\"" "Unexpected archive error: ${log}")
    expect("NOT EXISTS \"${downloads}/fixture-${hash}.tar\"" "An unverified archive was published")
endforeach()
file(GLOB partial "${downloads}/*.partial")
expect("NOT partial" "A rejected archive left a partial download: ${partial}")
file(REMOVE_RECURSE "${TEST_ROOT}")
