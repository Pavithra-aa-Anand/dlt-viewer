# Plugin Contract Hardening Implementation: What Changed, Why, and Demo (Phase 6)

## 1. Goal

Phase 6 formalizes and hardens plugin-stage contracts to improve determinism, thread safety, and safe decode behavior.

Main goals:

- Define explicit plugin stage boundaries (ingest/decode/enrich)
- Enforce safer decode-stage execution in shared runtime paths
- Reduce cross-thread decode re-entrancy risk for plugin code
- Add transactional decode semantics to avoid partial message mutation

## 2. What Changed

### 2.1 Stage Contract Structure

- Added a stage enum scaffold in plugin manager:
  - Ingest
  - Decode
  - Enrich
- This makes stage intent explicit for future extension and validation.

### 2.2 Decode Thread-Safety Hardening

- Added a dedicated decode-stage mutex in plugin manager.
- Decode execution now:
  - snapshots eligible decoder plugins under plugin list lock,
  - runs decode stage with serialized execution.
- This avoids concurrent decoder re-entrancy from parallel call paths.

### 2.3 Decode Context Normalization

- Normalized triggered-by-user decode context to strict 0/1 before plugin calls.
- This ensures consistent decode context semantics across callers.

### 2.4 Transactional Decode Semantics

- Decoder invocation in plugin wrapper is now transactional per plugin call:
  - run match and decode on a local message copy,
  - commit to the original message only when both steps succeed.
- If match fails or decode fails, the original message remains unchanged.

## 3. Why It Was Changed

Before this phase, decode behavior relied on plugin correctness under potentially parallel call paths, and failed decode paths could still expose mutation risk.

Phase 6 improves this by hardening plugin contracts at runtime:

- Serialized decode stage improves safety for plugins that are not fully thread-safe.
- Transactional message handling removes partial mutation side effects.
- Normalized context avoids inconsistent decode behavior based on caller conventions.

Benefits:

- More deterministic plugin decode behavior
- Reduced concurrency-related plugin instability risk
- Clearer contract boundary for plugin developers

## 4. Demo (Simple Walkthrough)

### Demo Setup

- Enable one or more decoder plugins.
- Trigger decode from table/search paths, including concurrent UI operations.

### Demo Flow

1. Plugin manager snapshots active decoder plugins.
2. Decode stage lock serializes plugin decode execution.
3. Each plugin evaluates and decodes using a local candidate message.
4. Candidate is committed only on successful match + decode.

### Demo Behavior to Show

- No partial message mutation when decode fails.
- Stable behavior under concurrent search/render activity.
- Priority-ordered first-success decoder behavior remains intact.

## 5. Before vs After (At a Glance)

```text
Before:
[Decode callers]
  |
  v
[Direct plugin decode on shared message]
  |
  v
[Potential concurrent decode re-entrancy + mutation risk]

After:
[Decode callers]
  |
  v
[Plugin manager decode stage]
  |
  v
[Serialized decode execution + normalized context]
  |
  v
[Transactional commit only on successful decode]
```

## 6. Files Updated in Phase 6

- qdlt/qdltpluginmanager.h
- qdlt/qdltpluginmanager.cpp
- qdlt/qdltplugin.cpp
- qdlt/decodecacheservice.cpp
- qdlt/qdltargument.cpp
