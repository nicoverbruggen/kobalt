set(KOBALT_ARCHIVE_SOURCE src/publication/plain_archive.cpp)
set(KOBALT_PROVIDER_SOURCE src/platform/nickel/common/plain_publication.cpp)
# Nickel and Kobo Qt5 interfaces for the firmware 4 integration.
# Shared headers with Q_OBJECT are listed because AUTOMOC only pairs a source
# with a header in its own directory.
set(KOBALT_REFRESH_SOURCE src/platform/nickel/qt5/refresh.cpp)
set(KOBALT_ANIMATION_SOURCE src/platform/nickel/display/page_animation.cpp)
set(KOBALT_ORIENTATION_SOURCE src/platform/nickel/qt5/orientation.cpp)
set(KOBALT_NICKEL4_ADAPTER_SOURCES
    ${KOBALT_ARCHIVE_SOURCE} src/publication/resource_server.cpp src/publication/loopback.cpp
    ${KOBALT_PROVIDER_SOURCE} src/platform/nickel/common/publication_source.cpp
    src/platform/nickel/qt5/native_opening.cpp src/platform/nickel/qt5/native_publication.cpp
    src/platform/nickel/qt5/native_progress.cpp src/platform/nickel/common/native_progress.h
    src/platform/nickel/qt5/native_reading_session.cpp
    src/platform/nickel/qt5/native_authorization.cpp src/platform/nickel/common/native_authorization.h
    src/platform/nickel/common/native_lifecycle.cpp src/platform/nickel/qt5/native_lifecycle.cpp src/platform/nickel/common/native_lifecycle.h
    src/platform/nickel/qt5/native_light.cpp src/platform/nickel/common/native_light.h
    src/platform/nickel/qt5/native_keyboard.cpp src/platform/nickel/common/native_keyboard.h)
set(KOBALT_INTEGRATION_SOURCES
    src/platform/nickel/common/book_menu.cpp src/platform/nickel/common/reading_bridge.cpp
    src/platform/nickel/common/reader_coordinator.cpp
    src/platform/nickel/common/reader_host_services.cpp src/platform/nickel/common/window_presentation.cpp
    src/platform/nickel/common/native_bluetooth.cpp
    src/platform/nickel/common/native_battery.cpp
    src/platform/nickel/qt5/hooks.cpp src/platform/nickel/qt5/reader_controller.cpp
    src/platform/nickel/qt5/touch_input.cpp src/platform/nickel/qt5/touch_rotation.cpp
    ${KOBALT_NICKEL4_ADAPTER_SOURCES}
    src/platform/nickel/qt5/native_ui.cpp src/platform/nickel/qt5/native_settings.cpp src/platform/nickel/qt5/native_labels.h
    src/platform/nickel/qt5/native_dictionary.cpp src/platform/nickel/common/native_dictionary.h
    src/publication/native_resources.cpp)
# The lifecycle model check compiles this adapter by itself.
set(KOBALT_LIFECYCLE_SOURCES src/platform/nickel/common/native_lifecycle.cpp src/platform/nickel/qt5/native_lifecycle.cpp src/platform/nickel/common/native_lifecycle.h)
