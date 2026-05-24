# Minimum 3.6 so this project can be consumed (add_subdirectory) from a
# host project pinned to cmake 3.6.0. The 3.6 floor implies:
#   - no CMAKE_CXX_STANDARD (added in 3.8); use -std=c++17 directly.
#   - no target_compile_features(cxx_std_17) (added in 3.8); same fix.
cmake_minimum_required(VERSION 3.6)
project(freq_control CXX)

# C++17 via a plain flag so we don't depend on CMAKE_CXX_STANDARD.
if(NOT CMAKE_CXX_FLAGS MATCHES "-std=c\\+\\+")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
endif()

# Warnings — applied as target options so consumers don't inherit them.
set(FREQ_CONTROL_WARN_FLAGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

add_library(freq_control STATIC
  src/freq_controller.cc
  src/device_config.cc
  src/sysfs_io.cc
  src/thread_affinity.cc
)
target_include_directories(freq_control
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(freq_control PRIVATE ${FREQ_CONTROL_WARN_FLAGS})

add_executable(freq_ctl app/freq_ctl.cc)
target_link_libraries(freq_ctl PRIVATE freq_control)
target_include_directories(freq_ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(freq_ctl PRIVATE ${FREQ_CONTROL_WARN_FLAGS})
