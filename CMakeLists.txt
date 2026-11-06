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
    include(cmake/Box2D.cmake)
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
    src/sim/Showcase.cpp
    src/sim/Reactions.cpp
    src/sim/GasGrid.cpp
    src/sim/GasSolverCpu.cpp
    src/sim/Particles.cpp
    src/sim/Bodies.cpp
    src/sim/Player.cpp
    src/sim/ChunkCodec.cpp
    src/sim/WorldGen.cpp
    src/sim/Streamer.cpp
    src/sim/Electric.cpp
)
target_include_directories(notita_sim PUBLIC src)
# Box2D v3 for the rigid bodies (src/sim/Bodies.*); still no SDL in the sim
include(cmake/Box2D.cmake)
target_link_libraries(notita_sim PRIVATE box2d)
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

# ---- Lighting (phase 6): CPU reference of the GPU light pass (src/app/shaders/light.hlsl); no SDL ----
add_library(notita_light STATIC src/light/Lighting.cpp)
target_link_libraries(notita_light PUBLIC notita_sim)
if(MSVC)
    target_compile_options(notita_light PRIVATE /W4 /permissive- /utf-8)
endif()

if(NOTITA_BUILD_TESTS)
    enable_testing()
    add_executable(notita_tests tests/test_main.cpp tests/test_sim.cpp tests/test_heat.cpp tests/test_gas.cpp tests/test_phase4.cpp tests/test_player.cpp tests/test_world.cpp tests/test_electric.cpp tests/test_showcase.cpp tests/test_flames.cpp)
    target_link_libraries(notita_tests PRIVATE notita_sim notita_script)
    add_test(NAME notita_tests COMMAND notita_tests)
    target_sources(notita_tests PRIVATE tests/test_light.cpp)
    target_link_libraries(notita_tests PRIVATE notita_light)
endif()

if(NOTITA_BUILD_APP)
    include(cmake/Dependencies.cmake)
    add_executable(notita
        src/app/main.cpp
        src/app/Renderer.cpp
        src/app/App.cpp
        src/app/GpuGasSolver.cpp
    )
    target_link_libraries(notita PRIVATE notita_sim notita_script imgui SDL3::SDL3-static)
    # phase 6 lighting: GPU pass + the App's light inputs / UI / self-test
    target_sources(notita PRIVATE src/app/GpuLight.cpp src/app/AppLight.cpp)
    # #477: the liquid surface pass (GpuLiquid + its App side)
    target_sources(notita PRIVATE src/app/GpuLiquid.cpp src/app/AppLiquid.cpp)
    # #480: the flame pass (GpuFlame + its App side)
    target_sources(notita PRIVATE src/app/GpuFlame.cpp src/app/AppFlame.cpp)
    target_link_libraries(notita PRIVATE notita_light)
    # version info: the exe stays notita.exe but describes itself as Fine Grain
    if(WIN32)
        target_sources(notita PRIVATE src/app/notita.rc)
    endif()

    # Gas grid compute kernels -> <build>/shaders/gas_<kernel>.dxil (next to the exe).
    # dxc ships with the Windows SDK; without it the app falls back to the CPU solver.
    find_program(NOTITA_DXC dxc HINTS "$ENV{WindowsSdkVerBinPath}x64")
    set(NOTITA_GAS_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shaders/gas.hlsl)
    if(NOTITA_DXC)
        set(_gas_outputs)
        foreach(kernel curl forces advect divergence jacobi project scale scalars)
            string(TOUPPER ${kernel} _def)
            set(_out ${CMAKE_BINARY_DIR}/shaders/gas_${kernel}.dxil)
            add_custom_command(OUTPUT ${_out}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
                COMMAND ${NOTITA_DXC} -nologo -T cs_6_0 -E main -HV 2021 -D K_${_def} -Fo ${_out} ${NOTITA_GAS_HLSL}
                DEPENDS ${NOTITA_GAS_HLSL}
                COMMENT "dxc gas_${kernel}"
                VERBATIM)
            list(APPEND _gas_outputs ${_out})
        endforeach()
        # view lighting kernels -> <build>/shaders/light_<kernel>.dxil (without them: CPU lighting)
        set(NOTITA_LIGHT_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shaders/light.hlsl)
        foreach(kernel cells mip sky trace tent blur composite)
            string(TOUPPER ${kernel} _def)
            set(_out ${CMAKE_BINARY_DIR}/shaders/light_${kernel}.dxil)
            add_custom_command(OUTPUT ${_out}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
                COMMAND ${NOTITA_DXC} -nologo -T cs_6_0 -E main -HV 2021 -D K_${_def} -Fo ${_out} ${NOTITA_LIGHT_HLSL}
                DEPENDS ${NOTITA_LIGHT_HLSL}
                COMMENT "dxc light_${kernel}"
                VERBATIM)
            list(APPEND _gas_outputs ${_out})
        endforeach()
        # liquid surface pass -> <build>/shaders/liquid.dxil (without it: liquids are drawn as pixels)
        set(NOTITA_LIQUID_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shaders/liquid.hlsl)
        set(_out ${CMAKE_BINARY_DIR}/shaders/liquid.dxil)
        add_custom_command(OUTPUT ${_out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
            COMMAND ${NOTITA_DXC} -nologo -T cs_6_0 -E main -HV 2021 -Fo ${_out} ${NOTITA_LIQUID_HLSL}
            DEPENDS ${NOTITA_LIQUID_HLSL}
            COMMENT "dxc liquid"
            VERBATIM)
        list(APPEND _gas_outputs ${_out})
        # flame pass -> <build>/shaders/flame.dxil (without it: flames at sim resolution on the CPU)
        set(NOTITA_FLAME_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shaders/flame.hlsl)
        set(_out ${CMAKE_BINARY_DIR}/shaders/flame.dxil)
        add_custom_command(OUTPUT ${_out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
            COMMAND ${NOTITA_DXC} -nologo -T cs_6_0 -E main -HV 2021 -Fo ${_out} ${NOTITA_FLAME_HLSL}
            DEPENDS ${NOTITA_FLAME_HLSL}
            COMMENT "dxc flame"
            VERBATIM)
        list(APPEND _gas_outputs ${_out})
        add_custom_target(notita_shaders ALL DEPENDS ${_gas_outputs})
        add_dependencies(notita notita_shaders)
    else()
        message(WARNING "dxc not found: the gas grid will run on the CPU solver only")
    endif()
    if(MSVC)
        target_compile_options(notita PRIVATE /W4 /permissive- /utf-8)
        set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT notita)
    endif()
endif()
