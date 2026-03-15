/*
 * Copyright (c) 2023, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, 2022, Red Hat, Inc. All rights reserved.
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

#include "gc/epsilon/epsilonHeap.hpp"
#include "gc/epsilon/epsilonInitLogger.hpp"
#include "gc/epsilon/epsilonMemoryPool.hpp"
#include "gc/epsilon/epsilonOracle.hpp"
#include "gc/epsilon/epsilonThreadLocalData.hpp"
#include "oops/klass.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "utilities/align.hpp"
#include "utilities/ostream.hpp"
#include "utilities/permitForbiddenFunctions.hpp"
#include "runtime/os.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "oops/klass.hpp"

#include <cinttypes>

jint EpsilonHeap::initialize() {
  size_t align = HeapAlignment;
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  // Initialize backing storage
  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);

  MemRegion committed_region((HeapWord*)_virtual_space.low(),          (HeapWord*)_virtual_space.high());

  initialize_reserved_region(heap_rs);

  _space = new ContiguousSpace();
  _space->initialize(committed_region, /* clear_space = */ true, /* mangle_space = */ true);

  // Precompute hot fields
  _max_tlab_size = MIN2(CollectedHeap::max_tlab_size(), align_object_size(EpsilonMaxTLABSize / HeapWordSize));
  _step_counter_update = MIN2<size_t>(max_byte_size / 16, EpsilonUpdateCountersStep);
  _step_heap_print = (EpsilonPrintHeapSteps == 0) ? SIZE_MAX : (max_byte_size / EpsilonPrintHeapSteps);
  _decay_time_ns = (int64_t) EpsilonTLABDecayTime * NANOSECS_PER_MILLISEC;

  // Enable monitoring
  _monitoring_support = new EpsilonMonitoringSupport(this);
  _last_counter_update = 0;
  _last_heap_print = 0;

  // Install barrier set
  BarrierSet::set_barrier_set(new EpsilonBarrierSet());

  // Initialize oracle malloc mode flag (cached for is_in() method)
  _oracle_malloc_mode = EpsilonOracleMallocMode;

  // Oracle mode initialization
  if (EpsilonOracleMode) {
    // Oracle mode requires TLABs to be disabled so we can track all allocations
    if (UseTLAB) {
      log_error(gc)("EpsilonOracleMode requires -XX:-UseTLAB to track all allocations");
      return JNI_ERR;
    }

    // Malloc mode requires compressed oops/class pointers to be disabled
    // because malloc'd addresses are outside the heap region and cannot be
    // encoded as compressed oops (32-bit offsets from heap base).
    // Auto-disable them when oracle mode is active with malloc mode.
    if (EpsilonOracleMallocMode) {
      if (UseCompressedOops) {
        log_info(gc)("Oracle malloc mode: auto-disabling UseCompressedOops");
        FLAG_SET_CMDLINE(UseCompressedOops, false);
      }
      if (UseCompressedClassPointers) {
        log_info(gc)("Oracle malloc mode: auto-disabling UseCompressedClassPointers");
        FLAG_SET_CMDLINE(UseCompressedClassPointers, false);
      }
    }

    _oracle = new EpsilonOracle();
    if (!_oracle->load_trace(EpsilonOracleTracePath)) {
      log_error(gc)("Failed to load oracle trace from: %s", EpsilonOracleTracePath);
      return JNI_ERR;
    }
    log_info(gc)("Oracle mode: Loaded %zu entries, using malloc/free for heap allocations",
                 _oracle->entry_count());
    log_info(gc)("Oracle mode: Resilient matching enabled, lookahead=%zu, death_delta=" UINT64_FORMAT
                 ", global_delta=" UINT64_FORMAT,
                 EpsilonOracleLookahead, EpsilonOracleDeathDelta, EpsilonOracleGlobalDelta);
  }

  // All done, print out the configuration
  EpsilonInitLogger::print();

  return JNI_OK;
}

