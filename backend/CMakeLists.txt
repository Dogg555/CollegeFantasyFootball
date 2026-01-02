cmake_minimum_required(VERSION 3.15)
project(college_fantasy_football LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_DROGON "Build with Drogon HTTP framework" ON)

add_executable(college_ff_server
    src/main.cpp
    src/json_utils.cpp
    src/handlers/league_handler.cpp
    src/league_models.cpp
    src/player_catalog.cpp
    src/cfbd_ingest.cpp
)

target_include_directories(college_ff_server PRIVATE src)

find_package(PostgreSQL REQUIRED)
find_package(PkgConfig REQUIRED)

# Prefer CMake configs when available; fall back to pkg-config for pqxx and cpr.
find_package(pqxx CONFIG)
if (NOT pqxx_FOUND)
    pkg_check_modules(PQXX REQUIRED libpqxx)
endif()

find_package(cpr CONFIG)
if (NOT cpr_FOUND)
    pkg_check_modules(CPR REQUIRED libcpr)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(college_ff_server PRIVATE
    PostgreSQL::PostgreSQL
)

if (pqxx_FOUND)
    target_link_libraries(college_ff_server PRIVATE pqxx::pqxx)
else()
    target_include_directories(college_ff_server PRIVATE ${PQXX_INCLUDE_DIRS})
    target_link_libraries(college_ff_server PRIVATE ${PQXX_LIBRARIES})
endif()

if (cpr_FOUND)
    target_link_libraries(college_ff_server PRIVATE cpr::cpr)
else()
    target_include_directories(college_ff_server PRIVATE ${CPR_INCLUDE_DIRS})
    target_link_libraries(college_ff_server PRIVATE ${CPR_LIBRARIES})
endif()

target_link_libraries(college_ff_server PRIVATE
    nlohmann_json::nlohmann_json
)
target_compile_definitions(college_ff_server PRIVATE CFF_HAS_POSTGRES)

if (ENABLE_DROGON)
    find_package(Drogon CONFIG REQUIRED)
    find_library(CRYPT_LIB crypt)
    if (NOT CRYPT_LIB)
        message(FATAL_ERROR "crypt library not found")
    endif()
    target_link_libraries(college_ff_server PRIVATE Drogon::Drogon ${CRYPT_LIB})
    target_compile_definitions(college_ff_server PRIVATE DROGON_FOUND)
else()
    message(WARNING "ENABLE_DROGON=OFF; building a stub server without HTTP bindings.")
endif()
