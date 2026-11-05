# Builds one dependency set into KOBALT_DEPENDENCY_ENTRY. The platform projects
# configure this project through cache.cmake, which chooses the entry, passes
# the compiler and SDK settings and publishes the entry when the build succeeds.
cmake_minimum_required(VERSION 3.22)
project(KobaltDependencies LANGUAGES C CXX)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
include(ExternalProject)
include("${CMAKE_CURRENT_SOURCE_DIR}/cache.cmake")
find_program(PATCH_EXECUTABLE patch REQUIRED)
find_program(MAKE_EXECUTABLE make REQUIRED)
find_program(KOBALT_HOST_PKG_CONFIG pkg-config NO_CMAKE_FIND_ROOT_PATH REQUIRED)
foreach(setting KOBALT_DEPENDENCY_ENTRY KOBALT_DEPENDENCY_DOWNLOADS KOBALT_DEPENDENCY_JOBS)
    if(NOT ${setting})
        message(FATAL_ERROR "Configure dependencies through dependencies/cache.cmake; ${setting} is missing")
    endif()
endforeach()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" manifest)
set(output "${KOBALT_DEPENDENCY_ENTRY}")
set(prefix "${output}/install")
set(common -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_INSTALL_PREFIX=${prefix}" "-DCMAKE_PREFIX_PATH=${prefix}"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DPKG_CONFIG_EXECUTABLE=${KOBALT_HOST_PKG_CONFIG}" "-DCMAKE_FIND_ROOT_PATH=${prefix}")
if(APPLE)
    list(APPEND common "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND common "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(ANDROID)
    list(APPEND common "-DANDROID_ABI=${ANDROID_ABI}" "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON "-DANDROID_NDK=${ANDROID_NDK}")
endif()
set(pkgconfig "${prefix}/lib/pkgconfig")
if(KOBALT_QT_MAJOR EQUAL 5)
    string(APPEND pkgconfig ":${CMAKE_SYSROOT}/usr/lib/pkgconfig")
elseif(KOBALT_QT_MAJOR EQUAL 6 AND NOT ANDROID)
    string(APPEND pkgconfig ":/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
endif()
set(environment --unset=PKG_CONFIG_SYSROOT_DIR "PKG_CONFIG_PATH=${pkgconfig}" "PKG_CONFIG_LIBDIR=${pkgconfig}")
if(APPLE)
    list(APPEND environment "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

function(kobalt_source name)
    cmake_parse_arguments(source "NATIVE_ENGINE" "" "PATCHES" ${ARGN})
    string(JSON url GET "${manifest}" "${name}" url)
    string(JSON hash GET "${manifest}" "${name}" sha256)
    if(KOBALT_SOURCE_ARCHIVES)
        set(url "${KOBALT_SOURCE_ARCHIVES}/${name}.archive")
    endif()
    # The shared download is verified on every use; extraction checks it again.
    kobalt_dependency_archive("${KOBALT_DEPENDENCY_DOWNLOADS}" "${name}" "${url}" "${hash}" archive)
    string(REPLACE "-" "_" content "kobalt_${name}")
    string(JOIN "\n" patch_list ${source_PATCHES})
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/${content}-patches.txt" CONTENT "${patch_list}" @ONLY)
    set(extraction)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        # Preserve release timestamps so Autotools does not regenerate shipped
        # configure scripts.
        set(extraction DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(${content}
        URL "${archive}" URL_HASH "SHA256=${hash}"
        ${extraction}
        SOURCE_DIR "${output}/sources/${name}"
        SOURCE_SUBDIR kobalt-no-project
        PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE=<SOURCE_DIR>"
            "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}" "-DPATCH_LIST=${CMAKE_BINARY_DIR}/${content}-patches.txt"
            "-DNATIVE_ENGINE=${source_NATIVE_ENGINE}" -P "${CMAKE_CURRENT_SOURCE_DIR}/apply-patches.cmake")
    FetchContent_MakeAvailable(${content})
    set(${name}_source "${${content}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(kobalt_library name)
    cmake_parse_arguments(lib "AUTOTOOLS;NO_INSTALL" "SOURCE;SUFFIX;ARCHIVE" "OPTIONS;DEPENDS;MAKE_OPTIONS" ${ARGN})
    if(NOT lib_SOURCE)
        set(lib_SOURCE "${${name}_source}${lib_SUFFIX}")
    endif()
    set(directory "${output}/${name}")
    set(byproduct "${prefix}/lib/${lib_ARCHIVE}")
    set(install INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>)
    if(lib_NO_INSTALL)
        set(install INSTALL_COMMAND "${CMAKE_COMMAND}" -E true)
        set(byproduct "${directory}/${lib_ARCHIVE}")
    endif()
    if(lib_AUTOTOOLS)
set(host)
        set(cc "${CMAKE_C_COMPILER}")
        set(cflags "-fPIC ${CMAKE_C_FLAGS}")
        if(ANDROID)
            set(host --host=aarch64-linux-android)
            string(APPEND cc " --target=${CMAKE_C_COMPILER_TARGET}")
            string(APPEND cflags " --sysroot=${CMAKE_SYSROOT}")
        elseif(CMAKE_CROSSCOMPILING)
            set(host --host=arm-linux-gnueabihf)
            if(CMAKE_SYSROOT)
                string(APPEND cflags " --sysroot=${CMAKE_SYSROOT}")
            endif()
        endif()
        ExternalProject_Add(dep-${name}
            SOURCE_DIR "${lib_SOURCE}" BINARY_DIR "${directory}" PREFIX "${directory}/steps"
            DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
            CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${environment}
                "CC=${cc}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}" "CFLAGS=${cflags}"
                sh <SOURCE_DIR>/configure "--prefix=${prefix}" --disable-shared --enable-static --with-pic ${host}
            BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${MAKE_EXECUTABLE}" -j "${KOBALT_DEPENDENCY_JOBS}" ${lib_MAKE_OPTIONS}
            INSTALL_COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${MAKE_EXECUTABLE}" install ${lib_MAKE_OPTIONS}
            BUILD_BYPRODUCTS "${prefix}/lib/${lib_ARCHIVE}"
            DEPENDS ${lib_DEPENDS})
    else()
        ExternalProject_Add(dep-${name}
            SOURCE_DIR "${lib_SOURCE}" BINARY_DIR "${directory}" PREFIX "${directory}/steps"
            DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
            CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${environment}
                "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR> -G Ninja ${common} ${lib_OPTIONS}
            BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${KOBALT_DEPENDENCY_JOBS}"
            ${install}
            BUILD_BYPRODUCTS "${byproduct}"
            DEPENDS ${lib_DEPENDS})
    endif()
endfunction()

foreach(name freetype harfbuzz fribidi utf8proc libjpeg-turbo xxhash libunibreak koreader-base)
    kobalt_source(${name})
endforeach()
set(svg_patches "${koreader-base_source}/thirdparty/lunasvg")
kobalt_source(lunasvg PATCHES "${svg_patches}/cmake_tweaks.patch" "${svg_patches}/extended.patch"
    "${svg_patches}/pkgconfig.patch" "${CMAKE_CURRENT_SOURCE_DIR}/patches/lunasvg-resource-limits.patch")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/crengine-patches.txt" engine_patches)
list(TRANSFORM engine_patches PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/patches/")
if(APPLE OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND NOT ANDROID))
    set(engine_options NATIVE_ENGINE)
endif()
kobalt_source(crengine ${engine_options} PATCHES ${engine_patches})

# Android builds pinned libpng. The Mac reuses libpng from its Qt SDK and Kobo
# from its prepared SDK, so Kobo keeps the same libpng ABI as Qt; cache.cmake
# finds those. Qt itself is never configured here.
if(ANDROID)
    kobalt_source(libpng)
    kobalt_library(libpng ARCHIVE libpng16.a OPTIONS -DPNG_SHARED=OFF -DPNG_STATIC=ON
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF)
    set(png "${prefix}/lib/libpng16.a")
    set(png_include "${prefix}/include")
    set(png_dependency dep-libpng)
else()
    set(png "${KOBALT_PNG_LIBRARY}")
    set(png_include "${KOBALT_PNG_INCLUDE}")
    file(MAKE_DIRECTORY "${prefix}/lib/pkgconfig")
    file(CONFIGURE OUTPUT "${prefix}/lib/pkgconfig/libpng.pc" CONTENT
        "Name: libpng\nDescription: libpng from the SDK\nVersion: ${KOBALT_PNG_VERSION}\nLibs: ${png} -lz\nCflags: -I${png_include}\n" @ONLY)
endif()
file(CONFIGURE OUTPUT "${prefix}/lib/pkgconfig/zlib.pc" CONTENT "Name: zlib\nDescription: SDK zlib\nVersion: 1\nLibs: -lz\n" @ONLY)
kobalt_library(freetype ARCHIVE libfreetype.a DEPENDS ${png_dependency} OPTIONS -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON
    -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HVF=ON -DFT_DISABLE_PNG=OFF "-DPNG_LIBRARY=${png}" "-DPNG_PNG_INCLUDE_DIR=${png_include}")
kobalt_library(harfbuzz ARCHIVE libharfbuzz.a DEPENDS dep-freetype OPTIONS -DHB_BUILD_UTILS=OFF
    -DHB_BUILD_SUBSET=OFF -DHB_BUILD_RASTER=OFF -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF -DHB_HAVE_FREETYPE=ON)
kobalt_library(fribidi AUTOTOOLS ARCHIVE libfribidi.a MAKE_OPTIONS SUBDIRS=lib)
kobalt_library(utf8proc ARCHIVE libutf8proc.a OPTIONS -DUTF8PROC_ENABLE_TESTING=OFF)
kobalt_library(libjpeg-turbo ARCHIVE libjpeg.a OPTIONS -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF)
kobalt_library(xxhash ARCHIVE libxxhash.a SUFFIX /cmake_unofficial OPTIONS -DXXHASH_BUILD_XXHSUM=OFF)
kobalt_library(libunibreak AUTOTOOLS ARCHIVE libunibreak.a)
kobalt_library(lunasvg ARCHIVE liblunasvg.a OPTIONS "-DXTENDED_DIR=${svg_patches}/xtended" -DLUNASVG_BUILD_EXAMPLES=OFF)
kobalt_library(crengine NO_INSTALL ARCHIVE libcrengine.a DEPENDS dep-freetype dep-harfbuzz dep-fribidi
    dep-utf8proc dep-libjpeg-turbo dep-xxhash dep-libunibreak dep-lunasvg OPTIONS
    -DAUTO=OFF -DFREETYPE=ON -DHARFBUZZ=ON -DLIBPNG=ON -DLIBJPEG=ON -DGIF=ON -DLUNASVG=ON
    -DZLIB=ON -DUTF8PROC=ON -DFRIBIDI=ON -DLIBUNIBREAK=ON -DMATHML=ON)
if(ANDROID)
    kobalt_source(openssl PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/openssl-android-libraries.patch")
    find_program(PERL_EXECUTABLE perl REQUIRED)
    get_filename_component(ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    set(openssl "${output}/openssl")
    ExternalProject_Add(dep-openssl SOURCE_DIR "${openssl_source}" BINARY_DIR "${openssl}" PREFIX "${openssl}/steps"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${ANDROID_NDK}"
            "PATH=${ndk_bin}:$ENV{PATH}" "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure android-arm64
            shared no-tests no-apps -U__ANDROID_API__ -D__ANDROID_API__=28
            -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
        BUILD_COMMAND "${CMAKE_COMMAND}" -E env "PATH=${ndk_bin}:$ENV{PATH}"
            "${MAKE_EXECUTABLE}" -j "${KOBALT_DEPENDENCY_JOBS}" build_generated
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${ndk_bin}:$ENV{PATH}"
            "${MAKE_EXECUTABLE}" -j "${KOBALT_DEPENDENCY_JOBS}" SHLIB_VERSION_NUMBER= libcrypto_3.so libssl_3.so
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E true
        BUILD_BYPRODUCTS "${openssl}/libcrypto_3.so" "${openssl}/libssl_3.so")
    kobalt_source(hiddenapibypass)
    file(SHA256 "${hiddenapibypass_source}/classes.jar" jar_hash)
    if(NOT jar_hash STREQUAL "021973912a8f481b84308aec519b6bb7f93ff4a82e90d494cbc658288c7b6e2f")
        message(FATAL_ERROR "AndroidHiddenApiBypass classes.jar checksum mismatch")
    endif()
endif()