void EpsilonHeap::initialize_serviceability() {
  _pool = new EpsilonMemoryPool(this);
  _memory_manager.add_pool(_pool);
}

GrowableArray<GCMemoryManager*> EpsilonHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(1);
  memory_managers.append(&_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> EpsilonHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_pool);
  return memory_pools;
}

size_t EpsilonHeap::unsafe_max_tlab_alloc(Thread* thr) const {
  // Return max allocatable TLAB size, and let allocation path figure out
  // the actual allocation size. Note: result should be in bytes.
  return _max_tlab_size * HeapWordSize;
}

EpsilonHeap* EpsilonHeap::heap() {
  return named_heap<EpsilonHeap>(CollectedHeap::Epsilon);
}

HeapWord* EpsilonHeap::allocate_work(size_t size, bool verbose) {
  assert(is_object_aligned(size), "Allocation size should be aligned: %zu", size);

  // Oracle mode: use malloc-based allocation
  if (EpsilonOracleMode) {
    return allocate_work_oracle(size, verbose);
  }

  // Standard Epsilon mode: bump-pointer allocation
  HeapWord* res = nullptr;
  while (true) {
    // Try to allocate, assume space is available
    res = _space->par_allocate(size);
    if (res != nullptr) {
      break;
    }

    // Allocation failed, attempt expansion, and retry:
    {
      MutexLocker ml(Heap_lock);

      // Try to allocate under the lock, assume another thread was able to expand
      res = _space->par_allocate(size);
      if (res != nullptr) {
        break;
      }

      // Expand and loop back if space is available
      size_t size_in_bytes = size * HeapWordSize;
      size_t uncommitted_space = max_capacity() - capacity();
      size_t unused_space = max_capacity() - used();
      size_t want_space = MAX2(size_in_bytes, EpsilonMinHeapExpand);
      assert(unused_space >= uncommitted_space,
             "Unused (%zu) >= uncommitted (%zu)",
             unused_space, uncommitted_space);

      if (want_space < uncommitted_space) {
        // Enough space to expand in bulk:
        bool expand = _virtual_space.expand_by(want_space);
        assert(expand, "Should be able to expand");
      } else if (size_in_bytes < unused_space) {
        // No space to expand in bulk, and this allocation is still possible,
        // take all the remaining space:
        bool expand = _virtual_space.expand_by(uncommitted_space);
        assert(expand, "Should be able to expand");
      } else {
        // No space left:
        return nullptr;
      }

      _space->set_end((HeapWord *) _virtual_space.high());
    }
  }

  size_t used = _space->used();

  // Allocation successful, update counters
  if (verbose) {
    size_t last = _last_counter_update;
    if ((used - last >= _step_counter_update) && Atomic::cmpxchg(&_last_counter_update, last, used) == last) {
      _monitoring_support->update_counters();
    }
  }

  // ...and print the occupancy line, if needed
  if (verbose) {
    size_t last = _last_heap_print;
    if ((used - last >= _step_heap_print) && Atomic::cmpxchg(&_last_heap_print, last, used) == last) {
      print_heap_info(used);
      print_metaspace_info();
    }
  }

  assert(is_object_aligned(res), "Object should be aligned: " PTR_FORMAT, p2i(res));
  return res;
}

volatile uint64_t _total_oracle_alloc_calls = 0;
volatile uint64_t _total_oracle_alloc_pre_app = 0;
volatile uint64_t _total_oracle_alloc_system = 0;
volatile uint64_t _total_oracle_alloc_tracked = 0;

