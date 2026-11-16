-- Initial LanSync schema.
-- Timestamps are UTC milliseconds since the Unix epoch (INTEGER).

CREATE TABLE devices (
    device_id        TEXT PRIMARY KEY,            -- hex SHA-256 of the device public key
    name             TEXT NOT NULL,
    public_key_pem   TEXT,                        -- pinned identity (set when paired)
    cert_fingerprint TEXT,                        -- SHA-256 of the certificate DER
    address          TEXT,
    port             INTEGER CHECK (port IS NULL OR (port BETWEEN 1 AND 65535)),
    pairing_status   TEXT NOT NULL DEFAULT 'unpaired'
        CHECK (pairing_status IN ('unpaired','pending_incoming','pending_outgoing','paired','revoked')),
    last_seen_at     INTEGER,
    created_at       INTEGER NOT NULL
) STRICT;

CREATE TABLE sync_folders (
    folder_id        TEXT PRIMARY KEY,
    local_path       TEXT NOT NULL,
    display_name     TEXT NOT NULL,
    remote_device_id TEXT REFERENCES devices(device_id) ON DELETE SET NULL,
    direction        TEXT NOT NULL DEFAULT 'bidirectional'
        CHECK (direction IN ('bidirectional','send_only','receive_only')),
    enabled          INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0,1)),
    created_at       INTEGER NOT NULL,
    UNIQUE (local_path, remote_device_id)
) STRICT;

CREATE TABLE files (
    file_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    folder_id        TEXT NOT NULL REFERENCES sync_folders(folder_id) ON DELETE CASCADE,
    relative_path    TEXT NOT NULL,               -- always '/' separated, validated
    is_directory     INTEGER NOT NULL DEFAULT 0 CHECK (is_directory IN (0,1)),
    size             INTEGER NOT NULL DEFAULT 0 CHECK (size >= 0),
    modified_at      INTEGER NOT NULL,
    content_hash     TEXT,                        -- hex SHA-256; NULL until hashed
    deleted          INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0,1)),
    version_vector   TEXT NOT NULL DEFAULT '{}',  -- JSON {device_id: counter}
    sync_status      TEXT NOT NULL DEFAULT 'idle'
        CHECK (sync_status IN ('idle','scanning','comparing','queued','transferring',
                               'verifying','completed','conflict','failed','paused')),
    last_synced_at   INTEGER,
    UNIQUE (folder_id, relative_path)
) STRICT;

CREATE INDEX idx_files_status ON files(folder_id, sync_status);
CREATE INDEX idx_files_hash   ON files(content_hash);

CREATE TABLE transfers (
    transfer_id        TEXT PRIMARY KEY,
    file_id            INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    source_device_id   TEXT NOT NULL,
    dest_device_id     TEXT NOT NULL,
    status             TEXT NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued','transferring','verifying','completed','failed','cancelled','paused')),
    bytes_transferred  INTEGER NOT NULL DEFAULT 0 CHECK (bytes_transferred >= 0),
    total_bytes        INTEGER NOT NULL DEFAULT 0 CHECK (total_bytes >= 0),
    resume_token       TEXT,                      -- JSON: temp file, verified offset, partial hash state
    error              TEXT,
    started_at         INTEGER,
    completed_at       INTEGER
) STRICT;

CREATE INDEX idx_transfers_status ON transfers(status);

CREATE TABLE file_versions (
    version_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id          INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    content_hash     TEXT NOT NULL,
    size             INTEGER NOT NULL CHECK (size >= 0),
    modified_at      INTEGER NOT NULL,
    storage_location TEXT NOT NULL,               -- path relative to the versions store
    created_at       INTEGER NOT NULL
) STRICT;

CREATE INDEX idx_versions_file ON file_versions(file_id, created_at);

CREATE TABLE conflicts (
    conflict_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id          INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    local_hash       TEXT NOT NULL,
    remote_hash      TEXT NOT NULL,
    local_modified_at  INTEGER NOT NULL,
    remote_modified_at INTEGER NOT NULL,
    conflict_copy_path TEXT,
    resolution       TEXT CHECK (resolution IS NULL OR resolution IN ('keep_local','keep_remote','keep_both')),
    detected_at      INTEGER NOT NULL,
    resolved_at      INTEGER
) STRICT;

CREATE TABLE sync_history (
    event_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id        TEXT,
    file_path        TEXT,
    operation        TEXT NOT NULL,
    result           TEXT NOT NULL CHECK (result IN ('success','failure','info')),
    detail           TEXT,
    timestamp        INTEGER NOT NULL
) STRICT;

CREATE INDEX idx_history_time ON sync_history(timestamp);

CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) STRICT;
