# OracleGC Implementation Issues

This document tracks known issues, bugs, and design problems in the current OracleGC implementation.

**Related**: See `ORACLE_DETERMINISM_TODO.md` for the broader determinism problem and proposed solutions.

---

## Critical Issues (P0)

### Issue 1: Thread Safety Race Condition

**Location**: `epsilonOracle.cpp` lines 536 and 155

**Code**:
```cpp
// In matches_expected_entry() - sets shared variable
const_cast<EpsilonOracle*>(this)->_last_matched_entry_idx = entry_idx;

// In register_allocation() - reads shared variable
size_t idx = _last_matched_entry_idx;
```

**Problem**: `_last_matched_entry_idx` is a single shared variable used to pass state between `matches_expected_entry()` and `register_allocation()`. With multiple threads:

```
Thread A: matches_expected_entry() sets _last_matched_entry_idx = 5
Thread B: matches_expected_entry() sets _last_matched_entry_idx = 7
Thread A: register_allocation() reads _last_matched_entry_idx = 7  ← WRONG!
```

**Impact**: Wrong trace entry gets registered, causing incorrect free timing.

**Fix Options**:
1. Use thread-local storage for `_last_matched_entry_idx`
2. Pass entry index through the call chain (return value or out parameter)
3. Combine matching and registration into single atomic operation

---

### Issue 2: Global Sequence Numbers Are Non-Deterministic

**Location**: `epsilonOracle.cpp`, `epsilonHeap.cpp`

**Problem**: The death_map is keyed by `free_at_seq`, a global sequence number:

```cpp
// Oracle trace says: free object when global allocation count reaches N
death_map[free_at_seq] = {ptr, size, ...}

// At runtime, process_deaths checks:
void process_deaths(uint64_t current_seq) {
  // Free all objects where free_at_seq == current_seq
}
```

In multi-threaded programs, allocation order varies between runs. The `free_at_seq` from the trace run won't match the allocation sequence in the measurement run.

**Impact**: Objects freed at wrong times - potentially freeing live objects or keeping dead objects too long.

**Fix**: See `ORACLE_DETERMINISM_TODO.md` for per-thread clock proposal.

---

## High Priority Issues (P1)

### Issue 3: Type-Based Matching Matches Wrong Objects

**Location**: `epsilonOracle.cpp` - `find_entry_by_type()`

**Current Approach**:
```cpp
// Find next available entry matching (type, size)
size_t find_entry_by_type(const char* type, size_t size) {
  // Returns entries in TRACE order, not runtime order
  bucket->next_available = bucket->next_available->next;
  return entry_idx;
}
```

**Problem**: When multiple objects have the same type and size but different lifetimes:

```
Trace:
  Entry 0: Node, size=24, free_at_seq=5    (short-lived)
  Entry 1: Node, size=24, free_at_seq=100  (long-lived)

Runtime allocates in different order:
  Alloc 1: Node (should be long-lived) → matches Entry 0 → freed at seq 5!
  Alloc 2: Node (should be short-lived) → matches Entry 1 → freed at seq 100!
```

**Impact**: Short-lived objects kept alive too long, long-lived objects freed prematurely.

**Fix Options**:
1. Use allocation call site (stack trace) as additional matching key
2. Use thread ID + per-thread sequence as matching key
3. Accept limitation: only works for programs with unique (type, size) combinations

---

### Issue 4: No Verification of Trace/Runtime Correspondence

**Location**: Throughout oracle implementation

**Problem**: No mechanism to detect when trace and runtime diverge:
- Object identity from trace (`obj_id`) is not used for verification
- No checksums or sanity checks
- If wrong objects are matched, program continues silently with corrupted state

**Impact**: Silent data corruption, incorrect experimental results.

**Fix Options**:
1. Track allocation patterns and detect statistical divergence
2. Use obj_id for verification (hash of object identity)
3. Add "canary" allocations with known patterns to detect drift
4. Log warnings when type/size matches but count seems wrong

---

### Issue 5: Untracked Allocations May Cause Sequence Drift

**Location**: `epsilonHeap.cpp` - `allocate_work_oracle()`

