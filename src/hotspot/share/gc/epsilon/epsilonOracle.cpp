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
  _skip_start(EpsilonOracleSkipThread0 ? 1 : 0),  // First assignable logical ID
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

  // Initialize thread signatures (for content-based thread matching)
  memset(_thread_signatures, 0, sizeof(_thread_signatures));

  // Initialize runtime thread buffer map (for multi-sig buffering)
  memset(_runtime_buffers, 0, sizeof(_runtime_buffers));

  // Allocate malloc pointer tracking map if in malloc mode
  if (EpsilonOracleMallocMode) {
    _malloc_ptr_map = NEW_C_HEAP_ARRAY(MallocPtrBucket*, MALLOC_PTR_MAP_SIZE, mtGC);
    memset(_malloc_ptr_map, 0, sizeof(MallocPtrBucket*) * MALLOC_PTR_MAP_SIZE);
  }

  // Initialize TYPE_EXHAUSTED diagnostic array
  _type_exhausted_diag = NEW_C_HEAP_ARRAY(TypeExhaustedDiag, TYPE_EXHAUSTED_DIAG_CAPACITY, mtGC);
  _type_exhausted_diag_count = 0;
  memset(_type_exhausted_diag, 0, sizeof(TypeExhaustedDiag) * TYPE_EXHAUSTED_DIAG_CAPACITY);

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
               _skip_start,
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
    // NOTE: build_type_queues normalizes entry types IN-PLACE, so it MUST run
    // before build_thread_signatures which reads the normalized types.
    build_type_queues();
    // Build first-allocation signatures for content-based thread matching
    // (must run AFTER build_type_queues so types are already normalized)
    build_thread_signatures();
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

void EpsilonOracle::build_thread_signatures() {
  // Build multi-allocation signatures for content-based thread matching.
  // For each oracle thread, record the first K allocations' (type, size).
  // At runtime, when a new thread makes tracked allocations, we compare
  // against these K-deep signatures instead of just the first allocation.
  size_t sig_depth = EpsilonOracleSignatureDepth;
  if (sig_depth > (size_t)ThreadMultiSignature::MAX_SIG_DEPTH) {
    sig_depth = ThreadMultiSignature::MAX_SIG_DEPTH;
  }

  log_info(gc)("Oracle: Building multi-allocation signatures (depth=%zu)...", sig_depth);

  int sigs_built = 0;
  for (int32_t t = 0; t < _num_oracle_threads && (size_t)t < MAX_LOGICAL_THREADS; t++) {
    ThreadEntryIndex& idx = _thread_entry_index[t];
    ThreadMultiSignature& sig = _thread_signatures[t];
    sig.count = 0;
    sig.claimed = false;

    size_t entries_to_record = (idx.entry_count < sig_depth) ? idx.entry_count : sig_depth;
    for (size_t i = 0; i < entries_to_record; i++) {
      OracleEntry& entry = _entries[idx.first_entry_idx + i];
      // Use memcpy + explicit null-termination to avoid GCC 11 -Wstringop-truncation
      memcpy(sig.entries[i].type, entry.type, sizeof(sig.entries[i].type) - 1);
      sig.entries[i].type[sizeof(sig.entries[i].type) - 1] = '\0';
      sig.entries[i].size = entry.size;
      sig.count++;
    }

    if (sig.count > 0) {
      sigs_built++;
      log_info(gc)("Oracle: Thread %d signature: depth=%d first_type=[%s] first_size=%zu (entries=%zu)",
                   t, sig.count, sig.entries[0].type, sig.entries[0].size, idx.entry_count);
    }
  }

  // Warn about duplicate first-allocation signatures (triggers buffering mode)
  for (int32_t t1 = 0; t1 < _num_oracle_threads && (size_t)t1 < MAX_LOGICAL_THREADS; t1++) {
    if (_thread_signatures[t1].count == 0) continue;
    for (int32_t t2 = t1 + 1; t2 < _num_oracle_threads && (size_t)t2 < MAX_LOGICAL_THREADS; t2++) {
      if (_thread_signatures[t2].count == 0) continue;
      if (strcmp(_thread_signatures[t1].entries[0].type, _thread_signatures[t2].entries[0].type) == 0 &&
          _thread_signatures[t1].entries[0].size == _thread_signatures[t2].entries[0].size) {
        log_info(gc)("Oracle: DUPLICATE FIRST-SIG threads %d and %d both start with "
                     "type=[%s] size=%zu -- will use multi-sig scoring for disambiguation",
                     t1, t2, _thread_signatures[t1].entries[0].type,
                     _thread_signatures[t1].entries[0].size);
      }
    }
  }

  log_info(gc)("Oracle: Built %d multi-allocation signatures (depth=%zu)", sigs_built, sig_depth);
}

