# Continue: Oracle Explicit Memory Manager for EpsilonGC

## Project Goal

Implement the Hertz & Berger (2005) methodology for comparing GC vs explicit memory management. We modify EpsilonGC to use a pre-computed "oracle" trace that tells us exactly when each object dies. Instead of garbage collection, we explicitly free objects at the precise moment they become unreachable.

## What's Already Done

I've implemented most of the oracle infrastructure in EpsilonGC:

```
src/hotspot/share/gc/epsilon/
├── epsilon_globals.hpp    # Added: EpsilonOracleMode, EpsilonOracleTracePath flags
├── epsilonOracle.hpp      # NEW: Oracle class with death map + free list
├── epsilonOracle.cpp      # NEW: CSV parsing, death processing, free list allocator
├── epsilonHeap.hpp        # Added: _oracle member
├── epsilonHeap.cpp        # Added: allocate_work_oracle() allocation path
└── epsilonArguments.cpp   # Added: Disable TLABs in oracle mode
```

**Working components:**
- Loads trace CSV (format: `alloc_seq,free_at_seq,size,type,obj_id`)
- Death map: hash table mapping free_at_seq → list of pointers to free
- Free list: first-fit allocator for reusing freed memory within heap
- Memory properly allocated from heap region (not os::malloc)
- Memory zeroed on allocation

## The Blocking Problem

**The allocation sequence counter starts counting at JVM startup, but the trace only contains application-level allocations.**

Example with TestProgram.java:
- Trace has 73 entries (ArrayList, Objects, Nodes, arrays)
- JVM allocates ~15,000 internal objects before application runs
- Our counter reaches 15,000+ before the first application object
- Trace entry 1 (ArrayList, dies at seq 2) gets applied to wrong object
- Result: Memory corruption, crashes

**We need to start the oracle counter only when the APPLICATION starts allocating, not when the JVM starts.**

## What I Need You To Do

Implement detection of when application code starts. The trace from Elephant Tracks only captures application objects, not JVM internals (Class mirrors, String pool, reflection objects, etc.).

**Recommended approach:** Check if the class being allocated is an application class.

Application classes do NOT start with:
- `java.`, `javax.`, `jdk.`, `sun.`, `com.sun.`, `org.graalvm.`

When we see the first allocation for a class outside these packages, that's when the application has started.

**Implementation options:**

1. **Pass Klass to allocate_work_oracle()** - Modify the interface to receive class info
2. **Check at MemAllocator level** - Hook earlier in allocation where Klass is known
3. **Hook into class loading** - Set flag when first non-JDK class is loaded
4. **Check current thread's stack** - See if any frame is from application code (expensive)

## Key Files to Read

1. `ORACLE_IMPLEMENTATION_STATUS.md` - Detailed status document
2. `src/hotspot/share/gc/epsilon/epsilonHeap.cpp` - Look at `allocate_work_oracle()`
3. `src/hotspot/share/gc/epsilon/epsilonOracle.hpp` - Oracle class structure
4. `testprogram_liveness.csv` - The test trace (73 entries)
5. `TestProgram.java` - Simple test program

## Build & Test

```bash
# Build
make CONF=macosx-x86_64-server-fastdebug images

# Test (currently crashes due to the app-start detection problem)
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=testprogram_liveness.csv \
    -Xshare:off \
    -Xlog:gc=info \
    TestProgram
```

## Critical Constraints

1. **Heap region only**: All Java object memory must come from the reserved heap region, not `os::malloc`. Otherwise oop validation fails.

2. **Zero memory**: All allocated memory must be zeroed. Stale data causes crashes.

3. **Application allocations only**: The oracle should ONLY track allocations for application classes. JVM internal allocations should use normal bump-pointer without oracle tracking.

## Success Criteria

When working correctly:
```
TestProgram: Starting...
List sum: 190
TestProgram: Complete.
Oracle Statistics:
  Trace entries: 73
  Allocations: 73        # Should match trace
  Frees: 73              # All objects freed at correct times
  Bytes allocated: ~3400
  Bytes freed: ~3400
```

The key metric: allocations tracked by oracle should equal trace entry count (73), not total JVM allocations (15,000+).