**Code**:
```cpp
if (!_oracle->matches_expected_entry(size_in_bytes, type_name)) {
  // UNTRACKED - doesn't increment trace_seq
  return bump_allocate();
}
// TRACKED - increments trace_seq
uint64_t trace_seq = _oracle->next_app_alloc_seq();
```

**Problem**: The number of untracked allocations (JVM internal: Class objects, String constants, etc.) may differ between trace and measurement runs due to:
- Different JIT compilation timing
- Different class loading order
- Different GC behavior during trace collection

If trace has 100 untracked allocations before app starts, but runtime has 105, all sequence numbers will be offset by 5.

**Impact**: All deaths processed at wrong times.

**Fix Options**:
1. Use type-based matching only (current approach, but has Issue 3)
2. Track ALL allocations (including internal) in trace
3. Use synchronization points (e.g., "main() started") to reset counters

---

## Medium Priority Issues (P2)

### Issue 6: Malloc Mode `is_in()` Check Too Permissive

**Location**: `epsilonHeap.hpp` - `is_in()`

**Status**: Partially fixed, needs more work

**Problem**: In malloc mode, we modified `is_in()` to return true for malloc'd pointers. But the check was too broad, returning true for metaspace pointers (Klass*) which broke assertions.

**Current State**: Build was interrupted during fix. Need to:
1. Complete the metaspace exclusion fix
2. Test thoroughly
3. Consider tracking malloc'd pointers explicitly instead of broad heuristic

---

### Issue 7: Hash Collision in Death Map

**Location**: `epsilonOracle.cpp` - death_map

**Code**:
```cpp
static const size_t DEATH_MAP_SIZE = 1 << 20;  // 1M buckets
size_t hash_seq(uint64_t seq) const {
  return (size_t)(seq & (DEATH_MAP_SIZE - 1));  // Simple modulo hash
}
```

**Problem**: Simple modulo hash with 1M buckets. For long-running programs with millions of allocations:
- Multiple sequences map to same bucket
- Linear scan through bucket chain
- Performance degrades

**Impact**: Performance issue, not correctness.

**Fix**: Use better hash function or resize dynamically.

---

## What's Working Correctly

| Component | Status | Notes |
|-----------|--------|-------|
| Overall flow (oracle → free → alloc → register) | ✓ | Correct methodology |
| Death map structure | ✓ | O(1) average lookup |
| Free list for memory reuse | ✓ | First-fit allocation |
| Malloc mode calling actual free() | ✓ | Uses `permit_forbidden_function::free` |
| Disabling TLABs | ✓ | All allocations go through `allocate_work()` |
| Type index for matching | ✓ | Efficient (type, size) lookup |
| Grace period support | ✓ | `EpsilonOracleGracePeriod` flag |

---

## Fix Priority Matrix

| Issue | Severity | Effort | Dependencies |
|-------|----------|--------|--------------|
| #1 Thread safety | Critical | Low | None |
| #2 Global sequences | Critical | High | Requires trace format change |
| #3 Type matching | High | Medium | May need #2 first |
| #4 No verification | High | Medium | None |
| #5 Sequence drift | High | Medium | Related to #2 |
| #6 is_in() check | Medium | Low | None |
| #7 Hash collisions | Low | Low | None |

---

## Recommended Fix Order

### Phase 1: Quick Fixes (Do Now)
- [ ] Fix #1: Thread safety with thread-local storage
- [ ] Fix #6: Complete is_in() metaspace exclusion

### Phase 2: Validation Infrastructure
- [ ] Fix #4: Add divergence detection and warnings
- [ ] Create test suite with known-good traces

### Phase 3: Determinism (Major Change)
- [ ] Fix #2: Implement per-thread clocks
- [ ] Fix #3: Update matching to use (thread, local_seq)
- [ ] Fix #5: Align trace collection with new scheme

### Phase 4: Polish
- [ ] Fix #7: Improve hash function if needed
- [ ] Performance optimization
- [ ] Documentation

---

## Testing Checklist

Before considering implementation "correct":

- [ ] Single-threaded program produces identical behavior across runs
- [ ] All tracked allocations have corresponding frees
- [ ] `alloc_count == free_count` at program end
- [ ] No crashes or assertions
- [ ] Memory usage matches expected pattern (liveness oracle should use less memory)
- [ ] Multi-threaded program... (requires Phase 3 fixes)
