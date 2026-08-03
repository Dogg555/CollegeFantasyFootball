#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/auth_routes.cpp").read_text(encoding="utf-8")
controller = (root / "backend/src/auth_controller.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/auth_controller.h").read_text(encoding="utf-8")

handlers = (
    "handleSignup",
    "handleLogin",
    "handleLogout",
    "handleVerifyEmail",
    "handleResendVerification",
    "handleRequestPasswordReset",
    "handleResetPassword",
)

for handler in handlers:
    if f"void {handler}(" in main:
        raise SystemExit(f"{handler} implementation leaked back into main.cpp")
    if main.count(f"cff::auth::{handler}(req,") != 0:
        raise SystemExit(f"main.cpp must not delegate directly to {handler}")
    if routes.count(f"{handler}(req,") != 1:
        raise SystemExit(f"auth_routes.cpp must delegate exactly once to {handler}")
    if controller.count(f"void {handler}(") != 1:
        raise SystemExit(f"auth_controller.cpp must define {handler} exactly once")
    if header.count(f"void {handler}(") != 1:
        raise SystemExit(f"auth_controller.h must declare {handler} exactly once")

required_contract_text = (
    "Email and password are required",
    "Account already exists",
    "Invalid credentials",
    "Email verification required",
    "Invalid or expired verification token",
    "If the account exists and needs verification, a verification email will be sent.",
    "If the account exists, a password reset email will be sent.",
    "Password reset. Existing sessions were revoked.",
)
for text in required_contract_text:
    if text not in controller:
        raise SystemExit(f"controller contract text missing: {text}")

cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")
if "auth_controller.cpp" not in cmake:
    raise SystemExit("auth_controller.cpp is not part of the production target")

print("authentication controller boundary contracts passed")
