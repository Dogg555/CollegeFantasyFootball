cmake_minimum_required(VERSION 3.15)
project(college_fantasy_football LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_DROGON "Build with Drogon HTTP framework" ON)

add_executable(college_ff_server
    src/main.cpp
)

target_include_directories(college_ff_server PRIVATE src)

if (ENABLE_DROGON)
    find_package(Drogon CONFIG REQUIRED)
    target_link_libraries(college_ff_server PRIVATE Drogon::Drogon)
    target_compile_definitions(college_ff_server PRIVATE DROGON_FOUND)
else()
    message(WARNING "ENABLE_DROGON=OFF; building a stub server without HTTP bindings.")
endif()
