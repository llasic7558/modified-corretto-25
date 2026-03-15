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
  int _app_code_depth;             // For oracle mode: depth counter set by InterpreterRuntime
  char _alloc_method[128];         // For oracle mode: "class#method" of the allocating method
  char _alloc_site[256];           // For oracle mode: "class:method:N@CallerClass:callerMethod" deepened site key

  EpsilonThreadLocalData() :
          _ergo_tlab_size(0),
          _last_tlab_time(0),
          _current_alloc_klass(nullptr),
          _app_code_depth(0) {
    _alloc_method[0] = '\0';
    _alloc_site[0] = '\0';
  }

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

  // Oracle mode: app code depth counter
  // Set to 1 by InterpreterRuntime before app allocations, reset to 0 after.
  // When depth > 0, the allocation is from application code and should be tracked.
  static int app_code_depth(Thread* thread) {
    return data(thread)->_app_code_depth;
  }

  static void set_app_code_depth(Thread* thread, int depth) {
    data(thread)->_app_code_depth = depth;
  }

  // Check if currently in application code (depth > 0)
  static bool in_app_code(Thread* thread) {
    return data(thread)->_app_code_depth > 0;
  }

  // Oracle mode: allocating method name (class.name#method format, matching ET methods.list)
  static const char* alloc_method(Thread* thread) {
    return data(thread)->_alloc_method;
  }

  static void set_alloc_method(Thread* thread, const char* class_name, const char* method_name) {
    char* buf = data(thread)->_alloc_method;
    if (class_name != nullptr && method_name != nullptr) {
      // class_name from external_name() uses dots (e.g., "org.apache.lucene.index.SegmentReader")
      snprintf(buf, sizeof(data(thread)->_alloc_method), "%s#%s", class_name, method_name);
    } else {
      buf[0] = '\0';
    }
  }

  static void clear_alloc_method(Thread* thread) {
    data(thread)->_alloc_method[0] = '\0';
  }

  // Oracle mode: allocation site key ("class:method:N" format for site-keyed matching)
  static const char* alloc_site(Thread* thread) {
    return data(thread)->_alloc_site;
  }

  static void set_alloc_site(Thread* thread, const char* site_key) {
    if (site_key != nullptr) {
      strncpy(data(thread)->_alloc_site, site_key, sizeof(data(thread)->_alloc_site) - 1);
      data(thread)->_alloc_site[sizeof(data(thread)->_alloc_site) - 1] = '\0';
    } else {
      data(thread)->_alloc_site[0] = '\0';
    }
  }

  static void clear_alloc_site(Thread* thread) {
    data(thread)->_alloc_site[0] = '\0';
  }
};

#endif // SHARE_GC_EPSILON_EPSILONTHREADLOCALDATA_HPP
