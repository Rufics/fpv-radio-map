-- Flight logs imported into a mission: one row per flight. `normalized` is the decoder's
-- flightlog/v1 entry for that flight (schemas/flightlog.schema.json), the source of truth the
-- replay view is rebuilt from, so a better processor improves old flights too. `summary` is
-- the logbook row, kept so listing a mission's flights does not reprocess every log.
create table flight_logs (
    id           text primary key,
    mission_id   text not null references missions(id) on delete cascade,
    file_name    text not null,
    file_sha256  text not null,
    log_index    integer not null,
    decoder      text not null,
    started_at   text,
    duration_s   double precision not null,
    normalized   jsonb not null,
    summary      jsonb not null,
    created_at   timestamptz not null default now(),
    unique (mission_id, file_sha256, log_index)
);
create index flight_logs_mission_idx on flight_logs (mission_id, created_at);
