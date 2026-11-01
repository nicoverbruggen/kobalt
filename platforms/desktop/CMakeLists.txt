cmake_minimum_required(VERSION 3.22)
project(Kobalt LANGUAGES C CXX)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The desktop app supports macOS and Linux.")
endif()

if(APPLE)
    set(KOBALT_DESKTOP_PLATFORM mac)
else()
    set(KOBALT_DESKTOP_PLATFORM linux)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
option(KOBALT_DESKTOP_SENSORS "Build real sensor adapter and sensor integration checks" OFF)
set(qt_components Core Gui Widgets Network Sql)
if(KOBALT_DESKTOP_SENSORS)
    list(APPEND qt_components Sensors)
endif()
if(BUILD_TESTING)
    # The scenario player sends input through the window system, as real input arrives.
    list(APPEND qt_components Test GuiPrivate)
endif()
find_package(Qt6 6.11 REQUIRED COMPONENTS ${qt_components})
file(REAL_PATH "${Qt6_DIR}/.." qt_config_root)
foreach(component IN LISTS qt_components)
    file(REAL_PATH "${Qt6${component}_DIR}/.." component_root)
    if(NOT component_root STREQUAL qt_config_root)
        message(FATAL_ERROR "Mixed Qt SDK cache for ${component}. Configure a fresh output or clear cached Qt6 package paths with -UQt6* before selecting the SDK.")
    endif()
endforeach()
get_filename_component(KOBALT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(retired KOBALT_DEPENDENCIES KOBALT_DEPENDENCY_CACHE KOBALT_ASSETS)
    unset(${retired} CACHE)
endforeach()

add_subdirectory("${KOBALT_ROOT}/dependencies" "${CMAKE_CURRENT_BINARY_DIR}/dependencies")
add_subdirectory("${KOBALT_ROOT}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets")

file(READ "${KOBALT_ASSETS}/manifest.json" assets)
string(JSON KOBALT_VERSION GET "${assets}" version)
string(JSON count LENGTH "${assets}" files)
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    string(JSON name GET "${assets}" files ${index} path)
    if(IS_ABSOLUTE "${name}" OR name MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Invalid asset path: ${name}")
    endif()
    string(JSON expected GET "${assets}" files ${index} sha256)
    file(SHA256 "${KOBALT_ASSETS}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Prepared runtime asset changed: ${name}")
    endif()
endforeach()
string(REGEX REPLACE "^v([0-9]+\\.[0-9]+(\\.[0-9]+)?).*$" "\\1" KOBALT_BUNDLE_VERSION "${KOBALT_VERSION}")

set(source "${KOBALT_ROOT}/src")
file(READ "${CRENGINE_SOURCE}/cr3gui/data/html5.css" READER_HTML_CSS)
configure_file("${source}/engine/crengine/reader_styles.h.in" reader_styles.h @ONLY)
file(GLOB patterns RELATIVE "${CRENGINE_SOURCE}/cr3gui/data/hyph" "${CRENGINE_SOURCE}/cr3gui/data/hyph/*.pattern")
list(SORT patterns)
list(TRANSFORM patterns PREPEND "\"")
list(TRANSFORM patterns APPEND "\"")
list(JOIN patterns ", " READER_HYPHENATION_PATTERNS)
configure_file("${source}/engine/crengine/reader_hyphenation.h.in" reader_hyphenation.h @ONLY)
include("${KOBALT_ROOT}/platforms/source-info.cmake")
kobalt_source_info("${KOBALT_ROOT}")
configure_file("${source}/support/build_info.h.in" build_info.h @ONLY)

# Keep portable objects separate so UI edits do not relink standalone checks.
file(GLOB_RECURSE shared CONFIGURE_DEPENDS
    "${source}/dictionaries/*.cpp" "${source}/engine/*.cpp"
    "${source}/library/*.cpp" "${source}/model/*.cpp"
    "${source}/reading/*.cpp" "${source}/storage/*.cpp")
add_library(kobalt-core OBJECT ${shared} display.cpp
    "${source}/assets/assets.cpp" "${source}/assets/bundled_fonts.cpp" "${source}/assets/font_catalog.cpp"
    "${source}/assets/system_fonts.cpp"
    "${source}/support/diagnostics.cpp" "${source}/support/main_thread.cpp"
    "${source}/publication/publication.cpp" "${source}/publication/zip_resource.cpp"
    "${source}/publication/resource_cache.cpp" "${source}/publication/local_resources.cpp"
    "${source}/publication/image_policy.cpp" "${source}/publication/layout_compatibility.cpp"
    "${source}/publication/native_resources.cpp" "${source}/publication/resource_server.cpp"
    "${source}/publication/plain_archive.cpp")
add_dependencies(kobalt-core kobalt-dependencies)
target_include_directories(kobalt-core PUBLIC "${KOBALT_ROOT}" "${source}" "${CMAKE_CURRENT_BINARY_DIR}"
    "${source}/interfaces/reader/ui/controls")
target_include_directories(kobalt-core SYSTEM PUBLIC
    "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${KOBALT_TEST_DEPENDENCIES}/include" "${KOBALT_TEST_DEPENDENCIES}/include/harfbuzz"
    "${KOBALT_TEST_DEPENDENCIES}/include/freetype2" "${KOBALT_TEST_DEPENDENCIES}/include/fribidi"
    "${KOBALT_TEST_PNG_INCLUDE}")
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
target_compile_definitions(kobalt-core PUBLIC KOBALT_READER_HARDWARE KOBALT_VERSION="${KOBALT_VERSION}"
    KOBALT_LAYOUT_POLICY="${layout_policy}")
if(APPLE)
    target_compile_definitions(kobalt-core PUBLIC fdatasync=fsync)
endif()
if(KOBALT_DESKTOP_SENSORS)
    target_link_libraries(kobalt-core PUBLIC Qt6::Sensors)
else()
    target_compile_definitions(kobalt-core PUBLIC KOBALT_NO_SENSORS)
endif()
target_compile_options(kobalt-core PRIVATE -Wall -Wextra)
option(KOBALT_SQLITE_ANNOTATIONS_DEFAULT "Use SQLite annotation storage by default" ON)
if(KOBALT_SQLITE_ANNOTATIONS_DEFAULT)
    set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
        COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=1)
else()
    set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
        COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=0)
