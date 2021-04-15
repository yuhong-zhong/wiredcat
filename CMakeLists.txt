cmake_minimum_required(VERSION 3.10)
project(wiredcat)

set(CMAKE_CXX_STANDARD 14)

add_executable(read read.cpp)
target_link_libraries(read /usr/local/lib/libwiredtiger.so)

add_executable(read_write read_write.cpp)
target_link_libraries(read_write /usr/local/lib/libwiredtiger.so pthread)

add_executable(random_workload random_workload.cpp)
target_link_libraries(random_workload /usr/local/lib/libwiredtiger.so pthread)
