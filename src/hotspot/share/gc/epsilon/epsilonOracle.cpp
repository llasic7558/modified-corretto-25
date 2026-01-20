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

#include "gc/epsilon/epsilonOracle.hpp"
#include "gc/epsilon/epsilonHeap.hpp"
#include "gc/epsilon/epsilon_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

EpsilonOracle::EpsilonOracle() :
  _entries(nullptr),
  _entry_count(0),
  _entry_capacity(0),
  _death_map(nullptr),
  _alloc_counter(0),
  _free_counter(0),
  _allocated_bytes(0),
  _freed_bytes(0),
  _current_entry_idx(0),
  _free_list(nullptr),
  _free_list_count(0),
  _free_list_bytes(0),
  _app_started(false),
  _pre_app_alloc_count(0),
  _app_alloc_counter(0) {

  // Allocate death map buckets (initialized to nullptr)
  _death_map = NEW_C_HEAP_ARRAY(DeathBucket*, DEATH_MAP_SIZE, mtGC);
  memset(_death_map, 0, sizeof(DeathBucket*) * DEATH_MAP_SIZE);
}

EpsilonOracle::~EpsilonOracle() {
  cleanup();
}

bool EpsilonOracle::load_trace(const char* path) {
  if (path == nullptr) {
    log_error(gc)("Oracle trace path is null");
    return false;
  }

  FILE* f = fopen(path, "r");
  if (f == nullptr) {
    log_error(gc)("Cannot open oracle trace file: %s", path);
    return false;
  }

  // Initial capacity for entries
  _entry_capacity = 1024 * 1024;  // 1M entries initially
  _entries = NEW_C_HEAP_ARRAY(OracleEntry, _entry_capacity, mtGC);
  _entry_count = 0;

  // Skip header line: alloc_seq,free_at_seq,size,type,obj_id
  char line[1024];
  if (fgets(line, sizeof(line), f) == nullptr) {
    log_error(gc)("Oracle trace file is empty or unreadable: %s", path);
    fclose(f);
    return false;
  }

  // Verify header format
  if (strstr(line, "alloc_seq") == nullptr) {
    log_warning(gc)("Oracle trace file may not have expected header: %s", line);
    // Rewind to start if no header detected
    rewind(f);
  }

  // Parse CSV entries
  // Format: alloc_seq,free_at_seq,size,type,obj_id
  uint64_t alloc_seq, free_at_seq, obj_id;
  size_t size;
  char type[512];

  while (fscanf(f, "%" SCNu64 ",%" SCNu64 ",%zu,%511[^,],%" SCNu64 "\n",
                &alloc_seq, &free_at_seq, &size, type, &obj_id) == 5) {

    // Grow array if needed
    if (_entry_count >= _entry_capacity) {
      size_t new_capacity = _entry_capacity * 2;
      OracleEntry* new_entries = NEW_C_HEAP_ARRAY(OracleEntry, new_capacity, mtGC);
      memcpy(new_entries, _entries, sizeof(OracleEntry) * _entry_count);
      FREE_C_HEAP_ARRAY(OracleEntry, _entries);
      _entries = new_entries;
      _entry_capacity = new_capacity;
    }

    OracleEntry& entry = _entries[_entry_count];
    entry.alloc_seq = alloc_seq;
    entry.free_at_seq = free_at_seq;
    entry.size = size;
    strncpy(entry.type, type, sizeof(entry.type) - 1);
    entry.type[sizeof(entry.type) - 1] = '\0';
    _entry_count++;
  }

  fclose(f);

  log_info(gc)("Oracle: Loaded %zu entries from %s", _entry_count, path);

  if (_entry_count == 0) {
    log_warning(gc)("Oracle trace file contained no valid entries");
  }

  return true;
}

size_t EpsilonOracle::expected_size(uint64_t alloc_seq) const {
  // alloc_seq is 1-based, array is 0-based
  size_t idx = (size_t)(alloc_seq - 1);
  if (idx < _entry_count) {
    return _entries[idx].size;
  }
  return 0;  // Unknown
}