endif()
target_link_libraries(kobalt-core PUBLIC Qt6::Widgets Qt6::Network Qt6::Sql
    "${CRENGINE_BUILD}/libcrengine.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libharfbuzz.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libxxhash.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libutf8proc.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libjpeg.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/liblunasvg.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libfreetype.a"
    "${KOBALT_TEST_DEPENDENCIES}/lib/libfribidi.a" "${KOBALT_TEST_DEPENDENCIES}/lib/libunibreak.a"
    "${KOBALT_TEST_PNG}" z)
# The engine and Qt must keep separate FreeType glyph implementations.
if(APPLE)
    target_link_options(kobalt-core INTERFACE "LINKER:-force_load,${KOBALT_TEST_DEPENDENCIES}/lib/libfreetype.a")
else()
    # Keep private static dependencies out of the executable dynamic symbol table,
    # so Qt and its plugins resolve their own font and image library symbols.
    target_link_options(kobalt-core INTERFACE "LINKER:--exclude-libs,ALL")
endif()
file(GLOB_RECURSE interfaces CONFIGURE_DEPENDS "${source}/interfaces/*.cpp")
add_library(kobalt-desktop OBJECT ${interfaces}
    window.cpp services.cpp
    ../android/reader.cpp ../android/start_screen.cpp
    "${source}/platform/api/reader_orientation.cpp")
target_link_libraries(kobalt-desktop PUBLIC kobalt-core)
set_source_files_properties("${source}/interfaces/reader/views/reading_page.cpp" PROPERTIES
    COMPILE_DEFINITIONS KOBALT_DESKTOP_POINTER)
