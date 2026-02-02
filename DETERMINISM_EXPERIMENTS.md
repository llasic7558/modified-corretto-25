# Determinism Experiments for OracleGC

## Core Questions

1. **Does allocation order diverge between runs?** (Probably yes for multi-threaded)
2. **Do we need 100% determinism?** (No - we need object lifetimes to match, not exact order)
3. **Are objects alive for the same DURATION?** (This is what matters!)
4. **Can per-thread clocks solve this?** (Likely yes for most cases)

---

## Key Insight: What Actually Matters

The oracle doesn't need to free objects at the exact same **global** moment. It needs to free objects after they've been used for the **same logical duration**.

```
Trace Run:
  Thread A: alloc(X) at global_seq=1, last_use at global_seq=10
  Object X lives for 9 allocations (from A's perspective)

Replay Run (different interleaving):
  Thread A: alloc(X) at global_seq=5, last_use at global_seq=14
  Object X STILL lives for 9 allocations from A's perspective

Key: The DURATION matters, not the absolute position!
```

---

## Experiment 1: Single-Core Trace Stability

**Goal**: Determine if traces are stable when pinned to a single core.

### Test Program: `ThreadStabilityTest.java`

```java
import java.util.ArrayList;
import java.util.List;

public class ThreadStabilityTest {
    static volatile int counter = 0;

    public static void main(String[] args) throws Exception {
        // Part 1: Single-threaded (should be deterministic)
        System.out.println("=== Single-threaded allocations ===");
        for (int i = 0; i < 20; i++) {
            Object o = new Object();
            use(o);
        }

        // Part 2: Multi-threaded (may diverge)
        System.out.println("=== Multi-threaded allocations ===");
        Thread t1 = new Thread(() -> {
            for (int i = 0; i < 10; i++) {
                Object o = new Object();
                use(o);
                counter++;
            }
        }, "Worker-1");

        Thread t2 = new Thread(() -> {
            for (int i = 0; i < 10; i++) {
                Object o = new Object();
                use(o);
                counter++;
            }
        }, "Worker-2");

        t1.start();
        t2.start();
        t1.join();
        t2.join();

        System.out.println("Total: " + counter);
    }

    static void use(Object o) {
        o.hashCode();
    }
}
```

### Run Script: `run_stability_test.sh`

```bash
#!/bin/bash

# Directory setup
TRACE_DIR="stability_traces"
mkdir -p $TRACE_DIR

ET_JAR="../elephant-tracks/target/elephant-tracks-3.0.0-jar-with-dependencies.jar"
JAVA="java"  # Use system Java for tracing

# Compile test
javac ThreadStabilityTest.java

echo "=== Running 5 traces WITHOUT core pinning ==="
for i in {1..5}; do
    echo "Run $i..."
    $JAVA -javaagent:$ET_JAR=output=$TRACE_DIR/unpinned_$i.etb.zst \
        ThreadStabilityTest 2>/dev/null
done

echo ""
echo "=== Running 5 traces WITH single-core pinning ==="
for i in {1..5}; do
    echo "Run $i..."
    # On macOS, no taskset - try limiting threads instead
    # On Linux: taskset -c 0 java ...
    $JAVA -XX:ActiveProcessorCount=1 \
        -javaagent:$ET_JAR=output=$TRACE_DIR/pinned_$i.etb.zst \
        ThreadStabilityTest 2>/dev/null
done

echo ""
echo "=== Comparing traces ==="
echo "Unpinned traces (should differ):"
for i in {2..5}; do
    python3 ../trace_converter.py $TRACE_DIR/unpinned_1.etb.zst /dev/stdout 2>/dev/null | head -100 > /tmp/t1.txt
    python3 ../trace_converter.py $TRACE_DIR/unpinned_$i.etb.zst /dev/stdout 2>/dev/null | head -100 > /tmp/t2.txt
    DIFF=$(diff /tmp/t1.txt /tmp/t2.txt | wc -l)
    echo "  Run 1 vs Run $i: $DIFF lines differ"
done

echo ""
echo "Pinned traces (should be identical or very similar):"
for i in {2..5}; do
    python3 ../trace_converter.py $TRACE_DIR/pinned_1.etb.zst /dev/stdout 2>/dev/null | head -100 > /tmp/t1.txt
    python3 ../trace_converter.py $TRACE_DIR/pinned_$i.etb.zst /dev/stdout 2>/dev/null | head -100 > /tmp/t2.txt
    DIFF=$(diff /tmp/t1.txt /tmp/t2.txt | wc -l)
    echo "  Run 1 vs Run $i: $DIFF lines differ"
done
```

