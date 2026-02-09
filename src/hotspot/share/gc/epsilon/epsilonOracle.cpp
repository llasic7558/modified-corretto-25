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
#include "gc/epsilon/epsilonThreadLocalData.hpp"
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/permitForbiddenFunctions.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

EpsilonOracle::EpsilonOracle() :
  _entries(nullptr),
  _entry_count(0),
  _entry_capacity(0),
  _death_map(nullptr),
  _runtime_thread_map(nullptr),
  _next_logical_thread_id(0),
  _num_oracle_threads(0),
  _total_alloc_counter(0),
  _free_counter(0),
  _allocated_bytes(0),
  _freed_bytes(0),
  _free_list(nullptr),
  _free_list_count(0),
  _free_list_bytes(0),
  _app_started(false),
  _pre_app_alloc_count(0),
  _tracked_alloc_counter(0),
  _malloc_ptr_map(nullptr),
  _malloc_tracked_count(0),
  _malloc_freed_count(0),
  _oracle_lock(nullptr) {

  // Initialize matching stats
  memset(&_matching_stats, 0, sizeof(_matching_stats));

  // Create mutex for thread-safe access
  _oracle_lock = new Mutex(Mutex::nosafepoint, "EpsilonOracle_lock");

  // Allocate death map buckets
  _death_map = NEW_C_HEAP_ARRAY(DeathBucket*, DEATH_MAP_SIZE, mtGC);
  memset(_death_map, 0, sizeof(DeathBucket*) * DEATH_MAP_SIZE);

  // Allocate runtime thread -> logical thread mapping
  _runtime_thread_map = NEW_C_HEAP_ARRAY(RuntimeThreadMapping*, RUNTIME_THREAD_MAP_SIZE, mtGC);
  memset(_runtime_thread_map, 0, sizeof(RuntimeThreadMapping*) * RUNTIME_THREAD_MAP_SIZE);

  // Initialize per-thread state array (for logical threads)
  memset(_thread_states, 0, sizeof(_thread_states));

  // Initialize thread entry index (for logical threads)
  memset(_thread_entry_index, 0, sizeof(_thread_entry_index));

  // Allocate malloc pointer tracking map if in malloc mode
  if (EpsilonOracleMallocMode) {
    _malloc_ptr_map = NEW_C_HEAP_ARRAY(MallocPtrBucket*, MALLOC_PTR_MAP_SIZE, mtGC);
    memset(_malloc_ptr_map, 0, sizeof(MallocPtrBucket*) * MALLOC_PTR_MAP_SIZE);
  }
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

  // Read and check header line
  char line[1024];
  if (fgets(line, sizeof(line), f) == nullptr) {
    log_error(gc)("Oracle trace file is empty: %s", path);
    fclose(f);
    return false;
  }

  // Check for new per-thread format vs legacy format
  bool is_per_thread_format = (strstr(line, "alloc_thread") != nullptr);

  if (!is_per_thread_format) {
    log_error(gc)("Oracle trace file is in legacy format. Please regenerate with new oracle_generator.py");
    log_error(gc)("Expected header: alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id");
    log_error(gc)("Got header: %s", line);
    fclose(f);
    return false;
  }

  // Parse CSV entries
  // Format: alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id
  // Thread IDs are now LOGICAL (small integers 0, 1, 2, ...)
  int alloc_thread, free_thread;
  uint64_t alloc_seq, free_seq;
  size_t size;
  char type[512];
  char obj_id[64];
  int32_t max_thread_id = -1;

  while (fscanf(f, "%d,%" SCNu64 ",%d,%" SCNu64 ",%zu,%511[^,],%63s\n",
                &alloc_thread, &alloc_seq, &free_thread, &free_seq, &size, type, obj_id) >= 6) {

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
    entry.alloc_thread = (int32_t)alloc_thread;
    entry.alloc_seq = alloc_seq;
    entry.free_thread = (int32_t)free_thread;
    entry.free_seq = free_seq;
    entry.size = size;
    strncpy(entry.type, type, sizeof(entry.type) - 1);
    entry.type[sizeof(entry.type) - 1] = '\0';
    _entry_count++;

    // Track max logical thread ID
    if (alloc_thread > max_thread_id) max_thread_id = alloc_thread;
    if (free_thread > max_thread_id) max_thread_id = free_thread;
  }

  fclose(f);

  _num_oracle_threads = max_thread_id + 1;
  log_info(gc)("Oracle: Loaded %zu entries from %s (per-thread format, %d logical threads)",
               _entry_count, path, _num_oracle_threads);

  if (_entry_count == 0) {
    log_warning(gc)("Oracle trace file contained no valid entries");
  } else {
    // Build thread entry index for efficient lookup
    build_thread_entry_index();
  }

  return true;
}

