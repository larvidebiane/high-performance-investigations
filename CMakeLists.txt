cmake_minimum_required(VERSION 3.13)
project(test)

set(CMAKE_CXX_STANDARD 17)

add_executable(exe-kernel-bypass kernel-bypass.cpp)
target_compile_options(exe-kernel-bypass PRIVATE -O3 -march=native)