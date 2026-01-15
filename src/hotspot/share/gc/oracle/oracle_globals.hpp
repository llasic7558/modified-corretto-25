/*
 * Copyright (c) 2024, Oracle GC Study
 * 
 * OracleGC Global Flags
 */

#ifndef SHARE_GC_ORACLE_ORACLE_GLOBALS_HPP
#define SHARE_GC_ORACLE_ORACLE_GLOBALS_HPP

#define GC_ORACLE_FLAGS(develop,                                              \
                        develop_pd,                                           \
                        product,                                              \
                        product_pd,                                           \
                        range,                                                \
                        constraint)                                           \
                                                                              \
  product(ccstr, OracleFile, nullptr, EXPERIMENTAL,                           \
          "Path to oracle file (CSV: alloc_seq,free_at_seq,size)")            \
                                                                              \
  product(bool, OraclePrintStats, true, EXPERIMENTAL,                         \
          "Print OracleGC statistics at shutdown")                            \
                                                                              \
  product(bool, OracleVerbose, false, EXPERIMENTAL,                           \
          "Enable verbose OracleGC logging")

#endif // SHARE_GC_ORACLE_ORACLE_GLOBALS_HPP
