cmake_minimum_required(VERSION 3.16)
project(droidvm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(ZLIB REQUIRED)
find_package(SDL2 REQUIRED)

add_library(droidcore STATIC
  src/apk/zip.cpp
  src/apk/dex.cpp
  src/apk/axml.cpp
  src/vm/vm.cpp
  src/vm/interp.cpp
  src/framework/java_lang.cpp
  src/framework/android.cpp
  src/gfx/raster.cpp
)
target_include_directories(droidcore PUBLIC src)
target_link_libraries(droidcore PUBLIC ZLIB::ZLIB)
target_compile_options(droidcore PRIVATE -Wall)

add_executable(droidvm src/main.cpp)
target_link_libraries(droidvm PRIVATE droidcore)
if(TARGET SDL2::SDL2)
  target_link_libraries(droidvm PRIVATE SDL2::SDL2)
else()
  target_include_directories(droidvm PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(droidvm PRIVATE ${SDL2_LIBRARIES})
endif()
target_compile_options(droidvm PRIVATE -Wall)

enable_testing()
add_subdirectory(tests)
