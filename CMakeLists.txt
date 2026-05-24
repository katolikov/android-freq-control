cmake_minimum_required(VERSION 3.18)
project(freq_control CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT FREQ_CONTROL_SOC)
  set(FREQ_CONTROL_SOC "s5e9955" CACHE STRING "Target SoC (matches config/soc/<name>.h)")
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

add_library(freq_control STATIC
  src/freq_controller.cc
  src/device_config.cc
  src/sysfs_io.cc
  src/thread_affinity.cc
)
target_include_directories(freq_control
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/config
)
target_compile_definitions(freq_control PRIVATE FREQ_CONTROL_SOC=${FREQ_CONTROL_SOC})

add_executable(freq_ctl app/freq_ctl.cc)
target_link_libraries(freq_ctl PRIVATE freq_control)
target_include_directories(freq_ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