target_compile_options(kobalt-desktop PRIVATE -Wall -Wextra)
# A build from a linked Git worktree is a variant named after the worktree's
# folder. A variant has its own name, bundle identifier and profile, so it can
# run beside other builds, and it opens no file types, so macOS does not offer
# every branch build for EPUB files. Set KOBALT_MAC_VARIANT to an empty string
# to build the regular app from a worktree.
set(default_variant "")
if(EXISTS "${KOBALT_ROOT}/.git")
    execute_process(COMMAND git rev-parse --path-format=absolute --git-dir --git-common-dir
        WORKING_DIRECTORY "${KOBALT_ROOT}" OUTPUT_VARIABLE git_dirs OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" git_dirs "${git_dirs}")
    list(GET git_dirs 0 git_dir)
    list(GET git_dirs 1 git_common_dir)
    if(NOT git_dir STREQUAL git_common_dir)
        get_filename_component(default_variant "${KOBALT_ROOT}" NAME)
    endif()
endif()
if(APPLE)
    set(variant_option KOBALT_MAC_VARIANT)
else()
    set(variant_option KOBALT_LINUX_VARIANT)
endif()
set(${variant_option} "${default_variant}" CACHE STRING "Name of a side-by-side desktop development build; empty for the regular app")
set(desktop_variant "${${variant_option}}")
if(NOT desktop_variant MATCHES "^[A-Za-z0-9-]*$")
    message(FATAL_ERROR "${variant_option} may only hold letters, digits and hyphens.")
endif()
if(desktop_variant)
    set(KOBALT_BUNDLE_NAME "Kobalt (${desktop_variant})")
    set(KOBALT_BUNDLE_IDENTIFIER "com.nicoverbruggen.kobalt.desktop.variant.${desktop_variant}")
    set(KOBALT_PROFILE_NAME "Kobalt-${desktop_variant}")
    set(KOBALT_DOCUMENT_TYPES "")
else()
    set(KOBALT_BUNDLE_NAME "Kobalt")
    set(KOBALT_BUNDLE_IDENTIFIER "com.nicoverbruggen.kobalt.desktop")
    set(KOBALT_PROFILE_NAME "Kobalt")
    set(KOBALT_DOCUMENT_TYPES "<key>CFBundleDocumentTypes</key><array><dict>
        <key>CFBundleTypeName</key><string>EPUB book</string>
        <key>CFBundleTypeRole</key><string>Viewer</string>
        <key>LSHandlerRank</key><string>Alternate</string>
        <key>LSItemContentTypes</key><array><string>org.idpf.epub-container</string></array>
    </dict></array>")
endif()
message(STATUS "Desktop app: ${KOBALT_BUNDLE_NAME}, ${KOBALT_BUNDLE_IDENTIFIER}")
add_executable(Kobalt main.cpp)
if(APPLE)
    set_target_properties(Kobalt PROPERTIES MACOSX_BUNDLE ON)
    set(runtime_asset_path "../Resources/assets")
else()
    set(runtime_asset_path "assets/runtime")
endif()
target_link_libraries(Kobalt PRIVATE kobalt-desktop kobalt-core)
# The profile folder under Application Support follows the application name.
target_compile_definitions(Kobalt PRIVATE KOBALT_PROFILE_NAME="${KOBALT_PROFILE_NAME}"
    KOBALT_RUNTIME_ASSET_PATH="${runtime_asset_path}")
set_target_properties(Kobalt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
set(resources "${KOBALT_ROOT}/assets")
file(GLOB notices CONFIGURE_DEPENDS "${resources}/notices/*")
qt_add_resources(kobalt-desktop desktop_artwork PREFIX "/desktop" BASE "${resources}" FILES "${resources}/kobalt-android.svg")
qt_add_resources(kobalt-desktop notices PREFIX "/kobalt" BASE "${resources}" FILES "${resources}/acknowledgments.json" ${notices}
    "${resources}/samples/words-and-letters.html" "${resources}/samples/layout.html")
if(APPLE)
    set(bundle_assets "${CMAKE_CURRENT_BINARY_DIR}/Kobalt.app/Contents/Resources/assets")
    add_custom_command(OUTPUT "${bundle_assets}/manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${bundle_assets}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KOBALT_ASSETS}" "${bundle_assets}"
        DEPENDS ${KOBALT_ASSET_INPUTS}
        VERBATIM)
    add_custom_target(kobalt-runtime-assets DEPENDS "${bundle_assets}/manifest.json")
    add_dependencies(Kobalt kobalt-runtime-assets)
endif()

# Compares two release-evaluation runs; see doc/release-evaluation.md.
add_executable(kobalt-eval-compare EXCLUDE_FROM_ALL "${KOBALT_ROOT}/platforms/eval/compare.cpp")
target_link_libraries(kobalt-eval-compare PRIVATE Qt6::Gui)

if(BUILD_TESTING)
    add_test(NAME build.cachePolicy COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cache-policy-test"
        "-DPOLICY_FILE=${KOBALT_ROOT}/platforms/cache-policy.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test-cache-policy.cmake")
    set_tests_properties(build.cachePolicy PROPERTIES LABELS "fast;build" TIMEOUT 30)
    add_test(NAME build.strings COMMAND "${CMAKE_COMMAND}"
        "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${KOBALT_ROOT}/platforms/strings-test.cmake")
    set_tests_properties(build.strings PROPERTIES LABELS "fast;build" TIMEOUT 30)
    add_test(NAME build.screenshotBooks COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-book-test"
        "-DPREPARE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/prepare-screenshot-books.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test-screenshot-books.cmake")
    set_tests_properties(build.screenshotBooks PROPERTIES LABELS "fast;build" TIMEOUT 30)
    find_program(KOBALT_TEST_ZIP zip REQUIRED)
    set(test_book "${KOBALT_ROOT}/tests/fixtures/trevelyans-type-tester.kepub.epub")
    file(SHA256 "${test_book}" test_book_hash)
    if(NOT test_book_hash STREQUAL "1050bc7af6a6b19267319c8b56965ca7cbf63abedfa8079ab5c4eacbb6c407ac")
        message(FATAL_ERROR "Type Tester fixture does not match the pinned v1.2 copy with the chapter-heading CSS fix")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${test_book}")
    set(test_variable_font "${KOBALT_ROOT}/tests/fixtures/AmstelvarAlpha-VF.ttf")
    file(SHA256 "${test_variable_font}" test_font_hash)
    if(NOT test_font_hash STREQUAL "8064db0a8d839ae41296252291acb8e4deffab64dddfb55485b0180420568626")
        message(FATAL_ERROR "Variable-font fixture does not match its pinned source")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${test_variable_font}")
    add_executable(kobalt-desktop-tests "${KOBALT_ROOT}/tests/desktop.cpp" "${source}/scenario/player.cpp")
    qt_add_resources(kobalt-desktop-tests "screenshot-gallery" PREFIX "/"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/screenshot-gallery.html")
    target_link_libraries(kobalt-desktop-tests PRIVATE kobalt-desktop kobalt-core Qt6::Test Qt6::GuiPrivate)
    target_compile_definitions(kobalt-desktop-tests PRIVATE
        KOBALT_TEST_ASSETS="${KOBALT_ASSETS}" KOBALT_TEST_ZIP="${KOBALT_TEST_ZIP}" KOBALT_TEST_BOOK="${test_book}"
        KOBALT_TEST_CORPUS="${KOBALT_ROOT}/tests/fixtures/public-domain"
        KOBALT_SCREENSHOT_BOOKS="${KOBALT_ROOT}/build/screenshot-books"
        KOBALT_SCREENSHOT_REFERENCES="${KOBALT_ROOT}/tests/fixtures/screenshot-books.json"
        KOBALT_SOURCE_ROOT="${KOBALT_ROOT}" KOBALT_SCENARIOS="${KOBALT_ROOT}/platforms/eval/scenarios.json"
        KOBALT_TEST_VARIABLE_FONT="${test_variable_font}" KOBALT_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
    set(headless_environment "QT_QPA_PLATFORM=minimal:enable_fonts:freetype" "QT_STYLE_OVERRIDE=Fusion" "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
    foreach(check pageAnimationCompatibility lightGestureFeedback lightGestureUnavailable lightGestureFeedbackPersistence annotationDefaultView galleryWorkerFailure galleryIntegrity sameProcessReadersDoNotRecoverEachOther standaloneHistorySourceKeys verticalJustification startup libraryOnboarding reading restartFinishedBooks errors diagnostics storageDiagnostics timingEvents features stylesheet bookmarks pinnedBookmarks bookmarkMerge annotations annotationModeFeedback annotationSelection coordinateProbeSerialization annotationGripMenus annotationCrossPageDrag selectionCrossPageDrag annotationCrossPageOverlap annotationOverlap touchCursor touchCursorOutsideReader links footnoteHitTargets footnoteHighlightShapes revealedLinkPress tappableLinks bookmarkCornerLinks
            annotationSectionBoundary headerFooterSizes batteryPercentageOnly headerFooterDismissal settings publisherFontDefault advancedText globalLayout columns fullWidthPanels dictionaryRedraw dictionaryAppearance dictionaryEditing dictionaryBookSearch dictionaryBookSearchUnicode dictionaryDismissal opticalSize desktopControls navigation largeInterfaceScale largeInterfaceScaleLongExit bookmarkReturn booxMenuDismissalRefresh booxRetirementFrame annotationTwoFingerTapConsumesWholeSequence annotationRightClickRejectsChords library libraryTextInheritance libraryGridRows libraryHold libraryScanningCaption libraryAtStart)
        set(lane fast)
        if(check MATCHES "^(pageAnimationCompatibility|lightGestureFeedback|lightGestureUnavailable|lightGestureFeedbackPersistence|annotationDefaultView|sameProcessReadersDoNotRecoverEachOther|standaloneHistorySourceKeys|verticalJustification|features|diagnostics|stylesheet|bookmarks|pinnedBookmarks|bookmarkMerge|annotations|annotationModeFeedback|annotationSelection|coordinateProbeSerialization|annotationGripMenus|annotationCrossPageDrag|annotationSectionBoundary|selectionCrossPageDrag|annotationCrossPageOverlap|annotationOverlap|touchCursor|touchCursorOutsideReader|links|tappableLinks|bookmarkCornerLinks|headerFooterSizes|batteryPercentageOnly|headerFooterDismissal|settings|publisherFontDefault|advancedText|globalLayout|columns|fullWidthPanels|dictionaryRedraw|dictionaryAppearance|dictionaryEditing|dictionaryBookSearch|dictionaryBookSearchUnicode|dictionaryDismissal|opticalSize|desktopControls|navigation|largeInterfaceScale|largeInterfaceScaleLongExit|bookmarkReturn|booxMenuDismissalRefresh|booxRetirementFrame|library|libraryTextInheritance|libraryGridRows|libraryHold|libraryScanningCaption|libraryAtStart)$")
            set(lane integration)
        endif()
        set(test_timeout 30)
        if(lane STREQUAL "integration")
            set(test_timeout 120)
        endif()
        math(EXPR ctest_timeout "${test_timeout} + 5")
        add_test(NAME desktop.${check} COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>" "-DTEST_ARGUMENTS=${check}"
            "-DTEST_TIMEOUT=${test_timeout}"
            "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
        set_tests_properties(desktop.${check} PROPERTIES ENVIRONMENT "${headless_environment}"
            LABELS "desktop;${lane}" TIMEOUT ${ctest_timeout})
    endforeach()
    add_test(NAME desktop.primaryViews COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>" "-DTEST_ARGUMENTS=--light-tests"
        "-DTEST_TIMEOUT=60" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(desktop.primaryViews PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "desktop;light" TIMEOUT 65)
    foreach(check startup reading errors storageDiagnostics galleryIntegrity galleryWorkerFailure)
        set_property(TEST desktop.${check} APPEND PROPERTY LABELS light)
    endforeach()
    foreach(profile 300-light 300-dark 0-light 0-dark)
        add_test(NAME desktop.wordSelection.${profile} COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>" "-DTEST_ARGUMENTS=wordSelection:${profile}"
            "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
        set_tests_properties(desktop.wordSelection.${profile} PROPERTIES ENVIRONMENT "${headless_environment}"
            LABELS "desktop;integration" TIMEOUT 125)
    endforeach()
    add_test(NAME desktop.libraryPageButtons COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>" "-DTEST_ARGUMENTS=libraryPageButtons"
        "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(desktop.libraryPageButtons PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "desktop;integration" TIMEOUT 125)
    foreach(view dictionary dictionary-choices footnote settings settings-fonts reading-fonts stats fields chapters
            bookmarks search acknowledgments license downloaded-dictionaries blocked-modal)
        add_test(NAME desktop.hardwarePageButtons.${view} COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>" "-DTEST_ARGUMENTS=hardwarePageButtons:${view}"
            "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
        set_tests_properties(desktop.hardwarePageButtons.${view} PROPERTIES ENVIRONMENT "${headless_environment}"
            LABELS "desktop;integration" TIMEOUT 125)
    endforeach()
    # Archive the existing objects so each standalone check links only the
    # production objects it uses, without compiling another implementation.
    add_library(kobalt-regression-runtime STATIC)
    set_target_properties(kobalt-regression-runtime PROPERTIES AUTOMOC OFF)
    target_link_libraries(kobalt-regression-runtime PUBLIC kobalt-core)
    add_custom_target(kobalt-tests DEPENDS kobalt-desktop-tests kobalt-eval-compare)
    add_executable(kobalt-startup-recovery-tests "${KOBALT_ROOT}/tests/startup_recovery.c"
        "${KOBALT_ROOT}/platforms/kobo/common/startup_recovery.c")
    set_target_properties(kobalt-startup-recovery-tests PROPERTIES AUTOMOC OFF)
    target_include_directories(kobalt-startup-recovery-tests PRIVATE "${KOBALT_ROOT}")
    target_compile_options(kobalt-startup-recovery-tests PRIVATE -Wall -Wextra -Werror)
    add_dependencies(kobalt-tests kobalt-startup-recovery-tests)
    add_test(NAME platform.startup-recovery COMMAND kobalt-startup-recovery-tests)
    set_tests_properties(platform.startup-recovery PROPERTIES LABELS "platform" TIMEOUT 30)
    add_test(NAME build.eval-device-outcomes COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=${CMAKE_COMMAND}"
        "-DTEST_ARGUMENTS=-DCOMPARE=$<TARGET_FILE:kobalt-eval-compare>;-P;${KOBALT_ROOT}/tests/eval_device_outcomes.cmake"
        "-DTEST_TIMEOUT=30" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(build.eval-device-outcomes PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "build;fast" TIMEOUT 35)
    add_custom_target(kobalt-fast-tests DEPENDS kobalt-desktop-tests kobalt-eval-compare)
    add_executable(kobalt-reader-lifecycle-tests "${KOBALT_ROOT}/tests/reader_lifecycle.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/engine_positions.cpp")
    target_link_libraries(kobalt-reader-lifecycle-tests PRIVATE kobalt-desktop kobalt-core Qt6::Test)
    target_include_directories(kobalt-reader-lifecycle-tests PRIVATE "${KOBALT_ROOT}/tests")
    target_compile_options(kobalt-reader-lifecycle-tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kobalt-reader-lifecycle-tests PRIVATE
        KOBALT_TEST_ASSET_ROOT="${KOBALT_ASSETS}" KOBALT_TEST_BOOK="${test_book}"
        KOBALT_TEST_ZIP="${KOBALT_TEST_ZIP}"
        KOBALT_FONT_DIRECTORY="${KOBALT_ASSETS}/fonts" KOBALT_INSTALLED_FONTS)
    add_dependencies(kobalt-tests kobalt-reader-lifecycle-tests)
    foreach(check teardownRemovesApplicationFilter shutdownFinalSaveTimesOut
            backgroundWriteFailureRetriesWithoutCountingPause backgroundCheckpointStopsBeforeOtherCallbacks standaloneReadingHistory uneditedPanelTransitions lightReplacesDefaultMenu appearanceApplyOnNavigation appearanceRelayoutPreservesPassage
            overflowClosesCurrentPanel appearanceSettingsRetainsDraft appearanceWriteFailureRetainsDraft
            appearanceSleepRetainsDraft appearanceFailedPreviewDismisses appearanceApplyFailureCanExit
            appearanceCloseDuringPreview appearanceSleepDuringFirstEdit appearanceDragsDuringPreview
            pageTurnsResumeWhenReaderIsShownAfterSleep clockStopsWhileReaderIsHidden closeWithPendingBookmark
            closeWaitsForLocalSave closeRetriesRequiredNativeSaves quitAfterFailedLocalSaveRetry closeWaitsForNativeTimeBeforePosition revokedNativeDrainRetainsLocalSave
            pageTurnTimingIgnoresNoopNavigation pageButtonsAfterTouchUseDeliveryTime pageGestureBurstIgnoresRapidTurns exitThroughReaderAndReopen exitGuardUsesTapTime staleSearchReplyIgnored searchSurvivesReflow
            searchHighlightClearsOnReflow
            sleepRequestFreezesOrientationBeforeHandoff closePublishesHeldOrientationBeforeBackgroundPaint
            closeRestoresOrientationBeforeBackgroundPaint rotationOwnership customSleepScreenIsOptIn
            closedReaderRetainsSleepScreen usbRetainsBackdropBeforeReleasingReader persistedSleepScreenRejectsStaleAndDamagedFrames
            homeSleepScreenRequiresUnambiguousLastBook nativeOpeningStatus
            sleepScreenUpdatesWhilePluggedIn sleepScreenDimsOnlyItsOwnedPage sleepTilesRepaintOnlyTheirArea
            sleepSnapshotMatchesTheDisplayedPage
            sleepSnapshotReplacesEachClockWithoutChangingTheReadingPage sleepTimeLeftUsesHoursAboveTwoHours
            sleepScreenModesKeepTheStockDefaultAndMigrateTheOptIn sleepCoverFitsWithoutCroppingOrDimming
            sleepCoverDecodingIsBoundedAndHonorsSourceCancellation wifiStatus remoteKeyActivityRequiresActiveReading
            finishChoiceSurvivesSleepAndRequiresActiveReader finishPromptPersistence progressPercentagesAgree kosyncAccountGating kosyncOpening kosyncNetwork kosyncManualToolbar kosyncManualConnection kosyncPasswordSleep kosyncFinal nativeProgressWaitsForSleep appearanceRetryCooldown closeWithUnusableSettings recoveryNoticeAfterFirstPage benchmarkNoticeAfterRun activityBatchesAndLowBattery usbFinalSave
            progressChoiceKeepAndGo finishedSyncWithoutPositionKeepsLocalReading
            progressChoiceForOlderAlert progressChoiceUnavailablePosition
            appearanceOpeningDoesNotRepaintToolbar annotationFramesKeepFirstPaintComplete annotationListResolvesPagesOnDemand)
        if(NOT KOBALT_DESKTOP_SENSORS AND check MATCHES "^(sleepRequestFreezesOrientationBeforeHandoff|closePublishesHeldOrientationBeforeBackgroundPaint)$")
            continue()
        endif()
        add_test(NAME lifecycle.${check} COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-reader-lifecycle-tests>" "-DTEST_ARGUMENTS=${check}"
            "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
        set_tests_properties(lifecycle.${check} PROPERTIES ENVIRONMENT "${headless_environment}"
            LABELS "lifecycle;integration" TIMEOUT 125)
    endforeach()
    add_executable(kobalt-shutdown-tests "${KOBALT_ROOT}/tests/kobo_shutdown.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/startup_recovery.c"
        "${KOBALT_ROOT}/platforms/kobo/common/native_lifecycle.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/standby_input.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/screensaver_compatibility.cpp")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(kobalt-screensaver-loader-tests "${KOBALT_ROOT}/tests/screensaver_loader.cpp"
            "${KOBALT_ROOT}/platforms/kobo/common/screensaver_compatibility.cpp")
        target_include_directories(kobalt-screensaver-loader-tests PRIVATE "${KOBALT_ROOT}" "${KOBALT_ROOT}/src")
        target_link_libraries(kobalt-screensaver-loader-tests PRIVATE Qt6::Core ${CMAKE_DL_LIBS})
        target_link_options(kobalt-screensaver-loader-tests PRIVATE "-Wl,--wrap=dl_iterate_phdr,--wrap=dlopen")
        target_compile_options(kobalt-screensaver-loader-tests PRIVATE -Wall -Wextra -Werror)
        add_dependencies(kobalt-tests kobalt-screensaver-loader-tests)
        add_test(NAME platform.screensaver-loader COMMAND kobalt-screensaver-loader-tests)
        set_tests_properties(platform.screensaver-loader PROPERTIES LABELS "platform" TIMEOUT 30)
    endif()
    add_library(kobalt-screensaver-fixture SHARED "${KOBALT_ROOT}/tests/support/screensaver_fixture.cpp")
    target_include_directories(kobalt-screensaver-fixture PRIVATE "${KOBALT_ROOT}")
    target_compile_definitions(kobalt-shutdown-tests PRIVATE
        KOBALT_SCREENSAVER_FIXTURE="$<TARGET_FILE:kobalt-screensaver-fixture>")
    add_dependencies(kobalt-shutdown-tests kobalt-screensaver-fixture)
    set_target_properties(kobalt-shutdown-tests PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(kobalt-shutdown-tests PRIVATE KOBALT_CONFIG_DIR="." KOBALT_SYSTEM_FONT_DIRECTORY="")
    target_link_libraries(kobalt-shutdown-tests PRIVATE kobalt-regression-runtime Qt6::Test Qt6::GuiPrivate)
    target_compile_options(kobalt-shutdown-tests PRIVATE -Wall -Wextra -Werror)
    add_dependencies(kobalt-tests kobalt-shutdown-tests)
    add_test(NAME platform.shutdown COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-shutdown-tests>"
        "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(platform.shutdown PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "platform;integration" TIMEOUT 125)
    add_executable(kobalt-sleep-persistence-tests "${KOBALT_ROOT}/tests/sleep_persistence.cpp"
        "${source}/storage/sleep_snapshot.cpp" "${source}/support/main_thread.cpp"
        "${source}/support/diagnostics.cpp")
    target_include_directories(kobalt-sleep-persistence-tests PRIVATE "${source}" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(kobalt-sleep-persistence-tests PRIVATE Qt6::Core Qt6::Gui)
    target_compile_options(kobalt-sleep-persistence-tests PRIVATE -Wall -Wextra -Werror)
    if(APPLE)
        target_compile_definitions(kobalt-sleep-persistence-tests PRIVATE fdatasync=fsync)
    endif()
    add_dependencies(kobalt-tests kobalt-sleep-persistence-tests)
    add_test(NAME platform.sleep-persistence COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-sleep-persistence-tests>"
        "-DTEST_TIMEOUT=30" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(platform.sleep-persistence PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "platform;integration" TIMEOUT 35)
    add_executable(kobalt-scenario-sleep-tests "${KOBALT_ROOT}/tests/scenario_sleep.cpp"
        "${source}/scenario/player.cpp")
    target_include_directories(kobalt-scenario-sleep-tests PRIVATE "${source}")
    target_link_libraries(kobalt-scenario-sleep-tests PRIVATE Qt6::Widgets Qt6::GuiPrivate Qt6::Network)
    target_compile_options(kobalt-scenario-sleep-tests PRIVATE -Wall -Wextra -Werror)
    add_dependencies(kobalt-tests kobalt-scenario-sleep-tests)
    add_test(NAME platform.scenario-sleep COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-scenario-sleep-tests>"
        "-DTEST_TIMEOUT=30" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(platform.scenario-sleep PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "platform;integration" TIMEOUT 35)
    add_executable(kobalt-power-screen-tests "${KOBALT_ROOT}/tests/kobo_power_screen.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/home_sleep_screen.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/window_presentation.cpp"
        "${KOBALT_ROOT}/platforms/kobo/common/native_font_scale.cpp")
    set_target_properties(kobalt-power-screen-tests PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(kobalt-power-screen-tests PRIVATE KOBALT_TEST_ASSET_ROOT="${KOBALT_ASSETS}")
    target_link_libraries(kobalt-power-screen-tests PRIVATE kobalt-desktop kobalt-core Qt6::Test)
    target_compile_options(kobalt-power-screen-tests PRIVATE -Wall -Wextra -Werror)
    add_dependencies(kobalt-tests kobalt-power-screen-tests)
    add_test(NAME platform.kobo-power-screen COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-power-screen-tests>"
        "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
    set_tests_properties(platform.kobo-power-screen PROPERTIES ENVIRONMENT "${headless_environment}"
        LABELS "platform;integration" TIMEOUT 125)
    add_executable(kobalt-reader-ui-tests "${KOBALT_ROOT}/tests/reader_ui.cpp")
    target_link_libraries(kobalt-reader-ui-tests PRIVATE kobalt-desktop kobalt-core Qt6::Test)
    target_compile_options(kobalt-reader-ui-tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kobalt-reader-ui-tests PRIVATE KOBALT_TEST_ASSET_ROOT="${KOBALT_ASSETS}")
    add_dependencies(kobalt-tests kobalt-reader-ui-tests)
    foreach(check widgetKeyboardEntry widgetKeyboardAccents widgetKeyboardLayouts widgetKeyboardCase widgetKeyboardLifetime
            pagedListSwipes pagedSwipesRespectControls pagedFontSwipesKeepHolds
            pageTurnTapFilter pageTurnFilterSeparatesInputClocks holdSourceTime holdTimerDoesNotRepeatOnRelease fontHoldCancellation fontSelectionDoesNotToggle
            holdCallbackCanDeleteReader canvasPreservesTouchTime cancelledCanvasTouchRestoresSlider
            canvasCancellationCanDeleteTarget
            canvasCancellationKeepsReentrantGesture
            canvasReleaseCanDeleteWidgets
            canvasInputRespectsModalBarrier
            connectedModalFooter
            libraryActionButtons
            libraryFolderPicker
            libraryScanCaption
            libraryCanvasFollowsViewport
            booxAppCanvases
            booxLibraryHandoff
            libraryOnboardingLayout
            libraryListFillsPage
            regularIconWeight
            annotationModeNotice
            continuationHintClears
            listPressFeedback
            marksSortingAndAnnotationRows
            annotationDetailsOnlyLoadVisiblePage
            hidingSelectionMenuKeepsDamageCompact
            delayedProgressPaintsAfterTheEventLoop
            progressPaintsOnlyChangedPixels
            openingProgressPaintsAfterTheEventLoop
            openingCoverLayout
            openingFirstPaintCallback
            readingStripChangesLeaveBodyUnpainted
            chapterPageSurvivesLongTitles
            batteryTextAlignment
            annotationPaintPreservesText
            annotationDamageBatchesLogicalUpdate
            annotationFrameDamageJoinsRawDamage
            annotationContourKeepsParagraphGaps annotationHandlesDragAndCancel)
        add_test(NAME ui.${check} COMMAND "${CMAKE_COMMAND}"
            "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-reader-ui-tests>" "-DTEST_ARGUMENTS=${check}"
            "-DTEST_TIMEOUT=120" "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
        set_tests_properties(ui.${check} PROPERTIES ENVIRONMENT "${headless_environment}"
            LABELS "ui;integration" TIMEOUT 125)
    endforeach()
    function(kobalt_regressions group)
        foreach(check IN LISTS ARGN)
            string(REPLACE "_" "-" name "${check}")
            add_executable(kobalt-${name}-tests "${KOBALT_ROOT}/tests/model/${check}.cpp")
            set_target_properties(kobalt-${name}-tests PROPERTIES AUTOMOC OFF)
            target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/tests/support")
            target_compile_options(kobalt-${name}-tests PRIVATE -Wall -Wextra -Werror)
            if(check STREQUAL "native_sync_network")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/native_sync_network.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/native_sync_network.h"
                    "${KOBALT_ROOT}/platforms/kobo/common/window_presentation.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/native_font_scale.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/native_wifi.cpp")
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}" "${source}")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime Qt6::Widgets Qt6::Network)
                set_target_properties(kobalt-${name}-tests PROPERTIES AUTOMOC ON ENABLE_EXPORTS ON)
            elseif(check STREQUAL "native_progress")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/native_progress.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/native_progress.h")
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}" "${source}")
                target_link_libraries(kobalt-${name}-tests PRIVATE Qt6::Core)
                target_compile_definitions(kobalt-${name}-tests PRIVATE KOBALT_READER_HARDWARE)
                set_target_properties(kobalt-${name}-tests PROPERTIES AUTOMOC ON ENABLE_EXPORTS ON)
            elseif(check STREQUAL "native_lifecycle")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/native_lifecycle.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/standby_input.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/startup_recovery.c"
                    "${KOBALT_ROOT}/platforms/kobo/common/screensaver_compatibility.cpp"
                    "${KOBALT_ROOT}/platforms/kobo/common/native_lifecycle.h")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime Qt6::Widgets Qt6::Sql Qt6::GuiPrivate)
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
                target_compile_definitions(kobalt-${name}-tests PRIVATE KOBALT_READER_HARDWARE
                    KOBALT_CONFIG_DIR="/unused" KOBALT_SYSTEM_FONT_DIRECTORY="/unused")
                set_target_properties(kobalt-${name}-tests PROPERTIES AUTOMOC ON ENABLE_EXPORTS ON)
            elseif(check STREQUAL "nickel_book_menu")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/book_menu.cpp")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime Qt6::Widgets Qt6::Sql)
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
                set_target_properties(kobalt-${name}-tests PROPERTIES ENABLE_EXPORTS ON)
            elseif(check STREQUAL "nickel_font_scale")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/native_font_scale.cpp")
                target_link_libraries(kobalt-${name}-tests PRIVATE Qt6::Widgets)
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}" "${source}")
            elseif(check STREQUAL "touch_rotation")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/standby_input.cpp")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime Qt6::GuiPrivate)
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
            elseif(check STREQUAL "standby_input")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/standby_input.cpp")
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime Qt6::GuiPrivate)
            elseif(group STREQUAL "component" OR check STREQUAL "reader_orientation")
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-desktop kobalt-core)
            else()
                target_link_libraries(kobalt-${name}-tests PRIVATE kobalt-regression-runtime)
            endif()
            if(check STREQUAL "android_assets")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/android/assets.cpp")
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
                set(fixture "${CMAKE_CURRENT_BINARY_DIR}/android-asset-fixture")
                file(MAKE_DIRECTORY "${fixture}/hyph")
                set(pattern "fixture patterns\n")
                string(SHA256 hash "${pattern}")
                string(LENGTH "${pattern}" size)
                file(WRITE "${fixture}/hyph/fixture.pattern" "${pattern}")
                file(WRITE "${fixture}/manifest.json" "{\"schema\":1,\"version\":\"${KOBALT_VERSION}\",\"files\":[{\"path\":\"hyph/fixture.pattern\",\"size\":${size},\"sha256\":\"${hash}\"}]}")
                qt_add_resources(kobalt-${name}-tests android_asset_fixture PREFIX "/android-assets" BASE "${fixture}"
                    FILES "${fixture}/manifest.json" "${fixture}/hyph/fixture.pattern")
            endif()
            target_compile_definitions(kobalt-${name}-tests PRIVATE
                KOBALT_TEST_ASSET_ROOT="${KOBALT_ASSETS}" KOBALT_FONT_DIRECTORY="${KOBALT_ASSETS}/fonts"
                KOBALT_INSTALLED_FONTS)
            if(check STREQUAL "vertical_spacing")
                target_compile_definitions(kobalt-${name}-tests PRIVATE KOBALT_TEST_ZIP="${KOBALT_TEST_ZIP}")
            endif()
            if(check STREQUAL "svg_limits")
                target_include_directories(kobalt-${name}-tests SYSTEM PRIVATE "${KOBALT_TEST_DEPENDENCIES}/include/lunasvg")
            endif()
            if(check STREQUAL "touch_rotation")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/qt5/touch_rotation.cpp")
            endif()
            if(check STREQUAL "crash_reports")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/crash_reports.cpp")
                target_include_directories(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}")
                # dladdr names the crashing test function only when it is exported.
                set_target_properties(kobalt-${name}-tests PROPERTIES ENABLE_EXPORTS ON)
            endif()
            if(check STREQUAL "reader")
                target_sources(kobalt-${name}-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/common/engine_positions.cpp")
                target_compile_definitions(kobalt-${name}-tests PRIVATE
                    KOBALT_TEST_VARIABLE_FONT_DIRECTORY="${KOBALT_ROOT}/tests/fixtures")
            endif()
            add_dependencies(kobalt-tests kobalt-${name}-tests)
            set(lane integration)
            if(group STREQUAL "model")
                set(lane fast)
                add_dependencies(kobalt-fast-tests kobalt-${name}-tests)
            endif()
            set(executable "$<TARGET_FILE:kobalt-${name}-tests>")
            set(arguments)
            if(group STREQUAL "session" OR check MATCHES "^(links|search|word_selection|reader|publisher_markup|local_reader|document_word_count|document_cache)$")
                set(arguments "--regression;$<TARGET_FILE:kobalt-${name}-tests>")
                set(executable "$<TARGET_FILE:kobalt-desktop-tests>")
            endif()
            if(check STREQUAL "system_fonts")
                set(arguments "${KOBALT_ASSETS}/fonts/ebook-core/NV_Bitter-Regular.ttf")
            endif()
            if(check STREQUAL "font_sources")
                set(arguments "${test_variable_font};${test_book}")
            endif()
            if(check STREQUAL "nickel_book_menu" OR check STREQUAL "alternate_layout")
                set(arguments "${test_book}")
            endif()
            set(test_timeout 120)
            if(lane STREQUAL "fast")
                set(test_timeout 30)
            endif()
            math(EXPR ctest_timeout "${test_timeout} + 5")
            add_test(NAME ${group}.${name} COMMAND "${CMAKE_COMMAND}"
                "-DTEST_EXECUTABLE=${executable}" "-DTEST_ARGUMENTS=${arguments}"
                "-DTEST_TIMEOUT=${test_timeout}"
                "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake")
            set_tests_properties(${group}.${name} PROPERTIES
                ENVIRONMENT "${headless_environment}" LABELS "${group};${lane}" TIMEOUT ${ctest_timeout})
        endforeach()
    endfunction()
    kobalt_regressions(model storage_assets android_assets profile_storage record_writer main_thread reading_history bookmark_store highlight_store annotation_database appearance_settings header_footer
        layout_cache resource_cache layout_compatibility reading_pace reading_speed engine_activity
        local_resources zip_resource diagnostics crash_reports dictionary_store image_damage page_pixels
        logical_word_counter brightness_gesture qt_compat page_refresh_regions screen_refresh plain_archive reading_progress
        library kosync)
    kobalt_regressions(model font_axes touch_rotation)
    kobalt_regressions(platform reader_orientation font_sources system_fonts nickel_dictionary_selection
        nickel_reading_pace_cache nickel_reading_save nickel_session_calls nickel_book_menu nickel_font_scale native_progress native_sync_network native_lifecycle standby_input usb_removal page_transition_native page_animation)
    kobalt_regressions(engine vertical_spacing source_index columns advanced_text paragraph_spacing beautifier alignment indentation page_layout vertical_writing mathml line_breaking
        fallback_styles svg_limits image_proportions whitespace links search word_selection reader publisher_markup local_reader document_word_count document_cache alternate_layout)
    kobalt_regressions(component ui acknowledgments kosync_ui)
    target_sources(kobalt-page-animation-tests PRIVATE "${KOBALT_ROOT}/platforms/kobo/display/page_animation.cpp")
    target_include_directories(kobalt-page-animation-tests PRIVATE "${KOBALT_ROOT}" "${source}")
    target_link_libraries(kobalt-ui-tests PRIVATE Qt6::GuiPrivate)
    target_link_libraries(kobalt-kosync-ui-tests PRIVATE Qt6::Test)
    kobalt_regressions(session reading_book bookmark_session highlight_session reader_session)
    kobalt_regressions(resource image_policy resource_provider)
    kobalt_regressions(corpus multilingual)
    target_compile_definitions(kobalt-publisher-markup-tests PRIVATE
        KOBALT_TEST_HYPHENATION_CORPUS="${KOBALT_ROOT}/tests/fixtures/hyphenation/ushyphex.tex")
    target_compile_definitions(kobalt-multilingual-tests PRIVATE
        KOBALT_TEST_ZIP="${KOBALT_TEST_ZIP}" KOBALT_TEST_CORPUS="${KOBALT_ROOT}/tests/fixtures/public-domain")
    add_custom_target(screenshots
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${KOBALT_ROOT}/build/screenshot-books"
            "-DREFERENCES=${KOBALT_ROOT}/tests/fixtures/screenshot-books.json"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-screenshot-books.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>"
            "-DTEST_ARGUMENTS=--screenshots;${KOBALT_ROOT}/build/screenshots/${KOBALT_DESKTOP_PLATFORM}"
            "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp" "-DTEST_TIMEOUT=600"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake"
        DEPENDS kobalt-desktop-tests USES_TERMINAL VERBATIM)
    # Timings come from one process at a time; KOBALT_PERF_FILTER and
    # KOBALT_PERF_REPETITIONS narrow or lengthen a run. taskpolicy keeps the
    # process on one kind of core, so repeated runs agree.
    set(perf_executable "$<TARGET_FILE:kobalt-desktop-tests>")
    set(perf_arguments "--perf;${CMAKE_BINARY_DIR}/perf/perf.json")
    if(APPLE)
        find_program(KOBALT_TASKPOLICY taskpolicy REQUIRED)
        set(perf_executable "${KOBALT_TASKPOLICY}")
        list(PREPEND perf_arguments -t 0 -l 0 "$<TARGET_FILE:kobalt-desktop-tests>")
    endif()
    # The large-book scenario opens a pinned Gutenberg edition, fetched once.
    add_custom_target(perf
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${KOBALT_ROOT}/build/screenshot-books"
            "-DREFERENCES=${KOBALT_ROOT}/tests/fixtures/screenshot-books.json" -DNAMES=war-and-peace
            -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-screenshot-books.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=${perf_executable}"
            "-DTEST_ARGUMENTS=${perf_arguments}"
            "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp" "-DTEST_TIMEOUT=600"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake"
        DEPENDS kobalt-desktop-tests USES_TERMINAL VERBATIM)
    add_custom_target(screenshots-light
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>"
            "-DTEST_ARGUMENTS=--screenshots;${KOBALT_ROOT}/build/screenshots/${KOBALT_DESKTOP_PLATFORM}-light;--light"
            "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp" "-DTEST_TIMEOUT=120"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake"
        DEPENDS kobalt-desktop-tests USES_TERMINAL VERBATIM)
    add_custom_target(screenshot-gallery
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:kobalt-desktop-tests>"
            "-DTEST_ARGUMENTS=--gallery;${KOBALT_ROOT}/build/screenshots/${KOBALT_DESKTOP_PLATFORM}"
            "-DTEST_TMP_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-tmp" "-DTEST_TIMEOUT=30"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-test.cmake"
        DEPENDS kobalt-desktop-tests USES_TERMINAL VERBATIM)
endif()