void EpsilonOracle::register_allocation(uint64_t alloc_seq, void* ptr, size_t size) {
  // Look up the oracle entry for this allocation
  // alloc_seq is 1-based (first allocation is seq 1)
  size_t idx = (size_t)(alloc_seq - 1);

  if (idx < _entry_count) {
    OracleEntry& entry = _entries[idx];

    // Validate sequence number matches
    if (entry.alloc_seq != alloc_seq) {
      log_warning(gc)("Oracle: Allocation sequence mismatch at idx %zu: expected " UINT64_FORMAT ", got " UINT64_FORMAT,
                      idx, entry.alloc_seq, alloc_seq);
    }

    // Size validation - log mismatches with type info
    if (entry.size != size) {
      log_info(gc)("Oracle: MISMATCH trace_seq=" UINT64_FORMAT " expected=[%s size=%zu] actual_size=%zu",
                    alloc_seq, entry.type, entry.size, size);
    } else {
      log_debug(gc)("Oracle: MATCH trace_seq=" UINT64_FORMAT " type=[%s] size=%zu",
                    alloc_seq, entry.type, size);
    }

    // Add to death map at the free_at_seq
    uint64_t free_seq = entry.free_at_seq;

    // Only add to death map if free_seq is valid (non-zero and not MAX)
    if (free_seq > 0 && free_seq != UINT64_MAX) {
      size_t bucket_idx = hash_seq(free_seq);

      DeathBucket* new_bucket = NEW_C_HEAP_OBJ(DeathBucket, mtGC);
      new_bucket->seq = free_seq;
      new_bucket->ptr = ptr;
      new_bucket->size = size;

      // Thread-safe insertion at head of bucket chain
      // Note: For simplicity, we use a non-atomic insert here.
      // In a multi-threaded scenario, we would need a lock or CAS.
      // Since allocations are serialized through allocate_work, this is safe.
      new_bucket->next = _death_map[bucket_idx];
      _death_map[bucket_idx] = new_bucket;

      log_info(gc)("Oracle: REGISTERED alloc_seq=" UINT64_FORMAT " type=[%s] ptr=" PTR_FORMAT " size=%zu -> scheduled FREE at seq=" UINT64_FORMAT,
                   alloc_seq, entry.type, p2i(ptr), size, free_seq);
    } else {
      log_info(gc)("Oracle: REGISTERED alloc_seq=" UINT64_FORMAT " type=[%s] ptr=" PTR_FORMAT " size=%zu (IMMORTAL - no free scheduled)",
                   alloc_seq, entry.type, p2i(ptr), size);
    }
  } else {
    // Allocation beyond trace - this can happen for VM internal allocations
    log_trace(gc)("Oracle: Allocation seq " UINT64_FORMAT " beyond trace entries (%zu)",
                  alloc_seq, _entry_count);
  }

  // Track allocated bytes
  Atomic::add(&_allocated_bytes, size);
}

void EpsilonOracle::process_deaths(uint64_t current_seq) {
  // Process all deaths scheduled for current_seq
  size_t bucket_idx = hash_seq(current_seq);

  DeathBucket** prev_ptr = &_death_map[bucket_idx];
  DeathBucket* bucket = *prev_ptr;

  while (bucket != nullptr) {
    DeathBucket* next = bucket->next;

    if (bucket->seq == current_seq) {
      // This object should be freed now
      HeapWord* ptr = (HeapWord*)bucket->ptr;
      size_t size_in_bytes = bucket->size;
      size_t size_in_words = size_in_bytes / HeapWordSize;

      // Log deallocation at info level so it's visible
      log_info(gc)("Oracle: FREE at seq=" UINT64_FORMAT " ptr=" PTR_FORMAT " size=%zu bytes (free_counter=" UINT64_FORMAT ")",
                    current_seq, p2i(ptr), size_in_bytes, _free_counter + 1);

      // Add to free list instead of calling os::free
      // (Memory is within heap region, not from os::malloc)
      add_to_free_list(ptr, size_in_words);

      // Update statistics
      Atomic::add(&_free_counter, (uint64_t)1);
      Atomic::add(&_freed_bytes, size_in_bytes);

      // Remove bucket from chain
      *prev_ptr = next;
      FREE_C_HEAP_OBJ(bucket);
    } else {
      // Keep this entry, move to next
      prev_ptr = &bucket->next;
    }

    bucket = next;
  }
}

