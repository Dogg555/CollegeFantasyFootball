#!/usr/bin/env python3
"""Move health payload and route registration out of backend/src/main.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
cmake_path = root / "backend/CMakeLists.txt"


def replace_exact(text: str, old: str, new: str, label: str, expected: int = 1) -> str:
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{label}: expected {expected} source anchor(s), found {count}")
    return text.replace(old, new)


def remove_between(text: str, start: str, end: str, label: str) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    if text.find(start, start_index + 1) >= 0:
        raise RuntimeError(f"{label}: start marker is not unique")
    return text[:start_index] + text[end_index:]


main = main_path.read_text(encoding="utf-8")
main = replace_exact(
    main,
    '#include "http_security.h"\n',
    '#include "http_security.h"\n#include "health_routes.h"\n',
    "include health route module",
)

for using_line in (
    "using cff::config::emailVerificationRequired;\n",
    "using cff::config::frontendBaseUrl;\n",
    "using cff::config::maxPasswordLength;\n",
    "using cff::config::minPasswordLength;\n",
    "using cff::config::persistentDbRequired;\n",
):
    main = replace_exact(main, using_line, "", f"remove moved configuration alias {using_line.strip()}")

main = replace_exact(
    main,
    '''bool emailDeliveryConfigured() {
    return frontendBaseUrl().has_value() && cff::emailDeliveryConfigured();
}

''',
    "",
    "remove health email readiness helper",
)

main = remove_between(
    main,
    "Json::Value healthPayload(const std::optional<std::string> &jwtSecret,",
    "std::string firstHeaderValue(std::string value) {",
    "remove health payload and status implementation",
)

main = remove_between(
    main,
    "    const auto healthHandler = [jwtSecret, allowedOrigins](const drogon::HttpRequestPtr&",
    "    if (ingestOnStartup) {",
    "remove inline health handler",
)

listener_block = '''    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency())
        .registerHandler("/health", healthHandler, {drogon::Get})
        .registerHandler("/api/health", healthHandler, {drogon::Get})
        .registerHandler("/api/secure/ping",
'''
listener_replacement = '''    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency());

    cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);

    app.registerHandler("/api/secure/ping",
'''
main = replace_exact(
    main,
    listener_block,
    listener_replacement,
    "delegate health route registration",
)

main = replace_exact(
    main,
    '        .registerHandler("/api/health", preflightHandler, {drogon::Options})\n',
    "",
    "move API health preflight route",
)

for leaked in (
    "Json::Value healthPayload(",
    "drogon::HttpStatusCode healthStatusCode(",
    "const auto healthHandler =",
    '.registerHandler("/health"',
    '.registerHandler("/api/health"',
):
    if leaked in main:
        raise RuntimeError(f"main.cpp still owns health behavior: {leaked}")

if main.count("cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);") != 1:
    raise RuntimeError("main.cpp must delegate health registration exactly once")

main_path.write_text(main, encoding="utf-8")

cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_exact(
    cmake,
    "    src/health_status.cpp\n    src/league_beta_stability.cpp\n",
    "    src/health_status.cpp\n    src/health_routes.cpp\n    src/league_beta_stability.cpp\n",
    "add health route production source",
)
cmake_path.write_text(cmake, encoding="utf-8")

print("health route extraction applied")