void EpsilonOracle::build_thread_entry_index() {
  log_info(gc)("Oracle: Building thread entry index for %zu entries (%d logical threads)...",
               _entry_count, _num_oracle_threads);

  // The entries are already sorted by (alloc_thread, alloc_seq) from oracle_generator.py
  // Build an index mapping logical_thread_id -> first entry index
  // Since logical IDs are small (0, 1, 2, ...), we use a direct array

  int32_t current_thread = -1;
  size_t current_start = 0;
  size_t current_count = 0;

  for (size_t i = 0; i < _entry_count; i++) {
    int32_t logical_thread = _entries[i].alloc_thread;

    if (logical_thread != current_thread) {
      // Save previous thread's index if any
      if (current_thread >= 0 && current_count > 0) {
        if ((size_t)current_thread < MAX_LOGICAL_THREADS) {
          _thread_entry_index[current_thread].first_entry_idx = current_start;
          _thread_entry_index[current_thread].entry_count = current_count;

          log_debug(gc)("Oracle: Logical thread %d: %zu entries starting at index %zu",
                        current_thread, current_count, current_start);
        }
      }

      // Start new thread
      current_thread = logical_thread;
      current_start = i;
      current_count = 1;
    } else {
      current_count++;
    }
  }

  // Don't forget the last thread
  if (current_thread >= 0 && current_count > 0) {
    if ((size_t)current_thread < MAX_LOGICAL_THREADS) {
      _thread_entry_index[current_thread].first_entry_idx = current_start;
      _thread_entry_index[current_thread].entry_count = current_count;

      log_debug(gc)("Oracle: Logical thread %d: %zu entries starting at index %zu",
                    current_thread, current_count, current_start);
    }
  }

  log_info(gc)("Oracle: Thread entry index built for %d logical threads", _num_oracle_threads);
}

// Map a runtime OS thread ID to a logical thread ID
// Returns the logical ID, assigning a new one if this is the first time seeing this thread
int32_t EpsilonOracle::map_runtime_to_logical(int64_t runtime_thread_id) {
  if (!_app_started) {
    return -1;  // Don't map threads before app starts
  }

  size_t bucket_idx = hash_runtime_thread(runtime_thread_id);

  // First check without lock (common case: already mapped)
  RuntimeThreadMapping* mapping = _runtime_thread_map[bucket_idx];
  while (mapping != nullptr) {
    if (mapping->runtime_thread_id == runtime_thread_id) {
      return mapping->logical_thread_id;
    }
    mapping = mapping->next;
  }

  // Need to create mapping - take lock
  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  // Double-check after acquiring lock
  mapping = _runtime_thread_map[bucket_idx];
  while (mapping != nullptr) {
    if (mapping->runtime_thread_id == runtime_thread_id) {
      return mapping->logical_thread_id;
    }
    mapping = mapping->next;
  }

  // Assign next logical thread ID
  int32_t logical_id = Atomic::add(&_next_logical_thread_id, (int32_t)1) - 1;

  if (logical_id >= _num_oracle_threads) {
    // More runtime threads than oracle threads - this allocation won't be tracked
    log_debug(gc)("Oracle: Runtime thread %" PRId64 " mapped to logical %d (beyond oracle's %d threads)",
                  runtime_thread_id, logical_id, _num_oracle_threads);
  } else {
    log_info(gc)("Oracle: Runtime thread %" PRId64 " mapped to logical thread %d",
                 runtime_thread_id, logical_id);
  }

  // Create and insert mapping
  mapping = NEW_C_HEAP_OBJ(RuntimeThreadMapping, mtGC);
  mapping->runtime_thread_id = runtime_thread_id;
  mapping->logical_thread_id = logical_id;
  mapping->next = _runtime_thread_map[bucket_idx];
  _runtime_thread_map[bucket_idx] = mapping;

  return logical_id;
}

int32_t EpsilonOracle::get_logical_thread(int64_t runtime_thread_id) const {
  size_t bucket_idx = hash_runtime_thread(runtime_thread_id);
  RuntimeThreadMapping* mapping = _runtime_thread_map[bucket_idx];
  while (mapping != nullptr) {
    if (mapping->runtime_thread_id == runtime_thread_id) {
      return mapping->logical_thread_id;
    }
    mapping = mapping->next;
  }
  return -1;  // Not mapped
}

ThreadAllocState* EpsilonOracle::get_or_create_thread_state(int32_t logical_thread) {
  if (logical_thread < 0 || (size_t)logical_thread >= MAX_LOGICAL_THREADS) {
    return nullptr;
  }

  // First check without lock (common case: already exists)
  ThreadAllocState* state = _thread_states[logical_thread];
  if (state != nullptr) {
    return state;
  }

  // Need to create - take lock
  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  // Double-check after acquiring lock
  state = _thread_states[logical_thread];
  if (state != nullptr) {
    return state;
  }

  // Create new state
  state = NEW_C_HEAP_OBJ(ThreadAllocState, mtGC);
  state->logical_thread = logical_thread;
  state->alloc_count = 0;
  state->next_entry_idx = _thread_entry_index[logical_thread].first_entry_idx;
  state->next = nullptr;

  // Initialize orphan pool
  state->orphan_pool = nullptr;
  state->orphan_count = 0;

  // Initialize lifetime tracking
  state->avg_lifetime = 100;  // Cold start default
  state->lifetime_sum = 0;
  state->lifetime_count = 0;

  _thread_states[logical_thread] = state;

  log_debug(gc)("Oracle: Created state for logical thread %d, first_entry_idx=%zu",
                logical_thread, state->next_entry_idx);

  return state;
}

