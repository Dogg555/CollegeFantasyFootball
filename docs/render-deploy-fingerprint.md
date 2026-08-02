# Render deployment fingerprint diagnosis

The API source exposes `emailDeliveryConfigured` from `/api/health`. If the live response only contains `status`, `service`, and `database`, the service is running an older successful image.

Render keeps the previous successful instance live whenever a newer build, pre-deploy command, startup, or health check fails. Confirm the newest deploy reaches `live`; a build entry existing in the Deploys page is not sufficient.

## Exact-commit cache-cleared deploy

Trigger the deploy through the Render API with both an exact commit and cache clearing:

```json
{
  "clearCache": "clear",
  "commitId": "<exact-main-sha>"
}
```

After the deploy reaches `live`, request both the Render hostname and custom API hostname with a unique query string to bypass any edge cache.

## Required build evidence

Retain the deploy logs showing:

- the exact Git commit;
- the Docker `COPY . /context` step;
- compilation of `src/main.cpp` and `src/email_delivery.cpp`;
- linking of `college_ff_server`;
- successful migration/pre-deploy completion;
- the new instance becoming healthy and live.

If any of these steps fails, diagnose the first failure instead of testing the still-running previous image.
