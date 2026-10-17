-- rfmap server schema. Applied once by the server at start (schema_migrations records it).

create extension if not exists postgis;

-- Missions. The owner key is never stored, only its SHA-256 (it is 256 random bits).
create table missions (
    id              text primary key,
    owner_key_hash  bytea not null,
    name            text not null,
    doc             jsonb not null,
    revision        integer not null default 1,
    pilot           geography(Point, 4326),
    last_result     jsonb,
    created_at      timestamptz not null default now(),
    updated_at      timestamptz not null default now()
);
create index missions_pilot_idx on missions using gist (pilot);

-- Share links: the token and the PIN are stored only as hashes (token SHA-256, PIN argon2id).
create table share_links (
    id               text primary key,
    mission_id       text not null references missions(id) on delete cascade,
    token_hash       bytea not null unique,
    pin_hash         text not null,
    role             text not null check (role in ('edit', 'view')),
    label            text,
    created_at       timestamptz not null default now(),
    last_opened_at   timestamptz,
    revoked_at       timestamptz,
    failed_attempts  integer not null default 0,
    lockouts         integer not null default 0,
    locked_until     timestamptz,
    open_count       integer not null default 0,
    expires_at       timestamptz
);
create index share_links_mission_idx on share_links (mission_id);

-- Sessions: random tokens, stored as SHA-256. link_id is null for the creator's own session.
create table sessions (
    token_hash  bytea primary key,
    mission_id  text not null references missions(id) on delete cascade,
    link_id     text references share_links(id) on delete cascade,
    role        text not null check (role in ('edit', 'view')),
    created_at  timestamptz not null default now(),
    expires_at  timestamptz not null
);
create index sessions_link_idx on sessions (link_id);
create index sessions_expiry_idx on sessions (expires_at);

-- Reference library. The current version of each item, and every version ever saved.
create table library_items (
    kind        text not null,
    id          text not null,
    version     integer not null,
    verified    boolean not null,
    doc         jsonb not null,
    updated_at  timestamptz not null default now(),
    updated_by  text not null,
    primary key (kind, id)
);

create table library_history (
    kind        text not null,
    id          text not null,
    version     integer not null,
    verified    boolean not null,
    doc         jsonb not null,
    changed_at  timestamptz not null default now(),
    changed_by  text not null,
    primary key (kind, id, version)
);

-- Seed files already imported, so a deleted or edited item is not re-imported on restart.
create table seeds_applied (
    name        text primary key,
    sha256      text not null,
    applied_at  timestamptz not null default now()
);

-- Built-in RF extensions (rf.*): admin switch and options, such as clutter heights per class.
create table builtin_modules (
    id          text primary key,
    enabled     boolean not null default true,
    options     jsonb not null default '{}'::jsonb,
    updated_at  timestamptz not null default now()
);

-- Second cache tier for results, keyed by the hash of every input that went into them.
create table result_cache (
    input_hash   text primary key,
    kind         text not null,
    result       jsonb not null,
    bytes        integer not null,
    created_at   timestamptz not null default now(),
    last_hit_at  timestamptz not null default now(),
    hits         integer not null default 0
);
create index result_cache_lru_idx on result_cache (last_hit_at);

-- Runtime settings the admin changes (cache limits); config file values are the defaults.
create table settings (
    key    text primary key,
    value  jsonb not null
);
