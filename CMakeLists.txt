cmake_minimum_required(VERSION 3.16)
project(dm32emu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(ZLIB REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic -O3)

option(DM32EMU_NATIVE "Build for this host CPU (-march=native)" ON)
if(DM32EMU_NATIVE)
  add_compile_options(-march=native)
endif()

if(NOT MSVC)
  add_compile_options(-flto)
  add_link_options(-flto)
endif()

add_library(dm32core STATIC
  src/elf.cpp
  src/firmware.cpp
  src/codec_backup.cpp
  src/cpu.cpp
  src/machine.cpp
  src/virtual_rf.cpp
  src/uart_bridge.cpp
)
target_include_directories(dm32core PUBLIC src)
target_link_libraries(dm32core PUBLIC ZLIB::ZLIB)

add_executable(dm32emu src/main.cpp)
target_link_libraries(dm32emu PRIVATE dm32core)

enable_testing()
add_test(NAME cpu_smoke COMMAND dm32emu --self-test)
add_subdirectory(tests)

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6Widgets_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(dm32gui
    src/gui/main_gui.cpp
    src/gui/radio_widget.cpp
    src/gui/emulator_thread.cpp
    src/gui/dual_radio_window.cpp
  )
  target_include_directories(dm32gui PRIVATE src/gui)
  target_link_libraries(dm32gui PRIVATE dm32core Qt6::Widgets)
  add_test(NAME gui_offscreen COMMAND dm32gui --smoke-test)
  set_tests_properties(gui_offscreen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
else()
  message(STATUS "Qt6 Widgets not found; the dm32gui target is disabled")
endif()
