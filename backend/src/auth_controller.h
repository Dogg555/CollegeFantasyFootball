#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <optional>
#include <string>

namespace cff::auth {

using AuthResponseCallback = std::function<void(const drogon::HttpResponsePtr &)>;

void handleSignup(const drogon::HttpRequestPtr &req,
                  AuthResponseCallback &&callback,
                  const std::optional<std::string> &jwtSecret);
void handleLogin(const drogon::HttpRequestPtr &req,
                 AuthResponseCallback &&callback,
                 const std::optional<std::string> &jwtSecret);
void handleLogout(const drogon::HttpRequestPtr &req,
                  AuthResponseCallback &&callback);
void handleVerifyEmail(const drogon::HttpRequestPtr &req,
                       AuthResponseCallback &&callback);
void handleResendVerification(const drogon::HttpRequestPtr &req,
                              AuthResponseCallback &&callback);
void handleRequestPasswordReset(const drogon::HttpRequestPtr &req,
                                AuthResponseCallback &&callback);
void handleResetPassword(const drogon::HttpRequestPtr &req,
                         AuthResponseCallback &&callback);

} // namespace cff::auth
