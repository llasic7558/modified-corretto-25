/*
 * Copyright (c) 2024, Oracle GC Study
 *
 * OracleGC Implementation - Uses malloc/free with oracle-guided deallocation.
 * Based on the methodology from "Quantifying the Performance of Garbage
 * Collection vs. Explicit Memory Management" (Hertz & Berger, OOPSLA 2005)
 */

#include "gc/oracle/oracleHeap.hpp"
#include "gc/epsilon/epsilonBarrierSet.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

#include <cstdlib>   // malloc, free
#include <cstring>   // memset
#include <fstream>
#include <sstream>

jint OracleHeap::initialize() {
  size_t align = HeapAlignment;
  size_t init_byte_size = align_up(InitialHeapSize, align);
  size_t max_byte_size  = align_up(MaxHeapSize, align);

  // Reserve address space for compatibility with is_in_reserved()
  // We don't actually use this for allocation - we use malloc() instead
  ReservedHeapSpace heap_rs = Universe::reserve_heap(max_byte_size, align);
  _virtual_space.initialize(heap_rs, init_byte_size);
  initialize_reserved_region(heap_rs);

  // Initialize malloc-based pointer tracking
  _live_ptrs = new std::unordered_set<void*>();
  _ptr_lock = new Mutex(Mutex::nosafepoint, "OracleGC_PtrSet");

  // Initialize oracle data structures
  _oracle_entries = new std::vector<OracleEntry>();
  _oracle_by_signature = new std::unordered_map<AllocationSignature, std::queue<OracleEntry*>, SignatureHash>();
  _live_allocations = new std::unordered_map<void*, LiveAllocation>();
  _scheduled_deaths = new std::unordered_map<size_t, std::vector<void*>>();

  // Load oracle file if specified
  if (OracleFile != nullptr && OracleFile[0] != '\0') {
    _oracle_loaded = load_oracle(OracleFile);
    if (_oracle_loaded) {
      log_info(gc)("OracleGC: Loaded oracle with %zu entries from %s",
                   _oracle_entries->size(), OracleFile);
      log_info(gc)("OracleGC: Built signature index with %zu unique signatures",
                   _oracle_by_signature->size());
    } else {
      log_warning(gc)("OracleGC: Failed to load oracle from %s, running without oracle",
                      OracleFile);
    }
  } else {
    log_info(gc)("OracleGC: No oracle file specified, running in tracking-only mode");
  }

  // Install barrier set (reuse Epsilon's since we don't need barriers)
  BarrierSet::set_barrier_set(new EpsilonBarrierSet());

  log_info(gc)("OracleGC initialized with malloc-based allocation");
  log_info(gc)("OracleGC: Reserved heap region: %zuM (not used for allocation)", max_byte_size / M);
  log_info(gc)("OracleGC: TLABs disabled: %s", UseTLAB ? "NO (WARNING!)" : "YES");
  log_info(gc)("OracleGC: Compressed oops disabled: %s", UseCompressedOops ? "NO (WARNING!)" : "YES");

  return JNI_OK;
}

bool OracleHeap::load_oracle(const char* filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return false;
  }

  std::string line;
  bool header = true;

  while (std::getline(file, line)) {
    if (header) {
      header = false;
      continue;  // Skip header row
    }

    // Parse CSV: alloc_seq,free_at_seq,size,type,obj_id
    std::istringstream iss(line);
    std::string token;

    OracleEntry entry;
    entry.matched = false;

    // alloc_seq
    if (!std::getline(iss, token, ',')) continue;
    entry.alloc_seq = std::stoull(token);

    // free_at_seq
    if (!std::getline(iss, token, ',')) continue;
    entry.free_at_seq = std::stoull(token);

    // size
    if (!std::getline(iss, token, ',')) continue;
    entry.size = std::stoull(token);

    // type
    if (!std::getline(iss, token, ',')) continue;
    entry.type = token;

    // obj_id (optional)
    if (std::getline(iss, token, ',')) {
      entry.obj_id = token;
    }

    // Add to entries vector
    _oracle_entries->push_back(entry);
  }

  // Build signature-based index
  // Each signature (size, type) maps to a queue of oracle entries with that signature
  // Entries are added in trace order, so first match = first in queue
  for (size_t i = 0; i < _oracle_entries->size(); i++) {
    OracleEntry* entry = &(*_oracle_entries)[i];
    AllocationSignature sig{entry->size, entry->type};
    (*_oracle_by_signature)[sig].push(entry);
  }

  return !_oracle_entries->empty();
}

