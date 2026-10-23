add_executable(desktopball_shader_compiler "${PROJECT_SOURCE_DIR}/tools/compile_shaders.cpp")
target_link_libraries(desktopball_shader_compiler PRIVATE desktopball_options d3dcompiler)
set(shader_header "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_bytecode.hpp")
add_custom_command(OUTPUT "${shader_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND desktopball_shader_compiler "${PROJECT_SOURCE_DIR}/shaders/capsule.hlsl" "${shader_header}"
    DEPENDS desktopball_shader_compiler "${PROJECT_SOURCE_DIR}/shaders/capsule.hlsl"
    VERBATIM)
add_library(desktopball_core STATIC
    "${shader_header}"
    monitoring/accelerator_sampler.cpp
    monitoring/gpu_identity.cpp
    monitoring/system_sampler.cpp
    monitoring/thermal_zone_sampler.cpp
    monitoring/rate_counter.cpp
    rendering/renderer.cpp
    window/overlay.cpp
    window/screens.cpp
    window/language.cpp
    window/single_instance.cpp
    window/startup.cpp
    window/popup_menu.cpp)
target_include_directories(desktopball_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(desktopball_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(desktopball_core PRIVATE "${PROJECT_SOURCE_DIR}/resources")
target_link_libraries(desktopball_core PUBLIC desktopball_options
    PRIVATE pdh iphlpapi d3d11 dxgi dcomp d2d1 dwrite shell32 wtsapi32 dwmapi advapi32 gdi32)

