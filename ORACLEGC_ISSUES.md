# OracleGC Implementation Issues Analysis

## Executive Summary

After reviewing the current OracleGC implementation, I've identified several issues that need attention before experiments can run properly. The most critical are around **malloc mode safety** and **thread synchronization**.

---

## Issue Categories

| Severity | Issue | Status |
|----------|-------|--------|
| 🔴 CRITICAL | `is_in()` too permissive in malloc mode | Needs Fix |
| 🔴 CRITICAL | Missing CompressedOops enforcement | Needs Fix |
| 🟡 MODERATE | Thread safety in type index | Should Fix |
| 🟡 MODERATE | `object_iterate` broken in malloc mode | Should Fix |
| 🟢 MINOR | No coalescing in free list | Acceptable |
| ✅ OK | Object header initialization | Working Correctly |
| ✅ OK | Relative lifetime calculation | Fixed |

---

## CRITICAL Issues

### Issue 1: `is_in()` Too Permissive in Malloc Mode

**Location**: `epsilonHeap.hpp:89-104`

```cpp
bool is_in(const void* p) const override {
  if (_space->is_in(p)) {
    return true;
  }
  // PROBLEM: Returns true for ANY aligned non-metaspace pointer!
  if (_oracle_malloc_mode && p != nullptr && is_object_aligned(p)) {
    if (Metaspace::initialized() && Metaspace::contains(const_cast<void*>(p))) {
      return false;
    }
    return true;  // ← Too permissive!
  }
  return false;
}
```

**Problem**: The JVM uses `is_in()` to verify pointers during:
- Object field iteration
- Heap verification (`-XX:+VerifyBeforeGC`)
- JNI critical region handling
- Serviceability agent operations

Returning `true` for arbitrary aligned pointers could cause the JVM to treat random memory as valid objects, leading to crashes.

**Fix Needed**: Track malloc'd addresses in a data structure and only return `true` for known malloc allocations. The `_malloc_ptr_map` already exists for this purpose!

```cpp
bool is_in(const void* p) const override {
  if (_space->is_in(p)) {
    return true;
  }
  if (_oracle_malloc_mode && _oracle != nullptr) {
    // Only return true if we've actually malloc'd this address
    return _oracle->is_malloc_tracked(const_cast<void*>(p));
  }
  return false;
}
```

---

### Issue 2: Missing CompressedOops/CompressedClassPointers Enforcement

**Location**: `epsilon_globals.hpp:114-116`

```cpp
product(bool, EpsilonOracleMallocMode, false, EXPERIMENTAL,
        "Use actual malloc/free instead of simulated free list. "
        "Requires -XX:-UseCompressedOops -XX:-UseCompressedClassPointers")
```

**Problem**: The comment says these flags are required, but there's no runtime check. With compressed oops:
- Object references are stored as 32-bit offsets from heap base
- Malloc'd addresses (outside heap region) cannot be encoded as compressed oops
- This causes incorrect pointer storage → crashes

**Fix Needed**: Add validation in `epsilonHeap.cpp:initialize()`:

```cpp
if (EpsilonOracleMallocMode) {
  if (UseCompressedOops) {
    log_error(gc)("EpsilonOracleMallocMode requires -XX:-UseCompressedOops");
    return JNI_ERR;
  }
  if (UseCompressedClassPointers) {
    log_error(gc)("EpsilonOracleMallocMode requires -XX:-UseCompressedClassPointers");
    return JNI_ERR;
  }
}
```

---

## MODERATE Issues

### Issue 3: Thread Safety in Type Index

**Location**: `epsilonOracle.cpp:621-646` (`find_entry_by_type`)

```cpp
size_t EpsilonOracle::find_entry_by_type(const char* type, size_t size) {
  // ...
  if (bucket->next_available != nullptr) {
    size_t entry_idx = bucket->next_available->entry_idx;
    bucket->next_available = bucket->next_available->next;  // ← Not thread-safe!
    return entry_idx;
  }
}
```

**Problem**: Multiple threads could simultaneously:
1. Read the same `next_available` pointer
2. Both return the same entry index
3. Both advance the pointer, skipping an entry

This could cause one object to be freed twice or not at all.

**Current Mitigation**: The code comment in `register_allocation` says:
> "Since allocations are serialized through allocate_work, this is safe."

But this is **incorrect**! Even with `-XX:-UseTLAB`, multiple threads can call `allocate_work` concurrently. The `_space->par_allocate()` uses atomic CAS, but our type index doesn't.

**Fix Needed**: Add mutex or use atomic operations for type index updates.

---

### Issue 4: `object_iterate` Broken in Malloc Mode

**Location**: `epsilonHeap.cpp:461-463`

```cpp
void EpsilonHeap::object_iterate(ObjectClosure *cl) {
  _space->object_iterate(cl);
}
```

