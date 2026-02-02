/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_EPSILON_EPSILONORACLE_HPP
#define SHARE_GC_EPSILON_EPSILONORACLE_HPP

#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutex.hpp"
#include "utilities/globalDefinitions.hpp"

// Oracle entry from the trace file (per-thread format with LOGICAL thread IDs)
// Format: alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id
// Thread IDs are LOGICAL (0, 1, 2, ...) based on order of first allocation in trace.
// At runtime, OS thread IDs are mapped to logical IDs in the same order.
struct OracleEntry {
  int32_t  alloc_thread;   // LOGICAL thread ID that allocated the object (0, 1, 2, ...)
  uint64_t alloc_seq;      // Per-thread allocation sequence number (1, 2, 3, ...)
  int32_t  free_thread;    // LOGICAL thread ID whose clock triggers free
  uint64_t free_seq;       // Per-thread sequence at which to free
  size_t   size;           // Object size in bytes
  char     type[128];      // Object type name (for debugging only, not used for matching)
};

// Death bucket for the death map (linked list node)
// Keyed by (logical_thread_id, free_seq) pair
struct DeathBucket {
  int32_t  logical_thread; // LOGICAL thread ID for this entry
  uint64_t seq;            // The free_seq for this entry
  void*    ptr;            // Pointer to the allocated memory
  size_t   size;           // Size for tracking
  DeathBucket* next;       // Next in chain
};

// Runtime thread to logical thread mapping
struct RuntimeThreadMapping {
  int64_t  runtime_thread_id;  // OS thread ID at runtime
  int32_t  logical_thread_id;  // Mapped logical thread ID (0, 1, 2, ...)
  RuntimeThreadMapping* next;  // Next in hash chain
};

// Free block for the free list (first-fit allocator within heap)
struct OracleFreeBlock {
  HeapWord* addr;          // Address of free block (in heap)
  size_t    size;          // Size in HeapWords
  OracleFreeBlock* next;   // Next in free list
};

// Malloc pointer tracking bucket (for EpsilonOracleMallocMode validation)
struct MallocPtrBucket {
  void*    ptr;            // The malloc'd pointer
  size_t   size;           // Size of allocation
  MallocPtrBucket* next;   // Next in hash chain
};

// Per-thread allocation state (keyed by LOGICAL thread ID)
struct ThreadAllocState {
  int32_t  logical_thread; // LOGICAL thread ID (0, 1, 2, ...)
  uint64_t alloc_count;    // Number of allocations on this thread
  size_t   next_entry_idx; // Next oracle entry index for this thread
  ThreadAllocState* next;  // Next in hash chain
};

// Oracle-based memory manager for deterministic malloc/free
// based on pre-computed trace from Elephant Tracks.
//
// This implementation uses PER-THREAD allocation sequence numbers
// instead of global sequences, enabling deterministic replay without
// requiring global execution determinism (which is impossible without
// a simulator like the original paper used).
//
// Key insight: Per-thread allocation order IS deterministic even when
// global thread interleaving varies.
//
// Matching strategy: Pure sequential per-thread
//   - N-th allocation on thread T matches N-th oracle entry for thread T
//   - No type matching needed (unlike the previous implementation)
//   - Objects freed when specified thread reaches specified per-thread sequence
class EpsilonOracle : public CHeapObj<mtGC> {
private:
  // Oracle entries loaded from trace file (sorted by alloc_thread, alloc_seq)
  OracleEntry* _entries;
  size_t _entry_count;
  size_t _entry_capacity;

  // Death map: hash table mapping (free_thread, free_seq) -> list of pointers to free
  static const size_t DEATH_MAP_SIZE = 1 << 20;  // 1M buckets
  DeathBucket** _death_map;

  // Per-thread allocation state: array indexed by LOGICAL thread ID
  // Direct array access since logical IDs are small (0, 1, 2, ...)
  static const size_t MAX_LOGICAL_THREADS = 256;  // Max threads we support
  ThreadAllocState* _thread_states[MAX_LOGICAL_THREADS];

  // Runtime thread ID -> logical thread ID mapping
  static const size_t RUNTIME_THREAD_MAP_SIZE = 1 << 10;  // 1K buckets
  RuntimeThreadMapping** _runtime_thread_map;
  volatile int32_t _next_logical_thread_id;  // Next logical ID to assign
  int32_t _num_oracle_threads;  // Number of unique threads in the oracle

  // Index to quickly find first entry for each LOGICAL thread
  // Direct array access since logical IDs are small
  struct ThreadEntryIndex {
    size_t first_entry_idx;
    size_t entry_count;      // Total entries for this thread
  };
  ThreadEntryIndex _thread_entry_index[MAX_LOGICAL_THREADS];

  // Global counters (volatile for atomic access)
  volatile uint64_t _total_alloc_counter;  // Total allocations across all threads
  volatile uint64_t _free_counter;
  volatile size_t   _allocated_bytes;
  volatile size_t   _freed_bytes;

  // Free list for recycling freed memory (first-fit allocator)
  OracleFreeBlock* _free_list;
  size_t _free_list_count;
  size_t _free_list_bytes;

