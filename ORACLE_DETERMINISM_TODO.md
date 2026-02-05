# Oracle Determinism: Issues and Required Changes

## Problem Statement

The current oracle-based approach assumes a **global allocation sequence number** that is deterministic across runs. This assumption breaks in multi-threaded programs.

### Current Approach
```
Trace Run:
  Thread A: alloc(Node)     → seq=1
  Thread B: alloc(Object)   → seq=2
  Thread A: alloc(ArrayList)→ seq=3
  Thread B: last_use(seq=2) → free_at_seq=3

Oracle says: "Free object allocated at seq=2 when seq=3 happens"

Measurement Run (different scheduling):
  Thread B: alloc(Object)   → seq=1  ← WRONG! Oracle expects this at seq=2
  Thread A: alloc(Node)     → seq=2
  Thread A: alloc(ArrayList)→ seq=3

Result: Wrong object gets freed!
```

### Why This Matters
- Thread scheduling is non-deterministic
- Allocation interleaving varies between runs
- Oracle trace from run 1 doesn't match execution of run 2
- Objects freed at wrong times → crashes, incorrect memory behavior

---

## Investigation Tasks

### Task 1: Single-Core Determinism Experiment

**Goal**: Determine if pinning to a single core produces deterministic traces.

**Experiment**:
```bash
# On Linux, pin to single core
taskset -c 0 java \
  -XX:ActiveProcessorCount=1 \
  -XX:ParallelGCThreads=1 \
  -XX:ConcGCThreads=1 \
  -javaagent:elephant-tracks.jar \
  TestProgram

# Run 10 times, compare traces
for i in {1..10}; do
  taskset -c 0 java ... -o trace_$i.et TestProgram
done

# Compare allocation sequences
diff trace_1.et trace_2.et
```

**Questions to Answer**:
1. Are traces byte-identical across runs?
2. If not, what differs? (timestamps? sequence numbers? thread IDs?)
3. Is the allocation ORDER stable even if exact values differ?

**Test Program** (create `ThreadDeterminismTest.java`):
```java
public class ThreadDeterminismTest {
    public static void main(String[] args) throws Exception {
        // Single-threaded baseline
        for (int i = 0; i < 100; i++) {
            Object o = new Object();
            use(o);
        }

        // Multi-threaded (should show non-determinism)
        Thread t1 = new Thread(() -> {
            for (int i = 0; i < 50; i++) {
                Object o = new Object();
                use(o);
            }
        });
        Thread t2 = new Thread(() -> {
            for (int i = 0; i < 50; i++) {
                Object o = new Object();
                use(o);
            }
        });
        t1.start(); t2.start();
        t1.join(); t2.join();
    }

    static void use(Object o) { o.hashCode(); }
}
```

---

### Task 2: Per-Thread Clock Design

**Concept**: Replace global `alloc_seq` with `(thread_id, thread_local_seq)` tuple.

**New Oracle Format**:
```csv
thread_id,local_alloc_seq,free_at_thread,free_at_local_seq,size,type,obj_id
1,1,1,5,24,Node,0x1234
2,1,2,3,16,java.lang.Object,0x5678
1,2,1,7,32,java.util.ArrayList,0x9abc
```

**Interpretation**:
- Object allocated by thread 1, local seq 1
- Should be freed when thread 1 reaches its local allocation 5

**Changes Required**:

#### A. Elephant Tracks (Trace Collection)
- File: `elephant-tracks/src/main/java/veroy/research/et2/...`
- Already captures thread ID per event
- Need to add per-thread allocation counter
- Output format change: include thread_local_seq

#### B. Oracle Generator
- File: `oracle_generator.py`
- Parse thread ID from trace events
- Maintain per-thread allocation counters
- Death events reference (thread, local_seq) instead of global seq
- Handle cross-thread references (object allocated by T1, last used by T2)

#### C. OracleGC (JVM)
- File: `epsilonOracle.cpp`, `epsilonHeap.cpp`
- Maintain per-thread allocation counters
- Death map keyed by (thread_id, local_seq)
- Process deaths when specific thread reaches specific count

**Pseudocode for OracleGC**:
```cpp
struct ThreadLocalCounter {
    uint64_t alloc_count;
    uint64_t current_seq;
};

std::unordered_map<int, ThreadLocalCounter> _thread_counters;

void on_allocation(Thread* thread, size_t size) {
    int tid = thread->osthread()->thread_id();
    uint64_t local_seq = ++_thread_counters[tid].alloc_count;

    // Process deaths for THIS thread at THIS local_seq
    process_deaths_for_thread(tid, local_seq);

    // ... allocate ...
}
```

---

### Task 3: Vector Clocks (If Needed)

**When Per-Thread Clocks Aren't Enough**:

If objects have cross-thread lifetime dependencies:
- Object X allocated by Thread A
- Object X last used by Thread B
- When should X be freed?

**Vector Clock Approach**:
```
Vector Clock = [count_thread1, count_thread2, ..., count_threadN]

Object X:
  allocated_at: [5, 0, 0]  (Thread 1 at its 5th allocation)
  free_at:      [7, 3, 0]  (When T1 >= 7 AND T2 >= 3)
```

**Happens-Before**:
- VC1 <= VC2 iff all components of VC1 <= corresponding components of VC2
- Free when current_vc >= free_at_vc