---

## Experiment 2: Object Lifetime Duration Analysis

**Goal**: Verify that object lifetimes (in logical time) are consistent even if global order varies.

### Analysis Script: `analyze_lifetimes.py`

```python
#!/usr/bin/env python3
"""
Analyze if object lifetimes are consistent across runs.
Key insight: We care about DURATION, not absolute position.
"""

import sys
import subprocess
from collections import defaultdict

def parse_trace(trace_path):
    """Parse trace and extract per-thread allocation sequences."""
    # Use trace_converter to get text
    result = subprocess.run(
        ['python3', '../trace_converter.py', trace_path, '/dev/stdout'],
        capture_output=True, text=True
    )

    allocations = {}  # obj_id -> {thread, size, type, alloc_seq, last_use_seq}
    thread_alloc_counts = defaultdict(int)  # thread_id -> count
    global_seq = 0

    for line in result.stdout.split('\n'):
        parts = line.strip().split()
        if not parts:
            continue

        event_type = parts[0]

        if event_type == 'A':  # Allocation
            # A <obj_id> <size> <type_id> <site_id> <array_len> <thread>
            obj_id = parts[1]
            size = int(parts[2])
            thread = parts[-1]
            global_seq += 1
            thread_alloc_counts[thread] += 1

            allocations[obj_id] = {
                'thread': thread,
                'size': size,
                'global_alloc_seq': global_seq,
                'thread_alloc_seq': thread_alloc_counts[thread],
                'last_use_global': global_seq,  # Updated on use
                'last_use_thread': thread_alloc_counts[thread],
            }

        elif event_type in ('R', 'U', 'M', 'E'):  # Read, Update, Method entry/exit
            # These indicate object use
            if len(parts) > 1:
                obj_id = parts[1] if event_type == 'R' else (parts[2] if event_type == 'U' else None)
                if obj_id and obj_id in allocations:
                    thread = parts[-1]
                    global_seq += 1
                    thread_alloc_counts[thread] += 1
                    allocations[obj_id]['last_use_global'] = global_seq
                    allocations[obj_id]['last_use_thread'] = thread_alloc_counts[thread]

    return allocations

def compute_lifetimes(allocations):
    """Compute lifetime durations."""
    lifetimes = []
    for obj_id, info in allocations.items():
        global_lifetime = info['last_use_global'] - info['global_alloc_seq']
        thread_lifetime = info['last_use_thread'] - info['thread_alloc_seq']
        lifetimes.append({
            'obj_id': obj_id,
            'size': info['size'],
            'thread': info['thread'],
            'global_lifetime': global_lifetime,
            'thread_lifetime': thread_lifetime,
        })
    return lifetimes

def compare_lifetimes(trace1, trace2):
    """Compare object lifetimes between two traces."""
    allocs1 = parse_trace(trace1)
    allocs2 = parse_trace(trace2)

    lifetimes1 = compute_lifetimes(allocs1)
    lifetimes2 = compute_lifetimes(allocs2)

    # Group by (size, thread) - these should have similar lifetime distributions
    by_size_thread_1 = defaultdict(list)
    by_size_thread_2 = defaultdict(list)

    for lt in lifetimes1:
        key = (lt['size'], lt['thread'])
        by_size_thread_1[key].append(lt['global_lifetime'])

    for lt in lifetimes2:
        key = (lt['size'], lt['thread'])
        by_size_thread_2[key].append(lt['global_lifetime'])

    print(f"Trace 1: {len(allocs1)} objects")
    print(f"Trace 2: {len(allocs2)} objects")
    print()

    print("Lifetime comparison by (size, thread):")
    all_keys = set(by_size_thread_1.keys()) | set(by_size_thread_2.keys())
    for key in sorted(all_keys):
        lt1 = by_size_thread_1.get(key, [])
        lt2 = by_size_thread_2.get(key, [])
        avg1 = sum(lt1) / len(lt1) if lt1 else 0
        avg2 = sum(lt2) / len(lt2) if lt2 else 0
        print(f"  {key}: avg_lifetime trace1={avg1:.1f}, trace2={avg2:.1f}, diff={abs(avg1-avg2):.1f}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} trace1.etb.zst trace2.etb.zst")
        sys.exit(1)
    compare_lifetimes(sys.argv[1], sys.argv[2])
```

---

