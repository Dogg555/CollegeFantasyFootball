# Operations route extraction

This maintenance branch moves `/api/secure/ping` and the four ingestion administration endpoints out of `backend/src/main.cpp` into a dedicated operations route module.

The task preserves authorization, response bodies, status codes, PostgreSQL ingestion status queries, CORS preflight behavior, and production deployment configuration. It targets `Test` only and must pass the structural boundary suite plus all three production authentication environments before merge.
