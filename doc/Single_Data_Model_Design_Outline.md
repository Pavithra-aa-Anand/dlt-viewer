# Single Data Model Design Outline

## 1. Problem Summary

Current UI features (main table, search, grouped views, CRLF view) use overlapping but separate data/index/model paths.
This causes duplicated logic, synchronization risk, and heavy refresh behavior.

Major problems:

- Decode and formatting logic repeated in multiple paths
- Multiple index/result lists representing similar subsets
- Frequent broad model refresh (`layoutChanged`) instead of targeted row updates
- Temporary inconsistencies possible during live logging + search/filter updates

## 2. Current Architecture 

```
+-------------------------------+
| Input                         |
| (File Load / Live Logging)    |
+---------------+---------------+
        |
        v
+-------------------------------+
| QDltFile                      |
| Index + Filter Data           |
+---+---------------+-----------+
  |               |            \
  v               v             v
+---------+    +-----------+   +----------------+
|TableModel|    |SearchDialog|  |CRLF Rebuilder |
|filtered  |    |workers     |  |QStandardItemM |
+----+----+    +-----+-----+   +----------------+
   |               |
   v               v
+-----------+   +----------------+
|Grouped ECU|   |SearchTableModel|
|Proxy Layers|  |Result List     |
+-----------+   +----------------+

Plugin decode hooks are invoked from multiple paths (index/live/render/search/CRLF).

+--------------------------+--------------------------------------------------------------+
| Component                | Responsibility                                               |
+--------------------------+--------------------------------------------------------------+
| Input                    | Provides file-load and live-log messages into the system     |
| QDltFile                 | Holds message data, indexes, and filter-related state        |
| TableModel               | Presents filtered rows to the main table view                |
| SearchDialog             | Runs search logic and worker-based matching                  |
| SearchTableModel         | Stores and displays search result rows                       |
| CRLF Rebuilder           | Builds a separate CRLF-specific row model                    |
| Grouped ECU Proxy Layers | Create grouped/filtered subsets for ECU-specific views       |
| Plugin decode hooks      | Decode/enrich messages from multiple paths                   |
+--------------------------+--------------------------------------------------------------+
```

## 3. New Architecture

```
+-------------------------------+
| Input                         |
| (File Load / Live Logging)    |
+---------------+---------------+
        |
        v
+-------------------------------+
| MessageStore                  |
| Canonical Immutable Records   |
+---------------+---------------+
        |
  +---------+---------+
  |                   |
  v                   v
+-------------+    +-------------------+
|IndexService |    |DecodeCacheService |
|Projections  |    |Decoded/Derived    |
+------+------+    +---------+---------+
   |                     |
   +----------+----------+
      |
      v
  +-------------------------------+
  | ProjectionAdapters            |
  | Main / Search / Grouped /CRLF|
  +---------------+---------------+
          |
          v
  +-------------------------------+
  | QTableView-based UI           |
  | Incremental row updates/      |
  | Full model refresh only for   |
  | structural invalidation       |
  +-------------------------------+

Plugin stage contracts integrate through IndexService and DecodeCacheService.

+------------------------+-------------------------------------------------------------------+
| Component              | Responsibility                                                    |
+------------------------+-------------------------------------------------------------------+
| Input                  | Provides new messages from file load or live logging              |
| MessageStore           | Keeps the canonical append-only message records                   |
| IndexService           | Builds and maintains filtered, sorted, grouped, and search views  |
| DecodeCacheService     | Stores decoded and derived values for reuse                       |
| ProjectionAdapters     | Convert shared projections into UI-model-friendly row access      |
| QTableView-based UI    | Displays rows using incremental updates                           |
| Plugin stage contracts | Define where plugins participate in ingest, decode, or enrich     |
|                        | stages                                                            |
+------------------------+-------------------------------------------------------------------+
```

Design principles:

- One source of truth for messages
- Storage and index responsibilities are separated
- Views consume projections, not private copies
- Decode/derived data is shared and centrally invalidated

## 4. Before vs After (What Changes)