HeapWord* EpsilonHeap::allocate_work_oracle(size_t size, bool verbose) {
  assert(EpsilonOracleMode, "Should only be called in oracle mode");
  assert(_oracle != nullptr, "Oracle should be initialized");

  Atomic::inc(&_total_oracle_alloc_calls);

  // Get OS thread ID for per-thread tracking
  int64_t thread_id = os::current_thread_id();

  // Calculate size in bytes (size parameter is in HeapWords)
  size_t size_in_bytes = size * HeapWordSize;

  // Helper lambda for bump-pointer allocation with expansion
  auto bump_allocate = [&]() -> HeapWord* {
    HeapWord* mem = _space->par_allocate(size);
    if (mem == nullptr) {
      MutexLocker ml(Heap_lock);
      mem = _space->par_allocate(size);
      if (mem == nullptr) {
        size_t uncommitted_space = max_capacity() - capacity();
        size_t want_space = MAX2(size_in_bytes, EpsilonMinHeapExpand);
        if (want_space <= uncommitted_space) {
          bool expand = _virtual_space.expand_by(want_space);
          assert(expand, "Should be able to expand");
          _space->set_end((HeapWord *) _virtual_space.high());
          mem = _space->par_allocate(size);
        }
      }
    }
    return mem;
  };

  // Before app starts, use bump-pointer allocation without tracking
  // (JVM bootstrap allocations are not in the trace)
  if (!_oracle->app_started()) {
    Atomic::inc(&_total_oracle_alloc_pre_app);
    _oracle->count_pre_app_alloc();

    // Auto-start after EpsilonOracleSkipAllocs allocations
    if (_oracle->total_alloc_count() >= EpsilonOracleSkipAllocs) {
      _oracle->signal_app_start();
      log_info(gc)("Oracle: Auto-started after " UINT64_FORMAT " allocations (EpsilonOracleSkipAllocs=" UINT64_FORMAT ")",
                   _oracle->total_alloc_count(), EpsilonOracleSkipAllocs);
    }

    HeapWord* mem = bump_allocate();
    if (verbose && mem != nullptr) {
      log_trace(gc)("Oracle PRE-APP alloc: thread=" INT64_FORMAT " ptr=" PTR_FORMAT " size=%zu bytes",
                    thread_id, p2i(mem), size_in_bytes);
    }
    return mem;
  }

  // Application has started - use per-thread sequential matching
  // The N-th allocation on thread T matches the N-th oracle entry for thread T
  // ONLY for application allocations (detected via JVM-internal classloader check
  // in InterpreterRuntime::_new/newarray/anewarray/multianewarray)

  // Check if this is an application allocation
  // JVM-internal detection sets app_code_depth=1 around app allocations in interpreter
  Thread* current_thread = Thread::current();
  bool is_app_allocation = EpsilonThreadLocalData::in_app_code(current_thread);

  // If not an application allocation, just do bump-pointer allocation without tracking
  if (!is_app_allocation) {
    Atomic::inc(&_total_oracle_alloc_system);
    HeapWord* mem = bump_allocate();
    if (verbose && mem != nullptr) {
      log_trace(gc)("Oracle SYSTEM alloc (not tracked): thread=%" PRId64 " ptr=" PTR_FORMAT
                    " size=%zu bytes",
                    thread_id, p2i(mem), size_in_bytes);
    }
    return mem;
  }

  Atomic::inc(&_total_oracle_alloc_tracked);

  // Extract type FIRST — needed for thread signature matching during map_runtime_to_logical().
  // The Klass was stored in thread-local data by memAllocator.cpp before calling mem_allocate().
  const char* alloc_type = nullptr;
  char normalized_type[128];
  Klass* klass = EpsilonThreadLocalData::current_alloc_klass(current_thread);
  if (klass != nullptr) {
    const char* raw_name = klass->external_name();
    EpsilonOracle::normalize_type_name(raw_name, normalized_type, sizeof(normalized_type));
    alloc_type = normalized_type;
  }

  // Extract allocating method from thread-local data (set by InterpreterRuntime)
  const char* alloc_method = nullptr;
  const char* tld_method = EpsilonThreadLocalData::alloc_method(current_thread);
  if (tld_method[0] != '\0') {
    alloc_method = tld_method;
  }

  // Extract allocation site key from thread-local data (set by InterpreterRuntime BCI scan)
  const char* alloc_site = nullptr;
  const char* tld_site = EpsilonThreadLocalData::alloc_site(current_thread);
  if (tld_site[0] != '\0') {
    alloc_site = tld_site;
  }

  // Get next per-thread allocation sequence number (1-based).
  // This also maps the runtime thread to a logical thread ID on first call,
  // using alloc_type and size_in_bytes for signature-based matching.
  uint64_t per_thread_seq = _oracle->next_thread_alloc_seq(thread_id, alloc_type, size_in_bytes);

  // Get logical thread ID (should be valid since next_thread_alloc_seq just mapped it)
  int32_t logical_thread = _oracle->get_logical_thread(thread_id);
  if (logical_thread < 0) {
    // Thread not mapped - shouldn't happen if app started
    log_trace(gc)("Oracle: Thread %" PRId64 " not mapped to logical thread", thread_id);
    HeapWord* mem = bump_allocate();
    return mem;
  }

  HeapWord* mem = nullptr;

  if (EpsilonOracleMallocMode) {
    // MALLOC MODE: Use actual malloc/free for true memory management measurement

    // Process deaths with actual free() for this logical thread at this sequence
    _oracle->process_deaths_malloc_mode(logical_thread, per_thread_seq);

    // Allocate with actual malloc
    void* malloc_mem = permit_forbidden_function::malloc(size_in_bytes);
    if (malloc_mem == nullptr) {
      log_error(gc)("Oracle MALLOC allocation failed: thread=" INT64_FORMAT " seq=" UINT64_FORMAT " size=%zu bytes",
                    thread_id, per_thread_seq, size_in_bytes);
      return nullptr;
    }

    // Zero memory (required for object initialization)
    memset(malloc_mem, 0, size_in_bytes);

    // Track this malloc'd pointer
    _oracle->track_malloc_ptr(malloc_mem, size_in_bytes);

    // Register for future deallocation (returns true if matched oracle entry)
    bool matched = _oracle->register_allocation(thread_id, malloc_mem, size_in_bytes, alloc_type, alloc_method, alloc_site);

    // Track allocated bytes
    Atomic::add(&_oracle_allocated_bytes, size_in_bytes);

    if (verbose) {
      Thread* cur = Thread::current();
      const char* tname = (cur != nullptr && cur->is_Java_thread()) ?
                          JavaThread::cast(cur)->name() : "<non-java>";
      log_info(gc)("Oracle MALLOC alloc: thread=" INT64_FORMAT " seq=" UINT64_FORMAT " ptr=" PTR_FORMAT
                   " size=%zu bytes type=[%s] method=[%s] matched=%s tname=[%s]",
                   thread_id, per_thread_seq, p2i(malloc_mem), size_in_bytes,
                   (alloc_type ? alloc_type : "?"),
                   (alloc_method ? alloc_method : ""),
                   matched ? "true" : "false",
                   (tname ? tname : "?"));
    }

    return (HeapWord*)malloc_mem;
  }

  // FREE-LIST MODE: Use in-heap free list simulation (default)

  // Process any deaths that should happen at this per-thread sequence number
  // This adds freed objects to the free list for reuse
  _oracle->process_deaths(logical_thread, per_thread_seq);

  // First, try to allocate from free list (first-fit)
  mem = _oracle->allocate_from_free_list(size);

  if (mem == nullptr) {
    // No suitable free block found - use bump-pointer allocation from heap
    mem = bump_allocate();

    if (mem == nullptr) {
      log_error(gc)("Oracle allocation failed: thread=" INT64_FORMAT " seq=" UINT64_FORMAT " size=%zu bytes (heap exhausted)",
                    thread_id, per_thread_seq, size_in_bytes);
      return nullptr;
    }
  }

  // Zero the memory - ALWAYS required for both free list and bump-pointer allocations
  // Free list memory may contain stale object data that must be cleared
  memset(mem, 0, size_in_bytes);

  // Register this allocation with the oracle for future deallocation
  bool matched = _oracle->register_allocation(thread_id, mem, size_in_bytes, alloc_type, alloc_method, alloc_site);

  // Track allocated bytes
  Atomic::add(&_oracle_allocated_bytes, size_in_bytes);

  if (verbose) {
    Thread* cur2 = Thread::current();
    const char* tname2 = (cur2 != nullptr && cur2->is_Java_thread()) ?
                         JavaThread::cast(cur2)->name() : "<non-java>";
    log_info(gc)("Oracle alloc: thread=" INT64_FORMAT " seq=" UINT64_FORMAT " ptr=" PTR_FORMAT
                 " size=%zu bytes type=[%s] matched=%s tname=[%s]",
                 thread_id, per_thread_seq, p2i(mem), size_in_bytes,
                 (alloc_type ? alloc_type : "?"), matched ? "true" : "false",
                 (tname2 ? tname2 : "?"));
  }

  return mem;
}

