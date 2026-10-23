add_executable(DesktopBall WIN32 main.cpp "${PROJECT_SOURCE_DIR}/resources/desktopball.rc")
target_link_libraries(DesktopBall PRIVATE desktopball_core)
target_link_options(DesktopBall PRIVATE /MANIFEST:NO)
set_target_properties(DesktopBall PROPERTIES OUTPUT_NAME TaiJiAura RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
