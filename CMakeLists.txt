cmake_minimum_required(VERSION 3.24)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Oldest macOS supported")
project(openpoptb VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)

# Recompiled port: x86 code translated to C (gen*/) + Win32/DirectX runtime on top of SDL3.
# One target per game executable: Populous: The Beginning and the Undiscovered Worlds expansion.
# POPTB_PREBUILT: the downloadable app. It contains only the runtime; the game code is generated and
# compiled on the player's Mac on first launch (runtime/prepare.c) and loaded as a bundle.
option(POPTB_PREBUILT "Build the runtime-only app that recompiles the player's copy on first launch" OFF)
option(POPTB_GEN_DEBUG "Build the generated code without optimization (exact lines in the debugger)" OFF)
file(GLOB RT_SRC CONFIGURE_DEPENDS runtime/*.c)
# Cached game bundles are rebuilt when anything that shapes the generated code or its ABI changes
set(POPTB_GEN_ABI "${PROJECT_VERSION}")
# (prepare.c: compile flags; package_release.sh: capstone/pefile versions; functions.tsv: seeds)
foreach(f recomp/gen.py recomp/image.py re/export/functions.tsv runtime/cpu.h runtime/rt_ops.h
          runtime/table.h runtime/prepare.c tools/package_release.sh)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${f})
  file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/${f} h)
  string(SUBSTRING ${h} 0 8 h)
  string(APPEND POPTB_GEN_ABI "-${h}")
endforeach()
function(poptb_target name gendir exe outname bundle)
  if(POPTB_PREBUILT)
    set(GEN_SRC "")
  else()
    file(GLOB GEN_SRC CONFIGURE_DEPENDS ${gendir}/*.c)
    if(NOT GEN_SRC)
      return()
    endif()
  endif()
  add_executable(${name} MACOSX_BUNDLE ${RT_SRC} ${GEN_SRC})
  target_include_directories(${name} PRIVATE runtime ${gendir})
  target_link_libraries(${name} PRIVATE SDL3::SDL3 m)
  target_compile_definitions(${name} PRIVATE POPTB_EXE="${exe}")
  if(POPTB_PREBUILT)
    target_compile_definitions(${name} PRIVATE POPTB_PREBUILT POPTB_GEN_ABI="${POPTB_GEN_ABI}")
  endif()
  target_compile_options(${name} PRIVATE -fno-strict-aliasing -fwrapv -Wno-parentheses -Wno-unused-value)
  if(POPTB_GEN_DEBUG)
    set_source_files_properties(${GEN_SRC} TARGET_DIRECTORY ${name} PROPERTIES COMPILE_OPTIONS "-O0;-g;-w")
  else()
    set_source_files_properties(${GEN_SRC} TARGET_DIRECTORY ${name} PROPERTIES COMPILE_OPTIONS "-O1;-w")
  endif()
  set_target_properties(${name} PROPERTIES
      OUTPUT_NAME "${outname}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "local.openpoptb.${name}"
      MACOSX_BUNDLE_BUNDLE_NAME "${bundle}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endfunction()
poptb_target(poptb gen popTB.exe PopulousTB "Populous TB")
poptb_target(poptbuw gen_uw popTBUW.exe PopulousUW "Populous UW")
poptb_target(poptbd3d gen_d3d D3DPopTB.exe PopulousTBD3D "Populous TB (Direct3D)")

# Hand-reimplemented terrain viewer (first approach; kept for reference)
add_executable(openpoptb-viewer
    src/main.c src/fs.c src/assets.c src/level.c src/render.c src/terrain.c)
target_include_directories(openpoptb-viewer PRIVATE src)
target_link_libraries(openpoptb-viewer PRIVATE SDL3::SDL3 m)
target_compile_options(openpoptb-viewer PRIVATE -Wall -Wextra -Wno-unused-parameter)