HeapWord* EpsilonHeap::allocate_new_tlab(size_t min_size,
                                         size_t requested_size,
                                         size_t* actual_size) {
  Thread* thread = Thread::current();

  // Defaults in case elastic paths are not taken
  bool fits = true;
  size_t size = requested_size;
  size_t ergo_tlab = requested_size;
  int64_t time = 0;

  if (EpsilonElasticTLAB) {
    ergo_tlab = EpsilonThreadLocalData::ergo_tlab_size(thread);

    if (EpsilonElasticTLABDecay) {
      int64_t last_time = EpsilonThreadLocalData::last_tlab_time(thread);
      time = (int64_t) os::javaTimeNanos();

      assert(last_time <= time, "time should be monotonic");

      // If the thread had not allocated recently, retract the ergonomic size.
      // This conserves memory when the thread had initial burst of allocations,
      // and then started allocating only sporadically.
      if (last_time != 0 && (time - last_time > _decay_time_ns)) {
        ergo_tlab = 0;
        EpsilonThreadLocalData::set_ergo_tlab_size(thread, 0);
      }
    }

    // If we can fit the allocation under current TLAB size, do so.
    // Otherwise, we want to elastically increase the TLAB size.
    fits = (requested_size <= ergo_tlab);
    if (!fits) {
      size = (size_t) (ergo_tlab * EpsilonTLABElasticity);
    }
  }

  // Always honor boundaries
  size = clamp(size, min_size, _max_tlab_size);

  // Always honor alignment
  size = align_up(size, MinObjAlignment);

  // Check that adjustments did not break local and global invariants
  assert(is_object_aligned(size),
         "Size honors object alignment: %zu", size);
  assert(min_size <= size,
         "Size honors min size: %zu <= %zu", min_size, size);
  assert(size <= _max_tlab_size,
         "Size honors max size: %zu <= %zu", size, _max_tlab_size);
  assert(size <= CollectedHeap::max_tlab_size(),
         "Size honors global max size: %zu <= %zu", size, CollectedHeap::max_tlab_size());

  if (log_is_enabled(Trace, gc)) {
    ResourceMark rm;
    log_trace(gc)("TLAB size for \"%s\" (Requested: %zuK, Min: %zu"
                          "K, Max: %zuK, Ergo: %zuK) -> %zuK",
                  thread->name(),
                  requested_size * HeapWordSize / K,
                  min_size * HeapWordSize / K,
                  _max_tlab_size * HeapWordSize / K,
                  ergo_tlab * HeapWordSize / K,
                  size * HeapWordSize / K);
  }

  // All prepared, let's do it!
  HeapWord* res = allocate_work(size);

  if (res != nullptr) {
    // Allocation successful
    *actual_size = size;
    if (EpsilonElasticTLABDecay) {
      EpsilonThreadLocalData::set_last_tlab_time(thread, time);
    }
    if (EpsilonElasticTLAB && !fits) {
      // If we requested expansion, this is our new ergonomic TLAB size
      EpsilonThreadLocalData::set_ergo_tlab_size(thread, size);
    }
  } else {
    // Allocation failed, reset ergonomics to try and fit smaller TLABs
    if (EpsilonElasticTLAB) {
      EpsilonThreadLocalData::set_ergo_tlab_size(thread, 0);
    }
  }

  return res;
}

