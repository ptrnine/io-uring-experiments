cmake_minimum_required(VERSION 3.23.2)
project(uring_game_serv)

set(CMAKE_CXX_STANDARD 23)

include_directories(SYSTEM "${CMAKE_BINARY_DIR}/3rd/include")
link_directories("${CMAKE_BINARY_DIR}/3rd/lib")
link_directories("${CMAKE_BINARY_DIR}/3rd/lib64")

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
    -Wold-style-cast
    -Woverloaded-virtual
    -Wredundant-decls
    -Wsign-conversion
    -Wsign-promo
    -Werror=unused-result
    -Wno-unused-function
    -Wno-missing-braces
)
foreach(_flag ${_cxx_flags})
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${_flag}>)
endforeach()

add_executable(uring_game_serv uring_game_serv.cpp)
target_link_libraries(uring_game_serv uring)

add_executable(test_client testudp.cpp)
