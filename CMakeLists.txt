cmake_minimum_required(VERSION 3.16)
project(annota CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The interpreter plus the C++ kernels that the standard library calls through the FFI
# (`native/seq_native.cpp` registers `seqnative`).  Plugins under plugins/ are built separately
# with `annota plugin build`.
file(GLOB ANNOTA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
                         "${CMAKE_CURRENT_SOURCE_DIR}/native/*.cpp")
add_executable(annota ${ANNOTA_SOURCES})
target_include_directories(annota PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Export the interpreter's symbols so a plugin compiled separately can resolve them at load time
# (`-rdynamic` on Linux; the equivalent import library on Windows).
set_target_properties(annota PROPERTIES ENABLE_EXPORTS ON)

if(WIN32)
  target_link_libraries(annota PRIVATE ws2_32)
endif()

# std::thread / std::mutex need the platform thread library (libpthread on Linux)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(annota PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(annota PRIVATE /utf-8 /W3 /wd4100 /wd4244 /wd4267)
else()
  target_compile_options(annota PRIVATE -Wall -Wno-unused-parameter -Wno-sign-compare)
endif()

# Optional Qt 6 front end: enables `--gui` (a real window) and `--gui-shot`.
# Configure with -DCMAKE_PREFIX_PATH=<Qt>/<version>/mingw_64 when Qt is not on the default path.
find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
  target_compile_definitions(annota PRIVATE ANNOTA_QT)
  target_link_libraries(annota PRIVATE Qt6::Widgets)
  message(STATUS "Annota: Qt 6 found, the graphical window is enabled")
else()
  message(STATUS "Annota: Qt 6 not found, building the interpreter without the GUI window")
endif()
