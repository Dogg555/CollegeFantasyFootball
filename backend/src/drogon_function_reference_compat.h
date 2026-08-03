#pragma once

#include <drogon/utils/FunctionTraits.h>

// Drogon 1.9.7 recognizes handler function pointers, but registerHandler's
// forwarding reference can preserve a named free function as a function
// reference. Teach its traits layer to decay that reference to the already
// supported pointer form. This can be removed after the production Drogon
// dependency includes native function-reference support.
namespace drogon::internal {

template <typename ReturnType, typename... Arguments>
struct FunctionTraits<ReturnType (&)(Arguments...)>
    : FunctionTraits<ReturnType (*)(Arguments...)> {
};

}  // namespace drogon::internal
