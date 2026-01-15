/*
 * Copyright (c) 2024, Oracle GC Study
 *
 * OracleGC - A garbage collector that uses oracle-guided explicit memory management.
 * Based on EpsilonGC but uses malloc/free with oracle-driven deallocation timing.
 *
 * This implements the methodology from:
 * "Quantifying the Performance of Garbage Collection vs. Explicit Memory Management"
 * Hertz & Berger, OOPSLA 2005
 */

#ifndef SHARE_GC_ORACLE_ORACLEHEAP_HPP
#define SHARE_GC_ORACLE_ORACLEHEAP_HPP

#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/softRefPolicy.hpp"
#include "services/memoryManager.hpp"
#include "memory/allocation.hpp"
#include "memory/virtualspace.hpp"
#include "runtime/mutex.hpp"
#include "utilities/resourceHash.hpp"

// Oracle entry loaded from CSV - stored in C heap array
struct OracleEntry {
  size_t alloc_seq;      // Original allocation sequence from trace
  size_t free_at_seq;    // When to free (allocation clock tick)
  size_t size;           // Object size in bytes
};

// Tracks a live allocation with scheduled death time
struct LiveAllocation {
  void* ptr;             // Actual pointer from malloc
  size_t size;           // Size in bytes
  size_t death_seq;      // Allocation clock tick when this should be freed
  LiveAllocation* next;  // For hash table chaining
};

// Pending free entry - linked list node
struct PendingFree {
  void* ptr;
  size_t size;
  size_t free_at_seq;
  PendingFree* next;
};

// Simple hash table for tracking live pointers
class LivePointerSet : public CHeapObj<mtGC> {
private:
  static const size_t BUCKETS = 16381;  // Prime number for good distribution
  void** _buckets;
  size_t _count;
  Mutex* _lock;

public:
  LivePointerSet();
  ~LivePointerSet();

  void insert(void* ptr);
  void remove(void* ptr);
  bool contains(void* ptr) const;
  size_t count() const { return _count; }

  // Iterator support for object_iterate
  template<typename Func>
  void for_each(Func f) const;
};

// Hash table bucket count for pending frees (must be power of 2 for fast modulo)
static const size_t PENDING_FREE_BUCKETS = 4096;

class OracleHeap : public CollectedHeap {
  friend class VMStructs;

private:
  GCMemoryManager _memory_manager;
  MemoryPool* _pool;

  // Malloc-based allocation tracking
  LivePointerSet* _live_ptrs;

  // Reserved region for compatibility
  VirtualSpace _virtual_space;

  // Allocation clock - increments on each allocation
  volatile size_t _alloc_clock;

  // Statistics
  volatile size_t _total_allocated;
  volatile size_t _total_freed;
  volatile size_t _live_bytes;
  volatile size_t _peak_live_bytes;
  volatile size_t _oracle_hits;
  volatile size_t _oracle_misses;
  volatile size_t _frees_performed;

  // Oracle data structures
  bool _oracle_loaded;
  size_t _oracle_size;
  size_t _oracle_capacity;

  // Oracle entries indexed by alloc_seq (direct array lookup)
  OracleEntry* _oracle_entries;

  // Pending frees hash table: free_at_seq -> linked list of PendingFree
  PendingFree** _pending_frees;

  // Internal methods
  HeapWord* allocate_work(size_t size);
  void process_pending_frees(size_t current_clock);
  bool load_oracle(const char* filename);
  OracleEntry* lookup_oracle(size_t alloc_seq);
  void add_pending_free(size_t free_at_seq, void* ptr, size_t size);

public:
  static OracleHeap* heap();

  OracleHeap() :
    _memory_manager("Oracle Heap"),
    _pool(nullptr),
    _live_ptrs(nullptr),
    _alloc_clock(0),
    _total_allocated(0),
    _total_freed(0),
    _live_bytes(0),
    _peak_live_bytes(0),
    _oracle_hits(0),
    _oracle_misses(0),
    _frees_performed(0),
    _oracle_loaded(false),
    _oracle_size(0),
    _oracle_capacity(0),
    _oracle_entries(nullptr),
    _pending_frees(nullptr) {}

  Name kind() const override { return CollectedHeap::Oracle; }
  const char* name() const override { return "Oracle"; }

  jint initialize() override;
  void initialize_serviceability() override;

  GrowableArray<GCMemoryManager*> memory_managers() override;
  GrowableArray<MemoryPool*> memory_pools() override;

  size_t max_capacity() const override { return _virtual_space.reserved_size(); }
  size_t capacity() const override { return _total_allocated; }
  size_t used() const override { return _live_bytes; }

  bool is_in(const void* p) const override;
  bool requires_barriers(stackChunkOop obj) const override { return false; }

  HeapWord* mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) override;

  HeapWord* allocate_new_tlab(size_t min_size, size_t requested_size, size_t* actual_size) override;
  size_t unsafe_max_tlab_alloc(Thread* thr) const override;
  size_t max_tlab_size() const override;
  size_t tlab_capacity(Thread* thr) const override { return max_capacity(); }
  size_t tlab_used(Thread* thr) const override { return _live_bytes; }

  void collect(GCCause::Cause cause) override;
  void do_full_collection(bool clear_all_soft_refs) override;

  void object_iterate(ObjectClosure* cl) override;

  // Required overrides
  void prepare_for_verify() override {}
  void verify(VerifyOption option) override {}
  void print_gc_on(outputStream* st) const override {}
  void gc_threads_do(ThreadClosure* tc) const override {}
  void register_nmethod(nmethod* nm) override {}
  void unregister_nmethod(nmethod* nm) override {}
  void verify_nmethod(nmethod* nm) override {}
  void pin_object(JavaThread* thread, oop obj) override {}
  void unpin_object(JavaThread* thread, oop obj) override {}

  HeapWord* block_start(const void* addr) const { return nullptr; }
  bool block_is_obj(const HeapWord* addr) const { return false; }

  MemRegion reserved_region() const { return _reserved; }
  bool is_in_reserved(const void* addr) const { return _reserved.contains(addr); }

  void print_heap_on(outputStream* st) const override;
  void print_tracing_info() const override;
  bool print_location(outputStream* st, void* addr) const override;

  // Statistics accessors
  size_t alloc_clock() const { return _alloc_clock; }
  size_t total_allocated() const { return _total_allocated; }
  size_t total_freed() const { return _total_freed; }
  size_t live_bytes() const { return _live_bytes; }
  size_t peak_live_bytes() const { return _peak_live_bytes; }
  size_t oracle_hits() const { return _oracle_hits; }
  size_t oracle_misses() const { return _oracle_misses; }
  size_t frees_performed() const { return _frees_performed; }
};

#endif // SHARE_GC_ORACLE_ORACLEHEAP_HPP