size_t EpsilonOracle::find_entry_for_thread(int32_t logical_thread, uint64_t per_thread_seq) {
  if (logical_thread < 0 || (size_t)logical_thread >= MAX_LOGICAL_THREADS) {
    return SIZE_MAX;
  }

  // Direct array lookup for logical thread
  ThreadEntryIndex& idx = _thread_entry_index[logical_thread];

  if (idx.entry_count == 0) {
    return SIZE_MAX;  // No entries for this thread
  }

  // Entry index = first_entry_idx + (per_thread_seq - 1)
  // (per_thread_seq is 1-based)
  size_t entry_idx = idx.first_entry_idx + (per_thread_seq - 1);

  // Bounds check
  if (entry_idx < idx.first_entry_idx + idx.entry_count) {
    // Verify the entry matches expected thread and sequence
    if (_entries[entry_idx].alloc_thread == logical_thread &&
        _entries[entry_idx].alloc_seq == per_thread_seq) {
      return entry_idx;
    }
  }

  return SIZE_MAX;  // Not found
}

uint64_t EpsilonOracle::get_thread_alloc_count(int32_t logical_thread) {
  ThreadAllocState* state = get_or_create_thread_state(logical_thread);
  if (state == nullptr) return 0;
  return state->alloc_count;
}

uint64_t EpsilonOracle::next_thread_alloc_seq(int64_t runtime_thread_id) {
  // Map runtime thread to logical thread
  int32_t logical_thread = map_runtime_to_logical(runtime_thread_id);
  if (logical_thread < 0) {
    return 0;  // App not started yet
  }

  ThreadAllocState* state = get_or_create_thread_state(logical_thread);
  if (state == nullptr) {
    return 0;  // Invalid logical thread
  }

  // Atomically increment and return
  uint64_t new_count = Atomic::add(&state->alloc_count, (uint64_t)1);
  Atomic::add(&_total_alloc_counter, (uint64_t)1);

  return new_count;
}

// --- Resilient matching helpers ---

bool EpsilonOracle::cursor_has_entries(ThreadAllocState* state, int32_t logical_thread) const {
  if (logical_thread < 0 || (size_t)logical_thread >= MAX_LOGICAL_THREADS) {
    return false;
  }
  const ThreadEntryIndex& idx = _thread_entry_index[logical_thread];
  size_t end_idx = idx.first_entry_idx + idx.entry_count;
  return state->next_entry_idx < end_idx;
}

OracleEntry& EpsilonOracle::cursor_entry(ThreadAllocState* state) const {
  return _entries[state->next_entry_idx];
}

void EpsilonOracle::advance_cursor(ThreadAllocState* state) {
  state->next_entry_idx++;
}

void EpsilonOracle::schedule_death_original(OracleEntry& entry, void* ptr, size_t size) {
  // Use the original oracle entry's (free_thread, free_seq) directly
  // Apply global delta if set (delays ALL frees to prevent use-after-free)
  int32_t free_thread = entry.free_thread;
  uint64_t free_seq = entry.free_seq + EpsilonOracleGlobalDelta;

  if (free_seq > 0) {
    MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

    size_t bucket_idx = hash_thread_seq(free_thread, free_seq);
    DeathBucket* bucket = NEW_C_HEAP_OBJ(DeathBucket, mtGC);
    bucket->logical_thread = free_thread;
    bucket->seq = free_seq;
    bucket->ptr = ptr;
    bucket->size = size;
    bucket->next = _death_map[bucket_idx];
    _death_map[bucket_idx] = bucket;
  }
}

void EpsilonOracle::schedule_death_estimated(int32_t alloc_thread, uint64_t estimated_seq, void* ptr, size_t size) {
  // Schedule death on the allocating thread at estimated sequence
  // Global delta already included in compute_estimated_death via EpsilonOracleDeathDelta,
  // but also add global delta for consistency
  uint64_t final_seq = estimated_seq + EpsilonOracleGlobalDelta;
  if (final_seq > 0) {
    MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

    size_t bucket_idx = hash_thread_seq(alloc_thread, final_seq);
    DeathBucket* bucket = NEW_C_HEAP_OBJ(DeathBucket, mtGC);
    bucket->logical_thread = alloc_thread;
    bucket->seq = final_seq;
    bucket->ptr = ptr;
    bucket->size = size;
    bucket->next = _death_map[bucket_idx];
    _death_map[bucket_idx] = bucket;
  }
}

uint64_t EpsilonOracle::compute_estimated_death(uint64_t lifetime, uint64_t current_seq,
                                                 ThreadAllocState* state) {
  uint64_t delta = EpsilonOracleDeathDelta;
  if (lifetime > 0) {
    return current_seq + lifetime + delta;
  }
  // Use average lifetime as fallback
  return current_seq + state->avg_lifetime + delta;
}

void EpsilonOracle::push_to_orphan_pool(ThreadAllocState* state, OracleEntry& entry) {
  // Cap orphan pool size
  if (state->orphan_count >= MAX_ORPHAN_POOL_SIZE) {
    // Discard oldest (tail of list) - just drop head for simplicity
    OrphanEntry* old = state->orphan_pool;
    if (old != nullptr) {
      state->orphan_pool = old->next;
      FREE_C_HEAP_OBJ(old);
      state->orphan_count--;
    }
  }

  OrphanEntry* orphan = NEW_C_HEAP_OBJ(OrphanEntry, mtGC);
  orphan->size = entry.size;
  orphan->alloc_seq = entry.alloc_seq;
  orphan->free_seq = entry.free_seq;
  orphan->free_thread = entry.free_thread;
  orphan->alloc_thread = entry.alloc_thread;

  // Compute lifetime: if same thread, use free_seq - alloc_seq; otherwise 0
  if (entry.free_thread == entry.alloc_thread && entry.free_seq > entry.alloc_seq) {
    orphan->lifetime = entry.free_seq - entry.alloc_seq;
  } else {
    orphan->lifetime = 0;
  }

  // Insert at head
  orphan->next = state->orphan_pool;
  state->orphan_pool = orphan;
  state->orphan_count++;

  Atomic::add(&_matching_stats.total_skipped, (uint64_t)1);
}