```
+--------------------+-----------------------------------------------------+----------------------------------------------------+
| Area               | Before                                              | After                                              |
+--------------------+-----------------------------------------------------+----------------------------------------------------+
| Source of truth    | Multiple overlapping lists/models                   | Single MessageStore                                |
| Index ownership    | Mixed across UI/model/indexer paths                 | Central IndexService                               |
| Decode path        | Repeated in render/search/live/indexing contexts    | DecodeCacheService with clear invalidation         |
| UI updates         | Often broad refresh (layoutChanged)                 | Row-level delta updates by projection              |
| Search consistency | Snapshot can become stale during concurrent updates | Explicit snapshot/rebase policy from IndexService  |
| CRLF/grouped views | Materialized copied rows/proxy stacks               | Projection adapters over shared indices            |
+--------------------+-----------------------------------------------------+----------------------------------------------------+
```

## 5. Target Data Ownership

```
+---------------------+        +----------------------+
| MessageStore        |        | DecodeCacheService   |
| - append messages   |        | - decoded fields     |
| - get by index      |        | - derived values     |
+----------+----------+        +----------+-----------+
           |                              |
           v                              |
     +----------------------+             |
     | IndexService         |<------------+
     | - full projection    |
     | - filtered view      |
     | - marker union       |
     | - search snapshot    |
     | - grouped/CRLF view  |
     +----------+-----------+
                |
                v
     +----------------------+
     | ProjectionAdapter    |
     | - row to message id  |
     | - row count          |
     +----------------------+

+--------------------+---------------------------------------------------+----------------------------------------------+
| Layer              | Owns                                              | Does not own                                |
+--------------------+---------------------------------------------------+----------------------------------------------+
| MessageStore       | Canonical message records, stable message identity| Filtered order, search results, grouped views|
| IndexService       | Full/filtered/search/grouped/CRLF projections     | Raw message storage, decoded payload cache   |
| DecodeCacheService | Decoded payloads, derived display values,         | Message ordering, UI row mapping             |
|                    | invalidation state                                |                                              |
| ProjectionAdapter  | Row-to-message mapping, row count,                | Canonical storage, projection computation    |
|                    | model delta publication                           |                                              |
+--------------------+---------------------------------------------------+----------------------------------------------+
```

## 6. Expected Benefits

- Better consistency between main table/search/grouped/CRLF views
- Less duplicate decode/formatting work
- Better performance from incremental updates
- Clear extension point for plugin stages and invalidation rules

## 7. Risks and Mitigation

```
+--------------------------------+--------------------------------------+------------------------------------------------------+
| Risk                           | Why it matters                       | Mitigation                                           |
+--------------------------------+--------------------------------------+------------------------------------------------------+
| Identity mismatch during       | Wrong row-to-message mapping         | Introduce stable MessageId early and adapter tests   |
| migration                      |                                      |                                                      |
| Cache invalidation bugs        | Stale decoded data in UI             | Versioned decode context + scoped invalidation hooks |
| Behavior regression in marker  | User-visible filter ordering changes | Keep deterministic merge rules + parity tests        |
| union                          |                                      |                                                      |
| Plugin side effects            | Context-dependent decode behavior    | Define plugin stage contracts and thread boundaries  |
| Partial migration complexity   | Mixed old/new paths temporarily      | Use adapters and feature flags per phase             |
+--------------------------------+--------------------------------------+------------------------------------------------------+
```

## 8. Migration Plan (Implementation Phases)

Phase 1: Foundation

- Add MessageStore API and stable MessageId
- Add adapters to read from current `QDltFile` path

Phase 2: Index centralization

- Introduce IndexService for full/filtered/marker-union projections
- Keep existing UI models but route index queries through service

Phase 3: Decode centralization

- Add DecodeCacheService
- Move decode calls out of table/search render paths to shared API

Phase 4: Incremental updates

- Replace broad refreshes in hot paths with row-level notifications
- Retain full reset fallback for structural invalidations

Phase 5: Derived view migration

- Move search, grouped ECU, CRLF to projection adapters
- Remove copied/materialized redundant row stores where safe

Phase 6: Plugin contract hardening

- Define ingest/decode/enrich stage behavior
- Define thread-safety and idempotence expectations