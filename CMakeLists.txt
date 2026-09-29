cmake_minimum_required(VERSION 3.16)
project(auto_hack_7in1
    VERSION 1.3.0
    DESCRIPTION "Windows C++/Win32 helper for seven GTA Online hacking minigames"
    LANGUAGES C CXX RC
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)
