#!/usr/bin/env python3
"""
Analyze cross-thread object sharing in Elephant Tracks traces.

If most objects are thread-local, per-thread clocks will work well.
If many objects are shared across threads, we may need vector clocks.

Usage:
    python3 analyze_cross_thread.py trace.etb.zst
"""

import sys
import os
from collections import defaultdict

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    from trace_converter import read_trace
except ImportError:
    print("Note: trace_converter not found, using simplified parsing")
    read_trace = None


def parse_trace_simple(trace_path):
    """Simple trace parsing using view_trace.sh."""
    import subprocess

    # Try using view_trace.sh
    view_trace = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'view_trace.sh')

    if os.path.exists(view_trace):
        result = subprocess.run(
            [view_trace, trace_path, 'view'],
            capture_output=True, text=True
        )
        return result.stdout.split('\n')
    else:
        # Try direct trace_converter
        trace_converter = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'trace_converter.py')
        result = subprocess.run(
            ['python3', trace_converter, trace_path, '/dev/stdout'],
            capture_output=True, text=True
        )
        return result.stdout.split('\n')


def analyze_sharing(trace_path):
    """Analyze how objects are used across threads."""

    print(f"Analyzing: {trace_path}")
    print()

    lines = parse_trace_simple(trace_path)

    # obj_id -> set of threads that used it
    object_threads = defaultdict(set)
    object_alloc_thread = {}
    object_type = {}

    # Track allocations and uses
    total_allocs = 0
    total_uses = 0

    for line in lines:
        parts = line.strip().split()
        if not parts or len(parts) < 2:
            continue

        event_type = parts[0]

        if event_type == 'A':  # Allocation
            # A <obj_id> <size> <type_id> <site_id> <array_len> <thread>
            if len(parts) >= 7:
                obj_id = parts[1]
                type_id = parts[3]
                thread = parts[-1]

                object_alloc_thread[obj_id] = thread
                object_threads[obj_id].add(thread)
                object_type[obj_id] = type_id
                total_allocs += 1

        elif event_type == 'R':  # Read
            # R <target> <field_id> <thread>
            if len(parts) >= 4:
                obj_id = parts[1]
                thread = parts[-1]
                if obj_id and obj_id != '0':
                    object_threads[obj_id].add(thread)
                    total_uses += 1

        elif event_type == 'U':  # Update (pointer write)
            # U <field_id> <old_target> <new_target> <thread>
            if len(parts) >= 5:
                old_target = parts[2]
                new_target = parts[3]
                thread = parts[-1]
                if old_target and old_target != '0':
                    object_threads[old_target].add(thread)
                if new_target and new_target != '0':
                    object_threads[new_target].add(thread)
                total_uses += 1

    # Statistics
    total_objects = len(object_threads)

    if total_objects == 0:
        print("No objects found in trace!")
        return

    thread_local = sum(1 for threads in object_threads.values() if len(threads) == 1)
    cross_thread = total_objects - thread_local

    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Total allocations:     {total_allocs}")
    print(f"Total object uses:     {total_uses}")
    print(f"Unique objects:        {total_objects}")
    print()
    print(f"Thread-local objects:  {thread_local} ({100*thread_local/total_objects:.1f}%)")
    print(f"Cross-thread objects:  {cross_thread} ({100*cross_thread/total_objects:.1f}%)")
    print()

    # Recommendation
    print("=" * 60)
    print("RECOMMENDATION")
    print("=" * 60)

    if cross_thread == 0:
        print("All objects are thread-local.")
        print("-> Per-thread clocks will work perfectly!")
        print("-> No need for vector clocks.")
    elif 100*cross_thread/total_objects < 5:
        print(f"Only {100*cross_thread/total_objects:.1f}% of objects are cross-thread.")
        print("-> Per-thread clocks should work well for most cases.")
        print("-> Consider special handling for shared objects.")
    elif 100*cross_thread/total_objects < 20:
        print(f"{100*cross_thread/total_objects:.1f}% of objects are cross-thread.")
        print("-> Per-thread clocks may work, but some mismatches expected.")
        print("-> Consider implementing vector clocks for accuracy.")
    else:
        print(f"WARNING: {100*cross_thread/total_objects:.1f}% of objects are cross-thread!")
        print("-> Vector clocks are recommended for correct behavior.")
    print()

    if cross_thread > 0:
        print("=" * 60)
        print("CROSS-THREAD DETAILS")
        print("=" * 60)

        # Breakdown by thread count
        by_thread_count = defaultdict(list)
        for obj_id, threads in object_threads.items():
            if len(threads) > 1:
                by_thread_count[len(threads)].append(obj_id)

        for n in sorted(by_thread_count.keys()):
            objects = by_thread_count[n]
            print(f"\nUsed by {n} threads: {len(objects)} objects")

            # Show first few examples
            for obj_id in objects[:3]:
                threads = object_threads[obj_id]
                alloc_thread = object_alloc_thread.get(obj_id, '?')
                typ = object_type.get(obj_id, '?')
                print(f"  - obj={obj_id}, type={typ}, alloc_by={alloc_thread}, used_by={threads}")

    # Thread statistics
    print()
    print("=" * 60)
    print("THREAD STATISTICS")
    print("=" * 60)

    thread_alloc_counts = defaultdict(int)
    for obj_id, thread in object_alloc_thread.items():
        thread_alloc_counts[thread] += 1

    for thread, count in sorted(thread_alloc_counts.items(), key=lambda x: -x[1]):
        print(f"  Thread {thread}: {count} allocations")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} trace.etb.zst")
        print()
        print("Analyzes cross-thread object sharing to determine if:")
        print("  - Per-thread clocks are sufficient")
        print("  - Vector clocks are needed")
        sys.exit(1)

    analyze_sharing(sys.argv[1])
