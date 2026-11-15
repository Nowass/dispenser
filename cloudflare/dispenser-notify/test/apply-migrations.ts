import { applyD1Migrations, env } from "cloudflare:test";

// Builds the local test database from the same migrations that production uses.
await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);
