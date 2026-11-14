file(STRINGS "${PATCH_LIST}" PATCHES)
foreach(patch IN LISTS PATCHES)
    execute_process(COMMAND "${PATCH_EXECUTABLE}" --batch --forward -p1 -i "${patch}"
        WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
