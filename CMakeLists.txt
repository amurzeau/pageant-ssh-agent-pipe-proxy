cmake_minimum_required(VERSION 3.12)
project(pageant-ssh-agent-pipe-proxy LANGUAGES CXX)

add_executable(pageant-pipe-proxy pageant-pipe-proxy.cpp)

add_executable(ssh-agent-pipe-proxy pipe-ssh-agent-unix.cpp)
target_link_libraries(ssh-agent-pipe-proxy PRIVATE ws2_32.lib)
