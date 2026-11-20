cmake_minimum_required(VERSION 3.24)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Oldest macOS supported")
project(openpoptb VERSION 0.2.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(EMSCRIPTEN)
  # WebAssembly build (emcmake cmake ...): for now headless under Node.js, with direct access to the
  # host file system (docs/wasm/PLAN.md, M1). SDL3 comes from the Emscripten port (audio only).
  set(POPTB_HEADLESS ON CACHE BOOL "" FORCE)
  set(POPTB_SDL3 "--use-port=sdl3")
  add_compile_options(-pthread ${POPTB_SDL3})
else()
  find_package(SDL3 REQUIRED CONFIG)
endif()

# Recompiled port: x86 code translated to C (gen*/) + Win32/DirectX runtime on top of SDL3.
# One target per game executable: Populous: The Beginning and the Undiscovered Worlds expansion.
# POPTB_PREBUILT: the downloadable app. It contains only the runtime; the game code is generated and
# compiled on the player's Mac on first launch (runtime/prepare.c) and loaded as a bundle.
option(POPTB_PREBUILT "Build the runtime-only app that recompiles the player's copy on first launch" OFF)
option(POPTB_GEN_DEBUG "Build the generated code without optimization (exact lines in the debugger)" OFF)
# Compact 512 MB guest address space, as used by the WebAssembly build (runtime/rt.h). Natively, any
# guest access outside the window faults, which makes this the bounds check for the web layout.
option(POPTB_GUEST_WINDOW "Use the compact WebAssembly guest memory layout" OFF)
option(POPTB_HEADLESS "No window: scripted input and screenshots only (runtime/plat_headless.c)" OFF)
file(GLOB RT_SRC CONFIGURE_DEPENDS runtime/*.c)
# one platform layer: SDL3 window (plat.c) or headless (plat_headless.c)
if(POPTB_HEADLESS)
  list(REMOVE_ITEM RT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/plat.c)
else()
  list(REMOVE_ITEM RT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/plat_headless.c)
endif()
if(EMSCRIPTEN)
  # macOS-only: folder dialog, installer unpacking, first-launch recompile
  list(REMOVE_ITEM RT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/prepare.c)
endif()
# Cached game bundles are rebuilt when anything that shapes the generated code or its ABI changes
set(POPTB_GEN_ABI "${PROJECT_VERSION}")
# (prepare.c: compile flags; functions.tsv: seeds)
foreach(f recomp/gen.py recomp/image.py re/export/functions.tsv runtime/cpu.h runtime/rt_ops.h
          runtime/table.h runtime/prepare.c)
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
  if(EMSCRIPTEN)
    target_link_options(${name} PRIVATE -pthread ${POPTB_SDL3}
        -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=16 -sEXIT_RUNTIME
        -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=768MB -sMAXIMUM_MEMORY=2GB
        -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB
        -sNODERAWFS -sENVIRONMENT=node)
    target_link_libraries(${name} PRIVATE m)
  else()
    target_link_libraries(${name} PRIVATE SDL3::SDL3 m)
  endif()
  target_compile_definitions(${name} PRIVATE POPTB_EXE="${exe}")
  if(POPTB_GUEST_WINDOW)
    target_compile_definitions(${name} PRIVATE POPTB_GUEST_WINDOW)
  endif()
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

