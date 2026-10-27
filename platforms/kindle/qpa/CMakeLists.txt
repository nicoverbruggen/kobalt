# Qt private headers are version-specific. Build this plugin with the bundled SDK.
add_library(qkindle MODULE kindle.cpp refresh_queue.cpp refresh_queue.h mtk_display.cpp mtk_display.h)
set_target_properties(qkindle PROPERTIES AUTOMOC ON LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/platforms")
target_compile_features(qkindle PRIVATE cxx_std_17)
target_link_libraries(qkindle PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Threads::Threads ZLIB::ZLIB)
target_link_options(qkindle PRIVATE LINKER:--no-undefined)
install(TARGETS qkindle LIBRARY DESTINATION plugins/platforms)
