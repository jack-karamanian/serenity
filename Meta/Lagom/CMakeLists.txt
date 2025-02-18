cmake_minimum_required (VERSION 3.0)
project (Lagom)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -Werror -std=c++17 -fPIC -g")

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconsumed")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-expansion-to-defined")
endif()

file(GLOB AK_SOURCES "../../AK/*.cpp")
file(GLOB LIBCORE_SOURCES "../../Libraries/LibCore/*.cpp")
file(GLOB LIBIPC_SOURCES "../../Libraries/LibIPC/*.cpp")
file(GLOB LIBJS_SOURCES "../../Libraries/LibJS/*.cpp")
file(GLOB LIBJS_SUBDIR_SOURCES "../../Libraries/LibJS/*/*.cpp")

set(SOURCES ${AK_SOURCES} ${LIBCORE_SOURCES} ${LIBIPC_SOURCES} ${LIBJS_SOURCES} ${LIBJS_SUBDIR_SOURCES})

include_directories (../../)
include_directories (../../Libraries/)
add_library(lagom ${SOURCES})

add_executable(TestApp TestApp.cpp)
target_link_libraries(TestApp lagom)
target_link_libraries(TestApp stdc++)

add_executable(TestJson TestJson.cpp)
target_link_libraries(TestJson lagom)
target_link_libraries(TestJson stdc++)

add_executable(js ../../Userland/js.cpp)
target_link_libraries(js lagom)
target_link_libraries(js stdc++)
target_link_libraries(js pthread)
