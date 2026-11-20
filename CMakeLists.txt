cmake_minimum_required(VERSION 3.16)
project(dm32emu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(dm32core STATIC
  src/elf.cpp
  src/firmware.cpp
  src/cpu.cpp
  src/machine.cpp
)
target_include_directories(dm32core PUBLIC src)

add_executable(dm32emu src/main.cpp)
target_link_libraries(dm32emu PRIVATE dm32core)

enable_testing()
add_test(NAME cpu_smoke COMMAND dm32emu --self-test)

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6Widgets_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(dm32gui
    src/gui/main_gui.cpp
    src/gui/radio_widget.cpp
    src/gui/emulator_thread.cpp
  )
  target_include_directories(dm32gui PRIVATE src/gui)
  target_link_libraries(dm32gui PRIVATE dm32core Qt6::Widgets)
  add_test(NAME gui_offscreen COMMAND dm32gui --smoke-test)
  set_tests_properties(gui_offscreen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
else()
  message(STATUS "Qt6 Widgets not found; the dm32gui target is disabled")
endif()