void EpsilonOracle::print_stats() const {
  log_info(gc)("Oracle Statistics:");
  log_info(gc)("  Trace entries:      %zu", _entry_count);
  log_info(gc)("  Total allocations:  " UINT64_FORMAT, _alloc_counter);
  log_info(gc)("  Pre-app allocations:" UINT64_FORMAT, _pre_app_alloc_count);
  log_info(gc)("  Tracked app allocs: " UINT64_FORMAT, _app_alloc_counter);
  log_info(gc)("  Frees:              " UINT64_FORMAT, _free_counter);
  log_info(gc)("  Bytes allocated:    %zu", _allocated_bytes);
  log_info(gc)("  Bytes freed:        %zu", _freed_bytes);
  log_info(gc)("  Bytes live:         %zu", _allocated_bytes - _freed_bytes);
  log_info(gc)("  Free list blocks:   %zu", _free_list_count);
  log_info(gc)("  Free list bytes:    %zu", _free_list_bytes);
}

HeapWord* EpsilonOracle::allocate_from_free_list(size_t size) {
  // First-fit allocation from free list
  // size is in HeapWords
  OracleFreeBlock** prev_ptr = &_free_list;
  OracleFreeBlock* block = _free_list;

  while (block != nullptr) {
    if (block->size >= size) {
      // Found a block that fits
      HeapWord* result = block->addr;

      if (block->size > size) {
        // Split the block: keep the remainder in the free list
        block->addr = block->addr + size;
        block->size = block->size - size;
        _free_list_bytes -= size * HeapWordSize;
      } else {
        // Exact fit: remove block from free list
        *prev_ptr = block->next;
        _free_list_count--;
        _free_list_bytes -= size * HeapWordSize;
        FREE_C_HEAP_OBJ(block);
      }

      log_trace(gc)("Oracle: Allocated from free list ptr=" PTR_FORMAT " size=%zu words",
                    p2i(result), size);
      return result;
    }
    prev_ptr = &block->next;
    block = block->next;
  }

  // No suitable block found
  return nullptr;
}

void EpsilonOracle::add_to_free_list(HeapWord* addr, size_t size) {
  // Add a block to the free list
  // Simple insertion at head (no coalescing for now)
  OracleFreeBlock* block = NEW_C_HEAP_OBJ(OracleFreeBlock, mtGC);
  block->addr = addr;
  block->size = size;
  block->next = _free_list;
  _free_list = block;
  _free_list_count++;
  _free_list_bytes += size * HeapWordSize;

  log_trace(gc)("Oracle: Added to free list ptr=" PTR_FORMAT " size=%zu words",
                p2i(addr), size);
}

void EpsilonOracle::signal_app_start() {
  if (_app_started) {
    log_warning(gc)("Oracle: signal_app_start called but app already started");
    return;
  }

  _pre_app_alloc_count = _alloc_counter;
  _app_started = true;

  log_info(gc)("Oracle: Application started (signaled by JVMTI agent)");
  log_info(gc)("Oracle: Pre-app allocations: " UINT64_FORMAT ", trace entries: %zu",
               _pre_app_alloc_count, _entry_count);
}

