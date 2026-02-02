# OracleGC Implementation Status

> **Last Updated**: January 2025
> **Purpose**: Comprehensive status report of OracleGC implementation in Amazon Corretto 25
> **Goal**: Reproduce Hertz & Berger 2005 methodology for comparing GC vs explicit memory management

---

## Overview

OracleGC modifies Corretto 25's **EpsilonGC** to support oracle-based explicit memory management. Instead of garbage collection, objects are freed at exactly the moment specified by a pre-computed oracle trace (generated from Elephant Tracks profiling runs).

This implements the methodology from:
> Hertz & Berger, "Quantifying the Performance of Garbage Collection vs. Explicit Memory Management", OOPSLA 2005

---

## Implementation Status Summary

| Component | Status | Notes |
|-----------|--------|-------|
| Core Oracle Infrastructure | ✅ Complete | CSV loading, death map, type index |
| Type-Based Allocation Matching | ✅ Complete | Handles all class/array name formats |
| Free List Allocator | ✅ Complete | First-fit with block splitting |
| Malloc Mode | ✅ Complete | Real malloc/free for measurement |
| Single-Threaded Programs | ✅ Working | TestProgram passes (73/73 objects freed) |
| Multi-Threaded Programs | ⚠️ TODO | Documented in ORACLE_DETERMINISM_TODO.md |
| DaCapo Benchmark Testing | 🔄 Pending | Ready to test with real traces |

---

## Files Changed/Created

### New Files (OracleGC-specific)

| File | Path | Size | Purpose |
|------|------|------|---------|
| `epsilonOracle.hpp` | `src/hotspot/share/gc/epsilon/` | 8.5 KB | Oracle class definitions, data structures |
| `epsilonOracle.cpp` | `src/hotspot/share/gc/epsilon/` | 27 KB | Full oracle implementation |

### Modified Files

| File | Path | Changes |
|------|------|---------|
| `epsilonHeap.hpp` | `src/hotspot/share/gc/epsilon/` | Added `_oracle`, `_oracle_allocated_bytes`, `_oracle_malloc_mode` members |
| `epsilonHeap.cpp` | `src/hotspot/share/gc/epsilon/` | Added `allocate_work_oracle()`, initialization, finalization |
| `epsilon_globals.hpp` | `src/hotspot/share/gc/epsilon/` | New configuration flags (6 flags) |
| `epsilonArguments.cpp` | `src/hotspot/share/gc/epsilon/` | Oracle mode validation and setup |
| `epsilonThreadLocalData.hpp` | `src/hotspot/share/gc/epsilon/` | Added `_current_alloc_klass` for type tracking |
| `memAllocator.cpp` | `src/hotspot/share/gc/shared/` | Sets Klass before allocation (4 lines) |

### Documentation Files

| File | Purpose |
|------|---------|
| `ORACLEGC_STATUS.md` | This file - comprehensive status |
| `ORACLE_IMPLEMENTATION_COMPLETE.md` | Detailed implementation report |
| `ORACLE_GC_IMPLEMENTATION.md` | Technical implementation guide |
| `ORACLE_DETERMINISM_TODO.md` | Multi-threading challenges and solutions |
| `ORACLE_IMPLEMENTATION_STATUS.md` | Earlier status (superseded) |
| `ImportantInfo.md` | Paper methodology reference |
| `CLAUDE.md` | Build instructions |

### Test Files

| File | Description |
|------|-------------|
| `TestProgram.java` | Simple test: ArrayList, Object, Node class, arrays |
| `testprogram_liveness.csv` | Trace file: 73 entries for TestProgram |

---

## Configuration Flags

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `EpsilonOracleMode` | bool | false | Enable oracle-based memory management |
| `EpsilonOracleTracePath` | string | null | **Required**: Path to oracle CSV file |
| `EpsilonOracleValidate` | bool | false | Validate allocation sizes against trace |
| `EpsilonOracleGracePeriod` | uint64 | 0 | Delay freeing by N allocations |
| `EpsilonOracleMallocMode` | bool | false | Use real malloc/free (not free list) |
| `EpsilonOracleSkipAllocs` | uint64 | 0 | Skip N allocations (deprecated) |

**Required JVM flags**:
- `-XX:-UseTLAB` (always required for deterministic tracking)
- `-XX:-UseCompressedOops -XX:-UseCompressedClassPointers` (only for malloc mode)

---

## Key Data Structures

### Oracle Entry (from trace file)
```cpp
struct OracleEntry {
    uint64_t alloc_seq;      // Allocation sequence in trace
    uint64_t free_at_seq;    // When to free this object
    size_t   size;           // Object size in bytes
    char     type[128];      // Class name (e.g., "java.util.ArrayList")
};
```

