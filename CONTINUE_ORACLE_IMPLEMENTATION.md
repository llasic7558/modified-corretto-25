# Prompt for Continuing Oracle EpsilonGC Implementation

## Context

I'm implementing an Oracular Explicit Memory Manager in Amazon Corretto 25's EpsilonGC, based on Hertz & Berger's 2005 paper "Quantifying the Performance of Garbage Collection vs. Explicit Memory Management".

The oracle uses a pre-generated trace from Elephant Tracks that specifies when each object should be freed. Instead of GC, we use malloc/free semantics with perfect knowledge of object lifetimes.

## Current State

Read `ORACLE_IMPLEMENTATION_STATUS.md` for full details. Key points:

1. **Basic implementation is done**: CSV loading, death map, free list allocator, allocation path
2. **Core problem**: The allocation counter starts at JVM startup, but the trace only contains application allocations. ~15,000 JVM internal allocations happen before the application starts.
3. **Need to solve**: Detect when application code starts allocating (vs JVM internals)

## What To Do Next

**Implement reliable application-start detection.** The trace only has application objects (ArrayList, Object, Node, etc.), not JVM internals (Class, String pool, etc.).

Recommended approach: **Check if the class being allocated is an application class** (not starting with `java.`, `jdk.`, `sun.`, `com.sun.`, etc.).

This requires either:
1. Passing Klass information down to `allocate_work_oracle()`
2. Or checking at a higher level in the allocation path (e.g., `MemAllocator`)
3. Or hooking into class loading to set a flag when first non-JDK class loads

## Key Files

- `src/hotspot/share/gc/epsilon/epsilonOracle.hpp` - Oracle class definition
- `src/hotspot/share/gc/epsilon/epsilonOracle.cpp` - Oracle implementation
- `src/hotspot/share/gc/epsilon/epsilonHeap.cpp` - `allocate_work_oracle()` function
- `testprogram_liveness.csv` - Test trace (73 application allocations)
- `TestProgram.java` - Simple test program

## Constraints

- Memory must be allocated within the Java heap region (not `os::malloc`)
- Must zero all allocated memory
- The trace format is: `alloc_seq,free_at_seq,size,type,obj_id`
- We only care about application allocations, not JVM internals