**Problem**: This only iterates objects in the bump-pointer space. In malloc mode, objects are allocated outside this region.

**Impact**:
- `jcmd <pid> GC.heap_dump` won't work
- Serviceability agents can't enumerate objects
- JFR object allocation events incomplete

**Fix**: Either track malloc'd objects for iteration, or document this limitation.

---

## MINOR Issues (Acceptable for Initial Testing)

### Issue 5: No Coalescing in Free List

**Location**: `epsilonOracle.cpp:422-435` (`add_to_free_list`)

```cpp
void EpsilonOracle::add_to_free_list(HeapWord* addr, size_t size) {
  // Simple insertion at head (no coalescing for now)
  OracleFreeBlock* block = NEW_C_HEAP_OBJ(OracleFreeBlock, mtGC);
  block->addr = addr;
  block->size = size;
  block->next = _free_list;
  _free_list = block;
  // ...
}
```

**Problem**: Adjacent freed blocks aren't merged, leading to fragmentation.

**Mitigation**: The paper used first-fit with no coalescing for their measurements too. This is actually consistent with their methodology.

---

## Confirmed WORKING

### Object Header Initialization ✅

**Analysis**: The `MemAllocator::finish()` method (in `memAllocator.cpp:405-417`) sets the mark word and klass pointer **after** the heap allocates memory. This means:

1. Our `allocate_work_oracle()` returns malloc'd memory (zeroed)
2. `MemAllocator::finish()` sets `mark` and `klass` on that memory
3. Object is properly initialized as a Java object

This is correct and matches the paper's approach.

### Relative Lifetime Calculation ✅

**Location**: `epsilonOracle.cpp:178-204`

The fix computes relative lifetime correctly:
```cpp
uint64_t lifetime = entry.free_at_seq - entry.alloc_seq;
replay_free_seq = alloc_seq + lifetime;
```

This handles the global clock mismatch when using type-based matching.

---

## Alignment with Paper's Methodology

### What the Paper Did (Section 2):

1. **Replaced GC allocator with malloc**: Every `new` bytecode calls `malloc()` instead of bump-pointer
2. **Used oracle to call free()**: Based on Merlin trace, called `free()` at exact moment object dies
3. **Measured with cycle-accurate simulator**: SimpleScalar counted malloc/free cycles, excluded oracle overhead

### Current Implementation Comparison:

| Paper's Approach | Our Implementation | Status |
|------------------|-------------------|--------|
| malloc for every allocation | ✅ `EpsilonOracleMallocMode=true` uses `permit_forbidden_function::malloc()` | Working |
| free at oracle time | ✅ `process_deaths_malloc_mode()` calls `permit_forbidden_function::free()` | Working |
| Cycle-accurate simulation | 🔴 Need to run in gem5 | Not Yet |
| Exclude oracle overhead | 🔴 Oracle lookups are included in time | Need gem5 |
| Type-based matching | ✅ Implemented | Working |

### Key Difference: No Simulator Integration Yet

The paper's key insight was to use a simulator to **subtract** the oracle overhead. We can achieve similar by:
1. Running in gem5 (cycle-accurate)
2. Measuring oracle function time separately
3. Subtracting from total runtime

---

## Recommended Fix Priority

1. **Add CompressedOops validation** (5 min fix, prevents crashes)
2. **Fix `is_in()` for malloc mode** (30 min fix, use existing `_malloc_ptr_map`)
3. **Add mutex to type index operations** (30 min fix)
4. **Document/fix object_iterate** (optional for initial testing)

---

## Test Plan After Fixes

1. **Simple test** (TestProgram with 73 objects):
   ```bash
   ./build/*/images/jdk/bin/java \
       -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
       -XX:+EpsilonOracleMode -XX:-UseTLAB \
       -XX:EpsilonOracleTracePath=../testprogram_liveness.csv \
       TestProgram
   ```

2. **Malloc mode test**:
   ```bash
   ./build/*/images/jdk/bin/java \
       -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
       -XX:+EpsilonOracleMode -XX:+EpsilonOracleMallocMode \
       -XX:-UseCompressedOops -XX:-UseCompressedClassPointers \
       -XX:-UseTLAB \
       -XX:EpsilonOracleTracePath=../testprogram_liveness.csv \
       TestProgram
   ```

3. **DaCapo lusearch**:
   ```bash
   ./build/*/images/jdk/bin/java \
       -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
       -XX:+EpsilonOracleMode -XX:+EpsilonOracleMallocMode \
       -XX:-UseCompressedOops -XX:-UseCompressedClassPointers \
       -XX:-UseTLAB -Xlog:gc=info \
       -XX:EpsilonOracleTracePath=../lusearch_fresh_liveness.csv \
       -jar ../benchmarks/dacapo-23.11-chopin.jar lusearch -s small
   ```