### Type Index (for allocation matching)
```cpp
struct TypeIndexBucket {
    char type[128];              // Normalized type name
    size_t size;                 // Object size
    TypeIndexNode* entries;      // All trace entries with this (type, size)
    TypeIndexNode* next_available; // Next unmatched entry
    TypeIndexBucket* next;       // Hash chain
};
```

### Death Map (for deallocation scheduling)
```cpp
struct DeathBucket {
    uint64_t free_at_seq;    // Sequence number
    HeapWord* ptr;           // Object pointer
    size_t size;             // Object size
    DeathBucket* next;       // Chain of objects dying at same seq
};
```

---

## Trace File Format

```csv
alloc_seq,free_at_seq,size,type,obj_id
1,2,24,java.util.ArrayList,357863579
2,3,16,java.lang.Object,114132791
52,54,24,Node,1007251739
72,73,416,int[],981361423
```

- **alloc_seq**: Order of allocation in profiling run
- **free_at_seq**: Sequence number at which to free
- **size**: Object size in bytes
- **type**: Class name (readable format)
- **obj_id**: Object ID from Elephant Tracks (for debugging)

---

## How It Works

### Allocation Flow

1. **Pre-allocation** (`memAllocator.cpp`):
   - Stores `Klass*` in thread-local storage

2. **Allocation** (`epsilonHeap.cpp::allocate_work_oracle`):
   - Retrieves Klass from thread-local storage
   - Looks up `(type, size)` in type index
   - **If match found**:
     - Gets trace entry, processes pending deaths
     - Allocates from heap or free list
     - Registers object in death map
   - **If no match**:
     - JVM internal allocation (not tracked)
     - Bump-pointer allocation only

3. **Deallocation** (`epsilonOracle.cpp::process_deaths`):
   - Called at each tracked allocation
   - Frees all objects scheduled for current sequence
   - Adds freed memory to free list (or calls `free()` in malloc mode)

4. **Shutdown** (`epsilonHeap.cpp::print_tracing_info`):
   - Calls `oracle->finalize()` to free remaining objects
   - Prints statistics

### Type-Based Matching (Critical Design Decision)

**Problem**: Allocations occur in different order during trace generation vs replay due to:
- JVM internal allocations interleaved with application allocations
- Elephant Tracks instrumentation changes timing
- Multi-threading causes non-determinism

**Solution**: Match by `(type, size)` instead of sequence number
- Build index of all `(type, size)` combinations from trace
- Each combination has ordered list of entries
- Consume entries in FIFO order per type

**Type Normalization**:
- `java.lang.String` ↔ `Ljava/lang/String;`
- `int[]` ↔ `[I`
- `java.lang.String[]` ↔ `[Ljava/lang/String;`

---

## Build & Test

### Build
```bash
cd corretto-25

# Configure (first time only)
bash configure --with-debug-level=fastdebug

# Build
make images CONF=macosx-x86_64-server-fastdebug
```

**Output**: `./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java`

### Test (Free List Mode)
```bash
# Compile test program
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/javac TestProgram.java

# Run with oracle
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=$(pwd)/testprogram_liveness.csv \
    -XX:-UseTLAB \
    -Xlog:gc=info \
    TestProgram
```

### Test (Malloc Mode)
```bash
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=$(pwd)/oracle.csv \
    -XX:+EpsilonOracleMallocMode \
    -XX:-UseCompressedOops \
    -XX:-UseCompressedClassPointers \
    -XX:-UseTLAB \
    -Xlog:gc=info \
    MyProgram
```

### Expected Output (TestProgram)
```
[gc] Using Epsilon (Oracle Mode) - Explicit Memory Management Simulation
[gc] Oracle: Loaded 73 entries from trace file
...
[gc] Oracle stats: tracked_allocs=73, total_frees=73
[gc] Oracle stats: bytes_allocated=1936, bytes_freed=1936, bytes_live=0
[gc] Oracle Result: SUCCESS - All 73 tracked allocations were freed
```

---

## What Works

✅ **Complete and tested**:
- CSV trace file parsing
- Type-based allocation matching
- Death map with hash buckets
- Free list allocator (first-fit)
- Type name normalization (all formats)
- Malloc mode with real malloc/free
- Single-threaded TestProgram (73/73 objects)

## What Needs Work

### 1. Multi-Threading Support (Priority: High)

**Problem**: Global allocation sequence doesn't work for multi-threaded programs because thread scheduling is non-deterministic.

