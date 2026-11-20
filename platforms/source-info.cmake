# Exported sources carry provenance without requiring a Git working tree.
function(kobalt_source_info root)
    set(commit unknown)
    set(modified OFF)
    if(EXISTS "${root}/.git")
        execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${root}"
            OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${root}"
            OUTPUT_VARIABLE changes COMMAND_ERROR_IS_FATAL ANY)
        if(changes)
            set(modified ON)
        endif()
    elseif(EXISTS "${root}/source-info.json")
        file(READ "${root}/source-info.json" info)
        string(JSON commit GET "${info}" commit)
        string(LENGTH "${commit}" length)
        if(NOT length EQUAL 40 OR NOT commit MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Invalid exported source commit")
        endif()
        string(SUBSTRING "${commit}" 0 7 commit)
    endif()
    set(KOBALT_COMMIT "${commit}" PARENT_SCOPE)
    set(KOBALT_COMMIT_MODIFIED "${modified}" PARENT_SCOPE)
endfunction()

function(kobalt_build_info_header root header)
    kobalt_source_info("${root}")
    configure_file("${root}/src/support/build_info.h.in" "${header}" @ONLY)
endfunction()

function(kobalt_desktop_build_info root)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
    # The Kobo syntax check can use the header immediately after configuration.
    kobalt_build_info_header("${root}" "${header}")
    add_custom_target(kobalt-build-info
        COMMAND "${CMAKE_COMMAND}" "-DKOBALT_BUILD_INFO_HEADER=${header}"
            -P "${root}/platforms/source-info.cmake"
        BYPRODUCTS "${header}"
        VERBATIM)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    if(DEFINED KOBALT_BUILD_INFO_HEADER)
        kobalt_build_info_header("${root}" "${KOBALT_BUILD_INFO_HEADER}")
        return()
    endif()
    kobalt_source_info("${root}")
    include("${CMAKE_CURRENT_LIST_DIR}/release-version.cmake")
    file(READ "${root}/assets/manifest.json" manifest)
    string(JSON fallback GET "${manifest}" version)
    kobalt_release_version(version "${fallback}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${KOBALT_COMMIT} ${KOBALT_COMMIT_MODIFIED} ${version}")
endif()
