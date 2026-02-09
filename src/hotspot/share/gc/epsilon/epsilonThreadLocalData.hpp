/*
 * Copyright (c) 2018, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_EPSILON_EPSILONTHREADLOCALDATA_HPP
#define SHARE_GC_EPSILON_EPSILONTHREADLOCALDATA_HPP

#include "gc/shared/gc_globals.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"

class Klass;  // Forward declaration

class EpsilonThreadLocalData {
private:
  size_t _ergo_tlab_size;
  int64_t _last_tlab_time;
  Klass* _current_alloc_klass;  // For oracle mode: Klass being allocated
  bool _app_allocation_pending;    // For oracle mode: legacy boolean API
  int _app_code_depth;             // For oracle mode: depth counter (preferred API)
  bool _tracking_suppressed;       // For oracle mode: suppress tracking during agent overhead

  EpsilonThreadLocalData() :
          _ergo_tlab_size(0),
          _last_tlab_time(0),
          _current_alloc_klass(nullptr),
          _app_allocation_pending(false),
          _app_code_depth(0),
          _tracking_suppressed(false) {}

  static EpsilonThreadLocalData* data(Thread* thread) {
    assert(UseEpsilonGC, "Sanity");
    return thread->gc_data<EpsilonThreadLocalData>();
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) EpsilonThreadLocalData();
  }

  static void destroy(Thread* thread) {
    data(thread)->~EpsilonThreadLocalData();
  }

  static size_t ergo_tlab_size(Thread *thread) {
    return data(thread)->_ergo_tlab_size;
  }

  static int64_t last_tlab_time(Thread *thread) {
    return data(thread)->_last_tlab_time;
  }

  static void set_ergo_tlab_size(Thread *thread, size_t val) {
    data(thread)->_ergo_tlab_size = val;
  }

  static void set_last_tlab_time(Thread *thread, int64_t time) {
    data(thread)->_last_tlab_time = time;
  }

  // Oracle mode: current allocation Klass tracking
  static Klass* current_alloc_klass(Thread* thread) {
    return data(thread)->_current_alloc_klass;
  }

  static void set_current_alloc_klass(Thread* thread, Klass* klass) {
    data(thread)->_current_alloc_klass = klass;
  }

  // Oracle mode: application allocation pending flag (legacy boolean API)
  static bool app_allocation_pending(Thread* thread) {
    return data(thread)->_app_allocation_pending;
  }

  static void set_app_allocation_pending(Thread* thread, bool pending) {
    data(thread)->_app_allocation_pending = pending;
  }

  // Oracle mode: app code depth counter (preferred API)
  // When depth > 0, allocations are from application code
  static int app_code_depth(Thread* thread) {
    return data(thread)->_app_code_depth;
  }

  static void set_app_code_depth(Thread* thread, int depth) {
    data(thread)->_app_code_depth = depth;
    // Keep legacy boolean in sync
    data(thread)->_app_allocation_pending = (depth > 0);
  }

  // Oracle mode: suppress tracking during agent overhead (class transformation)
  static bool tracking_suppressed(Thread* thread) {
    return data(thread)->_tracking_suppressed;
  }

  static void set_tracking_suppressed(Thread* thread, bool suppressed) {
    data(thread)->_tracking_suppressed = suppressed;
  }

  // Check if currently in application code and tracking is not suppressed
  static bool in_app_code(Thread* thread) {
    if (data(thread)->_tracking_suppressed) return false;
    return data(thread)->_app_code_depth > 0 || data(thread)->_app_allocation_pending;
  }
};

#endif // SHARE_GC_EPSILON_EPSILONTHREADLOCALDATA_HPP
