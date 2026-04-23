cmake_minimum_required(VERSION 3.22.1)
project(pdockerpty C)

add_library(pdockerpty SHARED pty.c)
target_link_libraries(pdockerpty log)
target_compile_options(pdockerpty PRIVATE -Wall -Wextra -Wno-unused-parameter)
