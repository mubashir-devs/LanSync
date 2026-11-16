# ADR 0006: Sync model, version vectors and conflicts

**Status:** Accepted (implemented)

## Model
Each device keeps a local index (`files` table, including tombstones). Every real local change bumps this
device's counter in the entry's **version vector**; vectors, not timestamps, decide who is newer.
`decide(local, remote)` (pure, unit-tested) returns: nothing / fetch / make-dir / apply-delete / merge /
keep-local / conflict. Metadata (size, mtime in ms) gates re-hashing; SHA-256 verifies content.
Events from the watcher are hints; periodic reconciliation scans are the source of truth.

## Conflicts: keep both, deterministically, without negotiation
Concurrent edits with different content:
* **Winner** = larger `(mtime, device id)`; both devices compute the same winner from data they both have.
* The **winner records the conflict and changes nothing else** (its file and version vector stay as they are).
* The **loser** sees the same concurrency, renames its own file to
  `<stem>.sync-conflict-<UTC time>-<id8><ext>`, adopts the winner's content, and sets its vector to the merge
  of both, which dominates the winner's. The conflict copy then syncs like any new file, so both devices end
  with both versions.
* Edit vs delete: the edit wins (`KeepLocal` bumps the vector so the file is resurrected).
* The user can later keep local, keep remote or keep both; the chosen content takes the original name and the other
  is archived in version history.

### What went wrong first (kept here as a warning)
An earlier design had the winner merge *and bump* its vector. The loser then saw a plain "remote is strictly
newer" update, replaced its file, and never created a copy. The end-to-end conflict test caught it. A related
race (a download started from a stale decision overwriting a locally edited file) is prevented by re-deciding at
apply time and deferring (`Busy`) when the local file has an unscanned edit.

## Safety rules
* Replaced or deleted content is archived first (`VersionStore`), so nothing is lost even when policy deletes a file.
* Retention is explicit (count and/or age); with both at 0 nothing is ever deleted.
* A file changing while hashed is "unstable": its previous record is kept and it is rescanned later.
* A missing or unreadable folder root or subtree is never interpreted as mass deletion.

## Known limits
Tombstones are not garbage collected; one peer per folder; `mtime` clock skew can change which version keeps the
name but never whether both are kept.
