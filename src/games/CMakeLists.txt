file(GLOB DOOMSDAY_LUA_SOURCES
    doomsday_unlock_module/third_party/lua-5.3.6/src/*.c
)
list(FILTER DOOMSDAY_LUA_SOURCES EXCLUDE REGEX "lua\\.c$|luac\\.c$|onelua\\.c$")
file(GLOB DOOMSDAY_PRESETS CONFIGURE_DEPENDS doomsday_unlock_module/presets/*.lua)
file(GLOB DOOMSDAY_MAPS CONFIGURE_DEPENDS doomsday_unlock_module/maps/*/layout.json)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(DOOMSDAY_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/doomsday_embedded.h)
add_custom_command(OUTPUT ${DOOMSDAY_EMBEDDED}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_doomsday.py
            ${CMAKE_CURRENT_SOURCE_DIR}/doomsday_unlock_module ${DOOMSDAY_EMBEDDED}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/embed_doomsday.py
            ${CMAKE_CURRENT_SOURCE_DIR}/doomsday_unlock_module/lua_runtime.js
            ${DOOMSDAY_PRESETS} ${DOOMSDAY_MAPS})
add_custom_target(doomsday_embedded_resources DEPENDS ${DOOMSDAY_EMBEDDED})

add_library(games STATIC
    ${DOOMSDAY_EMBEDDED}
    ../input/key_input.cpp
    ../input/key_input.h
    ../input/held_key_input.cpp
    ../input/held_key_input.h
    ../input/mouse_input.cpp
    ../input/mouse_input.h
    slider_module.cpp
    flashing_module.cpp
    choose_fingerprint_module.cpp
    sort_fingerprint_module.cpp
    fleeca_module.cpp
    find_number_module.cpp
    match_module.cpp
    doomsday_unlock_module/doomsday_unlock_module.cpp
    doomsday_unlock_module/doomsday_unlock_module.h
    doomsday_unlock_module/imgproc/imgproc.cpp
    doomsday_unlock_module/imgproc/imgproc.h
    doomsday_unlock_module/vision/vision.cpp
    doomsday_unlock_module/vision/vision.h
    doomsday_unlock_module/tracking/tracking.cpp
    doomsday_unlock_module/tracking/tracking.h
    doomsday_unlock_module/json/json.cpp
    doomsday_unlock_module/json/json.h
    doomsday_unlock_module/overlay/overlay.h
    doomsday_unlock_module/preset/level_matcher.cpp
    doomsday_unlock_module/preset/level_matcher.h
    doomsday_unlock_module/preset/motion.cpp
    doomsday_unlock_module/preset/motion.h
    doomsday_unlock_module/preset/preset_host.cpp
    doomsday_unlock_module/preset/preset_host.h
    ${DOOMSDAY_LUA_SOURCES}
    games.h
)
add_dependencies(games doomsday_embedded_resources)

target_include_directories(games PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/doomsday_unlock_module
    ${CMAKE_CURRENT_SOURCE_DIR}/doomsday_unlock_module/third_party/lua-5.3.6/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(games PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(games PUBLIC
    gdi32
    user32
    gdiplus
    dwmapi
)

# --- MSVC-specific settings ---
if(MSVC)
    set_property(TARGET games PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # The module sources contain UTF-8 localized string literals.  Explicitly
    # select UTF-8 so MSVC does not decode them with the host code page.
    target_compile_options(games PRIVATE /EHsc /utf-8)
endif()
