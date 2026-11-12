# Parsed-book compatibility is explicit, not a fingerprint of build inputs.
# Retain the beta 7 policy token so compatible updates can reuse its caches.
# Bump documentCacheChecksum's format for incompatible parser/preprocessing changes.
set(layout_policy "849b421de582bf6b89f84263eab8e27dabd97cc0a8cfc807504f170a6ac35b2a")

# Increment for layout/metrics changes not covered by CREngine's formatting
# version. Figure keep-together defaults require repagination, not reparsing.
# Version 3: pages without height are left out, so some books have fewer pages.
set(rendering_version 3)
file(GLOB cache_patterns CONFIGURE_DEPENDS "${KOBALT_ROOT}/assets/hyph/*.pattern")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${cache_patterns})
set(cache_hashes "${rendering_version};")
foreach(input IN LISTS cache_patterns)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH name "${KOBALT_ROOT}" "${input}")
    string(APPEND cache_hashes "${name}:${digest};")
endforeach()
string(SHA256 rendering_policy "${cache_hashes}")