int EpsilonOracle::score_thread_match(const RuntimeThreadBuffer* buffer, int32_t oracle_thread) const {
  if (oracle_thread < 0 || (size_t)oracle_thread >= MAX_LOGICAL_THREADS) return 0;
  const ThreadMultiSignature& sig = _thread_signatures[oracle_thread];

  int score = 0;
  int compare_depth = (buffer->count < sig.count) ? buffer->count : sig.count;

  for (int i = 0; i < compare_depth; i++) {
    if (strcmp(buffer->buffer[i].type, sig.entries[i].type) == 0) {
      if (buffer->buffer[i].size == sig.entries[i].size) {
        score += 2;  // Type + size match
      } else {
        score += 1;  // Type-only match
      }
    }
  }
  return score;
}

int32_t EpsilonOracle::finalize_buffered_mapping(int64_t runtime_thread_id, RuntimeThreadBuffer* buffer) {
  // Score all candidates and pick the best
  int best_score = -1;
  int32_t best_thread = -1;

  for (int i = 0; i < buffer->candidate_count; i++) {
    int32_t candidate = buffer->candidates[i];
    if (_thread_signatures[candidate].claimed) continue;  // Already taken

    int s = score_thread_match(buffer, candidate);
    log_info(gc)("Oracle: MULTI-SIG SCORE runtime thread %" PRId64 " vs oracle thread %d: %d/%d",
                 runtime_thread_id, candidate, s, buffer->count * 2);

    if (s > best_score) {
      best_score = s;
      best_thread = candidate;
    }
  }

  // Require score > K (50% match) to accept
  int threshold = (int)EpsilonOracleSignatureDepth;
  if (best_thread >= 0 && best_score >= threshold) {
    _thread_signatures[best_thread].claimed = true;
    Atomic::add(&_matching_stats.buffered_map, (uint64_t)1);
    log_info(gc)("Oracle: BUFFERED_MAP runtime thread %" PRId64 " -> logical %d "
                 "(score=%d/%d, threshold=%d)",
                 runtime_thread_id, best_thread, best_score, buffer->count * 2, threshold);
    return best_thread;
  }

  // Fallback: sequential assignment among candidates
  for (int i = 0; i < buffer->candidate_count; i++) {
    int32_t candidate = buffer->candidates[i];
    if (!_thread_signatures[candidate].claimed) {
      _thread_signatures[candidate].claimed = true;
      Atomic::add(&_matching_stats.fallback_map, (uint64_t)1);
      log_warning(gc)("Oracle: FALLBACK_MAP runtime thread %" PRId64 " -> logical %d "
                      "(best_score=%d below threshold=%d)",
                      runtime_thread_id, candidate, best_score, threshold);
      return candidate;
    }
  }

  // All candidates claimed, use global sequential fallback
  for (int32_t t = _skip_start; (size_t)t < MAX_LOGICAL_THREADS; t++) {
    if (!_thread_signatures[t].claimed) {
      _thread_signatures[t].claimed = true;
      Atomic::add(&_matching_stats.fallback_map, (uint64_t)1);
      log_warning(gc)("Oracle: FALLBACK_MAP (global) runtime thread %" PRId64 " -> logical %d",
                      runtime_thread_id, t);
      return t;
    }
  }

  return -1;  // No slots available
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

// Map a runtime OS thread ID to a logical thread ID using two-phase
// multi-allocation signature matching.
// Phase 1: If the first allocation's (type, size) uniquely matches one oracle
//   thread signature, map immediately.
// Phase 2: If multiple oracle threads share the same first-sig, buffer K
//   allocations and score against all candidates. Returns -2 during buffering.
// Fallback: Sequential assignment if scoring fails.
int32_t EpsilonOracle::map_runtime_to_logical(int64_t runtime_thread_id,
                                               const char* alloc_type, size_t alloc_size) {
  if (!_app_started) {
    return -1;  // Don't map threads before app starts
  }

  size_t bucket_idx = hash_runtime_thread(runtime_thread_id);

  // --- Fast path: already mapped ---
  RuntimeThreadMapping* mapping = _runtime_thread_map[bucket_idx];
  while (mapping != nullptr) {
    if (mapping->runtime_thread_id == runtime_thread_id) {
      return mapping->logical_thread_id;
    }
    mapping = mapping->next;
  }

  // --- Check if this thread is in buffering mode (no mapping yet, buffer exists) ---
  size_t buf_idx = ((uint64_t)runtime_thread_id * 2654435761ULL) & (RUNTIME_BUFFER_MAP_SIZE - 1);
  RuntimeThreadBuffer* buffer = _runtime_buffers[buf_idx];
  if (buffer != nullptr && buffer->runtime_thread_id != runtime_thread_id) {
    // Hash collision: different thread owns this buffer slot.
    Atomic::add(&_matching_stats.buffer_collision, (uint64_t)1);
    buffer = nullptr;
  }
  if (buffer != nullptr) {
    // Add this allocation to buffer
    if (alloc_type != nullptr && buffer->count < RuntimeThreadBuffer::MAX_BUFFER) {
      strncpy(buffer->buffer[buffer->count].type, alloc_type,
              sizeof(buffer->buffer[0].type) - 1);
      buffer->buffer[buffer->count].type[sizeof(buffer->buffer[0].type) - 1] = '\0';
      buffer->buffer[buffer->count].size = alloc_size;
      buffer->count++;
    }

    // Check if buffer is full -> finalize mapping
    if ((size_t)buffer->count >= EpsilonOracleSignatureDepth) {
      MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);
      uint64_t leaked_count = (uint64_t)buffer->count;
      int32_t logical_id = finalize_buffered_mapping(runtime_thread_id, buffer);

      // Record leaked count BEFORE freeing buffer
      Atomic::add(&_matching_stats.buffered_leaked, leaked_count);
      FREE_C_HEAP_OBJ(buffer);
      _runtime_buffers[buf_idx] = nullptr;

      if (logical_id >= 0) {
        // Create the real mapping now
        RuntimeThreadMapping* new_mapping = NEW_C_HEAP_OBJ(RuntimeThreadMapping, mtGC);
        new_mapping->runtime_thread_id = runtime_thread_id;
        new_mapping->logical_thread_id = logical_id;
        new_mapping->next = _runtime_thread_map[bucket_idx];
        _runtime_thread_map[bucket_idx] = new_mapping;
        return logical_id;
      }
    }

    return -2;  // Still buffering
  }

  // --- Need to create mapping - take lock ---
  MutexLocker ml(_oracle_lock, Mutex::_no_safepoint_check_flag);

  // Double-check after acquiring lock
  mapping = _runtime_thread_map[bucket_idx];
  while (mapping != nullptr) {
    if (mapping->runtime_thread_id == runtime_thread_id) {
      return mapping->logical_thread_id;
    }
    mapping = mapping->next;
  }

  // --- Phase 1: Try immediate mapping (unique first-sig match) ---
  int32_t logical_id = -1;

  if (alloc_type != nullptr && alloc_type[0] != '\0') {
    // Count candidates: oracle threads whose first allocation matches (type, size)
    int32_t candidates[MAX_LOGICAL_THREADS];
    int candidate_count = 0;

    for (int32_t t = _skip_start; t < _num_oracle_threads && (size_t)t < MAX_LOGICAL_THREADS; t++) {
      if (_thread_signatures[t].claimed || _thread_signatures[t].count == 0) continue;
      if (strcmp(_thread_signatures[t].entries[0].type, alloc_type) == 0 &&
          _thread_signatures[t].entries[0].size == alloc_size) {
        if (candidate_count < (int)MAX_LOGICAL_THREADS) {
          candidates[candidate_count++] = t;
        }
      }
    }

    if (candidate_count == 1) {
      // Unique match -> map immediately
      logical_id = candidates[0];
      _thread_signatures[logical_id].claimed = true;
      Atomic::add(&_matching_stats.immediate_map, (uint64_t)1);
      log_info(gc)("Oracle: IMMEDIATE_MAP runtime thread %" PRId64 " -> logical %d "
                   "(type=[%s] size=%zu, unique first-sig)",
                   runtime_thread_id, logical_id, alloc_type, alloc_size);
    } else if (candidate_count > 1) {
      // Multiple matches -> enter buffering mode
      log_info(gc)("Oracle: BUFFERING runtime thread %" PRId64 " (%d candidates for "
                   "type=[%s] size=%zu) -- will score after %zu allocations",
                   runtime_thread_id, candidate_count, alloc_type, alloc_size,
                   (size_t)EpsilonOracleSignatureDepth);

      // Allocate buffer
      RuntimeThreadBuffer* new_buffer = NEW_C_HEAP_OBJ(RuntimeThreadBuffer, mtGC);
      memset(new_buffer, 0, sizeof(RuntimeThreadBuffer));
      new_buffer->runtime_thread_id = runtime_thread_id;
      new_buffer->candidate_count = candidate_count;
      for (int i = 0; i < candidate_count && i < (int)MAX_LOGICAL_THREADS; i++) {
        new_buffer->candidates[i] = candidates[i];
      }

      // Add first allocation to buffer
      strncpy(new_buffer->buffer[0].type, alloc_type, sizeof(new_buffer->buffer[0].type) - 1);
      new_buffer->buffer[0].type[sizeof(new_buffer->buffer[0].type) - 1] = '\0';
      new_buffer->buffer[0].size = alloc_size;
      new_buffer->count = 1;

      _runtime_buffers[buf_idx] = new_buffer;

      // Return -2 to signal "buffering, allocate without oracle tracking"
      return -2;
    } else {
      // No first-sig match -> buffer with ALL unclaimed oracle threads as candidates
      int32_t all_candidates[MAX_LOGICAL_THREADS];
      int all_count = 0;
      for (int32_t t = _skip_start; t < _num_oracle_threads && (size_t)t < MAX_LOGICAL_THREADS; t++) {
        if (!_thread_signatures[t].claimed && _thread_signatures[t].count > 0) {
          if (all_count < (int)MAX_LOGICAL_THREADS) {
            all_candidates[all_count++] = t;
          }
        }
      }

      if (all_count > 1) {
        // Multiple unclaimed threads — enter buffering to score after K allocations
        RuntimeThreadBuffer* new_buffer = NEW_C_HEAP_OBJ(RuntimeThreadBuffer, mtGC);
        memset(new_buffer, 0, sizeof(RuntimeThreadBuffer));
        new_buffer->runtime_thread_id = runtime_thread_id;
        new_buffer->candidate_count = all_count;
        for (int i = 0; i < all_count && i < (int)MAX_LOGICAL_THREADS; i++) {
          new_buffer->candidates[i] = all_candidates[i];
        }

        // Add first allocation to buffer
        strncpy(new_buffer->buffer[0].type, alloc_type, sizeof(new_buffer->buffer[0].type) - 1);
        new_buffer->buffer[0].type[sizeof(new_buffer->buffer[0].type) - 1] = '\0';
        new_buffer->buffer[0].size = alloc_size;
        new_buffer->count = 1;

        _runtime_buffers[buf_idx] = new_buffer;

        log_info(gc)("Oracle: BUFFERING (no first-sig match) runtime thread %" PRId64
                     " (%d unclaimed oracle threads) -- will score after %zu allocations",
                     runtime_thread_id, all_count, (size_t)EpsilonOracleSignatureDepth);
        return -2;
      } else if (all_count == 1) {
        // Only one unclaimed thread — map directly (nothing to score against)
        logical_id = all_candidates[0];
        _thread_signatures[logical_id].claimed = true;
        Atomic::add(&_matching_stats.immediate_map, (uint64_t)1);
        log_info(gc)("Oracle: IMMEDIATE_MAP (last unclaimed) runtime thread %" PRId64
                     " -> logical %d (type=[%s] size=%zu)",
                     runtime_thread_id, logical_id, alloc_type, alloc_size);
      }
      // else: all_count == 0, fall through to sequential fallback below
    }
  }

  // --- Fallback: sequential assignment ---
  if (logical_id < 0) {
    for (int32_t t = _skip_start; t < _num_oracle_threads && (size_t)t < MAX_LOGICAL_THREADS; t++) {
      if (!_thread_signatures[t].claimed) {
        logical_id = t;
        _thread_signatures[t].claimed = true;
        break;
      }
    }
    if (logical_id < 0) {
      for (int32_t t = _num_oracle_threads; (size_t)t < MAX_LOGICAL_THREADS; t++) {
        if (!_thread_signatures[t].claimed) {
          logical_id = t;
          _thread_signatures[t].claimed = true;
          break;
        }
      }
    }

    if (logical_id < 0) {
      log_warning(gc)("Oracle: All logical thread slots claimed, runtime thread %" PRId64
                      " unmappable", runtime_thread_id);
      return -1;
    }

    Atomic::add(&_matching_stats.fallback_map, (uint64_t)1);
    log_info(gc)("Oracle: FALLBACK_MAP runtime thread %" PRId64 " -> logical %d "
                 "(type=[%s] size=%zu)",
                 runtime_thread_id, logical_id,
                 (alloc_type ? alloc_type : "?"), alloc_size);
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

uint64_t EpsilonOracle::next_thread_alloc_seq(int64_t runtime_thread_id,
                                                const char* alloc_type, size_t alloc_size) {
  // Map runtime thread to logical thread (uses signature matching on first allocation)
  int32_t logical_thread = map_runtime_to_logical(runtime_thread_id, alloc_type, alloc_size);
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
  // Schedule object death using the oracle entry's timing information.
  // Two strategies depending on whether free happens on the same or different thread:

  // --- ClassLoader protection ---
  // Never free ClassLoader objects. The JVM's ClassLoaderDataGraph holds live references
  // to classloader instances that persist beyond oracle-scheduled death times.
  // Freeing them causes is_oop() assertion failures during JVM shutdown.
  // Also protect java.lang.Class objects which participate in class loading metadata.
  if (entry.type[0] != '\0') {
    if (strstr(entry.type, "ClassLoader") != nullptr ||
        strcmp(entry.type, "java.lang.Class") == 0 ||
        strncmp(entry.type, "org.dacapo.", 11) == 0) {
      Atomic::add(&_matching_stats.classloader_leaked, (uint64_t)1);
      log_info(gc)("Oracle: INFRASTRUCTURE_LEAKED type=[%s] size=%zu ptr=" PTR_FORMAT
                   " -- infrastructure protection, leak safely",
                   entry.type, size, p2i(ptr));
      return;  // Don't schedule death -- leak this object
    }
  }

  int32_t target_thread;
  uint64_t target_seq;

  if (entry.free_thread == entry.alloc_thread) {
    // SAME-THREAD FREE: Use relative lifetime for resilience against sequence divergence.
    // lifetime = free_seq - alloc_seq is meaningful because both are on the same per-thread clock.
    // Adding this to the replay's current sequence preserves the correct object lifetime
    // even if the absolute allocation counts differ between trace and replay.
    uint64_t lifetime = (entry.free_seq > entry.alloc_seq)
                        ? (entry.free_seq - entry.alloc_seq)
                        : 1;  // Minimum lifetime of 1 to avoid immediate free

    // --- FIX A: Detect and leak long-lived singleton objects ---
    // Objects whose lifetime spans >= 90% of the oracle thread's total entries
    // are effectively "live until JVM exit" singletons (e.g., java.util.Locale).
    // In the trace, GC freed them at shutdown; freeing them during replay causes
    // use-after-free / SIGSEGV because the program still references them.
    // Leak these objects safely instead of scheduling a premature death.
    int32_t oracle_thread = entry.alloc_thread;
    if (oracle_thread >= 0 && (size_t)oracle_thread < MAX_LOGICAL_THREADS) {
      size_t thread_total_entries = _thread_entry_index[oracle_thread].entry_count;
      if (thread_total_entries > 0) {
        // Use integer arithmetic: lifetime * 10 >= thread_total_entries * 9 means >= 90%
        if (lifetime * 10 >= (uint64_t)thread_total_entries * 9) {
          Atomic::add(&_matching_stats.longlived_leaked, (uint64_t)1);
          log_info(gc)("Oracle: LONGLIVED_LEAKED type=[%s] size=%zu ptr=" PTR_FORMAT
                       " lifetime=%" PRIu64 " thread_entries=%zu (%.1f%% of thread %d entries)"
                       " -- singleton protection, leak safely",
                       entry.type, size, p2i(ptr),
                       lifetime, thread_total_entries,
                       100.0 * lifetime / thread_total_entries, oracle_thread);
          return;  // Do NOT schedule death -- leak this object
        }
      }
    }

    target_thread = replay_logical_thread;
    target_seq = replay_alloc_seq + lifetime + EpsilonOracleGlobalDelta;

    // --- FIX B: Hard cap -- never schedule death beyond oracle horizon ---
    // If the computed target_seq exceeds the replay thread's current sequence
    // by more than the total oracle entries for that thread, we are extrapolating
    // beyond what the oracle knows. This can happen when replay diverges
    // significantly from the trace. Leak safely instead.
    if (replay_logical_thread >= 0 && (size_t)replay_logical_thread < MAX_LOGICAL_THREADS) {
      size_t replay_thread_entries = _thread_entry_index[replay_logical_thread].entry_count;
      if (replay_thread_entries > 0 && target_seq > replay_alloc_seq + (uint64_t)replay_thread_entries) {
        Atomic::add(&_matching_stats.longlived_leaked, (uint64_t)1);
        log_info(gc)("Oracle: HORIZON_LEAKED type=[%s] size=%zu ptr=" PTR_FORMAT
                     " target_seq=%" PRIu64 " exceeds replay_seq=%" PRIu64
                     " + thread_entries=%zu -- beyond oracle horizon, leak safely",
                     entry.type, size, p2i(ptr),
                     target_seq, replay_alloc_seq, replay_thread_entries);
        return;  // Do NOT schedule death -- beyond oracle horizon
      }
    }
  } else {
    // CROSS-THREAD FREE: Use original (free_thread, free_seq) directly.
    // Relative lifetime (free_seq - alloc_seq) is meaningless across different per-thread
    // clocks. The runtime filters match ET's bytecode instrumentation filtering, so
    // per-thread allocation counts should closely match the oracle.
    target_thread = entry.free_thread;
    target_seq = entry.free_seq + EpsilonOracleGlobalDelta;
  }

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
    record_type_exhausted_diag(alloc_type);

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

bool EpsilonOracle::is_malloc_tracked(void* ptr) {
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
  // Count claimed runtime threads
  int claimed_count = 0;
  for (size_t i = 0; i < MAX_LOGICAL_THREADS; i++) {
    if (_thread_signatures[i].claimed) claimed_count++;
  }
  log_info(gc)("  Runtime threads:    %d", claimed_count);

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
  log_info(gc)("    Long-lived leaked:%" PRIu64, _matching_stats.longlived_leaked);
  log_info(gc)("  Thread mapping statistics:");
  log_info(gc)("    Immediate map:    %" PRIu64, _matching_stats.immediate_map);
  log_info(gc)("    Buffered map:     %" PRIu64, _matching_stats.buffered_map);
  log_info(gc)("    Fallback map:     %" PRIu64, _matching_stats.fallback_map);
  log_info(gc)("    Buffered leaked:  %" PRIu64, _matching_stats.buffered_leaked);
  log_info(gc)("    Buffer collision: %" PRIu64, _matching_stats.buffer_collision);

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
  log_info(gc)("    Long-lived leak:%" PRIu64, _matching_stats.longlived_leaked);
  log_info(gc)("    Classloader leak:%" PRIu64, _matching_stats.classloader_leaked);
  // Print TYPE_EXHAUSTED diagnostic breakdown
  print_type_exhausted_diag();
  log_info(gc)("  Thread mapping:");
  log_info(gc)("    Immediate map:  %" PRIu64, _matching_stats.immediate_map);
  log_info(gc)("    Buffered map:   %" PRIu64, _matching_stats.buffered_map);
  log_info(gc)("    Fallback map:   %" PRIu64, _matching_stats.fallback_map);
  log_info(gc)("    Buffered leaked:%" PRIu64, _matching_stats.buffered_leaked);
  log_info(gc)("    Buf collision:  %" PRIu64, _matching_stats.buffer_collision);

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

void EpsilonOracle::record_type_exhausted_diag(const char* type_name) {
  if (type_name == nullptr || type_name[0] == '\0') return;

  // Linear scan — fewer than 500 distinct types expected
  for (size_t i = 0; i < _type_exhausted_diag_count; i++) {
    if (strcmp(_type_exhausted_diag[i].type, type_name) == 0) {
      _type_exhausted_diag[i].count++;
      return;
    }
  }

  // New type — add if capacity allows
  if (_type_exhausted_diag_count < TYPE_EXHAUSTED_DIAG_CAPACITY) {
    strncpy(_type_exhausted_diag[_type_exhausted_diag_count].type, type_name, 127);
    _type_exhausted_diag[_type_exhausted_diag_count].type[127] = '\0';
    _type_exhausted_diag[_type_exhausted_diag_count].count = 1;
    _type_exhausted_diag_count++;
  }
}

void EpsilonOracle::print_type_exhausted_diag() {
  if (_type_exhausted_diag_count == 0) return;

  // Simple insertion sort by count descending (small array, one-time operation)
  for (size_t i = 1; i < _type_exhausted_diag_count; i++) {
    TypeExhaustedDiag temp = _type_exhausted_diag[i];
    size_t j = i;
    while (j > 0 && _type_exhausted_diag[j - 1].count < temp.count) {
      _type_exhausted_diag[j] = _type_exhausted_diag[j - 1];
      j--;
    }
    _type_exhausted_diag[j] = temp;
  }

  log_info(gc)("  TYPE_EXHAUSTED breakdown (top 20 types):");
  size_t limit = _type_exhausted_diag_count < 20 ? _type_exhausted_diag_count : 20;
  for (size_t i = 0; i < limit; i++) {
    log_info(gc)("    %3zu. [%s] = %" PRIu64, i + 1,
                 _type_exhausted_diag[i].type,
                 _type_exhausted_diag[i].count);
  }
  if (_type_exhausted_diag_count > 20) {
    log_info(gc)("    ... and %zu more types", _type_exhausted_diag_count - 20);
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

  // Clean up TYPE_EXHAUSTED diagnostic array
  if (_type_exhausted_diag != nullptr) {
    FREE_C_HEAP_ARRAY(TypeExhaustedDiag, _type_exhausted_diag);
    _type_exhausted_diag = nullptr;
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

  // Clean up runtime thread buffers (from multi-sig buffering)
  for (size_t i = 0; i < RUNTIME_BUFFER_MAP_SIZE; i++) {
    if (_runtime_buffers[i] != nullptr) {
      FREE_C_HEAP_OBJ(_runtime_buffers[i]);
      _runtime_buffers[i] = nullptr;
    }
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
