cmake_minimum_required(VERSION 3.13)
project(tiny)

set(CMAKE_CXX_STANDARD 14)

add_executable(tiny main.cpp csapp.h csapp.cpp sbuf.h sbuf.cpp)