HeapWord* EpsilonHeap::mem_allocate(size_t size, bool *gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  return allocate_work(size);
}

HeapWord* EpsilonHeap::allocate_loaded_archive_space(size_t size) {
  // Cannot use verbose=true because Metaspace is not initialized
  return allocate_work(size, /* verbose = */false);
}

void EpsilonHeap::collect(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_metadata_GC_threshold:
    case GCCause::_metadata_GC_clear_soft_refs:
      // Receiving these causes means the VM itself entered the safepoint for metadata collection.
      // While Epsilon does not do GC, it has to perform sizing adjustments, otherwise we would
      // re-enter the safepoint again very soon.

      assert(SafepointSynchronize::is_at_safepoint(), "Expected at safepoint");
      log_info(gc)("GC request for \"%s\" is handled", GCCause::to_string(cause));
      MetaspaceGC::compute_new_size();
      print_metaspace_info();
      break;
    default:
      log_info(gc)("GC request for \"%s\" is ignored", GCCause::to_string(cause));
  }
  _monitoring_support->update_counters();
}

void EpsilonHeap::do_full_collection(bool clear_all_soft_refs) {
  collect(gc_cause());
}

void EpsilonHeap::object_iterate(ObjectClosure *cl) {
  _space->object_iterate(cl);
}

