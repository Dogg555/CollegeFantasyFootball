cmake_minimum_required(VERSION 3.15)
project(college_fantasy_football LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_DROGON "Build with Drogon HTTP framework" ON)

if (NOT ENABLE_DROGON)
    message(WARNING "ENABLE_DROGON=OFF; building a stub server without HTTP bindings.")
    add_executable(college_ff_server
        src/main.cpp
        src/application_bootstrap.cpp
    )
    target_include_directories(college_ff_server PRIVATE src)
    return()
endif()

# Keep route registration and runtime modules in the production target so
# structural boundaries and Docker contracts exercise the same composition.
add_executable(college_ff_server
    src/main.cpp
    src/application_bootstrap.cpp
    src/app_composition.cpp
    src/app_config.cpp
    src/server_runtime.cpp
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
    src/league_onboarding_hardening.cpp
    src/draft_lifecycle.cpp
    src/draft_lifecycle_hardening.cpp
    src/roster_transaction.cpp
    src/roster_transaction_hardening.cpp
    src/waiver_lifecycle.cpp
    src/waiver_lifecycle_hardening.cpp
    src/trade_lifecycle.cpp
    src/trade_lifecycle_hardening.cpp
    src/scoring_lifecycle.cpp
    src/scoring_lifecycle_hardening.cpp
    src/schedule_lineup_lifecycle.cpp
    src/schedule_lineup_hardening.cpp
    src/stat_ingestion_lifecycle.cpp
    src/stat_ingestion_hardening.cpp
    src/public_routes.cpp
    src/league_beta_stability.cpp
    src/team_name_handler.cpp
    src/json_utils.cpp
    src/handlers/league_handler.cpp
    src/league_models.cpp
    src/league_schedule.cpp
    src/league_roster.cpp
    src/league_waiver.cpp
    src/league_trade.cpp
    src/player_catalog.cpp
    src/ingest_runtime.cpp
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
find_package(Threads REQUIRED)

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
    Threads::Threads
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

    add_executable(ingest_runtime_tests
        tests/ingest_runtime_tests.cpp
        src/ingest_runtime.cpp
    )
    target_include_directories(ingest_runtime_tests PRIVATE src)
    target_link_libraries(ingest_runtime_tests PRIVATE
        Threads::Threads
        nlohmann_json::nlohmann_json
    )
    add_test(NAME ingest_runtime_tests COMMAND ingest_runtime_tests)


    add_executable(league_schedule_tests
        tests/league_schedule_tests.cpp
        src/league_schedule.cpp
    )
    target_include_directories(league_schedule_tests PRIVATE src)
    target_link_libraries(league_schedule_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_schedule_tests COMMAND league_schedule_tests)


    add_executable(draft_lifecycle_tests
        tests/draft_lifecycle_tests.cpp
        src/draft_lifecycle.cpp
        src/league_schedule.cpp
    )
    target_include_directories(draft_lifecycle_tests PRIVATE src)
    target_link_libraries(draft_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME draft_lifecycle_tests COMMAND draft_lifecycle_tests)


    add_executable(roster_transaction_tests
        tests/roster_transaction_tests.cpp
        src/roster_transaction.cpp
        src/league_roster.cpp
        src/league_waiver.cpp
        src/json_utils.cpp
    )
    target_include_directories(roster_transaction_tests PRIVATE src)
    target_link_libraries(roster_transaction_tests PRIVATE Drogon::Drogon)
    add_test(NAME roster_transaction_tests COMMAND roster_transaction_tests)


    add_executable(waiver_lifecycle_tests
        tests/waiver_lifecycle_tests.cpp
        src/waiver_lifecycle.cpp
    )
    target_include_directories(waiver_lifecycle_tests PRIVATE src)
    target_link_libraries(waiver_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME waiver_lifecycle_tests COMMAND waiver_lifecycle_tests)


    add_executable(trade_lifecycle_tests
        tests/trade_lifecycle_tests.cpp
        src/trade_lifecycle.cpp
    )
    target_include_directories(trade_lifecycle_tests PRIVATE src)
    target_link_libraries(trade_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME trade_lifecycle_tests COMMAND trade_lifecycle_tests)


    add_executable(scoring_lifecycle_tests
        tests/scoring_lifecycle_tests.cpp
        src/scoring_lifecycle.cpp
    )
    target_include_directories(scoring_lifecycle_tests PRIVATE src)
    target_link_libraries(scoring_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME scoring_lifecycle_tests COMMAND scoring_lifecycle_tests)


    add_executable(schedule_lineup_lifecycle_tests
        tests/schedule_lineup_lifecycle_tests.cpp
        src/schedule_lineup_lifecycle.cpp
    )
    target_include_directories(schedule_lineup_lifecycle_tests PRIVATE src)
    target_link_libraries(schedule_lineup_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME schedule_lineup_lifecycle_tests COMMAND schedule_lineup_lifecycle_tests)


    add_executable(stat_ingestion_lifecycle_tests
        tests/stat_ingestion_lifecycle_tests.cpp
        src/stat_ingestion_lifecycle.cpp
    )
    target_include_directories(stat_ingestion_lifecycle_tests PRIVATE src)
    target_link_libraries(stat_ingestion_lifecycle_tests PRIVATE Drogon::Drogon)
    add_test(NAME stat_ingestion_lifecycle_tests COMMAND stat_ingestion_lifecycle_tests)


    add_executable(league_roster_tests
        tests/league_roster_tests.cpp
        src/league_roster.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_roster_tests PRIVATE src)
    target_link_libraries(league_roster_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_roster_tests COMMAND league_roster_tests)


    add_executable(league_waiver_tests
        tests/league_waiver_tests.cpp
        src/league_waiver.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_waiver_tests PRIVATE src)
    target_link_libraries(league_waiver_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_waiver_tests COMMAND league_waiver_tests)


    add_executable(league_trade_tests
        tests/league_trade_tests.cpp
        src/league_trade.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_trade_tests PRIVATE src)
    target_link_libraries(league_trade_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_trade_tests COMMAND league_trade_tests)
endif()
