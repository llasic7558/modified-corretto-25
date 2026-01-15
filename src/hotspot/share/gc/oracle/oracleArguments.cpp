/*
 * Copyright (c) 2024, Oracle GC Study
 * 
 * OracleGC Arguments Implementation
 */

#include "gc/oracle/oracleArguments.hpp"
#include "gc/oracle/oracleHeap.hpp"
#include "gc/shared/gcArguments.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/os.hpp"

size_t OracleArguments::conservative_max_heap_alignment() {
  return UseLargePages ? os::large_page_size() : os::vm_page_size();
}

void OracleArguments::initialize() {
  GCArguments::initialize();

  assert(UseOracleGC, "Sanity");

  // CRITICAL: Disable TLABs to ensure ALL allocations go through mem_allocate()
  // Without this, 99% of allocations bypass our tracking via TLAB fast path
  if (FLAG_IS_DEFAULT(UseTLAB)) {
    FLAG_SET_DEFAULT(UseTLAB, false);
  }

  // Disable compressed oops - malloc returns addresses outside heap region
  // Compressed oops assume objects are within a specific address range
  if (FLAG_IS_DEFAULT(UseCompressedOops)) {
    FLAG_SET_DEFAULT(UseCompressedOops, false);
  }
  if (FLAG_IS_DEFAULT(UseCompressedClassPointers)) {
    FLAG_SET_DEFAULT(UseCompressedClassPointers, false);
  }

  // Forcefully exit when OOME is detected. Nothing we can do at that point.
  if (FLAG_IS_DEFAULT(ExitOnOutOfMemoryError)) {
    FLAG_SET_DEFAULT(ExitOnOutOfMemoryError, true);
  }
}

void OracleArguments::initialize_alignments() {
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  size_t align = MAX2(os::vm_allocation_granularity(), page_size);
  SpaceAlignment = align;
  HeapAlignment  = align;
}

CollectedHeap* OracleArguments::create_heap() {
  return new OracleHeap();
}