OracleEntry* OracleHeap::match_oracle_entry(size_t size, const char* type_name) {
  if (!_oracle_loaded || _oracle_by_signature == nullptr) {
    return nullptr;
  }

  // Build signature for lookup
  std::string type_str = (type_name != nullptr) ? type_name : "";
  AllocationSignature sig{size, type_str};

  // Find matching queue
  auto it = _oracle_by_signature->find(sig);
  if (it == _oracle_by_signature->end() || it->second.empty()) {
    return nullptr;
  }

  // Get next entry from queue (FIFO order preserves trace sequence)
  OracleEntry* entry = it->second.front();
  it->second.pop();
  entry->matched = true;

  return entry;
}

void OracleHeap::schedule_death(void* ptr, size_t size, size_t death_seq) {
  // Add to scheduled deaths map
  (*_scheduled_deaths)[death_seq].push_back(ptr);

  // Track in live allocations map
  LiveAllocation alloc;
  alloc.ptr = ptr;
  alloc.size = size;
  alloc.death_seq = death_seq;
  (*_live_allocations)[ptr] = alloc;
}

void OracleHeap::process_scheduled_deaths(size_t current_clock) {
  // Check if any objects should die at this clock tick
  auto it = _scheduled_deaths->find(current_clock);
  if (it == _scheduled_deaths->end()) {
    return;
  }

  // Free all objects scheduled to die at this clock tick
  for (void* ptr : it->second) {
    // Look up allocation info
    auto alloc_it = _live_allocations->find(ptr);
    if (alloc_it == _live_allocations->end()) {
      continue;  // Already freed somehow
    }

    size_t freed_bytes = alloc_it->second.size;

    // Actually free the memory!
    ::free(ptr);

    // Remove from our tracking structures
    {
      MutexLocker ml(_ptr_lock, Mutex::_no_safepoint_check_flag);
      _live_ptrs->erase(ptr);
    }
    _live_allocations->erase(alloc_it);

    // Update statistics
    Atomic::add(&_total_freed, freed_bytes);
    Atomic::sub(&_live_bytes, freed_bytes);
    Atomic::inc(&_frees_performed);

    if (OracleVerbose) {
      log_trace(gc)("OracleGC: FREE at clock %zu, ptr %p, size %zu, live now %zu",
                    current_clock, ptr, freed_bytes, _live_bytes);
    }
  }

  // Remove this clock tick from scheduled deaths
  _scheduled_deaths->erase(it);
}

void OracleHeap::initialize_serviceability() {
  // Minimal serviceability
}

GrowableArray<GCMemoryManager*> OracleHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> managers(1);
  managers.append(&_memory_manager);
  return managers;
}

GrowableArray<MemoryPool*> OracleHeap::memory_pools() {
  GrowableArray<MemoryPool*> pools(0);
  return pools;
}

OracleHeap* OracleHeap::heap() {
  return named_heap<OracleHeap>(CollectedHeap::Oracle);
}

bool OracleHeap::is_in(const void* p) const {
  // Check if pointer is in our tracked malloc'd set
  if (_live_ptrs == nullptr || _ptr_lock == nullptr) {
    return false;
  }
  MutexLocker ml(_ptr_lock, Mutex::_no_safepoint_check_flag);
  return _live_ptrs->count(const_cast<void*>(p)) > 0;
}

