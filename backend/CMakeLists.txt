cmake_minimum_required(VERSION 3.15)
project(college_fantasy_football LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Drogon can be fetched via FetchContent or provided by the system.
# To keep this scaffold lightweight, we do not fetch dependencies automatically.
# Define DROGON_FOUND to ON if Drogon is available in your environment.

if (DROGON_FOUND)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_executable(college_ff_server
    src/main.cpp
)

target_include_directories(college_ff_server PRIVATE src)

if (DROGON_FOUND)
    target_link_libraries(college_ff_server PRIVATE Drogon::Drogon)
else()
    message(WARNING "Drogon not found; building a stub server without HTTP bindings.")
endif()
