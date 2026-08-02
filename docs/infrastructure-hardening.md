# Infrastructure hardening

This document records the runtime assumptions enforced by the infrastructure configuration and CI contracts.

## Deployment lifecycle

- Render owns production migrations through `preDeployCommand`.
- Render runs the guarded ESPN roster bootstrap once during pre-deploy.
- The API container skips migrations and roster bootstrap in Render through explicit environment flags.
- Standalone/local containers may opt into startup migrations, which wait for PostgreSQL readiness before applying the advisory-lock-protected migration set.

## Local container topology

- PostgreSQL, the backend, and the frontend bind only to loopback host addresses.
- Compose waits for PostgreSQL health before starting the backend and waits for backend health before starting the frontend.
- TLS terminates at the external edge. Traffic inside the isolated Compose network uses HTTP rather than HTTPS with certificate verification disabled.

## Proxy and caching

- NGINX has bounded connect, send, and read timeouts.
- Automatic upstream retries are disabled so mutation requests are not replayed.
- HTML, configuration, JavaScript, and CSS are not served as immutable assets until the frontend uses content-hashed filenames.

## Health semantics

- `/health` and `/api/health` return HTTP 503 whenever the generated health payload is not `status: ok`.
- Healthy instances continue to return HTTP 200.

## Image reproducibility

Production and local runtime base images are pinned by immutable multi-platform digest. Updating a base image is an intentional reviewed change and should include the matching contract-test update when applicable.
