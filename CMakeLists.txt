cmake_minimum_required(VERSION 3.24)
project(light_app_ambient LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The ambient engine currently requires Windows Desktop Duplication.")
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(ambient
    ambient_src/main.cpp
    ambient_src/screen_capture.cpp
    ambient_src/color_processor.cpp
    ambient_src/websocket_server.cpp
    ambient_src/udp_sender.cpp
)
target_compile_features(ambient PRIVATE cxx_std_20)
target_compile_definitions(ambient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ambient PRIVATE d3d11 dxgi dxguid bcrypt ws2_32 nlohmann_json::nlohmann_json)
set_target_properties(ambient PROPERTIES OUTPUT_NAME "ambient")

add_custom_command(TARGET ambient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/ambient_config.json"
        "$<TARGET_FILE_DIR:ambient>/ambient_config.json"
)