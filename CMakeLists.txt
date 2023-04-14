cmake_minimum_required(VERSION 3.23.2)
project(uring_game_serv)

set(CMAKE_CXX_STANDARD 23)

include_directories(SYSTEM "${CMAKE_BINARY_DIR}/3rd/include")
link_directories("${CMAKE_BINARY_DIR}/3rd/lib")
link_directories("${CMAKE_BINARY_DIR}/3rd/lib64")

include(CMake/git_submodule_build.cmake)

set(_cxx_flags
    -fstrict-aliasing
    -Wall
    -Wextra
    -Wpedantic
    -Wcast-align
    -Wconversion
    -Wctor-dtor-privacy
    -Wextra-semi
    -Wfloat-equal
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wredundant-decls
    -Wsign-conversion
    -Wsign-promo
    -Werror=unused-result
    -Wno-unused-function
    -Wno-missing-braces
    -Wno-missing-field-initializers
    -Wno-old-style-cast
)
foreach(_flag ${_cxx_flags})
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${_flag}>)
endforeach()

git_submodule_update()
git_submodule_copy_files(
    SPSCQueue
    INCLUDES include/rigtorp/SPSCQueue.h
    EXPLICIT_INCLUDE_DIR rigtorp)

find_package(Boost REQUIRED ALL)

add_executable(uring_game_serv uring_game_serv.cpp)
target_link_libraries(uring_game_serv uring boost_context boost_fiber)
#target_link_directories(uring_game_serv PRIVATE
#    "${CMAKE_SOURCE_DIR}/../diefastdiehard/easybuild/3rd/lib")
#include_directories("${CMAKE_SOURCE_DIR}/../diefastdiehard/easybuild/3rd/")

add_executable(recvmsg_game_serv recvmsg_game_serv.cpp)

add_executable(test_client testudp.cpp)