OrphanEntry* EpsilonOracle::find_in_orphan_pool(ThreadAllocState* state, size_t size) {
  OrphanEntry* orphan = state->orphan_pool;
  while (orphan != nullptr) {
    if (orphan->size == size) {
      return orphan;
    }
    orphan = orphan->next;
  }
  return nullptr;
}

void EpsilonOracle::remove_from_orphan_pool(ThreadAllocState* state, OrphanEntry* orphan, OrphanEntry* prev) {
  if (prev == nullptr) {
    state->orphan_pool = orphan->next;
  } else {
    prev->next = orphan->next;
  }
  state->orphan_count--;
  FREE_C_HEAP_OBJ(orphan);
}

void EpsilonOracle::update_lifetime_stats(ThreadAllocState* state, OracleEntry& entry) {
  // Only count same-thread lifetimes for meaningful average
  if (entry.free_thread == entry.alloc_thread && entry.free_seq > entry.alloc_seq) {
    uint64_t lifetime = entry.free_seq - entry.alloc_seq;
    state->lifetime_sum += lifetime;
    state->lifetime_count++;
    state->avg_lifetime = state->lifetime_sum / state->lifetime_count;
  }
}

// --- Core allocation registration with resilient matching ---

bool EpsilonOracle::register_allocation(int64_t runtime_thread_id, void* ptr, size_t size) {
  // Map runtime thread to logical thread
  int32_t logical_thread = get_logical_thread(runtime_thread_id);
  if (logical_thread < 0) {
    log_trace(gc)("Oracle: register_allocation called for unmapped thread %" PRId64, runtime_thread_id);
    return false;
  }

  // Get thread state
  ThreadAllocState* state = get_or_create_thread_state(logical_thread);
  if (state == nullptr) {
    return false;
  }
  uint64_t per_thread_seq = state->alloc_count;  // Already incremented by caller

  // ============================================================
  // Step 1: Try cursor entry (perfect sequential match)
  // ============================================================
  if (cursor_has_entries(state, logical_thread)) {
    OracleEntry& entry = cursor_entry(state);

    if (entry.size == size) {
      // Perfect match - use original death time
      update_lifetime_stats(state, entry);
      schedule_death_original(entry, ptr, size);
      advance_cursor(state);

      Atomic::add(&_matching_stats.perfect_matches, (uint64_t)1);
      Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
      Atomic::add(&_allocated_bytes, size);

      log_info(gc)("Oracle: PERFECT logical_thread=%d seq=%" PRIu64 " type=[%s] size=%zu ptr=" PTR_FORMAT
                   " -> free at logical_thread=%d seq=%" PRIu64,
                   logical_thread, per_thread_seq, entry.type, size, p2i(ptr),
                   entry.free_thread, entry.free_seq);

      return true;
    }

    // Size mismatch at cursor - proceed to lookahead
    log_debug(gc)("Oracle: Size mismatch at cursor for logical_thread=%d seq=%" PRIu64
                  ": cursor_size=%zu alloc_size=%zu cursor_type=[%s]",
                  logical_thread, per_thread_seq, entry.size, size, entry.type);
  }

  // ============================================================
  // Step 2: Lookahead scan (cursor+1 .. cursor+K)
  // ============================================================
  size_t lookahead = EpsilonOracleLookahead;

  if (lookahead > 0 && cursor_has_entries(state, logical_thread)) {
    ThreadEntryIndex& idx = _thread_entry_index[logical_thread];
    size_t end_idx = idx.first_entry_idx + idx.entry_count;
    size_t scan_start = state->next_entry_idx + 1;
    size_t scan_end = state->next_entry_idx + 1 + lookahead;
    if (scan_end > end_idx) scan_end = end_idx;

    for (size_t i = scan_start; i < scan_end; i++) {
      if (_entries[i].size == size) {
        // Found a match ahead - push all skipped entries (cursor through i-1) to orphan pool
        for (size_t j = state->next_entry_idx; j < i; j++) {
          update_lifetime_stats(state, _entries[j]);
          push_to_orphan_pool(state, _entries[j]);
        }

        // Use matched entry's original death time
        OracleEntry& matched = _entries[i];
        update_lifetime_stats(state, matched);
        schedule_death_original(matched, ptr, size);

        // Advance cursor past the matched entry
        state->next_entry_idx = i + 1;

        Atomic::add(&_matching_stats.lookahead_matches, (uint64_t)1);
        Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
        Atomic::add(&_allocated_bytes, size);

        log_info(gc)("Oracle: LOOKAHEAD logical_thread=%d seq=%" PRIu64 " type=[%s] size=%zu ptr=" PTR_FORMAT
                     " -> free at logical_thread=%d seq=%" PRIu64 " (skipped %zu entries)",
                     logical_thread, per_thread_seq, matched.type, size, p2i(ptr),
                     matched.free_thread, matched.free_seq, (i - scan_start + 1));

        return true;
      }
    }
  }

  // ============================================================
  // Step 3: Check orphan pool (find entry with matching size)
  // ============================================================
  {
    OrphanEntry* prev = nullptr;
    OrphanEntry* orphan = state->orphan_pool;
    while (orphan != nullptr) {
      if (orphan->size == size) {
        // Found orphan match - use estimated death time
        uint64_t est_death = compute_estimated_death(orphan->lifetime, per_thread_seq, state);
        schedule_death_estimated(logical_thread, est_death, ptr, size);

        // Remove from pool
        remove_from_orphan_pool(state, orphan, prev);

        // Push current cursor entry to orphan pool (if cursor available) and advance
        if (cursor_has_entries(state, logical_thread)) {
          OracleEntry& cur = cursor_entry(state);
          update_lifetime_stats(state, cur);
          push_to_orphan_pool(state, cur);
          advance_cursor(state);
        }

        Atomic::add(&_matching_stats.orphan_matches, (uint64_t)1);
        Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
        Atomic::add(&_allocated_bytes, size);

        log_info(gc)("Oracle: ORPHAN logical_thread=%d seq=%" PRIu64 " size=%zu ptr=" PTR_FORMAT
                     " -> estimated free at logical_thread=%d seq=%" PRIu64,
                     logical_thread, per_thread_seq, size, p2i(ptr),
                     logical_thread, est_death);

        return true;
      }
      prev = orphan;
      orphan = orphan->next;
    }
  }

  // ============================================================
  // Step 4: No match found, cursor entry available - use estimated death
  // ============================================================
  if (cursor_has_entries(state, logical_thread)) {
    OracleEntry& entry = cursor_entry(state);

    // Compute lifetime from cursor entry
    uint64_t lifetime = 0;
    if (entry.free_thread == entry.alloc_thread && entry.free_seq > entry.alloc_seq) {
      lifetime = entry.free_seq - entry.alloc_seq;
    }

    uint64_t est_death = compute_estimated_death(lifetime, per_thread_seq, state);
    schedule_death_estimated(logical_thread, est_death, ptr, size);

    // Update stats and advance cursor
    update_lifetime_stats(state, entry);
    advance_cursor(state);

    Atomic::add(&_matching_stats.estimated_deaths, (uint64_t)1);
    Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
    Atomic::add(&_allocated_bytes, size);

    log_info(gc)("Oracle: ESTIMATED logical_thread=%d seq=%" PRIu64 " size=%zu (cursor_size=%zu) ptr=" PTR_FORMAT
                 " -> estimated free at logical_thread=%d seq=%" PRIu64,
                 logical_thread, per_thread_seq, size, entry.size, p2i(ptr),
                 logical_thread, est_death);

    return true;
  }

  // ============================================================
  // Step 5: Oracle exhausted for this thread - use avg_lifetime
  // ============================================================
  {
    uint64_t est_death = compute_estimated_death(0, per_thread_seq, state);
    schedule_death_estimated(logical_thread, est_death, ptr, size);

    Atomic::add(&_matching_stats.oracle_exhausted, (uint64_t)1);
    Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
    Atomic::add(&_allocated_bytes, size);

    log_info(gc)("Oracle: EXHAUSTED logical_thread=%d seq=%" PRIu64 " size=%zu ptr=" PTR_FORMAT
                 " -> estimated free at logical_thread=%d seq=%" PRIu64 " (avg_lifetime=%" PRIu64 ")",
                 logical_thread, per_thread_seq, size, p2i(ptr),
                 logical_thread, est_death, state->avg_lifetime);

    return true;
  }
}