// Helper to normalize array type names
// Trace format: "int[]", "java.lang.String[]", "Node[]"
// JVM format:   "[I",    "[Ljava.lang.String;", "[LNode;"
static bool normalize_and_compare(const char* trace_type, const char* klass_type) {
  if (trace_type == nullptr || klass_type == nullptr) {
    return false;
  }

  // Check if trace_type is an array (ends with "[]")
  size_t trace_len = strlen(trace_type);
  bool trace_is_array = (trace_len >= 2 &&
                         trace_type[trace_len-2] == '[' &&
                         trace_type[trace_len-1] == ']');

  // Check if klass_type is an array (starts with "[")
  bool klass_is_array = (klass_type[0] == '[');

  // Both must be arrays or both must be non-arrays
  if (trace_is_array != klass_is_array) {
    return false;
  }

  if (trace_is_array) {
    // Handle array types
    // trace: "int[]" -> element = "int"
    // klass: "[I" -> need to decode

    // Handle primitive arrays
    if (strcmp(trace_type, "int[]") == 0) {
      return strcmp(klass_type, "[I") == 0;
    }
    if (strcmp(trace_type, "long[]") == 0) {
      return strcmp(klass_type, "[J") == 0;
    }
    if (strcmp(trace_type, "byte[]") == 0) {
      return strcmp(klass_type, "[B") == 0;
    }
    if (strcmp(trace_type, "char[]") == 0) {
      return strcmp(klass_type, "[C") == 0;
    }
    if (strcmp(trace_type, "short[]") == 0) {
      return strcmp(klass_type, "[S") == 0;
    }
    if (strcmp(trace_type, "float[]") == 0) {
      return strcmp(klass_type, "[F") == 0;
    }
    if (strcmp(trace_type, "double[]") == 0) {
      return strcmp(klass_type, "[D") == 0;
    }
    if (strcmp(trace_type, "boolean[]") == 0) {
      return strcmp(klass_type, "[Z") == 0;
    }

    // Handle object arrays: "java.lang.String[]" vs "[Ljava.lang.String;"
    // Extract element type from trace (remove "[]")
    char trace_element[256];
    strncpy(trace_element, trace_type, trace_len - 2);
    trace_element[trace_len - 2] = '\0';

    // Extract element type from klass (remove "[L" prefix and ";" suffix)
    if (klass_type[1] == 'L') {
      size_t klass_len = strlen(klass_type);
      char klass_element[256];
      strncpy(klass_element, klass_type + 2, klass_len - 3);  // Skip "[L" and ";"
      klass_element[klass_len - 3] = '\0';

      // Now compare trace_element and klass_element with '.' / '/' normalization
      const char* t = trace_element;
      const char* k = klass_element;
      while (*t && *k) {
        char tc = (*t == '.') ? '/' : *t;
        char kc = (*k == '.') ? '/' : *k;
        if (tc != kc) return false;
        t++; k++;
      }
      return (*t == '\0' && *k == '\0');
    }
    return false;
  }

  // Non-array: compare directly with '.' / '/' normalization
  while (*trace_type && *klass_type) {
    char tc = *trace_type;
    char kc = *klass_type;

    // Normalize separators
    if (tc == '.') tc = '/';
    if (kc == '.') kc = '/';

    if (tc != kc) {
      return false;
    }
    trace_type++;
    klass_type++;
  }

  // Both strings should end at the same point
  return (*trace_type == '\0' && *klass_type == '\0');
}

bool EpsilonOracle::matches_expected_entry(size_t size_in_bytes, const char* type_name) const {
  // Check if the current allocation matches the expected next trace entry
  // Uses the current _app_alloc_counter to peek at the next expected entry
  uint64_t next_trace_seq = _app_alloc_counter + 1;  // 1-based
  size_t idx = (size_t)(next_trace_seq - 1);         // 0-based array index

  if (idx >= _entry_count) {
    // Beyond trace entries - not a tracked allocation
    return false;
  }

  const OracleEntry& entry = _entries[idx];

  // Match by BOTH size AND type
  bool size_match = (entry.size == size_in_bytes);
  bool type_match = normalize_and_compare(entry.type, type_name);

  if (size_match && type_match) {
    log_info(gc)("Oracle: MATCH for trace_seq=" UINT64_FORMAT " type=[%s] size=%zu (klass=[%s])",
                  next_trace_seq, entry.type, size_in_bytes, type_name ? type_name : "null");
    return true;
  }

  // Log why it didn't match (at debug level to reduce noise, but helpful for debugging)
  if (type_name != nullptr) {
    log_debug(gc)("Oracle: No match. Expected trace_seq=" UINT64_FORMAT " [%s size=%zu], actual [%s size=%zu] size_match=%d type_match=%d",
                  next_trace_seq, entry.type, entry.size, type_name, size_in_bytes, size_match, type_match);
  } else {
    log_debug(gc)("Oracle: No match (no type info). Expected trace_seq=" UINT64_FORMAT " [%s size=%zu], actual size=%zu",
                  next_trace_seq, entry.type, entry.size, size_in_bytes);
  }
  return false;
}

uint64_t EpsilonOracle::next_app_alloc_seq() {
  // Atomically increment and return the 1-based trace sequence number
  return Atomic::add(&_app_alloc_counter, (uint64_t)1);
}

