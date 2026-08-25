cmake_minimum_required(VERSION 3.16)
project(AlienFX_SDK LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)

# ----------- LIBUSB ------------

FetchContent_Declare(
  libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG main
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libusb)

# ----------- LOGURU ------------

FetchContent_Declare(
  LoguruGitRepo
  GIT_REPOSITORY https://github.com/emilk/loguru.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
set(LOGURU_WITH_STREAMS ON)
FetchContent_MakeAvailable(LoguruGitRepo)

# ----------- JSON ------------
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)

# ----------- SDK SOURCES ------------

file(GLOB_RECURSE SDK_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(AlienFX_SDK STATIC ${SDK_SOURCES})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(AlienFX_SDK PRIVATE DEBUG)
endif()
target_include_directories(AlienFX_SDK
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(AlienFX_SDK PUBLIC usb-1.0 loguru::loguru
                                         nlohmann_json::nlohmann_json)
