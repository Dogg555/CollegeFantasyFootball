#include "commissioner_routes.h"

#include "commissioner_controls.h"
#include "app_config.h"
#include "http_security.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::commissioner_routes {
namespace {

#include "commissioner_routes_support.inc"
#include "commissioner_routes_handlers.inc"

} // namespace

#include "commissioner_routes_registration.inc"

} // namespace cff::commissioner_routes
