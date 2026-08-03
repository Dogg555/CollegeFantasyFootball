# Authentication controller extraction

This maintenance branch moves the seven authentication HTTP handlers from `backend/src/main.cpp` into `backend/src/auth_controller.cpp` while keeping route registration in `main.cpp`.

Persistence, endpoint paths, methods, payloads, status codes, CORS, migrations, and deployment settings remain unchanged. The branch targets `Test` only and must pass the controller boundary test plus the complete production-image authentication matrix before merge.
