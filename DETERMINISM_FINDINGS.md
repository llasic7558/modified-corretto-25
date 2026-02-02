# Determinism Findings: Multi-Threading Analysis

## Executive Summary

**Good news**: Multi-threaded programs produce **deterministic allocations per thread**. The current type-based matching in OracleGC should handle most cases. Per-thread clocks can be added if needed, but **vector clocks are NOT required**.

---

## Key Findings

| Question | Answer |
|----------|--------|
| Is allocation order deterministic? | **Per-thread: YES**, Global: NO (thread scheduling varies) |
| Does ET3 lambda exclusion matter? | **NO** - DaCapo benchmarks predate lambdas; exclusion filters JDK noise |
| Do we need vector clocks? | **NO** - Per-thread clocks sufficient if needed |
| Is current type-based matching enough? | **Likely YES** - test with real benchmark to confirm |

---

## Experiments Conducted

### Experiment 1: Single-Threaded Traces
- **Result**: 100% identical across runs
- **Conclusion**: Fully deterministic

### Experiment 2: Multi-Threaded Traces (Non-Lambda)
- **Result**: Same allocations occur, but in different global order
- **Key Data**:
  - Per-thread allocation counts: IDENTICAL across runs
  - Sorted `(size, type, thread)` tuples: IDENTICAL
  - Raw allocation order: DIFFERENT (thread scheduling)

### Experiment 3: Lambda vs Non-Lambda
- **Finding**: ET3 skips `lambda$` methods intentionally
- **Impact**: None for DaCapo - benchmarks don't use lambdas for core allocations
- **Rationale**: Filters JDK internal lambdas (streams, futures, etc.)

---

## Why Current Approach Should Work

The OracleGC uses **type-based matching** `(class_name, size)` instead of global sequence numbers. This already provides resilience to:

1. **JVM internal allocation variations** - Different counts of internal objects
2. **Thread interleaving** - Same types allocated, just different order
3. **Minor timing differences** - Type matching is order-agnostic

---

## If Type-Based Matching Fails

If testing reveals mismatches, add per-thread sequence tracking:

### Oracle Format Change
```csv
# Current (works for most cases)
alloc_seq,free_at_seq,size,type,obj_id

# Enhanced (if needed)
thread_id,thread_seq,free_thread,free_seq,size,type,obj_id
```

### Implementation Changes
1. **oracle_generator.py**: Track per-thread allocation counters
2. **epsilonOracle.cpp**: Maintain `std::unordered_map<thread_id, counter>`

---

## Verification Data

### Multi-Threaded Test (4 workers × 100 allocations)

| Metric | Run 1 | Run 2 | Run 3 |
|--------|-------|-------|-------|
| Total allocations | 464 | 464 | 464 |
| Main thread | 60 | 60 | 60 |
| Each worker | 101 | 101 | 101 |
| Sorted tuples match? | - | YES | YES |

### Lusearch Benchmark
- **Allocations tracked**: 973,981
- **Types**: Lucene classes, arrays, JDK types
- **No lambda-related issues observed**

---

## Recommended Next Steps

1. **Test OracleGC with lusearch oracle** - Verify type-based matching works
2. **If mismatches occur** - Add per-thread sequence tracking
3. **No changes to ET3** - Lambda exclusion is intentional and correct

---

## Test Commands

```bash
# Build OracleGC
cd corretto-25
make images CONF=macosx-x86_64-server-fastdebug

# Test with lusearch oracle
./build/*/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=../lusearch_fresh_liveness.csv \
    -XX:-UseTLAB \
    -Xlog:gc=info \
    -jar ../benchmarks/dacapo-23.11-chopin.jar lusearch -s small
```
## NextSteps                                                                                                      
1. **Implement per-thread sequence in oracle_generator.py**                                                        
   - Parse thread ID from trace events (column 7 in A events)                                                      
   - Maintain `thread_seq = defaultdict(int)`                                                                      
   - Output includes `thread_id,thread_seq`                                                                        
                                                                                                                   
2. **Update OracleGC in epsilonOracle.cpp**                                                                        
   - Add `std::unordered_map<int, uint64_t> _thread_alloc_counts`                                                  
   - Modify `process_deaths()` to check per-thread counts                                                          
   - Update type index to include thread ID                                                                        
                                                                                                                   
3. **Fix or document ET3 lambda issue**                                                                            
   - Option A: Remove `lambda$` exclusion (may have side effects)                                                  
   - Option B: Document that benchmarks must use explicit Runnables                                                
                                                                                                                   
4. **Test with DaCapo benchmarks**                                                                                 
   - Generate traces with non-lambda code paths                                                                    
   - Verify per-thread matching works                                                                              
   - Compare oracle results                                                                                        
                                                                                                                   
----                                                                                                                
                                                                                                                   
## References                                                                                                      
                                                                                                       
- Original analysis: `ORACLE_DETERMINISM_TODO.md`                                                                  
- Test programs: `StressTest.java`, `StressTestNoLambda.java`                                                      
- Trace comparison: `stability_traces/` 