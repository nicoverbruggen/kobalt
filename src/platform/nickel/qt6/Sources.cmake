set(KOBALT_ARCHIVE_SOURCE src/publication/plain_archive.cpp)
set(KOBALT_PROVIDER_SOURCE src/platform/nickel/common/plain_publication.cpp)
set(KOBALT_PUBLICATION_SOURCES src/platform/nickel/qt6/native_publication.cpp src/platform/nickel/qt6/native_opening.cpp)
# Nickel and patched Qt interfaces for the firmware 5 and 6 integration.
set(KOBALT_REFRESH_SOURCE src/platform/nickel/qt6/refresh.cpp)
set(KOBALT_ANIMATION_SOURCE src/platform/nickel/display/page_animation.cpp)
set(KOBALT_ORIENTATION_SOURCE src/platform/nickel/qt6/orientation.cpp)
set(KOBALT_INTEGRATION_SOURCES
    src/platform/nickel/common/book_menu.cpp src/platform/nickel/common/reading_bridge.cpp
    src/platform/nickel/common/reader_coordinator.cpp
    src/platform/nickel/common/reader_host_services.cpp src/platform/nickel/common/window_presentation.cpp
    src/platform/nickel/common/native_bluetooth.cpp
    src/platform/nickel/common/native_battery.cpp
    src/platform/nickel/qt6/hooks.cpp src/platform/nickel/qt6/reader_controller.cpp
    src/platform/nickel/qt6/native_keyboard.cpp src/platform/nickel/common/native_keyboard.h src/platform/nickel/qt6/native_progress.cpp src/platform/nickel/common/native_progress.h src/platform/nickel/qt6/native_reading_session.cpp src/platform/nickel/qt6/native_authorization.cpp src/platform/nickel/common/native_authorization.h
    src/platform/nickel/common/native_lifecycle.cpp src/platform/nickel/qt6/native_lifecycle.cpp src/platform/nickel/common/native_lifecycle.h src/platform/nickel/qt6/native_dictionary.cpp src/platform/nickel/common/native_dictionary.h src/platform/nickel/qt6/native_light.cpp src/platform/nickel/common/native_light.h
    ${KOBALT_ARCHIVE_SOURCE} src/publication/resource_server.cpp src/publication/loopback.cpp
    ${KOBALT_PROVIDER_SOURCE} src/platform/nickel/common/publication_source.cpp
    src/platform/nickel/qt6/native_ui.cpp ${KOBALT_PUBLICATION_SOURCES} src/publication/native_resources.cpp)
# Model checks compile these adapters by themselves.
set(KOBALT_AUTHORIZATION_SOURCES src/platform/nickel/qt6/native_authorization.cpp src/platform/nickel/common/native_authorization.h)
set(KOBALT_LIFECYCLE_SOURCES src/platform/nickel/common/native_lifecycle.cpp src/platform/nickel/qt6/native_lifecycle.cpp src/platform/nickel/common/native_lifecycle.h)
