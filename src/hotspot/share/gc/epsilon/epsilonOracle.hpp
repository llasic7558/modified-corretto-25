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
#include "utilities/globalDefinitions.hpp"

// Oracle entry from the trace file
struct OracleEntry {
  uint64_t alloc_seq;    // Allocation sequence number
  uint64_t free_at_seq;  // Sequence number at which to free
  size_t   size;         // Object size in bytes
  char     type[128];    // Object type name from trace
};

// Death bucket for the death map (linked list node)
struct DeathBucket {
  uint64_t seq;          // The free_at_seq for this entry
  void*    ptr;          // Pointer to the allocated memory
  size_t   size;         // Size for tracking
  DeathBucket* next;     // Next in chain
};

// Free block for the free list (first-fit allocator within heap)
struct OracleFreeBlock {
  HeapWord* addr;        // Address of free block (in heap)
  size_t    size;        // Size in HeapWords
  OracleFreeBlock* next;       // Next in free list
};

// Oracle-based memory manager for deterministic malloc/free
// based on pre-computed trace from Elephant Tracks
class EpsilonOracle : public CHeapObj<mtGC> {
private:
  // Oracle entries loaded from trace file
  OracleEntry* _entries;
  size_t _entry_count;
  size_t _entry_capacity;

  // Death map: hash table mapping free_at_seq -> list of pointers to free
  static const size_t DEATH_MAP_SIZE = 1 << 20;  // 1M buckets
  DeathBucket** _death_map;

  // Global counters (volatile for atomic access)
  volatile uint64_t _alloc_counter;
  volatile uint64_t _free_counter;
  volatile size_t   _allocated_bytes;
  volatile size_t   _freed_bytes;

  // Current index into entries array (for sequential matching)
  size_t _current_entry_idx;

  // Free list for recycling freed memory (first-fit allocator)
  OracleFreeBlock* _free_list;
  size_t _free_list_count;
  size_t _free_list_bytes;

  // Application start detection via JVMTI agent signal
  volatile bool _app_started;           // True once JVMTI agent signals main() entry
  volatile uint64_t _pre_app_alloc_count;  // Allocations before app started

  // Separate counter for app allocations that match the trace
  // This counter ONLY increments when we successfully match an allocation to a trace entry
  volatile uint64_t _app_alloc_counter;  // 1-based counter for matched app allocations

  // Hash function for death map
  size_t hash_seq(uint64_t seq) const {
    return (size_t)(seq & (DEATH_MAP_SIZE - 1));
  }

public:
  EpsilonOracle();
  ~EpsilonOracle();

  // Load oracle trace from CSV file
  // Format: alloc_seq,free_at_seq,size,type,obj_id
  bool load_trace(const char* path);

  // Get next allocation sequence number (atomic increment)
  uint64_t next_alloc_seq() {
    return Atomic::add(&_alloc_counter, (uint64_t)1);
  }

  // Get current allocation count (for checking)
  uint64_t current_alloc_seq() const { return _alloc_counter; }

  // Get expected size for current allocation (for validation)
  size_t expected_size(uint64_t alloc_seq) const;

  // Register an allocation - adds to death map for later freeing
  void register_allocation(uint64_t alloc_seq, void* ptr, size_t size);

  // Process all deaths at the given sequence number
  // Adds freed memory to free list instead of calling os::free
  void process_deaths(uint64_t current_seq);

  // Free list allocator methods
  // Try to allocate from free list (first-fit), returns nullptr if no fit found
  HeapWord* allocate_from_free_list(size_t size);

  // Add a block to the free list (called when processing deaths)
  void add_to_free_list(HeapWord* addr, size_t size);

  // Application start detection (signaled by JVMTI agent)
  void signal_app_start();
  bool app_started() const { return _app_started; }
  uint64_t pre_app_alloc_count() const { return _pre_app_alloc_count; }

  // Check if allocation matches expected trace entry (by size and type)
  // Returns true if this allocation should be tracked
  // type_name should be in format "java.util.ArrayList" or "java/util/ArrayList"
  bool matches_expected_entry(size_t size_in_bytes, const char* type_name) const;

  // Get next app allocation sequence number (only call if matches_expected_entry returned true)
  // Returns the 1-based trace sequence number
  uint64_t next_app_alloc_seq();

  // Get current app allocation count
  uint64_t app_alloc_count() const { return _app_alloc_counter; }

  // Statistics accessors
  size_t entry_count() const { return _entry_count; }
  uint64_t alloc_count() const { return _alloc_counter; }
  uint64_t free_count() const { return _free_counter; }
  size_t allocated_bytes() const { return _allocated_bytes; }
  size_t freed_bytes() const { return _freed_bytes; }

  // Print statistics
  void print_stats() const;

  // Finalize - free any remaining tracked objects and print final stats
  // Should be called before JVM shutdown while logging is still active
  void finalize();

  // Cleanup - free data structures (called by destructor)
  void cleanup();
};

#endif // SHARE_GC_EPSILON_EPSILONORACLE_HPP
