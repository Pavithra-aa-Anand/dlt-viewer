# Derived View Migration Implementation: What Changed, Why, and Demo (Phase 5)

## 1. Goal

Phase 5 moves derived views to projection adapters over shared indices and removes redundant copied/materialized row stores where safe.

Main goals:

- Move grouped ECU and CRLF views to projection adapters
- Keep one shared source-model rendering path instead of duplicate row materialization
- Reduce rebuild, export, and navigation overhead in derived views
- Preserve UI behavior and filtering semantics

## 2. What Changed

### 2.1 New Projection Adapter Model

Added a reusable adapter model in src:

- src/projectiontablemodel.h
- src/projectiontablemodel.cpp

CProjectionTableModel stores only projected source rows and forwards data/header requests to the source model.

Key behavior:

- No copied message payload/model rows
- Projection reset by row list update
- Source model updates propagated to projected rows

Projection helper APIs used by migrated views:

- `setProjectionRows(const std::vector<int>& rows)` to replace the active row projection.
- `sourceRowForRow(int projectedRow)` to map projected rows back to source rows for export/navigation.

### 2.2 Grouped ECU View Migration

Refactored grouped ECU tabs in:

- src/filtergrouplogs.h
- src/filtergrouplogs.cpp

Changes:

- Replaced per-tab EcuIdFilterProxyModel filtering with precomputed row projections
- Built ECU projections from active filtered projection in one pass using
	`buildActiveFilteredProjection()` + `CIndexService::snapshotProjection()`
- Replaced merged-tab proxy filtering with merged projection row vectors
- Removed artificial per-tab sleep during tab creation/merge
- Export path now maps projection rows directly to source indices
- Tab projections are updated through `setProjectionRows(...)` on `CProjectionTableModel`.

### 2.3 CRLF View Migration

Refactored CRLF window in:

- src/crlffilterwindow.h
- src/crlffilterwindow.cpp

Changes:

- Replaced QStandardItemModel materialized rows with CProjectionTableModel
- Added CRLF projection build over active filtered projection using
	`buildActiveFilteredProjection()` + `CIndexService::snapshotProjection()`
- Reused DecodeCacheService while evaluating CRLF payload matches
- Removed O(n*m) export remapping by directly mapping projection rows to source rows
- Double-click navigation now resolves message index from projected source row
- Export/double-click paths resolve source rows through `sourceRowForRow(...)`.

### 2.4 Service-Routed Derived View Completion

- MainWindow now injects shared MessageStore/IndexService/DecodeCacheService
	into grouped ECU and CRLF windows at feature entry points.
- Grouped ECU and CRLF projection scans are now service-routed and no longer
	depend on direct message retrieval from `QDltFile` in hot paths.
- CRLF row double-click navigation now resolves projected source rows via
	IndexService snapshot mapping before emitting main-window jump requests.

## 3. Why It Was Changed

Before this phase:

- CRLF view copied message fields into a separate materialized table model
- Grouped ECU tabs created many dynamic proxy filters over the same source model
- Export and navigation required extra remapping work

After this phase:

- Derived views are projections over one shared source-model path
- Memory overhead is reduced by storing row mappings instead of full row copies
- Rebuild/export paths do less work per view refresh

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Load a medium/large DLT file
- Open grouped ECU tabs
- Open CRLF window

### Demo Flow

1. Grouped ECU tabs are created from precomputed projection rows.
2. CRLF window builds projection rows for matching payloads.
3. Double-click CRLF row to navigate main table.
4. Export grouped/CRLF views and verify output consistency.

### Demo Behavior to Show

- Faster derived-view setup on larger files
- Lower memory growth vs copied materialized CRLF rows
- Correct row navigation/export mapping

## 5. Files Updated in Phase 5

- src/projectiontablemodel.h
- src/projectiontablemodel.cpp
- src/filtergrouplogs.h
- src/filtergrouplogs.cpp
- src/crlffilterwindow.h
- src/crlffilterwindow.cpp
- src/mainwindow.cpp
- src/CMakeLists.txt