void EpsilonOracle::process_deaths(int32_t logical_thread, uint64_t per_thread_seq) {
  // Process all deaths scheduled for this logical thread at this sequence
  size_t bucket_idx = hash_thread_seq(logical_thread, per_thread_seq);

  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  DeathBucket** prev_ptr = &_death_map[bucket_idx];
  DeathBucket* bucket = *prev_ptr;

  while (bucket != nullptr) {
    DeathBucket* next = bucket->next;

    if (bucket->logical_thread == logical_thread && bucket->seq == per_thread_seq) {
      // This object should be freed now
      HeapWord* ptr = (HeapWord*)bucket->ptr;
      size_t size_in_bytes = bucket->size;
      size_t size_in_words = size_in_bytes / HeapWordSize;

      log_info(gc)("Oracle: FREE logical_thread=%d seq=%" PRIu64 " ptr=" PTR_FORMAT
                   " size=%zu bytes (free_counter=%" PRIu64 ")",
                   logical_thread, per_thread_seq, p2i(ptr), size_in_bytes, _free_counter + 1);

      // Add to free list
      add_to_free_list(ptr, size_in_words);

      // Update statistics
      Atomic::add(&_free_counter, (uint64_t)1);
      Atomic::add(&_freed_bytes, size_in_bytes);

      // Remove bucket from chain
      *prev_ptr = next;
      FREE_C_HEAP_OBJ(bucket);
    } else {
      prev_ptr = &bucket->next;
    }

    bucket = next;
  }
}