**Complexity**:
- More storage per object (N integers instead of 2)
- More complex comparison
- May be overkill if most objects are thread-local

---

## Decision Tree

```
                    Single-Core Test
                          |
            +-------------+-------------+
            |                           |
      Traces Stable              Traces Differ
            |                           |
     Use single-core            Multi-threaded needed?
     for experiments                    |
            |                    +------+------+
            |                    |             |
            |                   No            Yes
            |                    |             |
            |             Per-Thread      Vector Clocks
            |               Clocks         (complex)
            |                    |
            +--------------------+
                      |
              Update Pipeline:
              1. Elephant Tracks
              2. oracle_generator.py
              3. OracleGC (epsilonOracle.cpp)
```

---

## Implementation Status (Updated 2024-02-05)

### Phase 1: Validation - COMPLETE ✓
- [x] Created test programs for determinism analysis
- [x] Ran experiments comparing multiple traces
- [x] **Finding**: Multi-threaded execution has ~6-7% trace variation
- [x] **Finding**: Single-thread mode (`-t 1`) reduces variation to ~0.5%
- [x] **Finding**: First ~5,467 allocations are 100% deterministic
- [x] **Finding**: Object IDs (identity hash codes) are 93% consistent across runs

### Phase 2: Design Decision - COMPLETE ✓
- [x] Per-thread clocks implemented (not vector clocks)
- [x] **New approach**: Lookahead matching for remaining ~0.1% non-determinism
- [x] Matching by (size, type) within a configurable window

### Phase 3: Implementation - COMPLETE ✓
- [x] Elephant Tracks trace format unchanged (already has thread info)
- [x] `oracle_generator.py` updated for per-thread format
- [x] `epsilonOracle.hpp/cpp` updated with:
  - Per-thread allocation tracking
  - Runtime → logical thread ID mapping
  - **Lookahead matching** (new!)
  - Consumed entry tracking
  - Match statistics (exact, lookahead, no-match)
- [x] `epsilonHeap.cpp` updated:
  - Type name passed to register_allocation
  - Class filter support
- [x] New JVM flags:
  - `EpsilonOracleLookahead=N` - window size for fuzzy matching
  - `EpsilonOracleRequireTypeMatch` - require type match in lookahead

### Phase 4: Validation - IN PROGRESS
- [ ] Test lookahead matching with lusearch benchmark
- [ ] Verify match rate improves with lookahead enabled
- [ ] Compare memory behavior with/without lookahead
- [ ] Run full DaCapo suite

---

## Lookahead Matching Implementation

### Overview

Even with per-thread sequence matching, ~0.1% of allocations differ between trace collection and replay due to:
- JIT compilation timing
- Lazy class initialization
- File I/O order variation

### How It Works

```
Runtime allocation: Thread 0, seq 5468, size 48, type java.util.StringBuilder

Expected oracle entry at seq 5468: size 56, type java.io.FileReader  ← MISMATCH!

With EpsilonOracleLookahead=20:
  Search entries 5458-5488 for:
    - size == 48
    - type == java.util.StringBuilder (if EpsilonOracleRequireTypeMatch=true)
    - entry not already consumed

  Found match at oracle seq 5471 → Use that entry for death scheduling
```

### JVM Flags

```bash
# Enable lookahead with 20-entry window
-XX:EpsilonOracleLookahead=20

# Require type match (default: true)
-XX:+EpsilonOracleRequireTypeMatch

# Or allow size-only matching
-XX:-EpsilonOracleRequireTypeMatch
```

### Statistics

At shutdown, the oracle prints matching statistics:

```
Oracle Statistics:
  Matching Statistics:
    Exact matches:    973000 (99.90%)
    Lookahead matches:    500 (0.05%)
    No match:             50 (0.05%)
    Lookahead window: 20 entries
    Type matching:    required
```

---

## Original Implementation Priority (Historical)

### Phase 1: Validation (Do First)
- [x] Create `ThreadDeterminismTest.java`
- [x] Run single-core experiment (10 runs, compare traces)
- [x] Document findings

### Phase 2: Design Decision
- [x] Analyze: Are most object lifetimes thread-local?
- [x] Decide: Per-thread clocks vs Vector clocks → **Per-thread + Lookahead**
- [x] Write design doc with examples

### Phase 3: Implementation (If Per-Thread Clocks)
- [x] Modify Elephant Tracks trace format
- [x] Update `oracle_generator.py` for new format
- [x] Update `epsilonOracle.hpp/cpp` for per-thread tracking
- [x] Update `epsilonHeap.cpp` allocation path
- [x] Test with single-threaded program
- [ ] Test with multi-threaded program

### Phase 4: Validation
- [ ] Run DaCapo benchmark traces
- [ ] Verify oracle replay matches expected behavior
- [ ] Compare liveness vs reachability oracle results

---

## References

- **Vector Clocks**: Lamport, "Time, Clocks, and the Ordering of Events in a Distributed System" (1978)
- **Original Paper**: Hertz & Berger, "Quantifying the Performance of Garbage Collection vs. Explicit Memory Management" (OOPSLA 2005)
  - How did they handle this? (Check if they used single-threaded benchmarks)

---

## Notes

The original Hertz & Berger paper may have:
1. Used single-threaded benchmarks only
2. Used a different tracing mechanism that captured deterministic order
3. Accepted some non-determinism as acceptable noise

**Action Item**: Review the original paper's methodology section for how they handled multi-threading.
