#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
header = (root / "backend/src/auth_session_store.h").read_text(encoding="utf-8")
store = (root / "backend/src/auth_session_store.cpp").read_text(encoding="utf-8")
controller = (root / "backend/src/auth_controller.cpp").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

required_header_contracts = (
    "bool revokeSessionToken(const std::string &token);",
)
for contract in required_header_contracts:
    if contract not in header:
        raise SystemExit(f"logout revocation header contract missing: {contract}")

required_store_contracts = (
    "bool revokeSessionToken(const std::string &token)",
    "bool persistentRevocationConfirmed = true;",
    "persistentRevocationConfirmed = revokeDatabaseToken(token);",
    "persistentRevocationConfirmed = false;",
    "return persistentRevocationConfirmed;",
)
for contract in required_store_contracts:
    if contract not in store:
        raise SystemExit(f"logout revocation store contract missing: {contract}")

required_controller_contracts = (
    "if (!cff::auth::revokeSessionToken(token))",
    'error["loggedOutLocally"] = true',
    "drogon::k503ServiceUnavailable",
    'resp->addHeader("Retry-After", "5")',
)
for contract in required_controller_contracts:
    if contract not in controller:
        raise SystemExit(f"logout controller contract missing: {contract}")

if "void revokeSessionToken(const std::string &token)" in header:
    raise SystemExit("old void logout revocation declaration remains")
if "void revokeSessionToken(const std::string &token)" in store:
    raise SystemExit("old void logout revocation implementation remains")
if "src/auth_session_store.cpp" not in cmake:
    raise SystemExit("auth_session_store.cpp is not part of the production target")

print("authentication logout revocation contracts passed")
