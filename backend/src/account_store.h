#pragma once

#include <string>

namespace cff::auth {

enum class AccountCreateStatus {
    Created,
    AlreadyExists,
    Unavailable,
    Failed,
};

struct AccountCreateResult {
    AccountCreateStatus status{AccountCreateStatus::Failed};
    bool verificationPrepared{false};
};

AccountCreateResult createAccount(const std::string &email,
                                  const std::string &passwordHash,
                                  const std::string &verificationToken,
                                  bool verificationRequired);

}  // namespace cff::auth
