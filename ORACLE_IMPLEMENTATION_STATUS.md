# Oracle EpsilonGC Implementation Status

## What Was Implemented

1. **New flags in `epsilon_globals.hpp`**:
   - `EpsilonOracleMode` - Enable oracle-based explicit memory management
   - `EpsilonOracleTracePath` - Path to CSV trace file
   - `EpsilonOracleValidate` - Optional size validation
   - `EpsilonOracleSkipAllocs` - Manual skip count (partially working)

2. **New files created**:
   - `epsilonOracle.hpp` - Oracle class with death map, free list structures
   - `epsilonOracle.cpp` - CSV parsing, death processing, free list allocator

3. **Modified files**:
   - `epsilonArguments.cpp` - Disables TLABs in oracle mode
   - `epsilonHeap.hpp` - Added `_oracle` member and accessors
   - `epsilonHeap.cpp` - Added `allocate_work_oracle()` path

## What Works

- CSV trace loading (format: `alloc_seq,free_at_seq,size,type,obj_id`)
- Death map for tracking when objects should be freed
- Free list allocator (first-fit) within heap region
- Basic allocation path when oracle tracking is disabled
- Registration of allocations with the oracle
- Death processing (adding freed memory to free list)

## The Core Problem

**The allocation counter starts at JVM startup, not when the application starts.**

- Elephant Tracks traces only contain **application-level allocations** (~73 for TestProgram)
- JVM allocates **~15,000+ internal objects** before application code runs
- These include: Class objects, String pool, reflection, class loading, etc.
- Our oracle counter increments for ALL allocations
- Result: Trace entry 1 (ArrayList) gets applied to wrong object (VM internal)

## What Needs To Be Done

**Find a reliable way to detect when the application starts allocating objects.**

Options explored:
1. `is_init_completed()` - Not sufficient, thousands of allocations happen after this
2. `EpsilonOracleSkipAllocs` manual count - Works but fragile, varies by JVM config
3. Pattern matching on sizes - Too specific, doesn't generalize

**Better approaches to explore**:
1. **Check the Klass being allocated** - Application classes don't start with `java.`, `jdk.`, `sun.`
   - Requires passing Klass info to `allocate_work()` or checking at higher level
2. **Hook into main class loading** - Set flag when non-JDK class first loads
3. **Check VM lifecycle phase** - After `System.initPhase3()` completes
4. **Use JVMTI marker** - Have Elephant Tracks emit a "start" marker in trace

## Key Technical Details

- Memory MUST be allocated within heap region (not `os::malloc`) for oop validation
- All allocated memory must be zeroed (stale data causes crashes)
- Free list uses `OracleFreeBlock` (renamed to avoid conflict with HotSpot's `FreeBlock`)
- Death map uses hash buckets keyed by `free_at_seq`
- Sizes in trace are bytes; allocation sizes in `allocate_work()` are HeapWords

## Build & Test Commands

```bash
# Build
make CONF=macosx-x86_64-server-fastdebug images

# Test without oracle (works)
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
    -Xshare:off TestProgram

# Test with oracle (needs app-start detection fix)
./build/macosx-x86_64-server-fastdebug/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode -XX:EpsilonOracleTracePath=testprogram_liveness.csv \
    -Xshare:off TestProgram
```

## Files Reference

- Trace file: `testprogram_liveness.csv` (73 entries)
- Test program: `TestProgram.java`
- Plan: `.claude/plans/memoized-discovering-reef.md`
