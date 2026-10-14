cmake_minimum_required(VERSION 3.24)
project(openpoptb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)

# Recompiled port: x86 code translated to C (gen*/) + Win32/DirectX runtime on top of SDL3.
# One target per game executable: Populous: The Beginning and the Undiscovered Worlds expansion.
option(POPTB_GEN_DEBUG "Build the generated code without optimization (exact lines in the debugger)" OFF)
file(GLOB RT_SRC CONFIGURE_DEPENDS runtime/*.c)
function(poptb_target name gendir exe outname bundle)
  file(GLOB GEN_SRC CONFIGURE_DEPENDS ${gendir}/*.c)
  if(NOT GEN_SRC)
    return()
  endif()
  add_executable(${name} MACOSX_BUNDLE ${RT_SRC} ${GEN_SRC})
  target_include_directories(${name} PRIVATE runtime ${gendir})
  target_link_libraries(${name} PRIVATE SDL3::SDL3 m)
  target_compile_definitions(${name} PRIVATE POPTB_EXE="${exe}")
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
      MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
endfunction()
poptb_target(poptb gen popTB.exe PopulousTB "Populous TB")
poptb_target(poptbuw gen_uw popTBUW.exe PopulousUW "Populous UW")

# Hand-reimplemented terrain viewer (first approach; kept for reference)
add_executable(openpoptb-viewer
    src/main.c src/fs.c src/assets.c src/level.c src/render.c src/terrain.c)
target_include_directories(openpoptb-viewer PRIVATE src)
target_link_libraries(openpoptb-viewer PRIVATE SDL3::SDL3 m)
target_compile_options(openpoptb-viewer PRIVATE -Wall -Wextra -Wno-unused-parameter)
