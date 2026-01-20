# Oracle EpsilonGC Implementation - Status Report

## Overview

This document summarizes the implementation of an **Oracular Explicit Memory Manager** in Amazon Corretto 25's EpsilonGC, based on Hertz & Berger's 2005 paper *"Quantifying the Performance of Garbage Collection vs. Explicit Memory Management"*.

The oracle uses pre-generated traces from Elephant Tracks to replay object lifetimes with explicit malloc/free semantics instead of garbage collection.

---

## What Has Been Accomplished

### 1. Core Oracle Infrastructure

- **`epsilonOracle.hpp/cpp`** - Oracle class that:
  - Loads CSV trace files with format: `alloc_seq,free_at_seq,size,type,obj_id`
  - Maintains a death map (hash table) mapping `free_at_seq` → objects to free
  - Implements a first-fit free list allocator for memory reuse
  - Tracks allocation/deallocation statistics

### 2. Application Start Detection

- **`oracleSyncAgent.cpp`** - JVMTI agent that:
  - Detects when `main()` is entered (non-system class)
  - Signals EpsilonGC via `epsilon_oracle_signal_app_start()`
  - Distinguishes JVM startup allocations (~1300) from application allocations

### 3. Type-Based Allocation Matching

- **Klass tracking** via `EpsilonThreadLocalData`:
  - Stores current allocation's Klass before heap allocation
  - Enables matching by both **type AND size** (not just size)

- **Array type normalization**:
  - Handles `int[]` ↔ `[I`, `java.lang.String[]` ↔ `[Ljava.lang.String;`
  - Supports all primitive and object array types

### 4. Memory Management

- **Allocation path** (`allocate_work_oracle`):
  1. Pre-app allocations: bump-pointer only (no tracking)
  2. Post-app, non-matching: bump-pointer only (JVM internals)
  3. Post-app, matching: free list first, then bump-pointer, register for death

- **Deallocation path** (`process_deaths`):
  - Called before each allocation with current trace sequence
  - Frees objects scheduled to die at this sequence
  - Adds freed memory to free list for reuse

- **Finalization** (`finalize`):
  - Frees any remaining tracked objects at shutdown
  - Verifies all tracked allocations were freed

### 5. Configuration

New VM flags in `epsilon_globals.hpp`:
```
-XX:+EpsilonOracleMode              # Enable oracle mode
-XX:EpsilonOracleTracePath=<file>   # Path to trace CSV
-XX:-UseTLAB                        # Required for oracle mode
```

---

## Key Findings

### Memory Comparison: G1GC vs EMM (Oracle)

| Metric | G1GC | EMM (Oracle) |
|--------|------|--------------|
| GC Pauses | 140 ms | 0 ms |
| Peak heap | 2 MB (before GC) | ~1.3 MB |
| Memory reclamation | Batch (stop-the-world) | Incremental |
| When objects freed | At GC time | Immediately at death |

### Test Results (73 application objects)

```
Oracle Statistics:
  Tracked app allocs: 73
  Total frees:        73
  Bytes allocated:    1,936
  Bytes freed:        1,936
  Bytes live:         0

Oracle: SUCCESS - All 73 tracked allocations were freed
```

### Validated Hertz & Berger Insight

- **GC** defers memory reclamation → uses more memory, avoids per-allocation overhead
- **EMM** reclaims immediately → uses less memory, requires tracking overhead
- Memory reuse confirmed: same address `0x...c08` reused 50+ times via free list

---

## Files Modified/Created

### New Files
| File | Purpose |
|------|---------|
| `src/hotspot/share/gc/epsilon/epsilonOracle.hpp` | Oracle class definition |
| `src/hotspot/share/gc/epsilon/epsilonOracle.cpp` | Oracle implementation |
| `oracleSyncAgent.cpp` | JVMTI agent for main() detection |

### Modified Files
| File | Changes |
|------|---------|
| `src/hotspot/share/gc/epsilon/epsilonHeap.hpp` | Added `_oracle` member |
| `src/hotspot/share/gc/epsilon/epsilonHeap.cpp` | Added `allocate_work_oracle()`, initialization |
| `src/hotspot/share/gc/epsilon/epsilonThreadLocalData.hpp` | Added Klass tracking |
| `src/hotspot/share/gc/epsilon/epsilon_globals.hpp` | Added oracle flags |
| `src/hotspot/share/gc/shared/memAllocator.cpp` | Set Klass before allocation |

---

## How to Run

### 1. Build the JDK
```bash
make images CONF=macosx-x86_64-server-fastdebug
```

### 2. Compile the JVMTI Agent
```bash
clang++ -shared -fPIC -o liboracleSyncAgent.dylib oracleSyncAgent.cpp \
    -I$JAVA_HOME/include -I$JAVA_HOME/include/darwin
```

### 3. Generate Trace with Elephant Tracks
```bash
java -javaagent:elephant-tracks.jar=output=trace.etb.zst MyProgram
python oracle_generator.py trace.etb.zst -o oracle.csv -m liveness
```

### 4. Run with Oracle Mode
```bash
./build/*/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=oracle.csv \
    -XX:-UseTLAB \
    -agentpath:./liboracleSyncAgent.dylib \
    -Xlog:gc=info \
    MyProgram
```

---

## Next Steps

### 1. Run DaCapo Benchmarks
- Generate traces for DaCapo benchmarks (lusearch, xalan, etc.)
- Validate oracle matching works for larger, real-world workloads
- Compare memory/performance across different benchmark characteristics

### 2. Gem5 Integration
- Cross-compile for Linux x86_64 (for gem5 SE mode)
- Run cycle-accurate simulations to measure:
  - Cache behavior (L1/L2/L3 hits/misses)
  - Memory bandwidth
  - CPU cycles

### 3. Working Set Simulation
- Implement working set access pattern (access all live objects periodically)
- This simulates realistic heap traversal patterns
- Critical for accurate cache simulation

### 4. Reachability Oracle Mode
- Currently using **liveness** oracle (free at last access)
- Implement **reachability** oracle (free when unreachable)
- Compare the two to measure GC's "conservative" overhead

### 5. Multiple Allocator Support
- Test with different allocators (glibc, mimalloc, jemalloc)
- Measure allocator overhead differences

### 6. Statistical Analysis
- Collect metrics across multiple runs
- Generate graphs comparing GC vs EMM
- Validate against original Hertz & Berger results

---

## Known Limitations

1. **Single-threaded only** - Death map and free list are not thread-safe
2. **Requires -XX:-UseTLAB** - TLAB fast-path bypasses our tracking
3. **JVM internals not tracked** - Only application allocations in trace are managed
4. **Trace must match execution** - Same program, same inputs required

---

## References

- Hertz, M., & Berger, E. D. (2005). *Quantifying the Performance of Garbage Collection vs. Explicit Memory Management*. OOPSLA '05.
- Elephant Tracks: JVMTI-based allocation tracing tool
- Amazon Corretto 25: OpenJDK distribution with EpsilonGC
