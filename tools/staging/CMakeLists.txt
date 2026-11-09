# A private build for work that is not in the engine yet.
#
# Someone writing a new plugin keeps its files in a staging folder of their own
# (staging\<name>\pulp\*.h for headers, staging\<name>\*.cpp for sources and tests) and builds them
# here together with the engine as it stands. Nothing they write is seen by the main build or by
# anyone else's staging build until it is moved into engine\ and tests\.
#
#   cmake -S tools\staging -B build\staging_<name> -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTAGING_DIR=<full path to staging\<name>>
#   cmake --build build\staging_<name>
#   build\staging_<name>\staging_tests.exe [filter]
cmake_minimum_required(VERSION 3.24)
project(PULP_STAGING LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  add_compile_options(/utf-8 /permissive- /Zc:preprocessor)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(NOT STAGING_DIR)
  message(FATAL_ERROR "pass -DSTAGING_DIR=<full path to your staging folder>")
endif()
get_filename_component(ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)

file(GLOB ENGINE_SRC CONFIGURE_DEPENDS "${ROOT}/engine/src/*.cpp")
add_library(staging_engine STATIC ${ENGINE_SRC})
target_include_directories(staging_engine PUBLIC "${ROOT}/engine/include")
if(MSVC)
  target_compile_options(staging_engine PRIVATE /W3 /fp:precise)
endif()

file(GLOB STAGING_SRC CONFIGURE_DEPENDS "${STAGING_DIR}/*.cpp")
add_executable(staging_tests ${STAGING_SRC} "${ROOT}/tests/main.cpp")
target_include_directories(staging_tests PRIVATE "${STAGING_DIR}" "${ROOT}/tests")
target_link_libraries(staging_tests PRIVATE staging_engine)
target_compile_definitions(staging_tests PRIVATE PULP_SOURCE_DIR="${ROOT}")
if(MSVC)
  target_compile_options(staging_tests PRIVATE /W4 /wd4100 /wd4244 /wd4267 /wd4305 /wd4456 /wd4458 /wd4459 /fp:precise)
endif()
