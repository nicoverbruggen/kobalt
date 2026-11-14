# Builds the repository-owned koblaze source for every platform. The shared
# dependency cache supplies its libraries; engine objects stay in this
# worktree so changing engine source rebuilds only the affected objects.
include(ExternalProject)

function(kobalt_local_engine)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    # Retire cached paths from the former external-checkout override.
    unset(KOBALT_ENGINE_SOURCE CACHE)
    set(engine "${root}/src/engine/blaze")
    if(NOT EXISTS "${engine}/crengine/include/lvdocview.h")
        message(FATAL_ERROR "Missing repository engine source: ${engine}")
    endif()
    message(STATUS "CREngine: repository source ${engine}")

    find_program(KOBALT_HOST_PKG_CONFIG pkg-config NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    # Keep the generated-header directory in sync with the include-path filter
    # in platforms/kobo/syntax.cmake.
    set(build "${CMAKE_CURRENT_BINARY_DIR}/blaze")
    set(pkgconfig "${prefix}/lib/pkgconfig")
    # The same search paths as dependencies/CMakeLists.txt uses for a Kobo SDK.
    if(KOBALT_QT_MAJOR EQUAL 5)
        string(APPEND pkgconfig ":${CMAKE_SYSROOT}/usr/lib/pkgconfig")
    elseif(KOBALT_QT_MAJOR EQUAL 6 AND CMAKE_CROSSCOMPILING AND NOT ANDROID)
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
    if(ANDROID)
        list(APPEND options "-DANDROID_ABI=${ANDROID_ABI}" "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
            -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON "-DANDROID_NDK=${ANDROID_NDK}")
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        # The desktop app also loads Qt's FreeType. Kobo and Android rename
        # these functions in their reader-engine archive instead.
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