## Experiment 3: Per-Thread Clock Prototype

**Goal**: Test if per-thread allocation counters produce stable matching.

### Modified Oracle Format

Current format (global clock):
```csv
alloc_seq,free_at_seq,size,type,obj_id
1,10,24,java.util.ArrayList,12345
```

New format (per-thread clock):
```csv
thread_id,thread_alloc_seq,free_thread_id,free_thread_seq,size,type,obj_id
1,1,1,5,24,java.util.ArrayList,12345
```

Interpretation:
- Object allocated by thread 1 at its 1st allocation
- Free when thread 1 reaches its 5th allocation

### Modified Oracle Generator: `oracle_generator_threaded.py`

```python
#!/usr/bin/env python3
"""
Oracle generator with per-thread clocks.
"""

import argparse
from collections import defaultdict

def generate_threaded_oracle(trace_path, output_path, mode='liveness'):
    """Generate oracle with per-thread sequence numbers."""

    # Track per-thread allocation counts
    thread_alloc_seq = defaultdict(int)

    # Object info: obj_id -> {thread, thread_seq, size, type, last_use_thread, last_use_seq}
    objects = {}

    # Parse trace (simplified - use actual trace_converter)
    import subprocess
    result = subprocess.run(
        ['python3', 'trace_converter.py', trace_path, '/dev/stdout'],
        capture_output=True, text=True
    )

    for line in result.stdout.split('\n'):
        parts = line.strip().split()
        if not parts:
            continue

        event_type = parts[0]
        thread = parts[-1]

        if event_type == 'A':  # Allocation
            obj_id = parts[1]
            size = int(parts[2])
            type_id = parts[3]

            thread_alloc_seq[thread] += 1

            objects[obj_id] = {
                'thread': thread,
                'thread_seq': thread_alloc_seq[thread],
                'size': size,
                'type': type_id,
                'last_use_thread': thread,
                'last_use_seq': thread_alloc_seq[thread],
            }

        elif event_type == 'R':  # Read - object use
            obj_id = parts[1]
            if obj_id in objects:
                thread_alloc_seq[thread] += 1  # Count as logical time
                objects[obj_id]['last_use_thread'] = thread
                objects[obj_id]['last_use_seq'] = thread_alloc_seq[thread]

        elif event_type == 'D':  # Death (for reachability mode)
            obj_id = parts[1]
            if obj_id in objects and mode == 'reachability':
                thread_alloc_seq[thread] += 1
                objects[obj_id]['last_use_thread'] = thread
                objects[obj_id]['last_use_seq'] = thread_alloc_seq[thread]

    # Write oracle
    with open(output_path, 'w') as f:
        f.write('alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id\n')
        for obj_id, info in objects.items():
            f.write(f"{info['thread']},{info['thread_seq']},"
                    f"{info['last_use_thread']},{info['last_use_seq']},"
                    f"{info['size']},{info['type']},{obj_id}\n")

    print(f"Generated oracle with {len(objects)} entries")
    print(f"Threads seen: {list(thread_alloc_seq.keys())}")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('trace', help='Input trace file')
    parser.add_argument('-o', '--output', required=True, help='Output oracle file')
    parser.add_argument('-m', '--mode', choices=['liveness', 'reachability'], default='liveness')
    args = parser.parse_args()

    generate_threaded_oracle(args.trace, args.output, args.mode)
```

---

## Experiment 4: Cross-Thread Object Sharing Analysis

**Goal**: Determine how often objects are shared across threads.

### Analysis Script: `analyze_cross_thread.py`

