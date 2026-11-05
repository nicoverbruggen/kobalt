# Checks that src/ui/controls/phosphor_geometry.cpp matches the Phosphor SVGs.
#
#   cmake -DTEST_ROOT=<scratch> -DGENERATOR=<kobalt-icon-geometry> -DICONS=<svg folder>
#         -DEXPECTED=<checked-in file> -P test-icon-geometry.cmake
cmake_minimum_required(VERSION 3.22)

foreach(variable IN ITEMS TEST_ROOT GENERATOR ICONS EXPECTED)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "Missing -D${variable}")
    endif()
endforeach()
# A launcher run owns this root so cancellation can remove only its scratch state.
if(NOT "$ENV{KOBALT_TEST_TMP_ROOT}" STREQUAL "")
    set(TEST_ROOT "$ENV{KOBALT_TEST_TMP_ROOT}/icon-geometry")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
execute_process(COMMAND "${GENERATOR}" "${ICONS}" "${TEST_ROOT}/phosphor_geometry.cpp"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    file(REMOVE_RECURSE "${TEST_ROOT}")
    message(FATAL_ERROR "The icon geometry generator failed: ${error}")
endif()
file(SHA256 "${TEST_ROOT}/phosphor_geometry.cpp" generated)
file(SHA256 "${EXPECTED}" checked_in)
file(REMOVE_RECURSE "${TEST_ROOT}")
if(NOT generated STREQUAL checked_in)
    message(FATAL_ERROR "${EXPECTED} does not match the SVGs in ${ICONS}. "
        "From platforms/desktop, run cmake --build --preset mac --target update-icon-geometry, or --preset linux on Linux.")
endif()
