# Versioned Migrations

`backend/db/migrate.sh` applies `schema.sql` once as `001_schema_snapshot`, then applies each `*.sql` file in this directory in filename order.

For future schema changes:

1. Add a new file such as `002_add_keeper_columns.sql`.
2. Keep it idempotent where possible.
3. Update `backend/db/schema.sql` so fresh databases still get the full current schema.
