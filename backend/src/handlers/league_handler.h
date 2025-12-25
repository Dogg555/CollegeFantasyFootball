#pragma once

#ifdef DROGON_FOUND
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <functional>

namespace cff::handlers {

void handleCreateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback);

}
#endif // DROGON_FOUND
