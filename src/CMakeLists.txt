add_subdirectory(games)

add_executable(${PROJECT_NAME} WIN32
    main.cpp
    app/app_ui.cpp
    app/app_ui.h
    app/localization.cpp
    app/localization.h
    app/app_runtime.cpp
    app/app_runtime.h
    capture/game_window.cpp
    capture/game_window.h
    capture/dxgi_capture.cpp
    capture/dxgi_capture.h
    resources/app.rc
    resources/resource.h
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    games
    gdi32
    user32
    shell32
    gdiplus
    comctl32
    dwmapi
    d3d11
    dxgi
)

# --- MSVC-specific settings ---
if(MSVC)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # Source files contain UTF-8 string literals (including localized Lua
    # errors).  MSVC otherwise uses the active system code page, which can
    # turn non-ASCII bytes into unterminated string literals.  Keep the
    # behavior unchanged while making the compiler interpret sources as UTF-8.
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /utf-8)
endif()

# --- MinGW-w64 specific settings ---
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -municode
        -mwindows
    )
endif()
