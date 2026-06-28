
## Scope and Platform

This consolidated guide focuses on two primary tools:
- `perf` for CPU performance measurement
- `heaptrack` for heap memory measurement

`perf` is a low-overhead Linux profiler for sampling CPU usage, call stacks, hardware counters,and hotspots while the application runs. 

`heaptrack` records heap allocations over time so you can identify which code paths are responsible for memory growth, high allocation churn, and peak heap use.

`perf` and `heaptrack` are Linux-native tools. If your main environment is Windows, run this
workflow in WSL2 (Ubuntu recommended) or on a Linux machine/VM.

---

## 1. Install and Permissions

Install tools (choose your distro):

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y linux-perf heaptrack heaptrack-gui
```

Verify tool availability:

```bash
perf --version
heaptrack --version
```

If `perf` reports permission issues:

```bash
sudo sysctl -w kernel.perf_event_paranoid=1
sudo sysctl -w kernel.kptr_restrict=0
```

To make this persistent across reboots:

```bash
echo "kernel.perf_event_paranoid=1" | sudo tee -a /etc/sysctl.conf
echo "kernel.kptr_restrict=0" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

---

## 2. `perf` Use Cases

Use `perf` when the implementation change may affect CPU time, hotspots, call stacks, cache
behavior, or event throughput.

| Use case | Feature under test | Goal | What to record |
|---|---|---|---|
| Idle baseline | Application startup with no file loaded | Confirm background CPU cost stays negligible | CPU%, top hot functions, context switches |
| Default project startup | Startup with default project, filters, and plugins enabled | Measure initialization overhead from persisted state restore | Startup wall time, CPU%, hot startup functions |
| Single file open | `.dlt` file parsing and initial model population | Measure CPU cost of loading one large file | Wall time, CPU%, `perf top`, `perf record` call stacks |
| Multi-file open | Loading multiple `.dlt` files together | Measure merge and index overhead when combining traces | Wall time, CPU%, hottest functions during load |
| Drag-and-drop open | Dragging one or more files into the viewer | Confirm alternate open path has no extra CPU overhead | Wall time, CPU%, same hotspots as standard open |
| Append file | Appending an additional file to an already loaded session | Measure incremental index and merge cost | Append time, CPU%, hot indexing functions |
| Cached reopen | Reopening a file that already has an index cache | Validate CPU reduction from cached metadata reuse | Reopen time, CPU%, reduced parser hotspots |
| Simple filter apply | One include filter with exact match fields | Measure base filter engine cost | Filter apply time, CPU%, hot filter functions |
| Complex filter apply | Multiple filters with regex, markers, and grouping rules | Measure worst-case filter evaluation cost | Wall time, CPU%, regex and matcher hotspots |
| Search next | Step-by-step search navigation | Check CPU cost per interactive search step | Per-step latency, CPU%, hot search functions |
| Search find-all | Search dialog full scan | Measure CPU cost for searching across the full dataset | Search wall time, CPU%, hot search functions |
| Search export view | Building and browsing the search result list | Measure cost of materializing result rows | Build time, CPU%, hot result-model functions |
| Table scroll | Table rendering and per-row decode on repaint | Detect expensive render and decode work while scrolling | CPU% during 10 s scroll, repeated hot call stacks |
| Sorting | Sorting by time or other visible columns | Measure proxy model and compare cost on large datasets | Sort wall time, CPU%, hot compare and remap functions |
| Auto-scroll live view | Keeping the table pinned to the latest message during streaming | Measure repaint and update pressure in live mode | CPU%, repaint hotspots, event rate |
| CRLF window open | CRLF extraction and window materialization | Check whether CRLF preparation is CPU-heavy | Open time, CPU%, hot allocation and render functions |
| TCP live logging | Single TCP connection with sustained incoming traffic | Measure sustained CPU cost under live network ingest | CPU% during 60 s, hottest update and index functions |
| Serial or UDP live logging | Alternate transport connection modes | Compare ingest cost across supported transports | CPU%, transport-specific hotspots |
| Multiple parallel connections | More than one active connection at the same time | Measure scheduling and merge overhead across streams | CPU%, hot connection and queue functions |
| Autoconnect and reconnect | Automatic connect and reconnect handling | Ensure retry logic does not create CPU spikes | CPU%, hot reconnect paths |
| ECU, app, and context refresh | Fetching target state and populating project tree | Measure control-path overhead for configuration reads | Response latency, CPU%, hot control functions |
| Markers and highlights | Applying markers to highlight specific messages | Measure CPU cost of marker evaluation and redraw | Apply time, CPU%, hot model update functions |
| Column selection and layout restore | Enabling columns and restoring saved UI layout | Check CPU cost of table reconfiguration | Apply time, CPU%, view and header hotspots |
| Project save and load | Saving and reloading `.dlp` projects | Measure serialization and restore cost | Save/load time, CPU%, hot project functions |
| Export ASCII or UTF-8 text | Export pipeline in viewer or `dlt-commander` | Measure conversion cost for text export | Export wall time, CPU%, IPC, cache-miss rate |
| Export CSV | CSV conversion with delimiter handling | Measure structured text export cost | Export wall time, CPU%, exporter hotspots |
| Export filtered or marked messages | Export of filtered or user-marked subsets | Measure selection and write cost on reduced datasets | Wall time, CPU%, selector and exporter hotspots |
| Export DLT or decoded DLT | Filtered re-save or decoded DLT export | Measure CPU cost of writing transformed logs | Wall time, CPU%, hot exporter functions |

