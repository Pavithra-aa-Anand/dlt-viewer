# Incremental Updates Implementation: What Changed, Why, and Demo (Phase 4)

## 1. Goal

Phase 4 reduces broad model refreshes in hot UI paths by switching to row-level notifications where possible.

Main goals:

- Replace `layoutChanged()`-driven refreshes with incremental model updates
- Keep repaint behavior correct for row append/remove and in-place data updates
- Retain full model reset fallback for structural invalidations
- Reduce avoidable cache churn during non-structural UI updates

## 2. What Changed

### 2.1 TableModel Incremental Delta Publication

- Reworked `CTableModel::modelChanged()` to publish deltas instead of emitting `layoutChanged()`.
- Added tracked model state:
  - `m_lastKnownRowCount`
  - `m_lastKnownColumnCount`
- Added `notifyModelDelta(previousRowCount, currentRowCount, currentColumnCount)` to emit:
  - `beginInsertRows()/endInsertRows()` for appended rows
  - `beginRemoveRows()/endRemoveRows()` for removed tail rows
  - `dataChanged()` for shared row range

### 2.2 Structural Invalidation Fallback

- `CTableModel::modelChanged()` now falls back to `beginResetModel()/endResetModel()` when:
  - first model notification after initialization, or
  - effective column count changes (structural change)

### 2.3 Scoped Cache Invalidation

- In `CTableModel`, decode/projection caches are no longer cleared on every `modelChanged()` call.
- Cache clear now happens only when:
  - structural reset path is taken, or
  - row count actually changes
- This avoids repeated decode/projection invalidation during highlight-only updates.

### 2.4 SearchTableModel Incremental Refresh

- Reworked `CSearchTableModel::modelChanged()`:
  - emit full `dataChanged()` for current visible range when shape is stable
  - use reset fallback when columns change or at first notification
- Added tracked model state:
  - `m_lastKnownRowCount`
  - `m_lastKnownColumnCount`
- Synced tracked state in search result mutation APIs:
  - `clear_SearchResults()`
  - `add_SearchResultEntry()`
  - `add_SearchResultEntries()`

## 3. Why It Was Changed

Before Phase 4, hot paths (for example search focus/marker repaint and model update loops) used broad model refresh notifications, forcing more expensive view refresh behavior than required.

Phase 4 improves this by using row-level signals for common non-structural changes:

- Incremental notifications reduce broad relayout pressure
- Stable-shape updates use `dataChanged()` instead of layout invalidation
- Structural cases still have safe reset fallback
- Cache invalidation is better scoped to actual data-shape changes

Benefits:

- Better responsiveness during frequent repaint/update cycles
- Lower avoidable refresh overhead in table and search result views
- Clearer model notification semantics for future projection-adapter migration

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Load a medium/large log file.
- Run search and repeatedly navigate matches.
- Trigger row growth/shrink scenarios (reload/filter changes).

### Demo Flow

1. Invoke `modelChanged()` after a non-structural change (for example highlight-only update).
2. Model emits `dataChanged()` for the active visible range.
3. For appended/removed rows, model emits insert/remove row notifications.
4. For structural shape change (such as column count change), model uses reset fallback.

### Demo Behavior to Show

- Search navigation updates rows without broad layout refresh.
- Row append/remove updates are reflected incrementally.
- Structural cases still recover correctly via full reset.

## 5. Before vs After (At a Glance)

```text
Before:
[TableModel/SearchTableModel]
  |
  v
[modelChanged() -> layoutChanged()]
  |
  v
[Broad refresh in hot paths]

After:
[TableModel/SearchTableModel]
  |
  v
[modelChanged() -> row/data delta notifications]
  |
  v
[rowsInserted/rowsRemoved/dataChanged]
  |
  v
[Reset fallback only for structural invalidation]
```

## 6. Files Updated in Phase 4

- src/tablemodel.h
- src/tablemodel.cpp
- src/searchtablemodel.h
- src/searchtablemodel.cpp
