# MessageStore Implementation
## 1. Goal

Phase 1 introduces the MessageStore foundation. Phase 2 and Phase 3 build on top of that foundation.

Main goals:

- Add stable message identity (`MessageId`)
- Add a store API (`CMessageStore`)
- Keep `QDltFile` as the source of truth through an adapter
- Use snapshot-based access for safe and deterministic parallel work

## 2. What Changed

### 2.1 Stable Message Identity

- Added `using MessageId = quint64`.
- Added invalid constant: `kInvalidMessageId`.
- In Phase 1, `MessageId` maps to the global row index in `QDltFile`.

### 2.2 New MessageStore API

- Added abstract interface `CMessageStore`.
- Added APIs for:
  - size and contains checks
  - global index <-> `MessageId` mapping
  - filtered row -> `MessageId` mapping
  - message retrieval (raw and decoded-ready)
    - snapshot retrieval (`const std::vector<MessageId>&`)

Recent API refinements:

- `rawMessage()` now returns `std::vector<char>` at the public API boundary.
- `CQDltFileMessageStoreAdapter` still reads bytes from `QDltFile` as `QByteArray`
    and converts to `std::vector<char>` at the adapter boundary.
- Snapshot methods now return cached non-owning views:
    - `snapshotAllMessageIds()` -> `const std::vector<MessageId>&`
    - `snapshotFilteredMessageIds()` -> `const std::vector<MessageId>&`

### 2.3 QDltFile Compatibility Adapter

- Added `CQDltFileMessageStoreAdapter`.
- Adapter exposes `CMessageStore` while using existing `QDltFile` internals.
- No data model migration needed in Phase 1.

### 2.4 Integration Scope

- Phase 1 focuses on MessageStore abstractions and the QDltFile adapter surface.
- Build/export wiring is handled in later phases.

### 2.5 Runtime Wiring Completion

- MainWindow now owns a shared `CQDltFileMessageStoreAdapter` and binds it to the
    active `QDltFile` during init, reload, and live-update transitions.
- Search row/message resolution paths now consume filtered snapshots from
    MessageStore and map through `MessageId` to global index.
- MainWindow selection/filter helper paths now resolve filtered row mappings via
    MessageStore APIs instead of direct filtered-index calls.

## 3. Why It Was Changed

The old path required repeated filtered-row to global-row mapping inside hot loops.
This created overhead and made parallel processing harder to reason about.

Phase 1 improves this by introducing snapshot semantics:

- Capture `std::vector<MessageId>` once.
- Process immutable IDs in workers.
- Keep deterministic behavior even if filters/live data change during execution.

Benefits:

- Better performance characteristics for find-all/search workloads
- More stable result navigation (`MessageId` -> global row)
- Lower coupling between UI/search logic and raw file indexing details

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Load a large DLT file.
- Optionally apply a filter.
- Start a find-all/search operation that uses a snapshot.

### Demo Flow

1. Capture snapshot once: `snapshotMessageIds = store.snapshotFilteredMessageIds()`
2. Run worker threads using only `snapshotMessageIds`
3. Convert match ID back when needed: `globalRow = store.globalIndexForMessageId(id)`

### Demo Behavior to Show

- If filter changes during run, in-flight results stay consistent with old snapshot.
- If live messages append during run, they are not included until next run.
- Invalid ID returns `-1` cleanly.

## 5. Before vs After (At a Glance)

```text
Before:
[Consumer]
    |
    v
[Repeated row mapping in loops]
    |
    v
[Frequent QDltFile index lookups]

After:
[Consumer]
    |
    v
[CMessageStore]
    |
    v
[Capture MessageId snapshot once]
    |
    v
[Workers process immutable IDs]
    |
    v
[Resolve final navigation by MessageId]
```

## 6. Files Updated in Phase 1

- `qdlt/messagestore.h`
- `qdlt/messagestore.cpp`
- `qdlt/qdltfile.h`
- `qdlt/qdltfile.cpp`
- `qdlt/qdltexporter.h`
- `qdlt/qdltexporter.cpp`
- `src/mainwindow.h`
- `src/mainwindow.cpp`
- `src/searchdialog.cpp`
- `src/dltfileindexer.h`
- `src/dltfileindexer.cpp`
