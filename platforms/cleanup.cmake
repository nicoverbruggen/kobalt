# Removes run outputs and scratch state without the risk of `rm -rf` on a
# variable path:
#
#   cmake -DPATHS="<path1>;<path2>" -P platforms/cleanup.cmake
#
# Each path is a file or a directory. Relative paths start at the working
# directory. A path is removed only when it lies strictly inside one of these
# folders, after all symlinks are resolved:
#
# - the `build/` folder of the checkout that holds this script, except the
#   shared dependency cache in `build/dependency-cache/`;
# - a Claude Code session scratchpad,
#   `/private/tmp/claude-<user>/<project>/<session>/scratchpad/` or the same
#   layout under `$TMPDIR`. Only the scratchpad of one session qualifies:
#   `claude-<user>/` holds the scratch folders of every project and session.
#
# Everything else is refused, including the checkout root, `build/` itself, a
# scratch folder itself, `..` escapes and a symlink that points outside. Both
# the path itself and the place its symlinks lead to must be inside. A symlink
# is removed as a link; its target stays. The removal does not follow symlinks
# inside a removed directory either.
#
# The script prints each path it removed, found absent or refused. It fails if
# it refused a path or could not remove one.
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PATHS OR PATHS STREQUAL "")
    message(FATAL_ERROR "Pass the paths to remove: cmake -DPATHS=\"<path1>;<path2>\" -P platforms/cleanup.cmake")
endif()

# Paths compare as resolved real paths, so the folders below resolve the same way.
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
set(build "${root}/build")
if(EXISTS "${build}")
    get_filename_component(build "${build}" REALPATH)
endif()
set(scratch_bases "")
foreach(base IN ITEMS "/private/tmp" "$ENV{TMPDIR}")
    if(NOT base STREQUAL "" AND IS_DIRECTORY "${base}")
        get_filename_component(base "${base}" REALPATH)
        list(APPEND scratch_bases "${base}")
    endif()
endforeach()
list(REMOVE_DUPLICATES scratch_bases)

# Sets output to the part of path below folder, with a trailing slash, or to
# NOTFOUND when path is not folder or below it. Folder itself gives "".
function(kobalt_cleanup_below path folder output)
    set(${output} NOTFOUND PARENT_SCOPE)
    string(FIND "${path}/" "${folder}/" position)
    if(position EQUAL 0)
        string(LENGTH "${folder}/" length)
        string(SUBSTRING "${path}/" ${length} -1 rest)
        set(${output} "${rest}" PARENT_SCOPE)
    endif()
endfunction()

# Sets output to the real path of an absolute path. REALPATH leaves a missing
# path unresolved, so this resolves its deepest existing folder and appends
# the missing names. A missing part that contains `.` or `..` stays as it is,
# and kobalt_cleanup_allowed refuses it.
function(kobalt_cleanup_resolve path output)
    set(head "${path}")
    set(tail "")
    while(NOT EXISTS "${head}" AND NOT IS_SYMLINK "${head}")
        get_filename_component(name "${head}" NAME)
        get_filename_component(parent "${head}" DIRECTORY)
        if(parent STREQUAL head OR parent STREQUAL "")
            break()
        endif()
        set(tail "/${name}${tail}")
        set(head "${parent}")
    endwhile()
    get_filename_component(head "${head}" REALPATH)
    set(${output} "${head}${tail}" PARENT_SCOPE)
endfunction()

# Sets output to TRUE when path lies strictly inside an allowed folder.
function(kobalt_cleanup_allowed path output)
    set(${output} FALSE PARENT_SCOPE)
    if("${path}/" MATCHES "/\\.\\.?/")
        return()
    endif()
    kobalt_cleanup_below("${path}" "${build}" rest)
    if(NOT rest STREQUAL "NOTFOUND")
        if(NOT rest STREQUAL "" AND NOT rest MATCHES "^dependency-cache/")
            set(${output} TRUE PARENT_SCOPE)
        endif()
        return()
    endif()
    foreach(base IN LISTS scratch_bases)
        kobalt_cleanup_below("${path}" "${base}" rest)
        if(rest MATCHES "^claude-[^/]+/[^/]+/[^/]+/scratchpad/.")
            set(${output} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

set(refused 0)
foreach(path IN LISTS PATHS)
    if(path STREQUAL "")
        continue()
    endif()
    get_filename_component(absolute "${path}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    kobalt_cleanup_resolve("${absolute}" real)
    # The entry to remove is the path with only its parent folders resolved, so
    # a symlink is removed as a link. A path that names no symlink resolves fully.
    set(entry "${real}")
    if(IS_SYMLINK "${absolute}")
        get_filename_component(parent "${absolute}" DIRECTORY)
        get_filename_component(name "${absolute}" NAME)
        kobalt_cleanup_resolve("${parent}" parent)
        set(entry "${parent}/${name}")
    endif()

    kobalt_cleanup_allowed("${entry}" entry_allowed)
    kobalt_cleanup_allowed("${real}" real_allowed)
    if(NOT entry_allowed OR NOT real_allowed)
        if(entry STREQUAL real)
            message(STATUS "Refused: ${path} (${real} is not inside a folder this script may clean)")
        else()
            message(STATUS "Refused: ${path} (${entry} leads to ${real}, which is not inside a folder this script may clean)")
        endif()
        math(EXPR refused "${refused} + 1")
    elseif(NOT EXISTS "${entry}" AND NOT IS_SYMLINK "${entry}")
        message(STATUS "Absent: ${entry}")
    else()
        file(REMOVE_RECURSE "${entry}")
        if(EXISTS "${entry}" OR IS_SYMLINK "${entry}")
            message(STATUS "Failed: ${entry} is still present")
            math(EXPR refused "${refused} + 1")
        else()
            message(STATUS "Removed: ${entry}")
        endif()
    endif()
endforeach()

if(refused GREATER 0)
    message(FATAL_ERROR "Refused or failed to remove ${refused} path(s)")
endif()
