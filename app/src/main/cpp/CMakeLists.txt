cmake_minimum_required(VERSION 3.22.1)
project(pdockerpty C)

add_library(pdockerpty SHARED pty.c)
target_link_libraries(pdockerpty log)
target_compile_options(pdockerpty PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(pdockerdirect pdocker_direct_exec.c)
target_compile_options(pdockerdirect PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(pdockergpuexecutor pdocker_gpu_executor.c)
target_link_libraries(pdockergpuexecutor EGL GLESv3 log)
target_compile_options(pdockergpuexecutor PRIVATE -Wall -Wextra -Wno-unused-parameter)
