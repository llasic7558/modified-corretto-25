/*
 * Copyright (c) 2024, Oracle GC Study
 * 
 * OracleGC Arguments - Command line options for OracleGC
 */

#ifndef SHARE_GC_ORACLE_ORACLEARGUMENTS_HPP
#define SHARE_GC_ORACLE_ORACLEARGUMENTS_HPP

#include "gc/shared/gcArguments.hpp"

class CollectedHeap;

class OracleArguments : public GCArguments {
private:
  virtual void initialize_alignments();

public:
  virtual void initialize();
  virtual size_t conservative_max_heap_alignment();
  virtual CollectedHeap* create_heap();
};

#endif // SHARE_GC_ORACLE_ORACLEARGUMENTS_HPP
