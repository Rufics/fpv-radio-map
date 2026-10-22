# Database migrations

Every change to the server's schema, and every change to existing data for a new version, is a
migration step. There are no hand-run scripts. The same steps move a live database forward at
start, and they move a backup forward when it is restored. The code is in
`server/src/migrations.{hpp,cpp}`.

## The version of a database

A database's version is the ordered list of the steps it has applied. `schema_migrations`
records each step's name and checksum:

- An SQL step's checksum is the SHA-256 of its file.
- A C++ step's checksum comes from its revision string.

A backup's header carries the same list, so a database, a backup and a server's own steps
(`MigrationCatalog`) all have a version of one type, `SchemaVersion`. Comparing two versions
position by position gives one of four results:

| Result | Meaning |
|---|---|
| Same | The same steps. |
| Behind | A prefix of the other: the other has more steps. |
| Ahead | The other is a prefix of this one: this has steps the other lacks. |
| Diverged | A different name, or a different checksum where both checksums are known. |

Rows written before checksums existed, and backups of format 1, have no checksum. Only their
names are compared, and the server fills in the missing checksums the next time it starts.

## One way to change version

`SchemaMigrator::migrateThrough(pivot, action)` runs in one transaction:

1. Take the migration lock (`pg_advisory_xact_lock(7461001)`), so one server migrates at a time.
2. Read the database's version and refuse it if it is Ahead of the server or Diverged from it.
3. Apply the steps up to `pivot`.
4. Run `action` on the schema of exactly that version.
5. Apply the remaining steps.
6. Check that the database now records the server's version.
7. Commit. Then the files that the steps staged move into place.

Three callers use it, and they differ only in what they pass:

| Caller | Pivot | Action | Breaking steps |
|---|---|---|---|
| Server start | none | none | Refused on a database that has data. Allowed on a new database. |
| `rfmap-server --migrate` | none | none | Applied. `--plan` lists the steps and changes nothing. |
| `rfmap-server --restore` | The backup's version | Load the rows and files | Applied. Only into a new, empty database. |

A restore builds the schema at the backup's version, loads the rows there, then applies every
step the backup does not have yet. Those steps adapt the restored data exactly as they adapt a
live database. That is why a backup from an older server restores into a newer one, and why
data conversion never lives in the backup code.

## Writing a step

**SQL** is the default. Add `server/sql/NNN_what_it_does.sql`, with the next free number. The
file runs inside the migration transaction. Never edit a step after it has been released: the
server refuses to start when an applied step's checksum changes. Write a new step instead.

**C++** is for what SQL cannot do. Examples:

- rewriting values with the server's own code, such as re-hashing or re-encoding them;
- reading or writing module files or the trust store.

Subclass `CodeMigration` and list it in `codeMigrations()` in `migrations.cpp`:

```cpp
class RehashShareTokens final : public CodeMigration {
public:
    RehashShareTokens() : CodeMigration("004_rehash_share_tokens", "rev 1", Breaking::Yes) {}
    void apply(MigrationContext& ctx) const override {
        // ctx.work(): the migration transaction. ctx.files(): the module root and the trust store.
        // ctx.staged().read(path): what a file holds now, including a file a restore has staged.
        // ctx.staged().write(path, bytes, mode): a file that moves into place after the commit.
    }
};
```

A C++ step's name has no `.sql`, but it sorts among the SQL files by its number. Two steps with
the same number are refused. Change the revision string whenever the step's effect changes.

**Breaking** steps change existing data so that the previous server version can no longer use
it. Mark an SQL step by making `-- rfmap: breaking` its first line, or pass `Breaking::Yes` to a
C++ step. A server will not apply a breaking step to a database that has data on its own. The
administrator takes a backup with the old version, then runs `--migrate` with the new one. The
steps are in [Updates](../admin/production-setup.md#11-updates).

## Backup format versions

The backup format and the schema have separate versions. The header's `version` is the format
(`BackupWriter::kVersion`, now 2). `BackupReader` brings an older format up to the current one
line by line, through `BackupReader::upgrades()`, so the restore code only ever reads the newest
format. To change the format:

1. Raise `kVersion`.
2. Add one entry to `upgrades()` that turns the previous format's header (and, if needed, each
   line) into the new one.
3. Add a test that restores a backup of the previous format.

Format 1 listed the migrations by name only. Its upgrade adds an empty checksum to each.

## Tests

- `server/tests/migrations_test.cpp` covers `SchemaVersion`.
- `server/tests/backup_test.cpp` covers the rest on PostgreSQL:
  - an old backup taken through a newer SQL step and a C++ step;
  - a format 1 backup;
  - a start that refuses a newer database, a changed step or a breaking step;
  - `--migrate --plan` and `--migrate`;
  - two steps with one number.
