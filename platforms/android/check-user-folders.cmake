cmake_minimum_required(VERSION 3.22)
find_package(Java 17 REQUIRED COMPONENTS Runtime Development)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT OUTPUT)
    set(OUTPUT "${root}/build/android-user-folders-tests")
endif()
file(MAKE_DIRECTORY "${OUTPUT}/classes")
execute_process(COMMAND "${Java_JAVAC_EXECUTABLE}" -d "${OUTPUT}/classes"
    "${root}/platforms/android/app/src/org/kobalt/android/UserFolders.java"
    "${root}/tests/android/UserFoldersTest.java" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot compile Android folder checks")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPATHS=${OUTPUT}/fixture"
    -P "${root}/platforms/cleanup.cmake" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Java_JAVA_EXECUTABLE}" -cp "${OUTPUT}/classes"
    org.kobalt.android.UserFoldersTest "${OUTPUT}/fixture" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Android folder checks failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPATHS=${OUTPUT}/fixture"
    -P "${root}/platforms/cleanup.cmake" COMMAND_ERROR_IS_FATAL ANY)
