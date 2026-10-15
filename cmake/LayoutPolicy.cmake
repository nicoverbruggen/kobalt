function(kobalt_layout_policy output root archive_source)
    # Changes to the inspection policy or its resource decoder invalidate old verdicts.
    # Changes to the engine or to the code that loads and marks the parsed book
    # invalidate cached books, which use the same policy.
    set(LAYOUT_POLICY_INPUTS src/publication/layout_compatibility.cpp src/publication/layout_compatibility.h
        src/publication/native_resources.cpp src/publication/local_resources.cpp src/publication/resource_cache.cpp src/publication/resource_cache.h
        src/publication/image_policy.cpp src/publication/image_policy.h src/publication/publication.cpp dependencies/manifest.json
        src/engine/crengine/reader_source.cpp src/engine/crengine/reader_alignment.cpp
        src/engine/crengine/reader_page_layout.cpp src/engine/crengine/reader_links.cpp
        ${archive_source})
    list(APPEND LAYOUT_POLICY_INPUTS dependencies/patches/lunasvg-resource-limits.patch)
    file(STRINGS "${root}/dependencies/crengine-patches.txt" patches)
    list(APPEND LAYOUT_POLICY_INPUTS dependencies/crengine-patches.txt)
    foreach(patch IN LISTS patches)
        list(APPEND LAYOUT_POLICY_INPUTS "dependencies/patches/${patch}")
    endforeach()
    set(LAYOUT_POLICY_HASHES "")
    foreach(input IN LISTS LAYOUT_POLICY_INPUTS)
        get_filename_component(input_path "${input}" ABSOLUTE BASE_DIR "${root}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input_path}")
        file(SHA256 "${input_path}" digest)
        string(APPEND LAYOUT_POLICY_HASHES "${digest}")
    endforeach()
    string(SHA256 digest "${LAYOUT_POLICY_HASHES}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()