To record `perf` commands:

Per-use-case capture details (example: Search find-all):

```bash
# Terminal A: get PID of running dlt-viewer
pgrep dlt-viewer
```

Finds the process ID (PID) of the running `dlt-viewer` app. Example output: `12345`.
You need this PID so `perf` knows which running process to observe.

```bash
# Terminal A: start summary counters
perf stat -d -p <PID> -o perf_findall_stat.txt
```

Collects CPU counter summary for that process. It reports totals such as cycles,
instructions, IPC, cache references, cache misses, and branch statistics.

- `-d` means detailed counters.
- `-p` means attach to that PID.
- `-o` writes results to a text file.

Run this while performing one use case, then stop with `Ctrl+C`.

```bash
# Terminal B: start hotspot sampling
perf record -g --call-graph dwarf -p <PID> -o perf_findall.data

# Stop CPU capture after the scenario completes
# In Terminal A and Terminal B, press Ctrl+C

# Generate report from recorded data
perf report -i perf_findall.data
```

Records sampled performance data for hotspot analysis.

- `-g` plus `--call-graph dwarf` captures call stacks (who called whom).
- The output is a data file used later by `perf report`.

This tells you where CPU time is spent in functions, not just summary counters.

---

## 3. `heaptrack` Use Cases

Use `heaptrack` when the implementation change may affect heap growth, allocation churn, object lifetime, or retained memory after a feature finishes.

| Use case | Feature under test | Goal | What to record |
|---|---|---|---|
| Idle baseline | Application startup with no file loaded | Confirm base heap footprint is stable | Peak heap, live heap after settle, top allocators |
| Default project startup | Startup with saved project, filters, and plugins restored | Detect extra retained state loaded at startup | Peak heap after settle, top retained allocators |
| Single file open | `.dlt` file parsing and initial data structures | Measure retained heap after one large file load | Peak heap, retained heap, biggest allocation sites |
| Multi-file open | Loading multiple `.dlt` files together | Detect duplicated storage and merge-related growth | Peak heap, retained heap, allocation hotspots |
| Drag-and-drop open | Alternate file open path | Confirm drag-and-drop does not allocate additional long-lived state | Peak heap, retained allocators after settle |
| Append file | Appending a second file to an active session | Measure incremental heap growth from merge and reindex | Heap delta, retained allocations after append |
| Cached reopen | Reopening a file with existing index cache | Verify cached reopen avoids repeated heavy allocations | Peak heap, reduced parser-side allocations |
| Simple filter apply | Filter result structures for one exact-match filter | Check base memory cost of filtered projections | Heap delta after apply, retained result allocations |
| Complex filter apply | Regex filters, markers, and grouped filtering | Detect duplicated vectors and temporary regex-related growth | Peak heap, retained allocations after settle |
| Search next | Step-by-step interactive search | Confirm repeated next/previous operations do not leak results | Allocation churn per step, retained heap after repeated use |
| Search find-all | Search result storage | Measure memory growth from result lists and temporary matches | Peak heap during search, retained heap after completion |
| Search export view | Search result model and row materialization | Detect large retained search-model structures | Peak heap spike, retained result-view allocations |
| Table scroll | Render-time temporary allocations | Confirm scrolling does not cause heavy allocation churn | Allocation rate during scroll, temporary hot allocators |
| Sorting | Sorting and proxy remap structures | Measure extra heap needed for large sorted views | Heap delta during sort, retained proxy allocations |
| Auto-scroll live view | Table updates during active streaming | Detect allocation churn from repeated UI refreshes | Allocation rate, retained growth during 60 s run |
| CRLF window open | CRLF extraction and secondary model creation | Detect large one-shot allocations when opening CRLF view | Peak heap spike, largest retained object groups |
| TCP live logging | Continuous ingest through one TCP connection | Measure steady heap growth under message arrival | Growth rate over 60 s, retained allocations by type |
| Serial or UDP live logging | Alternate transport connection modes | Compare heap behavior across transport implementations | Peak heap, retained transport-side buffers |
| Multiple parallel connections | More than one live target connection | Detect per-connection buffer duplication and queue growth | Heap growth rate, retained connection objects |
| Autoconnect and reconnect | Automatic reconnect handling | Ensure retries do not leak sockets, buffers, or state objects | Retained heap after repeated reconnect cycles |
| ECU, app, and context refresh | Control-path state population in the project tree | Measure heap cost of target metadata materialization | Heap delta, retained tree-model allocations |
| Markers and highlights | Highlight lists and marker metadata | Measure retained memory from user markers | Heap delta, retained marker objects |
| Column selection and layout restore | Table configuration and saved layout application | Confirm UI customization does not retain stale models | Heap delta, retained view and header allocations |
| Project save and load | Saving and loading `.dlp` projects | Measure memory cost of project serialization and restore | Peak heap, retained project-state allocations |
| Export ASCII or UTF-8 text | Export buffers and conversion temporaries | Check whether text export creates avoidable large temporary buffers | Peak heap during export, allocations by exporter path |
| Export CSV | CSV row assembly and buffering | Measure memory growth from delimiter and string assembly | Peak heap, retained exporter allocations |
| Export filtered or marked messages | Export of subsets selected by filters or markers | Check whether subset export duplicates selection storage | Peak heap, retained selector allocations |
| Export DLT or decoded DLT | Save filtered or decoded DLT output | Measure retained and temporary memory during write path | Peak heap, exporter-related allocation sites |