void EpsilonOracle::process_deaths_malloc_mode(int32_t logical_thread, uint64_t per_thread_seq) {
  // Process all deaths scheduled for this logical thread at this sequence with actual free()
  size_t bucket_idx = hash_thread_seq(logical_thread, per_thread_seq);

  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  DeathBucket** prev_ptr = &_death_map[bucket_idx];
  DeathBucket* bucket = *prev_ptr;

  while (bucket != nullptr) {
    DeathBucket* next = bucket->next;

    if (bucket->logical_thread == logical_thread && bucket->seq == per_thread_seq) {
      void* ptr = bucket->ptr;
      size_t size_in_bytes = bucket->size;

      // Untrack from malloc pointer map
      size_t tracked_size = 0;
      if (untrack_malloc_ptr(ptr, &tracked_size)) {
        log_info(gc)("Oracle: MALLOC FREE logical_thread=%d seq=%" PRIu64 " ptr=" PTR_FORMAT
                     " size=%zu bytes (free_counter=%" PRIu64 ")",
                     logical_thread, per_thread_seq, p2i(ptr), size_in_bytes, _free_counter + 1);

        // Actually free the memory
        permit_forbidden_function::free(ptr);

        // Update statistics
        Atomic::add(&_free_counter, (uint64_t)1);
        Atomic::add(&_freed_bytes, size_in_bytes);
      } else {
        log_warning(gc)("Oracle: MALLOC FREE failed - ptr=" PTR_FORMAT " not in tracking map",
                        p2i(ptr));
      }

      // Remove bucket from chain
      *prev_ptr = next;
      FREE_C_HEAP_OBJ(bucket);
    } else {
      prev_ptr = &bucket->next;
    }

    bucket = next;
  }
}

void EpsilonOracle::track_malloc_ptr(void* ptr, size_t size) {
  if (_malloc_ptr_map == nullptr) return;

  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  size_t bucket_idx = hash_ptr(ptr);
  MallocPtrBucket* bucket = NEW_C_HEAP_OBJ(MallocPtrBucket, mtGC);
  bucket->ptr = ptr;
  bucket->size = size;
  bucket->next = _malloc_ptr_map[bucket_idx];
  _malloc_ptr_map[bucket_idx] = bucket;

  Atomic::add(&_malloc_tracked_count, (size_t)1);

  log_debug(gc)("Oracle: MALLOC TRACK ptr=" PTR_FORMAT " size=%zu (total=%zu)",
                p2i(ptr), size, _malloc_tracked_count);
}

bool EpsilonOracle::untrack_malloc_ptr(void* ptr, size_t* out_size) {
  if (_malloc_ptr_map == nullptr) return false;

  // Note: caller should hold _oracle_lock
  size_t bucket_idx = hash_ptr(ptr);

  MallocPtrBucket** prev_ptr = &_malloc_ptr_map[bucket_idx];
  MallocPtrBucket* bucket = *prev_ptr;

  while (bucket != nullptr) {
    if (bucket->ptr == ptr) {
      if (out_size != nullptr) {
        *out_size = bucket->size;
      }
      *prev_ptr = bucket->next;
      FREE_C_HEAP_OBJ(bucket);
      Atomic::add(&_malloc_freed_count, (size_t)1);
      return true;
    }
    prev_ptr = &bucket->next;
    bucket = bucket->next;
  }

  return false;
}

bool EpsilonOracle::is_malloc_tracked(void* ptr) const {
  if (_malloc_ptr_map == nullptr) return false;

  size_t bucket_idx = hash_ptr(ptr);
  MallocPtrBucket* bucket = _malloc_ptr_map[bucket_idx];

  while (bucket != nullptr) {
    if (bucket->ptr == ptr) {
      return true;
    }
    bucket = bucket->next;
  }

  return false;
}

