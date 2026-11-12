# Builds CREngine from a local checkout of the koblaze fork, for engine
# development on the desktop and for trial packages for a Kobo. See doc/building.md#local-engine-source.
#
# Call after kobalt_dependencies(). When KOBALT_ENGINE_SOURCE names a checkout,
# this replaces CRENGINE_SOURCE and CRENGINE_BUILD and defines the target
# kobalt-local-engine. The checkout is compiled in this worktree's build
# folder, so an engine edit rebuilds only the changed engine files. The
# shared dependency cache still supplies every other library, and no cache
# entry is changed.
#
# The engine options repeat the ones of the crengine library in
# dependencies/CMakeLists.txt. Keep both lists the same. They are not shared,
# because a change to that file gives every dependency set a new key.
include(ExternalProject)

function(kobalt_local_engine)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    # KOBALT_ENGINE_SOURCE in the environment names the checkout for a build
    # in a container, which cannot follow a link to a folder of the host.
    set(default "$ENV{KOBALT_ENGINE_SOURCE}")
    if(NOT default AND EXISTS "${root}/vendor/koblaze/CMakeLists.txt")
        set(default "${root}/vendor/koblaze")
    endif()
    set(KOBALT_ENGINE_SOURCE "${default}" CACHE PATH
        "Local CREngine checkout to build in place of the pinned source; empty uses the pin")
    if(NOT KOBALT_ENGINE_SOURCE)
        return()
    endif()
    if(NOT EXISTS "${KOBALT_ENGINE_SOURCE}/crengine/include/lvdocview.h")
        message(FATAL_ERROR "KOBALT_ENGINE_SOURCE is not a CREngine checkout: ${KOBALT_ENGINE_SOURCE}")
    endif()
    if(ANDROID)
        message(FATAL_ERROR "A local engine source is for desktop and Kobo builds only")
    endif()
    file(REAL_PATH "${KOBALT_ENGINE_SOURCE}" engine)
    message(STATUS "CREngine: local source ${engine}, not the pinned source and patches")

    find_program(KOBALT_HOST_PKG_CONFIG pkg-config NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    # platforms/kobo/syntax.cmake finds the engine headers by `crengine` in
    # the include paths of the desktop build.
    set(build "${CMAKE_CURRENT_BINARY_DIR}/crengine-local")
    set(pkgconfig "${prefix}/lib/pkgconfig")
    # The same search paths as dependencies/CMakeLists.txt uses for a Kobo SDK.
    if(KOBALT_QT_MAJOR EQUAL 5)
        string(APPEND pkgconfig ":${CMAKE_SYSROOT}/usr/lib/pkgconfig")
    elseif(KOBALT_QT_MAJOR EQUAL 6 AND CMAKE_CROSSCOMPILING)
        string(APPEND pkgconfig ":/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
    endif()
    set(environment --unset=PKG_CONFIG_SYSROOT_DIR "PKG_CONFIG_PATH=${pkgconfig}"
        "PKG_CONFIG_LIBDIR=${pkgconfig}")
    set(options -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DCMAKE_PREFIX_PATH=${prefix}"
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DPKG_CONFIG_EXECUTABLE=${KOBALT_HOST_PKG_CONFIG}" "-DCMAKE_FIND_ROOT_PATH=${prefix}"
        -DAUTO=OFF -DFREETYPE=ON -DHARFBUZZ=ON -DLIBPNG=ON -DLIBJPEG=ON -DGIF=ON -DLUNASVG=ON
        -DZLIB=ON -DUTF8PROC=ON -DFRIBIDI=ON -DLIBUNIBREAK=ON
        -DMATHML=ON)
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND options "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        # The desktop app also loads Qt's FreeType. The pinned source gets
        # the same definitions from apply-patches.cmake. A Kobo build
        # renames the two functions in the built library.
        list(APPEND options -DKOBALT_FONT_GLYPH_FUNCTIONS=ON)
    endif()
    if(APPLE)
        list(APPEND environment "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        list(APPEND options "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    # BUILD_ALWAYS lets Ninja in the engine build decide what changed.
    ExternalProject_Add(kobalt-local-engine
        SOURCE_DIR "${engine}" BINARY_DIR "${build}" PREFIX "${build}-steps"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${environment}
            "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR> -G Ninja ${options}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target crengine
        BUILD_ALWAYS TRUE USES_TERMINAL_BUILD TRUE
        BUILD_BYPRODUCTS "${build}/libcrengine.a")
    set(CRENGINE_SOURCE "${engine}" PARENT_SCOPE)
    set(CRENGINE_BUILD "${build}" PARENT_SCOPE)
endfunction()