```python
#!/usr/bin/env python3
"""
Analyze cross-thread object sharing.
If most objects are thread-local, per-thread clocks will work well.
If many objects are shared, we may need vector clocks.
"""

import subprocess
from collections import defaultdict

def analyze_sharing(trace_path):
    """Analyze how objects are used across threads."""

    result = subprocess.run(
        ['python3', 'trace_converter.py', trace_path, '/dev/stdout'],
        capture_output=True, text=True
    )

    # obj_id -> set of threads that used it
    object_threads = defaultdict(set)
    object_alloc_thread = {}

    for line in result.stdout.split('\n'):
        parts = line.strip().split()
        if not parts:
            continue

        event_type = parts[0]
        thread = parts[-1]

        if event_type == 'A':
            obj_id = parts[1]
            object_alloc_thread[obj_id] = thread
            object_threads[obj_id].add(thread)

        elif event_type in ('R', 'U'):  # Read or Update
            obj_id = parts[1] if event_type == 'R' else parts[2]
            if obj_id:
                object_threads[obj_id].add(thread)

    # Statistics
    total_objects = len(object_threads)
    thread_local = sum(1 for threads in object_threads.values() if len(threads) == 1)
    cross_thread = total_objects - thread_local

    print(f"Total objects: {total_objects}")
    print(f"Thread-local objects: {thread_local} ({100*thread_local/total_objects:.1f}%)")
    print(f"Cross-thread objects: {cross_thread} ({100*cross_thread/total_objects:.1f}%)")

    if cross_thread > 0:
        print("\nCross-thread objects breakdown:")
        by_thread_count = defaultdict(int)
        for obj_id, threads in object_threads.items():
            if len(threads) > 1:
                by_thread_count[len(threads)] += 1

        for n, count in sorted(by_thread_count.items()):
            print(f"  Used by {n} threads: {count} objects")

if __name__ == '__main__':
    import sys
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} trace.etb.zst")
        sys.exit(1)
    analyze_sharing(sys.argv[1])
```

---

## Vector Clocks Primer

### What is a Vector Clock?

A vector clock is `[count_t1, count_t2, ..., count_tN]` where each entry tracks the logical time of one thread.

### Happens-Before Relation

Event A **happens-before** event B (A → B) if:
- `VC(A)[i] <= VC(B)[i]` for all threads i

### Example

```
Thread 1: alloc(X)  at VC=[1,0,0]
Thread 2: use(X)    at VC=[1,1,0]  (knows about T1's alloc)
Thread 1: use(X)    at VC=[2,0,0]
Thread 3: use(X)    at VC=[2,1,1]  (knows about both)

Object X should be freed when:
  current_VC >= [2, 1, 1]
  i.e., T1 >= 2 AND T2 >= 1 AND T3 >= 1
```

### When Do We Need Vector Clocks?

Only if cross-thread sharing is significant AND we need precise ordering.

For most Java programs:
- Objects are mostly thread-local (created and used by one thread)
- Per-thread clocks should suffice
- Vector clocks add complexity without much benefit

---

## Decision Framework

```
                 Run Experiment 1
                 (Trace Stability)
                        |
           +------------+------------+
           |                         |
    Traces Stable              Traces Differ
    (single-core works)        (need logical clocks)
           |                         |
     Use single-core           Run Experiment 4
     for experiments           (Cross-thread Analysis)
                                     |
                        +------------+------------+
                        |                         |
                  <5% cross-thread          >20% cross-thread
                        |                         |
                  Per-Thread Clocks         Vector Clocks
                  (simpler, faster)         (complex, correct)
```

---

## Recommended Action Plan

### Step 1: Run Stability Test (30 min)
```bash
cd corretto-25
javac ThreadStabilityTest.java
./run_stability_test.sh
```

### Step 2: Analyze Cross-Thread Sharing (15 min)
```bash
# Use an existing trace
python3 analyze_cross_thread.py ../lusearch_trace.etb.zst
```

### Step 3: Based on Results

**If traces are stable with single-core:**
- Use `-XX:ActiveProcessorCount=1` for experiments
- No changes needed to oracle generator
- This is the simplest path

**If cross-thread sharing is low (<5%):**
- Implement per-thread clocks
- Modify oracle generator to output `(thread_id, thread_seq)`
- Modify OracleGC to track per-thread counters

**If cross-thread sharing is high (>20%):**
- Implement vector clocks
- More complex but handles all cases
- May need to modify Elephant Tracks for clock propagation

---

## Quick Test: Does Order Even Matter?

Before implementing anything, test if the current type-based matching already handles this:

```bash
# Run the same trace twice and see if allocations match
cd corretto-25

# Generate fresh trace
java -javaagent:../elephant-tracks/target/elephant-tracks-*.jar=output=test1.etb.zst \
    ThreadStabilityTest

# Generate oracle
python3 ../oracle_generator.py test1.etb.zst -o oracle1.csv -m liveness

# Run with OracleGC
./build/*/images/jdk/bin/java \
    -XX:+UnlockExperimentalVMOptions \
    -XX:+UseEpsilonGC \
    -XX:+EpsilonOracleMode \
    -XX:EpsilonOracleTracePath=oracle1.csv \
    -XX:-UseTLAB \
    ThreadStabilityTest

# Check: Did all objects get freed correctly?
```

The type-based matching may already provide enough flexibility for small variations in order!