  // Application start detection via JVMTI agent signal
  volatile bool _app_started;
  volatile uint64_t _pre_app_alloc_count;

  // Tracked allocation counter (allocations that matched oracle entries)
  volatile uint64_t _tracked_alloc_counter;

  // Malloc pointer tracking (for EpsilonOracleMallocMode)
  static const size_t MALLOC_PTR_MAP_SIZE = 1 << 20;
  MallocPtrBucket** _malloc_ptr_map;
  volatile size_t _malloc_tracked_count;
  volatile size_t _malloc_freed_count;

  // Mutex for thread-safe access to thread map and death map
  Mutex* _oracle_lock;

  // Hash function for death map: combines logical_thread_id and seq
  size_t hash_thread_seq(int32_t logical_thread, uint64_t seq) const {
    // Mix logical thread ID and seq for distribution
    uint64_t h = (uint64_t)logical_thread ^ (seq * 2654435761ULL);
    return (size_t)(h & (DEATH_MAP_SIZE - 1));
  }

  // Hash function for runtime thread ID mapping
  size_t hash_runtime_thread(int64_t runtime_thread_id) const {
    return (size_t)(((uint64_t)runtime_thread_id * 2654435761ULL) & (RUNTIME_THREAD_MAP_SIZE - 1));
  }

  // Map runtime OS thread ID to logical thread ID
  // Returns -1 if app not started, or assigns new logical ID if first time seeing this thread
  int32_t map_runtime_to_logical(int64_t runtime_thread_id);

  // Hash function for malloc pointer tracking
  size_t hash_ptr(void* ptr) const {
    return ((uintptr_t)ptr >> 3) & (MALLOC_PTR_MAP_SIZE - 1);
  }

  // Build thread entry index after loading trace
  void build_thread_entry_index();

  // Get or create thread allocation state for a logical thread
  ThreadAllocState* get_or_create_thread_state(int32_t logical_thread);

  // Find oracle entry for logical thread at given per-thread sequence
  // Returns entry index or SIZE_MAX if not found
  size_t find_entry_for_thread(int32_t logical_thread, uint64_t per_thread_seq);

public:
  EpsilonOracle();
  ~EpsilonOracle();

  // Load oracle trace from CSV file
  // New format: alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id
  bool load_trace(const char* path);

  // Register an allocation for a specific thread (takes runtime OS thread ID)
  // Returns true if allocation was matched to an oracle entry
  // runtime_thread_id: OS thread ID of allocating thread (will be mapped to logical)
  // ptr: allocated memory
  // size: size in bytes
  bool register_allocation(int64_t runtime_thread_id, void* ptr, size_t size);

  // Process all deaths for a specific logical thread at given per-thread sequence
  // Adds freed memory to free list
  void process_deaths(int32_t logical_thread, uint64_t per_thread_seq);

  // Process deaths with actual malloc/free (for EpsilonOracleMallocMode)
  void process_deaths_malloc_mode(int32_t logical_thread, uint64_t per_thread_seq);

  // Get current per-thread allocation count for a logical thread
  uint64_t get_thread_alloc_count(int32_t logical_thread);

  // Increment and return per-thread allocation count (takes runtime OS thread ID)
  // Returns 0 if app not started or thread couldn't be mapped
  uint64_t next_thread_alloc_seq(int64_t runtime_thread_id);

  // Get logical thread ID for a runtime thread (or -1 if not mapped)
  int32_t get_logical_thread(int64_t runtime_thread_id) const;

  // Track a malloc'd pointer (for validation in EpsilonOracleMallocMode)
  void track_malloc_ptr(void* ptr, size_t size);

  // Untrack and free a malloc'd pointer, returns true if found
  bool untrack_malloc_ptr(void* ptr, size_t* out_size);

  // Check if a pointer is tracked in the malloc pointer map
  bool is_malloc_tracked(void* ptr) const;

  // Free list allocator methods
  HeapWord* allocate_from_free_list(size_t size);
  void add_to_free_list(HeapWord* addr, size_t size);

  // Application start detection (signaled by JVMTI agent)
  void signal_app_start();
  bool app_started() const { return _app_started; }
  uint64_t pre_app_alloc_count() const { return _pre_app_alloc_count; }

  // Count a pre-app allocation (for auto-start detection based on EpsilonOracleSkipAllocs)
  void count_pre_app_alloc();

  // Statistics accessors
  size_t entry_count() const { return _entry_count; }
  uint64_t total_alloc_count() const { return _total_alloc_counter; }
  uint64_t tracked_alloc_count() const { return _tracked_alloc_counter; }
  uint64_t free_count() const { return _free_counter; }
  size_t allocated_bytes() const { return _allocated_bytes; }
  size_t freed_bytes() const { return _freed_bytes; }

  // Print statistics
  void print_stats() const;

  // Finalize - free any remaining tracked objects and print final stats
  void finalize();

  // Cleanup - free data structures (called by destructor)
  void cleanup();
};

#endif // SHARE_GC_EPSILON_EPSILONORACLE_HPP
