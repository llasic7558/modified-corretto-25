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
#include "runtime/os.hpp"

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

  // Oracle mode initialization
  if (EpsilonOracleMode) {
    // Oracle mode requires TLABs to be disabled so we can track all allocations
    if (UseTLAB) {
      log_error(gc)("EpsilonOracleMode requires -XX:-UseTLAB to track all allocations");
      return JNI_ERR;
    }

    _oracle = new EpsilonOracle();
    if (!_oracle->load_trace(EpsilonOracleTracePath)) {
      log_error(gc)("Failed to load oracle trace from: %s", EpsilonOracleTracePath);
      return JNI_ERR;
    }
    log_info(gc)("Oracle mode: Loaded %zu entries, using malloc/free for heap allocations",
                 _oracle->entry_count());
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

HeapWord* EpsilonHeap::allocate_work_oracle(size_t size, bool verbose) {
  assert(EpsilonOracleMode, "Should only be called in oracle mode");
  assert(_oracle != nullptr, "Oracle should be initialized");

  // Get next global allocation sequence number (for logging)
  uint64_t alloc_seq = _oracle->next_alloc_seq();

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

  // Check if application has started (signaled by JVMTI agent when main() is entered)
  if (!_oracle->app_started()) {
    // Application hasn't started yet - use standard bump-pointer allocation
    HeapWord* mem = bump_allocate();
    if (mem != nullptr && verbose) {
      log_trace(gc)("Pre-app alloc #" UINT64_FORMAT ": size=%zu (waiting for main())",
                    alloc_seq, size_in_bytes);
    }
    return mem;
  }

  // Get the Klass being allocated (set by MemAllocator before calling heap allocation)
  Thread* thread = Thread::current();
  Klass* klass = EpsilonThreadLocalData::current_alloc_klass(thread);
  const char* type_name = nullptr;

  // Need ResourceMark because external_name() allocates in resource area
  ResourceMark rm;
  if (klass != nullptr) {
    type_name = klass->external_name();
  }

  // Application has started - check if this allocation matches expected trace entry
  if (!_oracle->matches_expected_entry(size_in_bytes, type_name)) {
    // Type/size mismatch - this is a JVM internal allocation (Strings, char[], etc.)
    // Use bump-pointer allocation WITHOUT tracking
    HeapWord* mem = bump_allocate();
    if (mem != nullptr && verbose) {
      log_trace(gc)("Untracked alloc #" UINT64_FORMAT ": type=[%s] size=%zu (JVM internal after main)",
                    alloc_seq, type_name ? type_name : "unknown", size_in_bytes);
    }
    return mem;
  }

  // Size matches expected trace entry - this is an application allocation!
  // Get the trace sequence number (1-based)
  uint64_t trace_seq = _oracle->next_app_alloc_seq();

  // Process any deaths that should happen at this trace sequence number
  // This adds freed objects to the free list for reuse
  _oracle->process_deaths(trace_seq);

  // First, try to allocate from free list (first-fit)
  HeapWord* mem = _oracle->allocate_from_free_list(size);

  if (mem == nullptr) {
    // No suitable free block found - use bump-pointer allocation from heap
    mem = bump_allocate();

    if (mem == nullptr) {
      log_error(gc)("Oracle allocation failed: trace_seq=" UINT64_FORMAT " size=%zu bytes (heap exhausted)",
                    trace_seq, size_in_bytes);
      return nullptr;
    }
  }

  // Zero the memory - ALWAYS required for both free list and bump-pointer allocations
  // Free list memory may contain stale object data that must be cleared
  memset(mem, 0, size_in_bytes);

  // Register this allocation with the oracle for future deallocation
  _oracle->register_allocation(trace_seq, mem, size_in_bytes);

  // Track allocated bytes
  Atomic::add(&_oracle_allocated_bytes, size_in_bytes);

  if (verbose) {
    log_info(gc)("Oracle TRACKED alloc: trace_seq=" UINT64_FORMAT " ptr=" PTR_FORMAT " size=%zu bytes",
                  trace_seq, p2i(mem), size_in_bytes);
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
