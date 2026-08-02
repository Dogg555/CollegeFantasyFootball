#pragma once

#include <string>

namespace cff {

bool emailDeliveryConfigured();
std::string emailDeliveryProvider();
bool sendTransactionalEmail(const std::string &to,
                            const std::string &subject,
                            const std::string &text,
                            const std::string &html);

}  // namespace cff
