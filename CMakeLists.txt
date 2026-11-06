cmake_minimum_required(VERSION 3.24)
project(notita LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NOTITA_BUILD_APP "Build the SDL3 sandbox app (fetches SDL3 + Dear ImGui)" ON)
option(NOTITA_BUILD_TESTS "Build the sim unit tests" ON)
option(NOTITA_DEPS_ONLY "Only configure third-party dependencies (bootstrap)" OFF)

if(NOTITA_DEPS_ONLY)
    include(cmake/Dependencies.cmake)
    include(cmake/Luau.cmake)
    return()
endif()

# ---- Simulation core: no SDL dependency, so tests and the benchmark stay light ----
add_library(notita_sim STATIC
    src/sim/Material.cpp
    src/sim/World.cpp
    src/sim/Simulator.cpp
    src/sim/ThreadPool.cpp
    src/sim/Brush.cpp
    src/sim/Scenes.cpp
    src/sim/Reactions.cpp
    src/sim/GasGrid.cpp
    src/sim/GasSolverCpu.cpp
)
target_include_directories(notita_sim PUBLIC src)
if(MSVC)
    target_compile_options(notita_sim PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(notita_sim PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(notita_sim PRIVATE -Wall -Wextra)
endif()
find_package(Threads REQUIRED)
target_link_libraries(notita_sim PUBLIC Threads::Threads)
target_compile_definitions(notita_sim PUBLIC NOTITA_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# ---- Scripting: Luau runs only at load time (data/reactions.luau -> ReactionTable) ----
include(cmake/Luau.cmake)
add_library(notita_script STATIC src/script/ReactionScript.cpp)
target_link_libraries(notita_script PUBLIC notita_sim PRIVATE Luau.Compiler Luau.VM)
if(MSVC)
    target_compile_options(notita_script PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(notita_bench src/bench/bench_main.cpp)
target_link_libraries(notita_bench PRIVATE notita_sim notita_script)

if(NOTITA_BUILD_TESTS)
    enable_testing()
    add_executable(notita_tests tests/test_main.cpp tests/test_sim.cpp tests/test_heat.cpp tests/test_gas.cpp)
    target_link_libraries(notita_tests PRIVATE notita_sim notita_script)
    add_test(NAME notita_tests COMMAND notita_tests)
endif()

if(NOTITA_BUILD_APP)
    include(cmake/Dependencies.cmake)
    add_executable(notita
        src/app/main.cpp
        src/app/Renderer.cpp
        src/app/App.cpp
    )
    target_link_libraries(notita PRIVATE notita_sim notita_script imgui SDL3::SDL3-static)
    if(MSVC)
        target_compile_options(notita PRIVATE /W4 /permissive- /utf-8)
        set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT notita)
    endif()
endif()