To record `heaptrack` commands:

```bash
# Start dlt-viewer under heaptrack before running the scenario
heaptrack --output heap_findall.gz ./build-asan/bin/dlt-viewer

# After scenario completion and app close, export text summary
heaptrack_print heap_findall.gz > heap_findall.txt

# Optional: open the interactive GUI report
heaptrack_gui heap_findall.gz
```
---

## 4. Feature Coverage Matrix

Use this checklist to ensure each implementation area is covered by the right tool.

| Feature area | `perf` | `heaptrack` |
|---|---|---|
| Idle startup | Yes | Yes |
| Default project startup | Yes | Yes |
| Single file open | Yes | Yes |
| Multi-file open | Yes | Yes |
| Drag-and-drop open | Yes | Yes |
| Append file | Yes | Yes |
| Cached reopen | Yes | Yes |
| Simple filter apply | Yes | Yes |
| Complex filter and regex apply | Yes | Yes |
| Search next | Yes | Yes |
| Search find-all | Yes | Yes |
| Search export view | Yes | Yes |
| Table scroll and render | Yes | Yes |
| Sorting | Yes | Yes |
| Auto-scroll live view | Yes | Yes |
| CRLF window | Yes | Yes |
| TCP live logging | Yes | Yes |
| Serial or UDP live logging | Yes | Yes |
| Multiple parallel connections | Yes | Yes |
| Autoconnect and reconnect | Yes | Yes |
| ECU, app, and context refresh | Yes | Yes |
| Markers and highlights | Yes | Yes |
| Column selection and layout restore | Yes | Yes |
| Project save and load | Yes | Yes |
| Export ASCII or UTF-8 | Yes | Yes |
| Export CSV | Yes | Yes |
| Export filtered or marked messages | Yes | Yes |
| Export DLT or decoded DLT | Yes | Yes |

## 5. Steps to Perform Per Use Case to Capture Data

1. Start a clean run.
2. Close any running `dlt-viewer` process.
3. Open your test file and keep everything ready, but do not click find-all yet.
4. Start CPU capture first.
5. In Terminal A, get PID:

```bash
pgrep dlt-viewer
```

6. In Terminal A, start summary counters:

```bash
perf stat -d -p <PID> -o perf_findall_stat.txt
```

7. In Terminal B, start hotspot sampling:

```bash
perf record -g --call-graph dwarf -p <PID> -o perf_findall.data
```

8. Run the use case in the app (Search find-all).
9. Wait until find-all completes.
10. Stop CPU capture by pressing `Ctrl+C` in both perf terminals.
11. Generate report:

```bash
perf report -i perf_findall.data
sudo perf report -i perf_findall.data --stdio > file.txt 2>&1
```

12. Capture heap data in a separate run.
13. Start app through `heaptrack` before the action:

```bash
heaptrack --output heap_findall.gz ./build-asan/bin/dlt-viewer
```

14. Repeat only the find-all scenario.
15. Close app after scenario.
16. Export heap summary:

```bash
heaptrack_print heap_findall.gz > heap_findall.txt
```

17. Optional GUI view:

```bash
heaptrack_gui heap_findall.gz
```

What you get:

- CPU: `perf_findall_stat.txt` plus `perf report` hot functions.
- Heap: `heap_findall.gz` plus `heap_findall.txt` (peak heap, alloc hotspots, retained memory).