void EpsilonHeap::print_heap_on(outputStream *st) const {
  st->print_cr("Epsilon Heap");

  StreamIndentor si(st, 1);

  _virtual_space.print_on(st);

  if (_space != nullptr) {
    st->print_cr("Allocation space:");

    StreamIndentor si(st, 1);
    _space->print_on(st, "");
  }
}

bool EpsilonHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<EpsilonHeap>::print_location(st, addr);
}

void EpsilonHeap::print_tracing_info() const {
  print_heap_info(used());
  print_metaspace_info();

  // Finalize and print oracle statistics if in oracle mode
  if (EpsilonOracleMode && _oracle != nullptr) {
    _oracle->print_stats();
    // Finalize: free any remaining tracked objects and print final stats
    // Cast away const since finalize modifies state (but this is shutdown)
    const_cast<EpsilonOracle*>(_oracle)->finalize();
  }
}

void EpsilonHeap::print_heap_info(size_t used) const {
  size_t reserved  = max_capacity();
  size_t committed = capacity();

  if (reserved != 0) {
    log_info(gc)("Heap: %zu%s reserved, %zu%s (%.2f%%) committed, "
                 "%zu%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc)("Heap: no reliable data");
  }
}

void EpsilonHeap::print_metaspace_info() const {
  MetaspaceCombinedStats stats = MetaspaceUtils::get_combined_statistics();
  size_t reserved  = stats.reserved();
  size_t committed = stats.committed();
  size_t used      = stats.used();

  if (reserved != 0) {
    log_info(gc, metaspace)("Metaspace: %zu%s reserved, %zu%s (%.2f%%) committed, "
                            "%zu%s (%.2f%%) used",
            byte_size_in_proper_unit(reserved),  proper_unit_for_byte_size(reserved),
            byte_size_in_proper_unit(committed), proper_unit_for_byte_size(committed),
            committed * 100.0 / reserved,
            byte_size_in_proper_unit(used),      proper_unit_for_byte_size(used),
            used * 100.0 / reserved);
  } else {
    log_info(gc, metaspace)("Metaspace: no reliable data");
  }
}