HeapWord* EpsilonOracle::allocate_from_free_list(size_t size) {
  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  // First-fit allocation from free list
  OracleFreeBlock** prev_ptr = &_free_list;
  OracleFreeBlock* block = _free_list;

  while (block != nullptr) {
    if (block->size >= size) {
      HeapWord* result = block->addr;

      if (block->size > size) {
        // Split the block
        block->addr = block->addr + size;
        block->size = block->size - size;
        _free_list_bytes -= size * HeapWordSize;
      } else {
        // Exact fit - remove block
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

  return nullptr;  // No suitable block
}

void EpsilonOracle::add_to_free_list(HeapWord* addr, size_t size) {
  // Note: caller should hold _oracle_lock
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

  _pre_app_alloc_count = _total_alloc_counter;
  _app_started = true;

  log_info(gc)("Oracle: Application started (signaled by JVMTI agent)");
  log_info(gc)("Oracle: Pre-app allocations: %" PRIu64 ", trace entries: %zu",
               _pre_app_alloc_count, _entry_count);
}

void EpsilonOracle::count_pre_app_alloc() {
  // Count allocations before app starts (for auto-start detection)
  // This increments the total counter without affecting per-thread tracking
  if (!_app_started) {
    Atomic::add(&_total_alloc_counter, (uint64_t)1);
  }
}

void EpsilonOracle::print_stats() const {
  log_info(gc)("Oracle Statistics:");
  log_info(gc)("  Trace entries:      %zu", _entry_count);
  log_info(gc)("  Total allocations:  %" PRIu64, _total_alloc_counter);
  log_info(gc)("  Pre-app allocations:%" PRIu64, _pre_app_alloc_count);
  log_info(gc)("  Tracked allocations:%" PRIu64, _tracked_alloc_counter);
  log_info(gc)("  Frees:              %" PRIu64, _free_counter);
  log_info(gc)("  Bytes allocated:    %zu", _allocated_bytes);
  log_info(gc)("  Bytes freed:        %zu", _freed_bytes);
  log_info(gc)("  Bytes live:         %zu", _allocated_bytes - _freed_bytes);
  log_info(gc)("  Oracle threads:     %d", _num_oracle_threads);
  log_info(gc)("  Runtime threads:    %d", _next_logical_thread_id);

  // Resilient matching statistics
  log_info(gc)("  Matching statistics:");
  log_info(gc)("    Perfect matches:  %" PRIu64, _matching_stats.perfect_matches);
  log_info(gc)("    Lookahead matches:%" PRIu64, _matching_stats.lookahead_matches);
  log_info(gc)("    Orphan matches:   %" PRIu64, _matching_stats.orphan_matches);
  log_info(gc)("    Estimated deaths: %" PRIu64, _matching_stats.estimated_deaths);
  log_info(gc)("    Oracle exhausted: %" PRIu64, _matching_stats.oracle_exhausted);
  log_info(gc)("    Total skipped:    %" PRIu64, _matching_stats.total_skipped);

  uint64_t total_matched = _matching_stats.perfect_matches + _matching_stats.lookahead_matches +
                           _matching_stats.orphan_matches + _matching_stats.estimated_deaths +
                           _matching_stats.oracle_exhausted;
  if (total_matched > 0) {
    log_info(gc)("    Perfect rate:     %.1f%%", 100.0 * _matching_stats.perfect_matches / total_matched);
  }

  if (EpsilonOracleMallocMode) {
    log_info(gc)("  Malloc mode:        ENABLED");
    log_info(gc)("  Malloc tracked:     %zu", _malloc_tracked_count);
    log_info(gc)("  Malloc freed:       %zu", _malloc_freed_count);
  } else {
    log_info(gc)("  Free list blocks:   %zu", _free_list_count);
    log_info(gc)("  Free list bytes:    %zu", _free_list_bytes);
  }

  // Print per-thread statistics (logical thread IDs)
  log_info(gc)("  Per-thread allocation counts:");
  for (size_t i = 0; i < MAX_LOGICAL_THREADS; i++) {
    ThreadAllocState* state = _thread_states[i];
    if (state != nullptr) {
      log_info(gc)("    Logical thread %d: %" PRIu64 " allocations, orphan_pool=%zu, avg_lifetime=%" PRIu64,
                   state->logical_thread, state->alloc_count, state->orphan_count, state->avg_lifetime);
    }
  }
}

void EpsilonOracle::finalize() {
  log_info(gc)("Oracle finalize: Processing remaining tracked objects...");

  size_t remaining_count = 0;
  size_t remaining_bytes = 0;

  // Free any remaining objects in death map
  for (size_t i = 0; i < DEATH_MAP_SIZE; i++) {
    DeathBucket* bucket = _death_map[i];
    while (bucket != nullptr) {
      DeathBucket* next = bucket->next;

      void* ptr = bucket->ptr;
      size_t size_in_bytes = bucket->size;

      log_info(gc)("Oracle finalize: FREE remaining ptr=" PTR_FORMAT
                   " size=%zu (was scheduled for logical_thread=%d seq=%" PRIu64 ")",
                   p2i(ptr), size_in_bytes, bucket->logical_thread, bucket->seq);

      if (EpsilonOracleMallocMode) {
        untrack_malloc_ptr(ptr, nullptr);
        permit_forbidden_function::free(ptr);
      } else {
        size_t size_in_words = size_in_bytes / HeapWordSize;
        add_to_free_list((HeapWord*)ptr, size_in_words);
      }

      Atomic::add(&_free_counter, (uint64_t)1);
      Atomic::add(&_freed_bytes, size_in_bytes);

      remaining_count++;
      remaining_bytes += size_in_bytes;

      FREE_C_HEAP_OBJ(bucket);
      bucket = next;
    }
    _death_map[i] = nullptr;
  }

  if (remaining_count > 0) {
    log_info(gc)("Oracle finalize: Freed %zu remaining objects (%zu bytes)",
                 remaining_count, remaining_bytes);
  } else {
    log_info(gc)("Oracle finalize: No remaining objects to free");
  }

  // Print final statistics
  log_info(gc)("Oracle Final Statistics (after finalize):");
  log_info(gc)("  Trace entries:      %zu", _entry_count);
  log_info(gc)("  Tracked allocations:%" PRIu64, _tracked_alloc_counter);
  log_info(gc)("  Total frees:        %" PRIu64, _free_counter);
  log_info(gc)("  Bytes allocated:    %zu", _allocated_bytes);
  log_info(gc)("  Bytes freed:        %zu", _freed_bytes);
  log_info(gc)("  Bytes live:         %zu",
               (_allocated_bytes > _freed_bytes) ? (_allocated_bytes - _freed_bytes) : 0);

  // Matching breakdown
  log_info(gc)("  Matching breakdown:");
  log_info(gc)("    Perfect:    %" PRIu64, _matching_stats.perfect_matches);
  log_info(gc)("    Lookahead:  %" PRIu64, _matching_stats.lookahead_matches);
  log_info(gc)("    Orphan:     %" PRIu64, _matching_stats.orphan_matches);
  log_info(gc)("    Estimated:  %" PRIu64, _matching_stats.estimated_deaths);
  log_info(gc)("    Exhausted:  %" PRIu64, _matching_stats.oracle_exhausted);

  // Verify all tracked allocations were freed
  if (_tracked_alloc_counter == _free_counter) {
    log_info(gc)("Oracle: SUCCESS - All %" PRIu64 " tracked allocations were freed",
                 _tracked_alloc_counter);
  } else {
    log_warning(gc)("Oracle: WARNING - Tracked allocs (%" PRIu64 ") != Frees (%" PRIu64 ")",
                    _tracked_alloc_counter, _free_counter);
  }
}

void EpsilonOracle::cleanup() {
  // Clean up death map
  if (_death_map != nullptr) {
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

  // Clean up per-thread state array (indexed by logical thread ID)
  for (size_t i = 0; i < MAX_LOGICAL_THREADS; i++) {
    ThreadAllocState* state = _thread_states[i];
    if (state != nullptr) {
      // Clean up orphan pool for this thread
      OrphanEntry* orphan = state->orphan_pool;
      while (orphan != nullptr) {
        OrphanEntry* next = orphan->next;
        FREE_C_HEAP_OBJ(orphan);
        orphan = next;
      }
      state->orphan_pool = nullptr;

      FREE_C_HEAP_OBJ(state);
      _thread_states[i] = nullptr;
    }
  }

  // Clean up runtime thread -> logical thread mapping
  if (_runtime_thread_map != nullptr) {
    for (size_t i = 0; i < RUNTIME_THREAD_MAP_SIZE; i++) {
      RuntimeThreadMapping* mapping = _runtime_thread_map[i];
      while (mapping != nullptr) {
        RuntimeThreadMapping* next = mapping->next;
        FREE_C_HEAP_OBJ(mapping);
        mapping = next;
      }
    }
    FREE_C_HEAP_ARRAY(RuntimeThreadMapping*, _runtime_thread_map);
    _runtime_thread_map = nullptr;
  }

  // Thread entry index is a direct array (not heap-allocated), just zero it
  memset(_thread_entry_index, 0, sizeof(_thread_entry_index));

  // Clean up free list
  OracleFreeBlock* block = _free_list;
  while (block != nullptr) {
    OracleFreeBlock* next = block->next;
    FREE_C_HEAP_OBJ(block);
    block = next;
  }
  _free_list = nullptr;
  _free_list_count = 0;
  _free_list_bytes = 0;

  // Clean up malloc pointer tracking map
  if (_malloc_ptr_map != nullptr) {
    for (size_t i = 0; i < MALLOC_PTR_MAP_SIZE; i++) {
      MallocPtrBucket* bucket = _malloc_ptr_map[i];
      while (bucket != nullptr) {
        MallocPtrBucket* next = bucket->next;
        FREE_C_HEAP_OBJ(bucket);
        bucket = next;
      }
    }
    FREE_C_HEAP_ARRAY(MallocPtrBucket*, _malloc_ptr_map);
    _malloc_ptr_map = nullptr;
  }

  // Free entries array
  if (_entries != nullptr) {
    FREE_C_HEAP_ARRAY(OracleEntry, _entries);
    _entries = nullptr;
    _entry_count = 0;
    _entry_capacity = 0;
  }

  // Free mutex
  if (_oracle_lock != nullptr) {
    delete _oracle_lock;
    _oracle_lock = nullptr;
  }
}

// Global C functions for JVMTI agent to call via dlsym

// Signal application start (called by Elephant Tracks or similar agent)
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_signal_app_start() {
  EpsilonHeap* heap = EpsilonHeap::heap();
  if (heap != nullptr && heap->oracle() != nullptr) {
    heap->oracle()->signal_app_start();
  }
}

// Enter app code: increment JVM-side thread-local depth counter
// Called by OracleSignal agent via libOracleSignal native library
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_enter_app_code() {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    int depth = EpsilonThreadLocalData::app_code_depth(thread) + 1;
    EpsilonThreadLocalData::set_app_code_depth(thread, depth);
  }
}

// Exit app code: decrement JVM-side thread-local depth counter
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_exit_app_code() {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    int depth = EpsilonThreadLocalData::app_code_depth(thread) - 1;
    if (depth < 0) depth = 0;
    EpsilonThreadLocalData::set_app_code_depth(thread, depth);
  }
}

// Suppress/restore tracking: used during agent overhead (class transformation)
// to prevent javassist allocations from being counted as app allocations
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_suppress_tracking(bool suppress) {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    EpsilonThreadLocalData::set_tracking_suppressed(thread, suppress);
  }
}

// Get current app code depth (for debugging / Java-side queries)
extern "C" __attribute__((visibility("default"))) int epsilon_oracle_get_app_code_depth() {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    return EpsilonThreadLocalData::app_code_depth(thread);
  }
  return 0;
}

// Set the app code depth counter directly (compat API)
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_set_app_code_depth(int depth) {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    EpsilonThreadLocalData::set_app_code_depth(thread, depth);
  }
}

// Set the app allocation pending flag (legacy boolean API, kept for compatibility)
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_set_app_alloc_pending(bool pending) {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    EpsilonThreadLocalData::set_app_allocation_pending(thread, pending);
  }
}

// Get the app allocation pending flag (for debugging)
extern "C" __attribute__((visibility("default"))) bool epsilon_oracle_get_app_alloc_pending() {
  Thread* thread = Thread::current();
  if (thread != nullptr && UseEpsilonGC) {
    return EpsilonThreadLocalData::in_app_code(thread);
  }
  return false;
}
