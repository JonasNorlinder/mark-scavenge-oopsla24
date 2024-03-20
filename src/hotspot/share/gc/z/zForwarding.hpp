/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
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
 */

#ifndef SHARE_GC_Z_ZFORWARDING_HPP
#define SHARE_GC_Z_ZFORWARDING_HPP

#include "gc/z/zArray.hpp"
#include "gc/z/zAttachedArray.hpp"
#include "gc/z/zForwardingEntry.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zLiveMap.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zVirtualMemory.hpp"
#include "runtime/atomic.hpp"

class ObjectClosure;
class ZForwardingAllocator;
class ZPage;
class ZRelocateQueue;
class ZGenerationYoung;

typedef size_t ZForwardingCursor;

class ZForwarding {
  friend class VMStructs;
  friend class ZForwardingTest;
  friend class ZGenerationYoung;

  enum class ZPublishState : int8_t {
    none,      // No publishing done yet
    published, // OC published remset field info, which YC will reject or accept
    reject,    // YC remset scanning accepted OC published remset field info
    accept     // YC remset scanning rejected OC published remset field info
  };

private:
  typedef ZAttachedArray<ZForwarding, ZForwardingEntry> AttachedArray;
  typedef ZArray<volatile zpointer*> PointerArray;

  const ZVirtualMemory   _virtual;
  const size_t           _object_alignment_shift;
  const AttachedArray    _entries;
  ZPage* const           _page;
  ZPageAge               _from_age;
  ZPageAge               _to_age;
  volatile bool          _claimed;
  volatile bool          _claimed2;
  mutable ZConditionLock _ref_lock;
  volatile int32_t       _ref_count;
  volatile bool          _done;
  volatile bool          _evacuated;

  // Relocated remembered set fields support
  volatile ZPublishState _relocated_remembered_fields_state;
  PointerArray           _relocated_remembered_fields_array;
  uint32_t               _relocated_remembered_fields_publish_young_seqnum;

  // In-place relocation support
  bool                   _in_place;
  zoffset_end            _in_place_top_at_start;

  // Debugging
  volatile Thread*       _in_place_thread;

  // Deferred
  const bool             _is_deferrable;
  ZLiveMap               _zlivemap;
  const size_t           _live_bytes;
  size_t                 _evacuated_bytes;
  volatile bool          _in_placed;
  bool                   _livemap_copied;

  ZForwardingEntry* entries() const;
  ZForwardingEntry at(ZForwardingCursor* cursor) const;
  ZForwardingEntry first(uintptr_t from_index, ZForwardingCursor* cursor) const;
  ZForwardingEntry next(ZForwardingCursor* cursor) const;

  template <typename Function>
  void object_iterate_forwarded_via_livemap(Function function);

  ZForwarding(ZPage* page, ZPageAge to_age, size_t nentries);

  void livemap_set(zaddress addr);

public:
  static uint32_t nentries(const ZPage* page);
  static ZForwarding* alloc(ZForwardingAllocator* allocator, ZPage* page, ZPageAge to_age);

  bool is_deferrable() const;
  void copy_livemap();
  bool livemap_copied() const;

  ZPageType type() const;
  ZPageAge from_age() const;
  ZPageAge to_age() const;
  zoffset start() const;
  zoffset_end end() const;
  size_t size() const;
  size_t object_alignment_shift() const;
  ZLiveMap* livemap_copy();

  bool is_promotion() const;

  template <typename Function>
  void object_iterate_via_livemap(Function function);

  // Visit from-objects
  template <typename Function>
  void object_iterate(Function function);

  template <typename Function>
  void address_unsafe_iterate_via_table(Function function);

  // Visit to-objects
  template <typename Function>
  void object_iterate_forwarded(Function function);

  template <typename Function>
  void object_iterate_forwarded_via_table(Function function);

  template <typename Function>
  void oops_do_in_forwarded(Function function);

  template <typename Function>
  void oops_do_in_forwarded_via_table(Function function);

  bool claim();
  bool claim2();
  bool is_claim2();
  bool unclaim2();
  bool is_in_place() const {
    return Atomic::load(&_in_placed);
  }
  void mark_in_place() {
    Atomic::store(&_in_placed, true);
  }

  // In-place relocation support
  bool in_place_relocation() const;
  bool in_place_relocation_claim_page(bool return_if_evacuated=false);
  void in_place_relocation_start(zoffset relocated_watermark);
  void in_place_relocation_finish();
  bool in_place_relocation_is_below_top_at_start(zoffset addr) const;
  bool try_in_place_relocation_claim_page();
  bool try_retain_page();
  bool try_fast_zero_rc(int32_t initial_rc);

  bool retain_page(ZRelocateQueue* queue, bool deferred, bool fail_fast=false);
  bool retain_page(bool deferred);
  void release_page();

  ZPage* detach_page();
  ZPage* page();
  int32_t ref_count() const;
  void wait_until_done() const;

  void mark_done(bool notify=true);
  bool is_done() const;
  bool inc_evacuated_bytes(size_t bytes);
  size_t evacuated_bytes() const {
    return _evacuated_bytes;
  }
  size_t deferred_bytes() const {
    return _live_bytes - _evacuated_bytes;
  }
  size_t live_bytes() const {
    return _live_bytes;
  }

  void mark_evacuated();
  bool is_evacuated() const;

  zaddress find(zaddress_unsafe addr);

  ZForwardingEntry find(uintptr_t from_index, ZForwardingCursor* cursor) const;
  zoffset insert(uintptr_t from_index, zoffset to_offset, ZForwardingCursor* cursor);

  // Relocated remembered set fields support
  void relocated_remembered_fields_register(volatile zpointer* p);
  void relocated_remembered_fields_after_relocate();
  void relocated_remembered_fields_publish();
  void relocated_remembered_fields_notify_concurrent_scan_of();
  bool relocated_remembered_fields_is_concurrently_scanned() const;
  template <typename Function>
  void relocated_remembered_fields_apply_to_published(Function function);
  bool relocated_remembered_fields_published_contains(volatile zpointer* p);

  void verify() const;
};

#endif // SHARE_GC_Z_ZFORWARDING_HPP
