# DecodeCacheService Implementation: What Changed, Why, and Demo (Phase 3)

## 1. Goal

Phase 3 centralizes decoded message access and cache invalidation through a shared decode service.

Main goals:

- Provide one shared decode/read API for table and search paths
- Avoid repeated plugin decode cost in render and search hot loops
- Add bounded caching with explicit invalidation hooks
- Establish canonical decoded-data ownership in one service

## 2. What Changed

### 2.1 New DecodeCacheService API

- Added CDecodeCacheService in qdlt.
- Added message() API that:
  - Loads raw bytes by global index
  - Parses QDltMsg
  - Optionally applies plugin decode
  - Returns decoded message with optional cache lookup/store
- Added invalidation APIs:
  - clear()
  - clearForFile(file)

### 2.2 Cache Model and Concurrency

- Added cache key dimensions:
  - file pointer
  - global index
  - decode-enabled state
  - triggered-by-user mode
- Cache entries are keyed by (file, globalIndex, decodeEnabled, triggeredByUser).
- Added bounded FIFO-style cache with maximum entry cap.
- Added mutex-protected access for thread-safe cache operations.
- Invalidation is explicit via clear() and clearForFile(file).

### 2.3 Integration in TableModel

- TableModel service hooks and decode-cache integration points were introduced at
  interface level in this phase.

### 2.4 Integration in SearchDialog

- SearchDialog decode-cache integration points were introduced at interface level
  in this phase.

### 2.5 Integration in SearchTableModel

- Search result rendering paths (display, foreground, background) now fetch decoded messages via DecodeCacheService.
- Search result reset clears per-file decode cache.

### 2.6 Build and Header Integration

- Added decodecacheservice.h and decodecacheservice.cpp to qdlt library build.
- Exported DecodeCacheService via qdlt umbrella header.

### 2.7 Runtime Wiring Completion

- MainWindow now owns a shared `CDecodeCacheService` instance and performs
  lifecycle invalidation (`clearForFile`) on file clear/filter transitions.
- Grouped ECU and CRLF feature entry points receive shared
  `CDecodeCacheService` via MainWindow service wiring.
- CRLF and grouped projection scans now decode through DecodeCacheService first,
  with MessageStore fallback only when decode cache path is unavailable.

## 3. Why It Was Changed

Before Phase 3, decode calls were repeated across render/search/model paths, increasing repeated plugin work and making invalidation behavior harder to reason about.

Phase 3 improves this with a centralized decode path:

- Shared cache reduces repeated decode for the same message/context.
- Explicit per-file and full cache clear improves operational control.
- Decode ownership boundary is clearer for upcoming plugin contract hardening.

Benefits:

- Better responsiveness during table/search repaint and navigation
- Lower repeated decode cost in hot paths
- More predictable invalidation semantics at model/search boundaries

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Load a file with repeated view/search interactions.
- Enable plugin decode.
- Run Find-All and navigate/scroll results.

### Demo Flow

1. Call DecodeCacheService message() for global indices from table/search code.
2. First access decodes and stores cache entry.
3. Repeated accesses for same key return cached QDltMsg.
4. Model/search reset triggers clearForFile(file) to invalidate stale entries.

### Demo Behavior to Show

- First-hit decode cost followed by fast repeated access.
- Cache key separation by decode mode and triggeredByUser mode.
- File switch/reset removes stale decoded entries via clearForFile().

## 5. Before vs After (At a Glance)

```text
Before:
[Table/Search/SearchTableModel]
  |
  v
[Independent decode calls]
  |
  v
[Repeated plugin decode in hot paths]

After:
[Table/Search/SearchTableModel]
  |
  v
[CDecodeCacheService::message(file, globalIndex, decodeEnabled, triggeredByUser)]
  |
  v
[Load raw bytes from QDltFile by global index]
  |
  v
[Parse QDltMsg]
  |
  v
[Optionally run plugin decode]
  |
  v
[Return decoded QDltMsg copy]
  |
  v
[Bounded shared cache keyed by (file, globalIndex, decodeEnabled, triggeredByUser)]
  |
  v
[Explicit clear and clearForFile invalidation]
```

## 6. Files Updated in Phase 3

- qdlt/decodecacheservice.h
- qdlt/decodecacheservice.cpp
- qdlt/CMakeLists.txt
- qdlt/qdlt.h
- qdlt/qdltexporter.h
- qdlt/qdltexporter.cpp
- src/tablemodel.h
- src/tablemodel.cpp
- src/searchdialog.h
- src/searchdialog.cpp
- src/searchtablemodel.cpp
- src/searchtablemodel.h
- src/mainwindow.h
- src/mainwindow.cpp
- src/dltfileindexer.h
- src/dltfileindexer.cpp
- src/dltfileindexerthread.h
- src/dltfileindexerthread.cpp
- src/dltfileindexerdefaultfilterthread.h
- src/dltfileindexerdefaultfilterthread.cpp
- src/filtergrouplogs.h
- src/filtergrouplogs.cpp
- src/crlffilterwindow.h
- src/crlffilterwindow.cpp
