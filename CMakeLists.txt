cmake_minimum_required(VERSION 3.16)
project(cpp_canvas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CPP_CANVAS_BUILD_DEMO "Build the demo program" ON)
option(CPP_CANVAS_BUILD_TESTS "Build the tests and the benchmark (native only)" ON)
option(CPP_CANVAS_SKIA "Browser build: draw with Skia in WebGL instead of the page's Canvas2D" OFF)
set(CPP_CANVAS_SKIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/skia-wasm" CACHE PATH
    "Prebuilt Skia static libraries for wasm32-emscripten (rust-skia 0.153.3 binaries)")

if(CPP_CANVAS_SKIA AND NOT EMSCRIPTEN)
  message(FATAL_ERROR "CPP_CANVAS_SKIA is for the browser build: configure with emcmake")
endif()

# --- the library -------------------------------------------------------------
# canvas.cpp (or canvas_skia.cpp), font.cpp, svg.cpp and window.cpp are the
# whole library; everything under detail/ is private and #included by them.
if(CPP_CANVAS_SKIA)
  set(CPP_CANVAS_BACKEND canvas_skia.cpp)
else()
  set(CPP_CANVAS_BACKEND canvas.cpp)
endif()
add_library(cpp_canvas STATIC ${CPP_CANVAS_BACKEND} font.cpp svg.cpp window.cpp touch_gesture.cpp)
target_include_directories(cpp_canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(EMSCRIPTEN)
  if(CPP_CANVAS_SKIA)
    set(SKIA_TAG m153-0.101.2)
    set(SKIA_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/skia-${SKIA_TAG})
    if(NOT EXISTS "${CPP_CANVAS_SKIA_DIR}/libskia.wasm.a")
      message(FATAL_ERROR "CPP_CANVAS_SKIA needs the prebuilt Skia: no libskia.wasm.a in ${CPP_CANVAS_SKIA_DIR}")
    endif()
    if(NOT EXISTS "${SKIA_HEADERS}/include/core/SkCanvas.h")
      set(archive ${CMAKE_CURRENT_BINARY_DIR}/skia-${SKIA_TAG}.tar.gz)
      message(STATUS "cpp_canvas: fetching the Skia ${SKIA_TAG} headers (once)")
      file(DOWNLOAD "https://codeload.github.com/rust-skia/skia/tar.gz/${SKIA_TAG}" "${archive}" TLS_VERIFY ON STATUS status)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not fetch the Skia headers: ${status}")
      endif()
      file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
           PATTERNS "skia-${SKIA_TAG}/include" "skia-${SKIA_TAG}/modules/skcms")
      file(REMOVE "${archive}")
    endif()
    target_compile_definitions(cpp_canvas PUBLIC CPP_CANVAS_SKIA)
    target_include_directories(cpp_canvas PRIVATE ${SKIA_HEADERS})
    target_compile_definitions(cpp_canvas PRIVATE SK_RELEASE SK_GANESH SK_GL SK_ASSUME_WEBGL=1 SKVX_DISABLE_SIMD
                               SK_FORCE_8_BYTE_ALIGNMENT SK_DISABLE_TRACING SK_CODEC_DECODES_BMP SK_CODEC_DECODES_WBMP)
    target_compile_options(cpp_canvas PRIVATE -msimd128 -fno-exceptions)
    target_link_libraries(cpp_canvas PUBLIC
      ${CPP_CANVAS_SKIA_DIR}/libskia.wasm.a ${CPP_CANVAS_SKIA_DIR}/libfreetype2.wasm.a ${CPP_CANVAS_SKIA_DIR}/libpng.wasm.a
      ${CPP_CANVAS_SKIA_DIR}/libjpeg.wasm.a ${CPP_CANVAS_SKIA_DIR}/libwebp.wasm.a ${CPP_CANVAS_SKIA_DIR}/libwuffs.wasm.a
      ${CPP_CANVAS_SKIA_DIR}/libskcms.wasm.a ${CPP_CANVAS_SKIA_DIR}/libexpat.wasm.a ${CPP_CANVAS_SKIA_DIR}/libzlib.wasm.a)
    target_link_options(cpp_canvas PUBLIC "-sMAX_WEBGL_VERSION=2" "-sSUPPORT_LONGJMP=wasm" "-fno-exceptions")
  else()
    # Element canvases are drawn with WebGL2 (detail/webgpu.h).
    target_link_options(cpp_canvas PUBLIC "-sMAX_WEBGL_VERSION=2")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  find_package(Threads REQUIRED)
  # SDL for Window; threads for the rasterizer's band pool.
  target_link_libraries(cpp_canvas PUBLIC PkgConfig::SDL2 Threads::Threads)
endif()

# --- the demo ----------------------------------------------------------------
if(CPP_CANVAS_BUILD_DEMO)
  add_executable(canvas_demo main.cpp)
  target_link_libraries(canvas_demo PRIVATE cpp_canvas)
  if(EMSCRIPTEN)
    target_link_options(canvas_demo PRIVATE "-sALLOW_MEMORY_GROWTH=1" "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
    set_target_properties(canvas_demo PROPERTIES SUFFIX ".html"
                          LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
  endif()
endif()

# --- tests and benchmark (native) ----------------------------------------------
if(CPP_CANVAS_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  add_executable(canvas_tests tests/canvas_tests.cpp tests/scenes.cpp)
  target_link_libraries(canvas_tests PRIVATE cpp_canvas)
  add_test(NAME canvas_tests COMMAND canvas_tests)
  add_executable(canvas_bench tests/bench.cpp)
  target_link_libraries(canvas_bench PRIVATE cpp_canvas)
endif()
