# Oracle-based Explicit Memory Management in EpsilonGC

## Overview

Modified HotSpot's EpsilonGC to implement oracle-based explicit memory management (malloc/free) based on pre-computed traces from Elephant Tracks, following the Hertz & Berger 2005 paper methodology.

## Key Implementation: Type-Based Matching

**Problem**: Allocations occur in different order between trace generation (Elephant Tracks with instrumentation) and replay (Oracle GC without instrumentation). Sequential matching by allocation number doesn't work.

**Solution**: Match allocations by `(type, size)` tuple instead of sequence number.

### Data Structures (in `epsilonOracle.hpp`)

```cpp
// Type index: hash table mapping (type, size) -> list of entry indices
TypeIndexBucket** _type_index;  // 64K buckets

struct TypeIndexBucket {
  char type[128];           // Type name (key part 1)
  size_t size;              // Object size (key part 2)
  TypeIndexNode* entries;   // Linked list of matching entries (in trace order)
  TypeIndexNode* next_available;  // Pointer to next unmatched entry
  TypeIndexBucket* next;    // Next bucket in hash chain
};
```

### Key Functions (in `epsilonOracle.cpp`)

- `build_type_index()` - Called after loading trace, builds the hash table
- `hash_type(type, size)` - Hash function for type index
- `find_entry_by_type(type, size)` - Returns next available entry index for given type/size
- `matches_expected_entry(size, type)` - Returns true if allocation matches trace

## Critical Bug Fixed

**File**: `src/hotspot/share/gc/epsilon/epsilonHeap.cpp`

**Problem**: Lines 234-243 had an `_app_started` check that blocked ALL allocation tracking:

```cpp
// REMOVED - This blocked all tracking when no JVMTI agent present
if (!_oracle->app_started()) {
  return result;  // Always returned early!
}
```

**Why it failed**: The `_app_started` flag was only set by a JVMTI agent calling `signal_app_start()`. Without the agent, this check caused the oracle to track 0 allocations.

**Fix**: Removed the entire `_app_started` mechanism. The `matches_expected_entry()` function already filters JVM internal allocations by checking if `(type, size)` exists in the trace.

## Configuration Flags (in `epsilon_globals.hpp`)

| Flag | Type | Description |
|------|------|-------------|
| `EpsilonOracleMode` | bool | Enable oracle mode |
| `EpsilonOracleTracePath` | ccstr | Path to oracle trace CSV |
| `EpsilonOracleValidate` | bool | Validate sizes against trace |
| `EpsilonOracleSkipAllocs` | uint64_t | Skip N allocations before tracking |
| `EpsilonOracleGracePeriod` | uint64_t | Delay freeing by N allocations |

## Test Files

| File | Location | Description |
|------|----------|-------------|
| TestProgram.java | `corretto-25/TestProgram.java` | Simple test program |
| Test trace | `corretto-25/testprogram_liveness.csv` | 73 entries |
| Lusearch trace | `lusearch_fresh_liveness.csv` | 973,981 entries |

## What To Do Next

### 1. Test on TestProgram

```bash
cd /Users/luka/Desktop/Honors_Thesis/revisit_quantifying_gc/corretto-25

# Compile
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/javac TestProgram.java

# Run with Oracle GC
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
  -XX:+UnlockExperimentalVMOptions \
  -XX:+UseEpsilonGC \
  -XX:+EpsilonOracleMode \
  -XX:EpsilonOracleTracePath=/Users/luka/Desktop/Honors_Thesis/revisit_quantifying_gc/corretto-25/testprogram_liveness.csv \
  -Xlog:gc \
  TestProgram
```

**Expected**: Should now see "Tracked app allocs: 73" instead of 0.

### 2. Test on Lusearch Benchmark

After TestProgram works, test on lusearch with the 973K entry trace.

### 3. If Still Not Working

- Check log output for "Type index built with X unique (type, size) combinations"
- Verify type names match between trace and allocation (e.g., `java.lang.Object` vs `java/lang/Object`)
- Add debug logging in `matches_expected_entry()` to see what's being compared

## Key Source Files

| File | Purpose |
|------|---------|
| `src/hotspot/share/gc/epsilon/epsilonHeap.cpp` | Main allocation logic |
| `src/hotspot/share/gc/epsilon/epsilonOracle.hpp` | Oracle data structures |
| `src/hotspot/share/gc/epsilon/epsilonOracle.cpp` | Oracle implementation |
| `src/hotspot/share/gc/epsilon/epsilon_globals.hpp` | Configuration flags |

## Build Commands

```bash
cd /Users/luka/Desktop/Honors_Thesis/revisit_quantifying_gc/corretto-25

# Rebuild after changes
make images

# Build output location
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java
```

## Trace File Format

CSV format: `alloc_seq,free_at_seq,size,type,obj_id`

Example from `testprogram_liveness.csv`:
```
alloc_seq,free_at_seq,size,type,obj_id
1,2,24,java.util.ArrayList,357863579
2,3,16,java.lang.Object,114132791
52,54,24,Node,1007251739
72,73,416,int[],981361423
```

5 unique (type, size) combinations in test trace:
- `java.util.ArrayList` / 24 bytes (1 entry)
- `java.lang.Object` / 16 bytes (50 entries)
- `Node` / 24 bytes (20 entries)
- `int[]` / 416 bytes (1 entry)
- `java.lang.String[]` / 216 bytes (1 entry)