**Current Status**: Documented in `ORACLE_DETERMINISM_TODO.md`

**Proposed Solutions**:
1. **Per-thread sequence counters**: Each thread maintains its own allocation counter
2. **Vector clocks**: Track cross-thread dependencies
3. **Thread ID in trace**: Match by (thread_id, type, size, per_thread_seq)

**Impact**: DaCapo benchmarks are multi-threaded, so this blocks real benchmark testing.

### 2. DaCapo Benchmark Validation (Priority: High)

**Status**: Ready to attempt, but may hit multi-threading issues

**Steps**:
1. Generate trace with Elephant Tracks: `./experiment.py trace lusearch --size small`
2. Generate oracle: `python oracle_generator.py trace.etb.zst -o oracle.csv -m liveness`
3. Run with OracleGC (may need single-threaded mode if available)

### 3. Allocator Integration (Priority: Medium)

**Current**: Uses internal free list or system malloc

**Goal**: Integrate mimalloc, jemalloc for comparison

**Approach**: In malloc mode, link against alternative allocators

### 4. gem5 Integration (Priority: Medium)

**Goal**: Cycle-accurate simulation for paper-comparable results

**Status**: `oracle_replayer.c` exists for native replay; need JVM integration path

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Application Code                               │
│                    (e.g., DaCapo lusearch benchmark)                    │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │ new Object()
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         memAllocator.cpp                                 │
│               Sets Klass* in EpsilonThreadLocalData                     │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    epsilonHeap.cpp::allocate_work_oracle()              │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ 1. Get Klass from thread-local                                   │   │
│  │ 2. Lookup (type, size) in oracle->type_index                    │   │
│  │ 3. If match: process_deaths(), allocate, register_death()       │   │
│  │ 4. If no match: bump-pointer only (JVM internal)                │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         epsilonOracle.cpp                                │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐  │
│  │    Type Index    │  │    Death Map     │  │     Free List        │  │
│  │  (type,size)→    │  │  free_at_seq→    │  │   [freed blocks]     │  │
│  │   trace entries  │  │   objects to     │  │   for reuse          │  │
│  │                  │  │   free           │  │                      │  │
│  └──────────────────┘  └──────────────────┘  └──────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## File Locations (Absolute Paths)

### Core Implementation
```
corretto-25/src/hotspot/share/gc/epsilon/
├── epsilonOracle.hpp          # Oracle class definition
├── epsilonOracle.cpp          # Oracle implementation (27KB)
├── epsilonHeap.hpp            # Modified: oracle members
├── epsilonHeap.cpp            # Modified: allocate_work_oracle()
├── epsilon_globals.hpp        # Modified: configuration flags
├── epsilonArguments.cpp       # Modified: oracle initialization
└── epsilonThreadLocalData.hpp # Modified: Klass tracking
```

### Shared GC Code
```
corretto-25/src/hotspot/share/gc/shared/
└── memAllocator.cpp           # Modified: sets Klass before alloc
```

### Test Files
```
corretto-25/
├── TestProgram.java           # Test program
└── testprogram_liveness.csv   # Test trace (73 entries)
```

---

## References

- **Original Paper**: Hertz & Berger, OOPSLA 2005
- **Elephant Tracks**: Trace generation tool (in `../elephant-tracks/`)
- **Oracle Generator**: `../oracle_generator.py`
- **Experiment Pipeline**: `../experiment.py`

---

## Quick Reference: Command Cheatsheet

```bash
# Build
cd corretto-25
make images CONF=macosx-x86_64-server-fastdebug

# Set up alias
alias ojava='./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java'

# Run with oracle (standard)
ojava -XX:+UnlockExperimentalVMOptions \
      -XX:+UseEpsilonGC \
      -XX:+EpsilonOracleMode \
      -XX:EpsilonOracleTracePath=/path/to/oracle.csv \
      -XX:-UseTLAB \
      -Xlog:gc=info \
      MainClass

# Run with malloc mode
ojava -XX:+UnlockExperimentalVMOptions \
      -XX:+UseEpsilonGC \
      -XX:+EpsilonOracleMode \
      -XX:EpsilonOracleTracePath=/path/to/oracle.csv \
      -XX:+EpsilonOracleMallocMode \
      -XX:-UseCompressedOops \
      -XX:-UseCompressedClassPointers \
      -XX:-UseTLAB \
      MainClass

# Generate trace (from project root)
./experiment.py trace lusearch --size small

# Generate oracle
python oracle_generator.py traces/lusearch_small.etb.zst -o oracle.csv -m liveness
```
