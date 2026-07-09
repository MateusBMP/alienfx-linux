cmake_minimum_required(VERSION 3.16)
project(
  AlienFan_SDK
  LANGUAGES CXX
  VERSION 0.0.1)
add_compile_definitions(VERSION="${PROJECT_VERSION}")

file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(AlienFan_SDK STATIC ${CLI_SOURCES})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(AlienFan_SDK PRIVATE DEBUG)
endif()
target_include_directories(AlienFan_SDK
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AlienFan_SDK PUBLIC loguru::loguru)