void EpsilonOracle::finalize() {
  // Free any remaining tracked objects that weren't freed during execution
  if (_death_map != nullptr) {
    size_t remaining_count = 0;
    size_t remaining_bytes = 0;

    log_info(gc)("Oracle finalize: Processing remaining tracked objects...");

    for (size_t i = 0; i < DEATH_MAP_SIZE; i++) {
      DeathBucket* bucket = _death_map[i];
      while (bucket != nullptr) {
        DeathBucket* next = bucket->next;

        // This object was never freed during execution - free it now
        HeapWord* ptr = (HeapWord*)bucket->ptr;
        size_t size_in_bytes = bucket->size;
        size_t size_in_words = size_in_bytes / HeapWordSize;

        log_info(gc)("Oracle finalize: FREE remaining ptr=" PTR_FORMAT " size=%zu bytes (was scheduled for seq=" UINT64_FORMAT ")",
                     p2i(ptr), size_in_bytes, bucket->seq);

        // Add to free list (memory is within heap region)
        add_to_free_list(ptr, size_in_words);

        // Update statistics
        Atomic::add(&_free_counter, (uint64_t)1);
        Atomic::add(&_freed_bytes, size_in_bytes);

        remaining_count++;
        remaining_bytes += size_in_bytes;

        // Remove from death map (will be freed in cleanup)
        bucket = next;
      }
      // Clear the bucket chain - cleanup() will free the memory
      _death_map[i] = nullptr;
    }

    if (remaining_count > 0) {
      log_info(gc)("Oracle finalize: Freed %zu remaining objects (%zu bytes)",
                   remaining_count, remaining_bytes);
    } else {
      log_info(gc)("Oracle finalize: No remaining objects to free");
    }
  }

  // Print final statistics
  log_info(gc)("Oracle Final Statistics (after finalize):");
  log_info(gc)("  Trace entries:      %zu", _entry_count);
  log_info(gc)("  Tracked app allocs: " UINT64_FORMAT, _app_alloc_counter);
  log_info(gc)("  Total frees:        " UINT64_FORMAT, _free_counter);
  log_info(gc)("  Bytes allocated:    %zu", _allocated_bytes);
  log_info(gc)("  Bytes freed:        %zu", _freed_bytes);
  log_info(gc)("  Bytes live:         %zu", (_allocated_bytes > _freed_bytes) ? (_allocated_bytes - _freed_bytes) : 0);

  // Verify all tracked allocations were freed
  if (_app_alloc_counter == _free_counter) {
    log_info(gc)("Oracle: SUCCESS - All " UINT64_FORMAT " tracked allocations were freed", _app_alloc_counter);
  } else {
    log_warning(gc)("Oracle: WARNING - Tracked allocs (" UINT64_FORMAT ") != Frees (" UINT64_FORMAT ")",
                    _app_alloc_counter, _free_counter);
  }
}

void EpsilonOracle::cleanup() {
  // Clean up death map buckets (finalize() already cleared the entries)
  if (_death_map != nullptr) {
    // Just free any remaining bucket metadata
    for (size_t i = 0; i < DEATH_MAP_SIZE; i++) {
      DeathBucket* bucket = _death_map[i];
      while (bucket != nullptr) {
        DeathBucket* next = bucket->next;
        FREE_C_HEAP_OBJ(bucket);
        bucket = next;
      }
    }
    FREE_C_HEAP_ARRAY(DeathBucket*, _death_map);
    _death_map = nullptr;
  }

  // Clean up free list metadata
  if (_free_list != nullptr) {
    OracleFreeBlock* block = _free_list;
    while (block != nullptr) {
      OracleFreeBlock* next = block->next;
      FREE_C_HEAP_OBJ(block);
      block = next;
    }
    _free_list = nullptr;
    _free_list_count = 0;
    _free_list_bytes = 0;
  }

  // Free entries array
  if (_entries != nullptr) {
    FREE_C_HEAP_ARRAY(OracleEntry, _entries);
    _entries = nullptr;
    _entry_count = 0;
    _entry_capacity = 0;
  }
}

// Global C function for JVMTI agent to call via dlsym
// This signals that application main() has been entered
// Use visibility attribute to ensure symbol is exported
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_signal_app_start() {
  EpsilonHeap* heap = EpsilonHeap::heap();
  if (heap != nullptr && heap->oracle() != nullptr) {
    heap->oracle()->signal_app_start();
  }
}
