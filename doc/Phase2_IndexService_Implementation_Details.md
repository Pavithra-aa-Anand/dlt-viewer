# IndexService Implementation

## 1. Goal

Phase 2 centralizes row/index projection logic behind a dedicated service.

Main goals:

- Introduce a single API for row-count and row-to-global-index mapping over provided projections
- Provide projection snapshots for deterministic search and rendering loops
- Isolate QDltFile-specific projection extraction in a dedicated helper
- Reduce repeated filtered-row remapping in hot UI/search paths

## 2. What Changed

### 2.1 New IndexService API

- Added abstract interface CIndexService in qdlt.
- Added APIs for:
  - Full and filtered row counts
  - Full and filtered row to global index mapping
  - A single `snapshotProjection()` method that returns a cleaned global-index projection
- The internal `.cpp` helper `snapshotValidProjection()` keeps the projection
  cleanup focused on removing invalid entries while leaving the API itself minimal.

### 2.2 QDltFile Compatibility Adapter

- Added `qdltfileprojection.h/cpp` with `buildActiveFilteredProjection()` helper to extract
  the active filtered projection from QDltFile internals.
- Phase 2 consumes the active filtered projection via `buildActiveFilteredProjection()` and 
  applies centralized projection mapping via `CIndexService`.
- `CQDltFileIndexServiceAdapter` bridges QDltFile to the `CIndexService` interface for row count
  and row-to-global-index mapping.
- `snapshotProjection()` cleans invalid entries from the projection it receives.

### 2.3 Search Integration

- SearchDialog later adopted the Phase 2 projection API by snapshotting the active filtered projection once per run:
  - Parallel Find-All path takes one snapshot before chunk dispatch
  - Single-step find loop also consumes one projection snapshot
- This is a downstream UI integration of the Phase 2 `IndexService` API, not part of the core service itself.

### 2.4 Runtime Wiring Completion

- MainWindow now owns a shared `CIndexService` instance for runtime projection
  snapshot operations after filter/reload transitions.
- Grouped ECU and CRLF feature entry points now receive shared `CIndexService`
  via MainWindow service wiring.
- Double-click/navigation and projection row mapping paths in derived views now
  resolve through `buildActiveFilteredProjection()` + `snapshotProjection()`.

## 3. Why It Was Changed

Before Phase 2, row mapping logic was spread across model and search loops, causing duplicate logic and extra mapping calls in hot paths.

Phase 2 improves this by introducing projection snapshots and central mapping:

- One snapshot per search run gives deterministic scan input.
- Row/index ownership becomes clearer for later migrations (grouped and CRLF projections).

Benefits:

- Lower per-iteration index mapping overhead
- More consistent behavior between table/search paths
- Cleaner transition path toward adapter-based derived views

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Load a large DLT file.
- Apply filters and/or marker selection.
- Start Find-All and scroll table concurrently.

### Demo Flow

1. SearchDialog creates a CIndexService instance and builds the active filtered projection from the active file.
2. SearchDialog snapshots the active filtered projection once.
3. Workers iterate snapshot entries to get global indices.
4. Search resolves row-to-global-index mapping from that stable snapshot.

### Demo Behavior to Show

- Search run works against a stable snapshot for that run.
- Search path resolves rows through one stable projection snapshot, reducing remap churn.
- Invalid or out-of-range rows resolve to -1 safely.

## 5. Before vs After (At a Glance)

```text
Before:
[Search paths]
  |
  v
[Direct and repeated filtered-row mapping]
  |
  v
[Duplicated index logic in multiple loops]

After:
[Search paths]
  |
  v
[CIndexService]
  |
  v
[Snapshot projection once]
  |
  v
[Reuse projection for row to global mapping]
```

## 6. Files Updated in Phase 2

- qdlt/indexservice.h
- qdlt/indexservice.cpp
- src/tablemodel.cpp
- src/searchdialog.cpp
- qdlt/qdltfileprojection.h
- qdlt/qdltfileprojection.cpp
- src/mainwindow.h
- src/mainwindow.cpp
- src/filtergrouplogs.h
- src/filtergrouplogs.cpp
- src/crlffilterwindow.h
- src/crlffilterwindow.cpp
