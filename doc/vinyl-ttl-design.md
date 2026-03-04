# Vinyl Per-Tuple TTL: Design Document

## Overview

Add built-in TTL (Time-To-Live) support to the vinyl storage engine,
allowing each tuple to have its own expiration time. Expired tuples
become invisible to reads and are garbage-collected during compaction.

## User-Facing API

### Space Creation

TTL is enabled per-space by declaring an expiration field in the format
and referencing it in space options:

```lua
box.schema.space.create('sessions', {
    engine = 'vinyl',
    format = {
        {name = 'user_id', type = 'unsigned'},
        {name = 'token',   type = 'string'},
        {name = 'data',    type = 'string'},
        {name = 'expires_at', type = 'number'},
    },
    ttl = {field = 'expires_at'},
})

space:create_index('pk', {parts = {'user_id'}})
space:create_index('by_token', {parts = {'token'}})
```

The `expires_at` field holds an absolute Unix timestamp (seconds since
epoch). The engine treats tuples with `expires_at < now` as expired.
A value of 0 or NULL means the tuple never expires.

### DML

No changes to DML semantics. The application sets `expires_at` as a
regular field value:

```lua
-- Insert with 1-hour TTL
space:insert({1, 'abc', 'payload', clock.realtime() + 3600})

-- Refresh TTL (normal REPLACE, matches on primary key)
space:replace({1, 'abc', 'payload', clock.realtime() + 7200})

-- Point lookup (works as usual, expired tuples are invisible)
space:get(1)

-- Insert without TTL (lives forever)
space:insert({2, 'def', 'payload', 0})
```

REPLACE matches on the primary key and overwrites the entire tuple,
including the new `expires_at` value. No special TTL-aware DML is
needed.

### Altering TTL

The timeout isn't stored in the engine -- the application controls
expiration by setting `expires_at` per tuple. To change the TTL policy,
the application simply writes different `expires_at` values going
forward.

The `ttl` space option can be altered to point to a different field
or to disable TTL:

```lua
-- Disable TTL (expired tuples become visible again until compacted)
box.space.sessions:alter({ttl = false})
```

## Storage

### Primary Index

The `expires_at` field is a regular field in the tuple body. No special
storage treatment -- it's part of the tuple format like any other field.

### Secondary Indexes

Vinyl secondary index entries store secondary key parts + primary key
parts. They do not store other fields. To enable independent TTL
filtering in secondary indexes, the engine includes the `expires_at`
value as an extra trailing field when constructing secondary index
entries.

The engine fully controls secondary index tuple construction, so this
requires no user-visible changes to index definitions. The TTL field
is appended automatically when the space has `ttl` enabled.

Example of what gets stored for `by_token` index:

```
[token, user_id, expires_at]
 ^^^^^  ^^^^^^^  ^^^^^^^^^^
 secondary key  primary key  TTL (auto-appended)
```

### Run-Level Metadata

Each run file's index (`.index`) stores the minimum and maximum
`expires_at` values across all tuples in that run. This enables
fast decisions without reading tuple data:

- `max_expires_at < now`: entire run is fully expired
- `min_expires_at > now`: no expired tuples in this run

## Read-Time Filtering

Every read path (point lookup, iterator) filters expired tuples:

```
if tuple.expires_at != 0 and tuple.expires_at < now:
    skip
```

The `now` value is captured once at the start of the read operation to
ensure consistency within a single request.

### Expired Tuples Are Tombstones

An expired tuple suppresses older versions of the same key. Consider:

1. `INSERT {1, 'a'}` -- no TTL, lives forever
2. `REPLACE {1, 'b', expires_at = T}` -- TTL set
3. After time T: key 1 is gone (does not revert to 'a')

The most recent write was a tuple with TTL, and it has expired. The key
is dead. This means expired tuples must behave like DELETE tombstones
in the read iterator -- they mask any older versions below them in the
LSM tree.

## Compaction Behavior

### Non-Last Level

Expired tuples must be **preserved** during compaction (or converted to
lightweight DELETE tombstones). They suppress older versions that may
exist at deeper levels. Dropping them prematurely would resurrect old
data.

### Last Level

Expired tuples are **dropped** -- there are no deeper levels, so
nothing to suppress. This is the same rule vinyl already applies to
DELETE tombstones.

### Whole-Run Dropping

If a run is at the last level and its `max_expires_at < now`, the
entire run file can be deleted without reading any pages. This is
an O(1) operation -- just unlink the file.

For non-last-level runs, whole-run dropping is not safe (the expired
tuples serve as tombstones).

### Interaction with Deferred DELETEs

When primary index compaction at the last level drops an expired tuple,
it generates deferred DELETEs for secondary indexes -- the same
mechanism used for normal tuple overwrites. This cleans up the
corresponding secondary index entries.

Before the deferred DELETEs arrive, secondary index reads already
filter these entries via their own `expires_at` value, so correctness
is maintained.

## Summary

| Aspect | Design |
|--------|--------|
| User API | Explicit `expires_at` field in format, `ttl = {field = ...}` in space options |
| Expiration value | Absolute Unix timestamp, set by application |
| Primary index | `expires_at` stored as a regular tuple field |
| Secondary indexes | `expires_at` appended automatically by engine to secondary entries |
| Read filtering | Expired tuples skipped (behave as tombstones) |
| Compaction (non-last level) | Expired tuples preserved (suppress older versions) |
| Compaction (last level) | Expired tuples dropped |
| Whole-run drop | At last level when `max_expires_at < now` |
| Run metadata | min/max `expires_at` per run for fast filtering |
| DML changes | None -- standard INSERT/REPLACE/DELETE |
