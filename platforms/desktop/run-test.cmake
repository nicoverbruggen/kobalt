# A launcher run owns this root so cancellation can remove only its scratch state.
if(NOT "$ENV{KOBALT_TEST_TMP_ROOT}" STREQUAL "")
    set(TEST_TMP_ROOT "$ENV{KOBALT_TEST_TMP_ROOT}")
endif()
# Own temporary files outside the test process: standalone checks can exit or
# abort before their QTemporaryDir destructors run.
if(NOT EXISTS "${TEST_EXECUTABLE}" OR NOT IS_DIRECTORY "${TEST_TMP_ROOT}")
    message(FATAL_ERROR "A test executable and temporary root are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run)
set(work "${TEST_TMP_ROOT}/kobalt-regression-${run}")
# Fast checks default to 30 seconds. Integration matrices and the screenshot
# run pass longer limits. CTest leaves a cleanup margin outside this guard.
if(NOT DEFINED TEST_TIMEOUT)
    set(TEST_TIMEOUT 30)
endif()
if(EXISTS "${work}")
    message(FATAL_ERROR "Test temporary directory already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")
# Keep diagnostics capturable with Linux SDKs that otherwise log to the journal.
# Strict main-thread checks abort a test that blocks the main thread while the
# reader takes input (see src/support/main_thread.h). Set the variable to
# `trace` in the environment to list every violation instead.
set(strict "$ENV{KOBALT_STRICT_MAIN_THREAD}")
if(strict STREQUAL "")
    set(strict 1)
endif()
# Whole-book checks default to Accurate. CTest registers separate .hybrid
# cases with mode 2 and .fast cases with mode 1. Both run in the native CI suite.
set(chapter_mode "$ENV{KOBALT_CHAPTER_MODE}")
if(chapter_mode STREQUAL "")
    set(chapter_mode 0)
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${work}" "QT_FORCE_STDERR_LOGGING=1" "KOBALT_STRICT_MAIN_THREAD=${strict}"
            "KOBALT_CHAPTER_MODE=${chapter_mode}"
            "${TEST_EXECUTABLE}" ${TEST_ARGUMENTS}
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE result
    TIMEOUT ${TEST_TIMEOUT})
file(REMOVE_RECURSE "${work}")
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Regression test failed: ${result}")
endif()
