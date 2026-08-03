#!/usr/bin/env python3
"""Move shared authorization and CORS helpers into http_security.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
auth_routes_path = root / "backend/src/auth_routes.cpp"
cmake_path = root / "backend/CMakeLists.txt"
workflow_path = root / ".github/workflows/app-config-tests.yml"


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
    return text[:start_index] + text[end_index:]


def replace_present(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count < 1:
        raise RuntimeError(f"{label}: source text not found")
    return text.replace(old, new)


main = main_path.read_text(encoding="utf-8")
main = replace_exact(
    main,
    '#include "auth_routes.h"\n',
    '#include "auth_routes.h"\n#include "http_security.h"\n',
    "include shared HTTP security",
)
main = remove_between(
    main,
    "bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {",
    "std::string firstHeaderValue(std::string value) {",
    "remove bearer authorization and CORS helper block",
)
main = remove_between(
    main,
    "std::optional<std::string> accountEmailForRequest(const drogon::HttpRequestPtr &req,",
    "\n} // namespace\n#endif",
    "remove account and administrator guard block",
)
main = replace_present(
    main,
    "applyCorsHeaders(req, resp, allowedOrigins);",
    "cff::http::applyCorsHeaders(req, resp, allowedOrigins);",
    "delegate CORS response headers",
)
main = replace_present(
    main,
    "buildPreflightResponse(req, allowedOrigins)",
    "cff::http::buildPreflightResponse(req, allowedOrigins)",
    "delegate preflight responses",
)
main = replace_present(
    main,
    "isAuthorized(req, jwtSecret)",
    "cff::http::isAuthorized(req, jwtSecret)",
    "delegate secure endpoint authorization",
)
main = replace_present(
    main,
    "requireAccount(req, callback, jwtSecret, accountEmail)",
    "cff::http::requireAccount(req, callback, jwtSecret, accountEmail)",
    "delegate account guards",
)
main = replace_present(
    main,
    "requireAdmin(req, callback, jwtSecret, adminIdentity)",
    "cff::http::requireAdmin(req, callback, jwtSecret, adminIdentity)",
    "delegate administrator guards",
)

for leaked in (
    "bool isAuthorized(",
    "std::optional<std::string> accountEmailForRequest(",
    "bool requireAccount(",
    "bool isAdminRequest(",
    "bool requireAdmin(",
    "void applyCorsHeaders(",
    "drogon::HttpResponsePtr buildPreflightResponse(",
):
    if leaked in main:
        raise RuntimeError(f"main.cpp still owns shared HTTP security: {leaked}")
main_path.write_text(main, encoding="utf-8")


auth_routes = auth_routes_path.read_text(encoding="utf-8")
auth_routes = replace_exact(
    auth_routes,
    '#include "email_delivery.h"\n',
    '#include "email_delivery.h"\n#include "http_security.h"\n',
    "include shared HTTP security in auth routes",
)
auth_routes = replace_exact(
    auth_routes,
    "using cff::config::sharedSecretAuthAllowed;\n",
    "",
    "remove local shared-secret authorization dependency",
)
auth_routes = remove_between(
    auth_routes,
    "std::optional<std::string> bearerToken(const drogon::HttpRequestPtr &req) {",
    "\n} // namespace\n\nvoid registerAuthRoutes",
    "remove duplicated auth-route security helpers",
)
auth_routes = replace_present(
    auth_routes,
    "const auto token = bearerToken(req);",
    "const auto token = cff::http::bearerToken(req);",
    "delegate bearer parsing from auth routes",
)
auth_routes = replace_present(
    auth_routes,
    "const bool authorized = isAuthorized(req, jwtSecret);",
    "const bool authorized = cff::http::isAuthorized(req, jwtSecret);",
    "delegate auth-route authorization",
)
auth_routes = replace_present(
    auth_routes,
    "callback(buildPreflightResponse(req, allowedOrigins));",
    "callback(cff::http::buildPreflightResponse(req, allowedOrigins));",
    "delegate auth-route preflight handling",
)

for leaked in (
    "bool isAuthorized(",
    "void applyCorsHeaders(",
    "drogon::HttpResponsePtr buildPreflightResponse(",
):
    if leaked in auth_routes:
        raise RuntimeError(f"auth_routes.cpp still owns shared HTTP security: {leaked}")
auth_routes_path.write_text(auth_routes, encoding="utf-8")


cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_exact(
    cmake,
    "    src/auth_routes.cpp\n    src/auth_account_store.cpp\n",
    "    src/auth_routes.cpp\n    src/http_security.cpp\n    src/auth_account_store.cpp\n",
    "add HTTP security production source",
)
cmake_path.write_text(cmake, encoding="utf-8")


workflow = workflow_path.read_text(encoding="utf-8")
workflow = replace_exact(
    workflow,
    '      - "backend/src/auth_routes.cpp"\n      - "backend/src/auth_account_store.h"\n',
    '      - "backend/src/auth_routes.cpp"\n      - "backend/src/http_security.h"\n      - "backend/src/http_security.cpp"\n      - "backend/src/auth_account_store.h"\n',
    "add HTTP security workflow source paths",
    expected=2,
)
workflow = replace_exact(
    workflow,
    '      - "backend/tests/auth_routes_boundary_tests.py"\n      - "backend/tests/auth_account_store_tests.cpp"\n',
    '      - "backend/tests/auth_routes_boundary_tests.py"\n      - "backend/tests/http_security_boundary_tests.py"\n      - "backend/tests/auth_account_store_tests.cpp"\n',
    "add HTTP security workflow test paths",
    expected=2,
)
security_job = '''
  http-security-boundary:
    name: Shared HTTP security boundary contracts
    runs-on: ubuntu-24.04
    timeout-minutes: 5
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
      - name: Verify authorization and CORS ownership
        run: python backend/tests/http_security_boundary_tests.py

'''
workflow = replace_exact(
    workflow,
    "  auth-account-contracts:\n",
    security_job + "  auth-account-contracts:\n",
    "add HTTP security boundary job",
)
workflow_path.write_text(workflow, encoding="utf-8")

print("shared HTTP security extraction applied")
