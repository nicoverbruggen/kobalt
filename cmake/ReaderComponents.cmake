# Shared components use supplied paths and services. The host links its native
# adapters and diagnostics implementation into the final binary.
get_filename_component(KOBALT_COMPONENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(kobalt_publication_library name)
    add_library(${name} STATIC
        "${KOBALT_COMPONENT_ROOT}/src/publication/publication.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/publication/zip_resource.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/publication/resource_cache.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/publication/local_resources.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/publication/image_policy.cpp")
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
    target_link_libraries(${name} PUBLIC Qt${KOBALT_QT_MAJOR}::Core z)
endfunction()

# Position conversion is optional integration code. The plain native reader
# links the engine without this target; Kobo and codec tests link it explicitly.
function(kobalt_nickel_position_library name model)
    add_library(${name} STATIC "${KOBALT_COMPONENT_ROOT}/src/platform/nickel/common/engine_positions.cpp")
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
    # This adapter reads CREngine positions, so it needs the same private
    # engine headers and configured dependency headers as the model target.
    get_target_property(engine_headers ${model} INCLUDE_DIRECTORIES)
    target_include_directories(${name} SYSTEM PRIVATE ${engine_headers})
    target_link_libraries(${name} PUBLIC ${model})
endfunction()

function(kobalt_reading_library name model publication layout_policy)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KOBALT_COMPONENT_ROOT}/resources/dictionaries.json")
    file(READ "${KOBALT_COMPONENT_ROOT}/resources/dictionaries.json" DICTIONARY_CATALOG)
    configure_file("${KOBALT_COMPONENT_ROOT}/src/dictionaries/catalog.h.in" dictionary_catalog.h @ONLY)
    add_library(${name} STATIC
        "${KOBALT_COMPONENT_ROOT}/src/dictionaries/manager.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/dictionaries/manager.h"
        "${KOBALT_COMPONENT_ROOT}/src/dictionaries/stardict.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/reader_session.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/appearance_settings.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/bookmark_session.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/header_footer.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/reading_book.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/reading/reading_progress.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/storage/reader_storage.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/storage/bookmark_store.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/storage/layout_cache.cpp"
        "${KOBALT_COMPONENT_ROOT}/src/publication/layout_compatibility.cpp")
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
    target_compile_definitions(${name} PRIVATE KOBALT_LAYOUT_POLICY="${layout_policy}")
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PUBLIC ${model} ${publication} Qt${KOBALT_QT_MAJOR}::Gui Qt${KOBALT_QT_MAJOR}::Network PRIVATE Threads::Threads)
endfunction()
