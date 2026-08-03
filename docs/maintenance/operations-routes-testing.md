# Operations route test plan

The final pull request must pass:

- source-boundary ownership checks for all five operations endpoints
- strict existing backend unit and boundary suites
- production Docker image compilation
- verification-disabled PostgreSQL and SMTP authentication contracts
- verification-required PostgreSQL and SMTP authentication contracts
- required-database-unavailable contracts
- runtime secure-ping authorization checks
- runtime administrator token, account-token rejection, ingestion status, live-status, and CORS preflight checks
- secret scanning and Snyk
