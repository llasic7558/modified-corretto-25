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

// --- Type name normalization ---

// Normalize JVM internal type names to Java format matching oracle CSV.
// JVM external_name() returns:
//   "byte[]" for [B, "int[]" for [I, etc. (already correct for primitives)
//   "java.lang.String[]" for [Ljava.lang.String; (already correct)
// But the oracle CSV may use slightly different formats, so we normalize both sides.
//
// This function handles:
//   "[B" -> "byte[]", "[I" -> "int[]", etc. (JVM internal descriptor format)
//   "byte[]" -> "byte[]" (pass-through, already normalized)
//   Any other format passes through unchanged
void EpsilonOracle::normalize_type_name(const char* raw, char* out, size_t out_len) {
  if (raw == nullptr || out_len == 0) {
    if (out_len > 0) out[0] = '\0';
    return;
  }

  // Handle JVM internal descriptor format (starts with '[')
  if (raw[0] == '[') {
    // Count array dimensions
    int dims = 0;
    const char* p = raw;
    while (*p == '[') {
      dims++;
      p++;
    }

    // Decode element type
    const char* elem_type = nullptr;
    char class_buf[256];

    switch (*p) {
      case 'B': elem_type = "byte"; break;
      case 'C': elem_type = "char"; break;
      case 'D': elem_type = "double"; break;
      case 'F': elem_type = "float"; break;
      case 'I': elem_type = "int"; break;
      case 'J': elem_type = "long"; break;
      case 'S': elem_type = "short"; break;
      case 'Z': elem_type = "boolean"; break;
      case 'L': {
        // Object type: L<classname>;
        p++;  // skip 'L'
        size_t len = strlen(p);
        if (len > 0 && p[len-1] == ';') len--;  // strip trailing ';'
        if (len >= sizeof(class_buf)) len = sizeof(class_buf) - 1;
        memcpy(class_buf, p, len);
        class_buf[len] = '\0';
        // Replace '/' with '.'
        for (size_t i = 0; i < len; i++) {
          if (class_buf[i] == '/') class_buf[i] = '.';
        }
        elem_type = class_buf;
        break;
      }
      default:
        // Unknown descriptor, pass through
        strncpy(out, raw, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    // Build result: elem_type + "[]" * dims
    size_t elem_len = strlen(elem_type);
    size_t total_len = elem_len + (size_t)dims * 2;
    if (total_len >= out_len) {
      strncpy(out, raw, out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
    memcpy(out, elem_type, elem_len);
    for (int i = 0; i < dims; i++) {
      out[elem_len + i * 2] = '[';
      out[elem_len + i * 2 + 1] = ']';
    }
    out[total_len] = '\0';
    return;
  }

  // Not a descriptor format - pass through (already in Java format)
  // Just replace any '/' with '.'
  size_t len = strlen(raw);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, raw, len);
  out[len] = '\0';
  for (size_t i = 0; i < len; i++) {
    if (out[i] == '/') out[i] = '.';
  }
}

// Hash a type name to a bucket index
size_t EpsilonOracle::hash_type_name(const char* type_name) {
  // djb2 hash
  size_t hash = 5381;
  const char* p = type_name;
  while (*p) {
    hash = ((hash << 5) + hash) + (unsigned char)*p;
    p++;
  }
  return hash & (ThreadAllocState::TYPE_QUEUE_BUCKETS - 1);
}

EpsilonOracle::EpsilonOracle() :
  _entries(nullptr),
  _entry_count(0),
  _entry_capacity(0),
  _death_map(nullptr),
  _runtime_thread_map(nullptr),
  _next_logical_thread_id(EpsilonOracleSkipThread0 ? 1 : 0),  // Skip thread 0 if ET overhead present
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
  _finalized(false),
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

  // Initialize delayed-free buffer
  _delayed_free_buf = nullptr;
  _delayed_free_head = 0;
  _delayed_free_tail = 0;
  _delayed_free_count = 0;
  if (EpsilonOracleFreeDelay > 0 && EpsilonOracleMallocMode) {
    _delayed_free_buf = NEW_C_HEAP_ARRAY(DelayedFreeEntry, DELAYED_FREE_CAPACITY, mtGC);
    memset(_delayed_free_buf, 0, sizeof(DelayedFreeEntry) * DELAYED_FREE_CAPACITY);
    log_info(gc)("Oracle: Delayed-free buffer allocated (capacity=%zu, delay=%" PRIu64 " allocs)",
                 DELAYED_FREE_CAPACITY, EpsilonOracleFreeDelay);
  }

  log_info(gc)("Oracle: Logical thread IDs start at %d (EpsilonOracleSkipThread0=%s)",
               _next_logical_thread_id,
               EpsilonOracleSkipThread0 ? "true" : "false");
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

  // Detect column count from header: count commas to determine 7-col vs 9-col format
  int header_commas = 0;
  for (const char* p = line; *p; p++) {
    if (*p == ',') header_commas++;
  }
  bool is_9col = (header_commas >= 8);
  log_info(gc)("Oracle: Detected %d-column CSV format (commas=%d)",
               is_9col ? 9 : 7, header_commas);

  // Parse CSV entries using line-based parsing to handle both 7 and 9 column formats.
  // 7-col: alloc_thread,alloc_seq,free_thread,free_seq,size,type,obj_id
  // 9-col: alloc_thread,alloc_seq,free_thread,free_seq,size,type,type_hash,relative_lifetime,obj_id
  int32_t max_thread_id = -1;

  while (fgets(line, sizeof(line), f) != nullptr) {
    // Skip empty lines
    size_t line_len = strlen(line);
    if (line_len == 0) continue;
    // Strip trailing newline/carriage return
    while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
      line[--line_len] = '\0';
    }
    if (line_len == 0) continue;

    // Parse fields by splitting on commas
    // We need: alloc_thread (0), alloc_seq (1), free_thread (2), free_seq (3), size (4), type (5)
    // For 9-col, fields 6 and 7 (type_hash, relative_lifetime) are skipped; field 8 is obj_id (unused)
    // For 7-col, field 6 is obj_id (unused)
    char* fields[10];
    int field_count = 0;
    char* pos = line;
    fields[0] = pos;
    field_count = 1;

    while (*pos && field_count < 10) {
      if (*pos == ',') {
        *pos = '\0';
        pos++;
        fields[field_count++] = pos;
      } else {
        pos++;
      }
    }

    // Need at least 6 fields (alloc_thread through type)
    if (field_count < 6) continue;

    int alloc_thread = (int)strtol(fields[0], nullptr, 10);
    uint64_t alloc_seq = (uint64_t)strtoull(fields[1], nullptr, 10);
    int free_thread = (int)strtol(fields[2], nullptr, 10);
    uint64_t free_seq = (uint64_t)strtoull(fields[3], nullptr, 10);
    size_t size = (size_t)strtoull(fields[4], nullptr, 10);
    const char* type = fields[5];

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
    // Build type-keyed FIFO queues for type-based matching
    build_type_queues();
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

void EpsilonOracle::build_type_queues() {
  log_info(gc)("Oracle: Building type-keyed FIFO queues for %zu entries...", _entry_count);

  // For each oracle entry, normalize the type name and add to the
  // per-thread type queue. We pre-create ThreadAllocState for each
  // thread seen in the oracle.
  size_t total_queues = 0;
  size_t total_nodes = 0;

  for (size_t i = 0; i < _entry_count; i++) {
    OracleEntry& entry = _entries[i];
    int32_t logical_thread = entry.alloc_thread;

    if (logical_thread < 0 || (size_t)logical_thread >= MAX_LOGICAL_THREADS) continue;

    // Get or create thread state
    ThreadAllocState* state = get_or_create_thread_state(logical_thread);
    if (state == nullptr) continue;

    // Normalize the oracle entry's type name in-place for consistency
    char normalized[128];
    normalize_type_name(entry.type, normalized, sizeof(normalized));
    strncpy(entry.type, normalized, sizeof(entry.type) - 1);
    entry.type[sizeof(entry.type) - 1] = '\0';

    // Hash the normalized type name
    size_t bucket = hash_type_name(normalized);

    // Find or create type queue for this type in this thread's hash table
    TypeQueue* queue = state->type_queues[bucket];
    while (queue != nullptr) {
      if (strcmp(queue->type_name, normalized) == 0) break;
      queue = queue->next;
    }

    if (queue == nullptr) {
      // Create new type queue
      queue = NEW_C_HEAP_OBJ(TypeQueue, mtGC);
      queue->head = nullptr;
      queue->tail = nullptr;
      queue->count = 0;
      strncpy(queue->type_name, normalized, sizeof(queue->type_name) - 1);
      queue->type_name[sizeof(queue->type_name) - 1] = '\0';
      queue->next = state->type_queues[bucket];
      state->type_queues[bucket] = queue;
      total_queues++;
    }

    // Create node and enqueue at tail (FIFO)
    TypeQueueNode* node = NEW_C_HEAP_OBJ(TypeQueueNode, mtGC);
    node->entry_idx = i;
    node->next = nullptr;

    if (queue->tail != nullptr) {
      queue->tail->next = node;
    } else {
      queue->head = node;
    }
    queue->tail = node;
    queue->count++;
    total_nodes++;
  }

  log_info(gc)("Oracle: Built %zu type queues with %zu total nodes across %d threads",
               total_queues, total_nodes, _num_oracle_threads);
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

  // Initialize type queue hash table
  memset(state->type_queues, 0, sizeof(state->type_queues));

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

void EpsilonOracle::schedule_death_relative(OracleEntry& entry, void* ptr, size_t size,
                                             int32_t replay_logical_thread,
                                             uint64_t replay_alloc_seq) {
  // Use RELATIVE lifetime to handle allocation sequence divergence between trace and replay.
  // The oracle entry records absolute (alloc_seq, free_seq) from the trace run.
  // During replay, the per-thread allocation count may diverge due to filtering differences.
  // By computing relative lifetime and adding it to the replay sequence, we preserve the
  // correct object lifetime duration regardless of absolute sequence divergence.

  int32_t target_thread;
  uint64_t target_seq;

  // FIX: For both same-thread and cross-thread frees, compute relative lifetime
  // and schedule on the ALLOCATING thread. Cross-thread frees using absolute
  // (free_thread, free_seq) are unreliable when the free_thread's allocation count
  // diverges between trace and replay. Scheduling on the allocating thread with
  // relative lifetime preserves the correct object lifetime duration.
  uint64_t lifetime = (entry.free_seq > entry.alloc_seq)
                      ? (entry.free_seq - entry.alloc_seq)
                      : 1;  // Minimum lifetime of 1 to avoid immediate free
  target_thread = replay_logical_thread;
  target_seq = replay_alloc_seq + lifetime + EpsilonOracleGlobalDelta;

  if (target_seq > 0) {
    MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

    size_t bucket_idx = hash_thread_seq(target_thread, target_seq);
    DeathBucket* bucket = NEW_C_HEAP_OBJ(DeathBucket, mtGC);
    bucket->logical_thread = target_thread;
    bucket->seq = target_seq;
    bucket->ptr = ptr;
    bucket->size = size;
    bucket->next = _death_map[bucket_idx];
    _death_map[bucket_idx] = bucket;
  }
}

void EpsilonOracle::push_to_orphan_pool(ThreadAllocState* state, OracleEntry& entry) {
  // Cap orphan pool size - evict oldest (tail of list) when full
  if (state->orphan_count >= MAX_ORPHAN_POOL_SIZE) {
    // Walk to the tail to evict the oldest entry
    OrphanEntry* prev = nullptr;
    OrphanEntry* curr = state->orphan_pool;
    while (curr != nullptr && curr->next != nullptr) {
      prev = curr;
      curr = curr->next;
    }
    if (curr != nullptr) {
      if (prev != nullptr) {
        prev->next = nullptr;
      } else {
        state->orphan_pool = nullptr;
      }
      FREE_C_HEAP_OBJ(curr);
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

// --- Core allocation registration with type-keyed matching ---

bool EpsilonOracle::register_allocation(int64_t runtime_thread_id, void* ptr, size_t size, const char* alloc_type) {
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

  // Thread 0 handling controlled by EpsilonOracleSkipThread0 flag.
  // When true (default): logical IDs start at 1, oracle thread 0 skipped.
  // When false: logical IDs start at 0, first runtime thread matches oracle thread 0.
  // Type-keyed FIFO handles divergence via TYPE_EXHAUSTED (leak safely).

  // ============================================================
  // Type-keyed FIFO matching (primary path when type is available)
  // ============================================================
  if (alloc_type != nullptr && alloc_type[0] != '\0') {
    size_t bucket = hash_type_name(alloc_type);

    // Find type queue for this type
    TypeQueue* queue = state->type_queues[bucket];
    while (queue != nullptr) {
      if (strcmp(queue->type_name, alloc_type) == 0) break;
      queue = queue->next;
    }

    if (queue != nullptr && queue->head != nullptr) {
      // Scan FIFO for entry matching BOTH type AND size.
      // FIFO order alone is unreliable when allocation patterns diverge between
      // trace and replay. Size validation prevents assigning wrong lifetimes
      // (e.g., a short-lived 144-byte byte[] lifetime to a 140KB byte[] buffer).
      TypeQueueNode* prev = nullptr;
      TypeQueueNode* node = queue->head;
      TypeQueueNode* matched_node = nullptr;
      int scan_count = 0;
      int scan_limit = (int)EpsilonOracleLookahead;  // Reuse lookahead limit

      while (node != nullptr && scan_count < scan_limit) {
        OracleEntry& candidate = _entries[node->entry_idx];
        if (candidate.size == size) {
          // Found type+size match -- remove from FIFO
          matched_node = node;
          if (prev == nullptr) {
            queue->head = node->next;
          } else {
            prev->next = node->next;
          }
          if (queue->tail == node) {
            queue->tail = prev;
          }
          queue->count--;
          break;
        }
        prev = node;
        node = node->next;
        scan_count++;
      }

      if (matched_node != nullptr) {
        // Use RELATIVE lifetime from the matched oracle entry (fixes seq divergence)
        OracleEntry& entry = _entries[matched_node->entry_idx];
        update_lifetime_stats(state, entry);
        schedule_death_relative(entry, ptr, size, logical_thread, per_thread_seq);

        Atomic::add(&_matching_stats.type_matches, (uint64_t)1);
        Atomic::add(&_tracked_alloc_counter, (uint64_t)1);
        Atomic::add(&_allocated_bytes, size);

        log_info(gc)("Oracle: TYPE_MATCH logical_thread=%d seq=%" PRIu64 " type=[%s] size=%zu ptr=" PTR_FORMAT
                     " -> free at logical_thread=%d seq=%" PRIu64 " (scanned %d)",
                     logical_thread, per_thread_seq, alloc_type, size, p2i(ptr),
                     entry.free_thread, entry.free_seq, scan_count);

        FREE_C_HEAP_OBJ(matched_node);
        return true;
      }

      // Type matched but no size match within lookahead -- leak safely.
      // The oracle entry is for a different-sized object of the same type.
      Atomic::add(&_matching_stats.type_size_mismatch, (uint64_t)1);

      log_debug(gc)("Oracle: TYPE_SIZE_MISMATCH logical_thread=%d seq=%" PRIu64 " type=[%s] size=%zu ptr=" PTR_FORMAT
                    " (no size match in %d entries - leak safely)",
                    logical_thread, per_thread_seq, alloc_type, size, p2i(ptr), scan_count);

      return false;
    }

    // Type queue empty or not found - this allocation doesn't match any oracle entry.
    // This is expected for JVM-internal allocations that happen while depth > 0
    // (e.g., java.lang.Class, byte[] for class data during class loading).
    // Leak safely: don't schedule any death. Estimated deaths cause premature frees
    // and memory corruption because we have no oracle data for these objects.
    Atomic::add(&_matching_stats.type_exhausted, (uint64_t)1);

    log_debug(gc)("Oracle: TYPE_EXHAUSTED logical_thread=%d seq=%" PRIu64 " type=[%s] size=%zu ptr=" PTR_FORMAT
                  " (no oracle entry - leak safely)",
                  logical_thread, per_thread_seq, alloc_type, size, p2i(ptr));

    return false;
  }

  // ============================================================
  // Fallback: size-based matching (when type is not available)
  // This shouldn't happen with -Xint + -XX:-UseTLAB but kept as safety net
  // ============================================================
  Atomic::add(&_matching_stats.type_fallback, (uint64_t)1);

  log_debug(gc)("Oracle: TYPE_FALLBACK logical_thread=%d seq=%" PRIu64 " size=%zu ptr=" PTR_FORMAT
                " (no type available, using size-based matching)",
                logical_thread, per_thread_seq, size, p2i(ptr));

  // ============================================================
  // Step 1: Try cursor entry (perfect sequential match by size)
  // ============================================================
  if (cursor_has_entries(state, logical_thread)) {
    OracleEntry& entry = cursor_entry(state);

    if (entry.size == size) {
      // Perfect match - use relative lifetime for resilient scheduling
      update_lifetime_stats(state, entry);
      schedule_death_relative(entry, ptr, size, logical_thread, per_thread_seq);
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

        // Use matched entry's relative lifetime for resilient scheduling
        OracleEntry& matched = _entries[i];
        update_lifetime_stats(state, matched);
        schedule_death_relative(matched, ptr, size, logical_thread, per_thread_seq);

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

        // Free memory (possibly delayed to prevent use-after-free)
        delayed_free(ptr, size_in_bytes);
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

// --- Delayed free buffer ---

void EpsilonOracle::delayed_free(void* ptr, size_t size) {
  // Note: caller must hold _oracle_lock

  // Fast path: no delay configured -- free immediately
  if (EpsilonOracleFreeDelay == 0 || _delayed_free_buf == nullptr) {
    permit_forbidden_function::free(ptr);
    Atomic::add(&_free_counter, (uint64_t)1);
    Atomic::add(&_freed_bytes, size);
    return;
  }

  // If buffer full, force-drain oldest entry
  if (_delayed_free_count >= DELAYED_FREE_CAPACITY) {
    DelayedFreeEntry& oldest = _delayed_free_buf[_delayed_free_head];
    log_debug(gc)("Oracle: DELAYED FREE (buffer full) ptr=" PTR_FORMAT " size=%zu age=%" PRIu64,
                  p2i(oldest.ptr), oldest.size,
                  _total_alloc_counter - oldest.freed_at_global_seq);
    permit_forbidden_function::free(oldest.ptr);
    Atomic::add(&_free_counter, (uint64_t)1);
    Atomic::add(&_freed_bytes, oldest.size);
    _delayed_free_head = (_delayed_free_head + 1) % DELAYED_FREE_CAPACITY;
    _delayed_free_count--;
  }

  // Push to buffer
  DelayedFreeEntry& entry = _delayed_free_buf[_delayed_free_tail];
  entry.ptr = ptr;
  entry.size = size;
  entry.freed_at_global_seq = _total_alloc_counter;
  _delayed_free_tail = (_delayed_free_tail + 1) % DELAYED_FREE_CAPACITY;
  _delayed_free_count++;

  // Drain entries that have aged past the delay
  drain_delayed_frees();
}

void EpsilonOracle::drain_delayed_frees() {
  // Note: caller must hold _oracle_lock
  uint64_t current = _total_alloc_counter;
  while (_delayed_free_count > 0) {
    DelayedFreeEntry& entry = _delayed_free_buf[_delayed_free_head];
    if (current - entry.freed_at_global_seq < EpsilonOracleFreeDelay) break;

    log_debug(gc)("Oracle: DELAYED FREE ptr=" PTR_FORMAT " size=%zu age=%" PRIu64,
                  p2i(entry.ptr), entry.size,
                  current - entry.freed_at_global_seq);
    permit_forbidden_function::free(entry.ptr);
    Atomic::add(&_free_counter, (uint64_t)1);
    Atomic::add(&_freed_bytes, entry.size);
    _delayed_free_head = (_delayed_free_head + 1) % DELAYED_FREE_CAPACITY;
    _delayed_free_count--;
  }
}

void EpsilonOracle::drain_all_delayed_frees() {
  // Force-free everything in the buffer regardless of age (for finalize)
  // Note: caller must hold _oracle_lock
  while (_delayed_free_count > 0) {
    DelayedFreeEntry& entry = _delayed_free_buf[_delayed_free_head];
    log_debug(gc)("Oracle: DELAYED FREE (finalize) ptr=" PTR_FORMAT " size=%zu",
                  p2i(entry.ptr), entry.size);
    permit_forbidden_function::free(entry.ptr);
    Atomic::add(&_free_counter, (uint64_t)1);
    Atomic::add(&_freed_bytes, entry.size);
    _delayed_free_head = (_delayed_free_head + 1) % DELAYED_FREE_CAPACITY;
    _delayed_free_count--;
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

  // FIX: Acquire lock to prevent race with concurrent track/untrack operations
  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

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
  // FIX: Guard unsigned subtraction to prevent wrap-around
  log_info(gc)("  Bytes live:         %zu",
               (_allocated_bytes > _freed_bytes) ? (_allocated_bytes - _freed_bytes) : 0);
  log_info(gc)("  Oracle threads:     %d", _num_oracle_threads);
  log_info(gc)("  Runtime threads:    %d", _next_logical_thread_id);

  // Matching statistics
  log_info(gc)("  Matching statistics:");
  log_info(gc)("    Thread 0 skipped: %" PRIu64, _matching_stats.thread0_skipped);
  log_info(gc)("    Type matches:     %" PRIu64, _matching_stats.type_matches);
  log_info(gc)("    Type exhausted:   %" PRIu64, _matching_stats.type_exhausted);
  log_info(gc)("    Type size mism:   %" PRIu64, _matching_stats.type_size_mismatch);
  log_info(gc)("    Type fallback:    %" PRIu64, _matching_stats.type_fallback);
  log_info(gc)("    Perfect matches:  %" PRIu64, _matching_stats.perfect_matches);
  log_info(gc)("    Lookahead matches:%" PRIu64, _matching_stats.lookahead_matches);
  log_info(gc)("    Orphan matches:   %" PRIu64, _matching_stats.orphan_matches);
  log_info(gc)("    Estimated deaths: %" PRIu64, _matching_stats.estimated_deaths);
  log_info(gc)("    Oracle exhausted: %" PRIu64, _matching_stats.oracle_exhausted);
  log_info(gc)("    Total skipped:    %" PRIu64, _matching_stats.total_skipped);

  uint64_t total_matched = _matching_stats.type_matches +
                           _matching_stats.perfect_matches +
                           _matching_stats.lookahead_matches +
                           _matching_stats.orphan_matches + _matching_stats.estimated_deaths +
                           _matching_stats.oracle_exhausted;
  if (total_matched > 0) {
    log_info(gc)("    Type match rate:  %.1f%%", 100.0 * _matching_stats.type_matches / total_matched);
  }

  if (EpsilonOracleMallocMode) {
    log_info(gc)("  Malloc mode:        ENABLED");
    log_info(gc)("  Malloc tracked:     %zu", _malloc_tracked_count);
    log_info(gc)("  Malloc freed:       %zu", _malloc_freed_count);
    if (EpsilonOracleFreeDelay > 0) {
      log_info(gc)("  Free delay:         %" PRIu64 " allocs", EpsilonOracleFreeDelay);
      log_info(gc)("  Delayed free pending:%zu", _delayed_free_count);
    }
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
  // FIX: Guard against double-finalize
  if (_finalized) {
    log_warning(gc)("Oracle finalize: Already finalized, skipping");
    return;
  }
  _finalized = true;

  log_info(gc)("Oracle finalize: Processing remaining tracked objects...");

  // First, drain any entries still in the delayed-free buffer
  if (_delayed_free_count > 0) {
    MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);
    log_info(gc)("Oracle finalize: Draining %zu delayed-free entries", _delayed_free_count);
    drain_all_delayed_frees();
  }

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
  log_info(gc)("    Thread0 skip:   %" PRIu64, _matching_stats.thread0_skipped);
  log_info(gc)("    Type match:     %" PRIu64, _matching_stats.type_matches);
  log_info(gc)("    Type exhausted: %" PRIu64, _matching_stats.type_exhausted);
  log_info(gc)("    Type size mism: %" PRIu64, _matching_stats.type_size_mismatch);
  log_info(gc)("    Type fallback:  %" PRIu64, _matching_stats.type_fallback);
  log_info(gc)("    Perfect:        %" PRIu64, _matching_stats.perfect_matches);
  log_info(gc)("    Lookahead:      %" PRIu64, _matching_stats.lookahead_matches);
  log_info(gc)("    Orphan:         %" PRIu64, _matching_stats.orphan_matches);
  log_info(gc)("    Estimated:      %" PRIu64, _matching_stats.estimated_deaths);
  log_info(gc)("    Exhausted:      %" PRIu64, _matching_stats.oracle_exhausted);

  // Verify all tracked allocations were freed
  if (_tracked_alloc_counter == _free_counter) {
    log_info(gc)("Oracle: SUCCESS - All %" PRIu64 " tracked allocations were freed",
                 _tracked_alloc_counter);
  } else {
    log_warning(gc)("Oracle: WARNING - Tracked allocs (%" PRIu64 ") != Frees (%" PRIu64 ")",
                    _tracked_alloc_counter, _free_counter);
  }

  // Print InterpreterRuntime filter diagnostics
  extern void oracle_print_filter_stats();
  oracle_print_filter_stats();

  // Print EpsilonHeap allocation counters
  extern volatile uint64_t _total_oracle_alloc_calls;
  extern volatile uint64_t _total_oracle_alloc_pre_app;
  extern volatile uint64_t _total_oracle_alloc_system;
  extern volatile uint64_t _total_oracle_alloc_tracked;
  log_info(gc)("EpsilonHeap allocation path stats:");
  log_info(gc)("  Total allocate_work_oracle calls: %" PRIu64, _total_oracle_alloc_calls);
  log_info(gc)("  Pre-app allocations:              %" PRIu64, _total_oracle_alloc_pre_app);
  log_info(gc)("  System (untracked) allocations:   %" PRIu64, _total_oracle_alloc_system);
  log_info(gc)("  Tracked (oracle) allocations:     %" PRIu64, _total_oracle_alloc_tracked);
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

      // Clean up type queues for this thread
      for (size_t b = 0; b < ThreadAllocState::TYPE_QUEUE_BUCKETS; b++) {
        TypeQueue* queue = state->type_queues[b];
        while (queue != nullptr) {
          TypeQueue* next_queue = queue->next;
          // Free all nodes in this queue
          TypeQueueNode* node = queue->head;
          while (node != nullptr) {
            TypeQueueNode* next_node = node->next;
            FREE_C_HEAP_OBJ(node);
            node = next_node;
          }
          FREE_C_HEAP_OBJ(queue);
          queue = next_queue;
        }
        state->type_queues[b] = nullptr;
      }

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

  // Free delayed-free buffer
  if (_delayed_free_buf != nullptr) {
    FREE_C_HEAP_ARRAY(DelayedFreeEntry, _delayed_free_buf);
    _delayed_free_buf = nullptr;
    _delayed_free_count = 0;
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

// Global C functions exported for JVMTI agent communication via dlsym

// Signal application start (called by Elephant Tracks or similar agent)
extern "C" __attribute__((visibility("default"))) void epsilon_oracle_signal_app_start() {
  EpsilonHeap* heap = EpsilonHeap::heap();
  if (heap != nullptr && heap->oracle() != nullptr) {
    heap->oracle()->signal_app_start();
  }
}