size_t OracleHeap::unsafe_max_tlab_alloc(Thread* thr) const {
  // TLABs are disabled for OracleGC
  return 0;
}

size_t OracleHeap::max_tlab_size() const {
  return CollectedHeap::max_tlab_size();
}

HeapWord* OracleHeap::allocate_new_tlab(size_t min_size, size_t requested_size, size_t* actual_size) {
  // TLABs should be disabled, but handle gracefully
  Thread* thread = Thread::current();

  size_t size = MIN2(requested_size, max_tlab_size());
  size = MAX2(size, min_size);
  size = align_up(size, MinObjAlignment);

  HeapWord* res = allocate_work(size, nullptr);
  if (res != nullptr) {
    *actual_size = size;
  }
  return res;
}

HeapWord* OracleHeap::mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  return allocate_work(size, nullptr);
}

HeapWord* OracleHeap::mem_allocate_with_type(size_t size, const char* type_name, bool* gc_overhead_limit_was_exceeded) {
  *gc_overhead_limit_was_exceeded = false;
  return allocate_work(size, type_name);
}

HeapWord* OracleHeap::allocate_work(size_t size, const char* type_name) {
  assert(is_object_aligned(size), "Allocation size should be aligned: %zu", size);

  // Increment allocation clock
  size_t clock = Atomic::add(&_alloc_clock, (size_t)1);

  // FIRST: Process any pending frees scheduled for this allocation clock tick
  // This ensures frees happen at the right time relative to allocations
  process_scheduled_deaths(clock);

  size_t alloc_bytes = size * HeapWordSize;

  // Try to match this allocation to an oracle entry
  OracleEntry* entry = match_oracle_entry(alloc_bytes, type_name);

  if (entry != nullptr) {
    Atomic::inc(&_oracle_hits);

    if (OracleVerbose) {
      log_trace(gc)("OracleGC: MATCH clock %zu -> oracle seq %zu, type %s, size %zu, dies at %zu",
                    clock, entry->alloc_seq, entry->type.c_str(), alloc_bytes, entry->free_at_seq);
    }
  } else if (_oracle_loaded) {
    Atomic::inc(&_oracle_misses);

    if (OracleVerbose) {
      log_trace(gc)("OracleGC: MISS clock %zu, type %s, size %zu (no oracle match)",
                    clock, type_name ? type_name : "(unknown)", alloc_bytes);
    }
  }

  // Perform the actual allocation using malloc
  void* ptr = ::malloc(alloc_bytes);
  if (ptr == nullptr) {
    log_error(gc)("OracleGC: malloc failed for %zu bytes at clock %zu", alloc_bytes, clock);
    return nullptr;
  }

  // Zero memory (JVM expects clean memory for object initialization)
  memset(ptr, 0, alloc_bytes);

  // Track this allocation in our live pointer set
  {
    MutexLocker ml(_ptr_lock, Mutex::_no_safepoint_check_flag);
    _live_ptrs->insert(ptr);
  }

  // Update statistics
  Atomic::add(&_total_allocated, alloc_bytes);
  Atomic::add(&_live_bytes, alloc_bytes);

  // Track peak live bytes (working set)
  size_t current_live = _live_bytes;
  size_t peak = _peak_live_bytes;
  while (current_live > peak) {
    if (Atomic::cmpxchg(&_peak_live_bytes, peak, current_live) == peak) {
      break;
    }
    peak = _peak_live_bytes;
  }

  // Schedule future deallocation if oracle entry matched
  if (entry != nullptr) {
    // Calculate death time relative to current clock
    // Oracle entry has absolute sequence numbers, we need relative
    size_t lifetime = entry->free_at_seq - entry->alloc_seq;
    size_t death_clock = clock + lifetime;

    if (death_clock > clock) {
      schedule_death(ptr, alloc_bytes, death_clock);

      if (OracleVerbose) {
        log_trace(gc)("OracleGC: Scheduled death at clock %zu (lifetime %zu ticks)",
                      death_clock, lifetime);
      }
    } else {
      // Object dies immediately (same tick) - still allocate but don't schedule
      // It will be freed on next allocation's process_scheduled_deaths call
      schedule_death(ptr, alloc_bytes, clock + 1);
    }
  }

  return (HeapWord*)ptr;
}

