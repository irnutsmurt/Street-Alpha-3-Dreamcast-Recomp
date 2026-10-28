cmake_minimum_required(VERSION 3.20)
project(sfa3 C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(Vulkan REQUIRED)

# SFA3_GEN: another copy of the recompiled code (tools/read_trace.py makes an
# instrumented one); SFA3_READ_TRACE: log which instructions read game data
set(SFA3_GEN "" CACHE PATH "recompiled code directory (default build/generated)")
option(SFA3_READ_TRACE "debug: record the game-data addresses each instruction reads" OFF)
if(SFA3_GEN)
  set(GEN ${SFA3_GEN})
else()
  set(GEN ${CMAKE_SOURCE_DIR}/build/generated)
endif()
if(SFA3_READ_TRACE)
  add_compile_definitions(SFA3_READ_TRACE)
endif()
if(NOT EXISTS ${GEN}/sources.txt)
  message(FATAL_ERROR "No recompiled code found. Run ./sfa3port.py setup <your disc.gdi> first.")
endif()
file(STRINGS ${GEN}/sources.txt GEN_FILES)
list(TRANSFORM GEN_FILES PREPEND ${GEN}/)
# PSP characters (tools/psp/build_char.py), when built
file(GLOB PSP_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/build/psp_out/psp_*.c)
if(PSP_FILES)
  list(APPEND GEN_FILES ${PSP_FILES})
else()
  list(APPEND GEN_FILES ${CMAKE_SOURCE_DIR}/runtime/psp_none.c)
endif()

# Recompiled game code: large, machine-generated; optimise but skip warnings.
add_library(sfa3_game STATIC ${GEN_FILES})
target_include_directories(sfa3_game PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${GEN})
target_compile_options(sfa3_game PRIVATE -O2 -w -fno-strict-aliasing -fwrapv)

set(RENDERER "vulkan" CACHE STRING "Renderer backend: vulkan or null")
set(RUNTIME_SRC
  runtime/main.c runtime/sh4.c runtime/irq.c runtime/mmio.c runtime/pvr.c
  runtime/ta.c runtime/widescreen.c runtime/wide_filler.c runtime/hle.c runtime/gdi.c runtime/maple.c runtime/aica.c
  runtime/overlay.c runtime/input_sdl.c runtime/task.c
  runtime/fiber.c runtime/sjlj.c runtime/arm_rt.c runtime/settings.c runtime/menu.c runtime/controls.c runtime/options.c runtime/display_menu.c runtime/buttoncfg.c runtime/pause_menu.c runtime/title_quit.c runtime/net.c runtime/netplay.c
  runtime/gameui.c runtime/lobby.c runtime/sprite_swap.c runtime/cheats.c runtime/option_menu.c runtime/psp.c runtime/psp_load.c runtime/read_trace.c runtime/chartables.c runtime/sprite_log.c)
if(RENDERER STREQUAL "null")
  list(APPEND RUNTIME_SRC runtime/renderer_null.c)
else()
  list(APPEND RUNTIME_SRC runtime/vk_renderer.c runtime/pvr_tex.c)
endif()

find_program(GLSLC glslc REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(s pvr.vert pvr.frag blit.vert blit.frag ext.vert ext.frag)
  add_custom_command(
    OUTPUT ${SHADER_OUT}/${s}.inc
    COMMAND ${GLSLC} -O -mfmt=c ${CMAKE_SOURCE_DIR}/runtime/shaders/${s} -o ${SHADER_OUT}/${s}.inc
    DEPENDS ${CMAKE_SOURCE_DIR}/runtime/shaders/${s})
  list(APPEND SHADER_INCS ${SHADER_OUT}/${s}.inc)
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_INCS})

# The port's own images (assets/*.png) as C arrays
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ASSET_OUT ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${ASSET_OUT})
set(ASSET_INCS)
foreach(a vmu_card vmu_empty ps_cross ps_circle ps_square ps_triangle ps_l1 ps_r1 ps_l2 ps_r2 xb_a xb_b xb_x xb_y xb_lb xb_rb xb_lt xb_rt)
  add_custom_command(
    OUTPUT ${ASSET_OUT}/${a}.inc
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/png2inc.py ${CMAKE_SOURCE_DIR}/assets/${a}.png ${ASSET_OUT}/${a}.inc ${a}
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/${a}.png ${CMAKE_SOURCE_DIR}/tools/png2inc.py)
  list(APPEND ASSET_INCS ${ASSET_OUT}/${a}.inc)
endforeach()
add_custom_target(assets DEPENDS ${ASSET_INCS})

add_executable(sfa3 ${RUNTIME_SRC})
add_dependencies(sfa3 shaders assets)
target_include_directories(sfa3 PRIVATE runtime ${GEN} ${SHADER_OUT} ${ASSET_OUT})
target_compile_options(sfa3 PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -fwrapv)
target_link_libraries(sfa3 PRIVATE sfa3_game PkgConfig::SDL3 Vulkan::Vulkan m)
target_link_options(sfa3 PRIVATE -rdynamic)
