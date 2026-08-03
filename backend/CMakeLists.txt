cmake_minimum_required(VERSION 3.15)
project(college_fantasy_football LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_DROGON "Build with Drogon HTTP framework" ON)

if (NOT ENABLE_DROGON)
    message(WARNING "ENABLE_DROGON=OFF; building a stub server without HTTP bindings.")
    add_executable(college_ff_server src/main.cpp)
    target_include_directories(college_ff_server PRIVATE src)
    return()
endif()

# Keep route registration modules in the production target so structural
# boundaries and Docker contracts exercise the same server composition.
add_executable(college_ff_server
    src/main.cpp
    src/app_config.cpp
    src/auth_core.cpp
    src/auth_controller.cpp
    src/auth_routes.cpp
    src/http_security.cpp
    src/auth_account_store.cpp
    src/auth_session_store.cpp
    src/email_delivery.cpp
    src/league_invite_email.cpp
    src/security_hardening.cpp
    src/signup_response.cpp
    src/health_status.cpp
    src/health_routes.cpp
    src/operations_routes.cpp
    src/league_routes.cpp
    src/public_routes.cpp
    src/league_beta_stability.cpp
    src/team_name_handler.cpp
    src/json_utils.cpp
    src/handlers/league_handler.cpp
    src/league_models.cpp
    src/player_catalog.cpp
    src/cfbd_ingest.cpp
    src/live_scores.cpp
)

# Drogon 1.9.7's handler traits support free-function pointers but not the
# function-reference type preserved by registerHandler's forwarding argument.
# Apply the narrow compatibility specialization only to the module that uses
# named free-function registration directly.
set_source_files_properties(
    src/league_beta_stability.cpp
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/drogon_function_reference_compat.h"
)

target_include_directories(college_ff_server PRIVATE src)

find_package(PostgreSQL REQUIRED)
find_package(CURL REQUIRED)

# Prefer CMake configs when available; fall back to pkg-config for pqxx and cpr.
find_package(pqxx CONFIG QUIET)
find_package(cpr CONFIG QUIET)

if (NOT pqxx_FOUND OR NOT cpr_FOUND)
    find_package(PkgConfig REQUIRED)
endif()

if (NOT pqxx_FOUND)
    pkg_check_modules(PQXX REQUIRED libpqxx)
endif()

if (NOT cpr_FOUND)
    pkg_check_modules(CPR REQUIRED libcpr)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(college_ff_server PRIVATE
    PostgreSQL::PostgreSQL
    CURL::libcurl
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

find_package(Drogon CONFIG REQUIRED)
find_library(CRYPT_LIB crypt)
if (NOT CRYPT_LIB)
    message(FATAL_ERROR "crypt library not found")
endif()
target_link_libraries(college_ff_server PRIVATE Drogon::Drogon ${CRYPT_LIB})
target_compile_definitions(college_ff_server PRIVATE DROGON_FOUND)


include(CTest)
if (BUILD_TESTING)
    add_executable(app_config_tests
        tests/app_config_tests.cpp
        src/app_config.cpp
    )
    target_include_directories(app_config_tests PRIVATE src)
    add_test(NAME app_config_tests COMMAND app_config_tests)

    add_executable(auth_core_tests
        tests/auth_core_tests.cpp
        src/auth_core.cpp
    )
    target_include_directories(auth_core_tests PRIVATE src)
    target_link_libraries(auth_core_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_core_tests COMMAND auth_core_tests)

    add_executable(auth_session_store_tests
        tests/auth_session_store_tests.cpp
        src/auth_session_store.cpp
        src/auth_core.cpp
        src/app_config.cpp
    )
    target_include_directories(auth_session_store_tests PRIVATE src)
    target_link_libraries(auth_session_store_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_session_store_tests COMMAND auth_session_store_tests)

    add_executable(auth_account_store_tests
        tests/auth_account_store_tests.cpp
        src/auth_account_store.cpp
        src/app_config.cpp
    )
    target_include_directories(auth_account_store_tests PRIVATE src)
    add_test(NAME auth_account_store_tests COMMAND auth_account_store_tests)
endif()