void OracleHeap::collect(GCCause::Cause cause) {
  log_info(gc)("OracleGC: GC request for \"%s\" ignored (oracle-driven)",
               GCCause::to_string(cause));
}

void OracleHeap::do_full_collection(bool clear_all_soft_refs) {
  collect(gc_cause());
}

void OracleHeap::object_iterate(ObjectClosure* cl) {
  // With malloc-based allocation, we can't iterate objects linearly
  // Iterate over our tracked pointers
  if (_live_ptrs == nullptr) return;

  MutexLocker ml(_ptr_lock, Mutex::_no_safepoint_check_flag);
  for (void* ptr : *_live_ptrs) {
    cl->do_object((oop)ptr);
  }
}

void OracleHeap::print_heap_on(outputStream* st) const {
  st->print_cr("Oracle Heap (malloc-based with signature matching)");
  st->print_cr("  Allocation clock: %zu", _alloc_clock);
  st->print_cr("  Total allocated: %zu bytes (%.2f MB)",
               _total_allocated, (double)_total_allocated / M);
  st->print_cr("  Total freed: %zu bytes (%.2f MB)",
               _total_freed, (double)_total_freed / M);
  st->print_cr("  Currently live: %zu bytes (%.2f MB)",
               _live_bytes, (double)_live_bytes / M);
  st->print_cr("  Peak live (working set): %zu bytes (%.2f MB)",
               _peak_live_bytes, (double)_peak_live_bytes / M);
  st->print_cr("  Live objects: %zu", _live_ptrs != nullptr ? _live_ptrs->size() : 0);
  st->print_cr("  Frees performed: %zu", _frees_performed);
  st->print_cr("  Oracle hits: %zu (%.1f%%)",
               _oracle_hits,
               _alloc_clock > 0 ? 100.0 * _oracle_hits / _alloc_clock : 0.0);
  st->print_cr("  Oracle misses: %zu", _oracle_misses);
  if (_oracle_entries != nullptr) {
    st->print_cr("  Oracle entries: %zu", _oracle_entries->size());
  }
  if (_scheduled_deaths != nullptr) {
    size_t pending = 0;
    for (const auto& pair : *_scheduled_deaths) {
      pending += pair.second.size();
    }
    st->print_cr("  Pending deaths: %zu", pending);
  }
}

void OracleHeap::print_tracing_info() const {
  log_info(gc)("=== OracleGC Statistics ===");
  log_info(gc)("Allocation clock: %zu", _alloc_clock);
  log_info(gc)("Total allocated: %zu bytes (%.2f MB)",
               _total_allocated, (double)_total_allocated / M);
  log_info(gc)("Total freed: %zu bytes (%.2f MB)",
               _total_freed, (double)_total_freed / M);
  log_info(gc)("Final live: %zu bytes (%.2f MB)",
               _live_bytes, (double)_live_bytes / M);
  log_info(gc)("Peak live (working set): %zu bytes (%.2f MB)",
               _peak_live_bytes, (double)_peak_live_bytes / M);
  log_info(gc)("Frees performed: %zu", _frees_performed);
  log_info(gc)("Oracle hits: %zu (%.1f%%)",
               _oracle_hits,
               _alloc_clock > 0 ? 100.0 * _oracle_hits / _alloc_clock : 0.0);
  log_info(gc)("Oracle misses: %zu", _oracle_misses);
}

bool OracleHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<OracleHeap>::print_location(st, addr);
}
