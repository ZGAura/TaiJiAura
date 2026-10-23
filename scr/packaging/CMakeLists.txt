add_executable(DesktopBallUninstall WIN32 uninstall.cpp)
target_link_libraries(DesktopBallUninstall PRIVATE desktopball_options shell32 advapi32)
set_target_properties(DesktopBallUninstall PROPERTIES
    OUTPUT_NAME uninstall
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
install(TARGETS DesktopBallUninstall RUNTIME DESTINATION .)
