-- Transfers must be recordable before a local `files` row exists (a file being
-- downloaded for the first time), so they are identified by folder + path, with
-- an optional link to the file row. The table held no data in any release.

DROP TABLE transfers;

CREATE TABLE transfers (
    transfer_id        TEXT PRIMARY KEY,
    folder_id          TEXT NOT NULL REFERENCES sync_folders(folder_id) ON DELETE CASCADE,
    relative_path      TEXT NOT NULL,
    file_id            INTEGER REFERENCES files(file_id) ON DELETE SET NULL,
    direction          TEXT NOT NULL CHECK (direction IN ('download','upload')),
    source_device_id   TEXT NOT NULL,
    dest_device_id     TEXT NOT NULL,
    status             TEXT NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued','transferring','verifying','completed','failed','cancelled','paused')),
    bytes_transferred  INTEGER NOT NULL DEFAULT 0 CHECK (bytes_transferred >= 0),
    total_bytes        INTEGER NOT NULL DEFAULT 0 CHECK (total_bytes >= 0),
    content_hash       TEXT,
    error              TEXT,
    started_at         INTEGER,
    completed_at       INTEGER
) STRICT;

CREATE INDEX idx_transfers_status ON transfers(status);
CREATE INDEX idx_transfers_folder ON transfers(folder_id);

-- Folders are matched across devices by (share name, remote device): it must be unambiguous.
CREATE UNIQUE INDEX idx_folders_share ON sync_folders(display_name, remote_device_id);
