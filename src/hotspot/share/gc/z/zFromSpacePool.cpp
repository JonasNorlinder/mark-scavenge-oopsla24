#include "gc/z/zFromSpacePool.inline.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zForwarding.hpp"
#include "gc/z/zGeneration.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zPage.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zRelocate.hpp"
#include "gc/z/zThread.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

static bool in_phase_mc() {
  static ZGenerationYoung* const young = ZGeneration::young();
  return young->is_phase_mark_complete();
}

ZFromSpacePool* ZFromSpacePool::_pool = nullptr;

ZFromSpacePool::ZFromSpacePool() :
  _fsp(1024),
  _fsp_pages(0),
  _fsp_start(0),
  _in_placed_pages(),
  _target(),
  _target_guard(),
  _per_cpu_free_list(),
  _shared_free_list(),
  _size_in_bytes(0),
  _evacuated_bytes(0),
  _in_placed_bytes(0),
  _deferrable_bytes(0),
  _evacuated_page_count(0),
  _in_placed_page_count(0),
  _stat_evacuated_pages(),
  _stat_fsp_percent_evacuated() {
    _pool = this;
  };

// DEBUG -- should be removed eventually

void ZFromSpacePool::compact_in_place(ZForwarding* f) {
  if (claim_and_remove_specific(f)) {
    assert(f->ref_count() == -1, "Bug!");

    if (f->is_evacuated()) {
      f->release_page();
      free_page(f, nullptr);

    } else {
      size_t bytes_in_placed = ZRelocate::compact_in_place(f);
      inc_in_placed_page_count_and_bytes(bytes_in_placed, f);
      f->release_page();
      f->mark_in_place();
      f->mark_done();
      assert(f->ref_count() == 0, "");
      append_to_in_placed_pages(f->page(), f->to_age());
    }
  } else {
    // Someone else won the race to compact and we
    // were blocked until they were done!
  }
}

bool ZFromSpacePool::try_free_if_evacuated_else_release(ZForwarding* f, size_t from_rc, ZPage** result) {
  if (f->try_fast_zero_rc(from_rc /* f's expected RC */)) {
    auto res = f->claim();
    assert(res, "");
    free_page(f, result);
    return true;
  } else {
    // We must assert that f's RC >= from_rc
    if (from_rc > 1) {
      f->release_page();
      try_free_if_evacuated_else_release(f, from_rc - 1, result);
    }
    return false;
  }
}

bool ZFromSpacePool::alloc_page_from_cache(ZPage** result) {
  ZPage* p = nullptr;
  auto cpu_id = ZCPU::id();
  // TODO Use lock-free stack if this is not performant?
  for (size_t i = 0; i < ZCPU::count(); ++i) {
    ZFreeList& pages = _per_cpu_free_list.get((i + cpu_id) % ZCPU::count());
    ZLocker<ZLock> guard(&pages._guard);
    p = pages._list.remove_first();
    if (p != nullptr) break;
  }

  if (p == nullptr && in_phase_mc()) {
    ZLocker<ZLock> guard(&_shared_free_list._guard);
    p = _shared_free_list._list.remove_first();
  }

  if (p) {
    // TODO should we
    // Atomic::sub(&_size_in_bytes, f->size() - f->live_bytes(), memory_order_relaxed);?
    if (result) {
      *result = p;
    } else {
      ZHeap::heap()->free_page(p);
    }
    return true;
  }
  return false;
}

void ZFromSpacePool::reset_start() {
  ZLocker<ZLock> guard(&_shared_free_list._guard);

  // Sum of bytes of all pages which haven't yet been evacuated
  size_t evacuated_bytes = 0;
  ZArrayIterator<ZForwarding*> iter(&_fsp);
  for (ZForwarding* f; iter.next(&f);) {
    if (f->claim2() && f->claim()) {
      f->mark_done();
      evacuated_bytes += f->evacuated_bytes();
      // These can be any age now
      ZPage* p = f->page();
      p->mark_as_fsp_current_cycle();
      guarantee(!p->in_any_pool(), "");
      _shared_free_list._list.insert_last(p);
    }
  }
  _evacuated_bytes += evacuated_bytes;
}

static void empty_free_list(ZFreeList& free_list) {
  ZList<ZPage>* list = &free_list._list;
  ZLock* g = &free_list._guard;

  g->lock();
  ZArray<ZPage*> empty_pages(0);
  while (ZPage* p = list->remove_first()) {
    g->unlock();
    empty_pages.append(p);

    if (empty_pages.length() == 64) {
      ZHeap::heap()->free_empty_pages(&empty_pages);
      empty_pages.clear();
    }

    if (!g->try_lock()) {
      ZHeap::heap()->free_empty_pages(&empty_pages);
      empty_pages.clear();
      g->lock();
    }
  }
  g->unlock();
  if (empty_pages.length() > 0) {
    ZHeap::heap()->free_empty_pages(&empty_pages);
  }
}

// Assumes that _target_guard is held
void ZFromSpacePool::reset_target() {
  for (uint i = 0; i < ZPageAgeMax; i++) {
    // ZPage* p = const_cast<ZPage*>(_target[i]);
    if (_target[i]) {
      ZAllocationFlags flags;
      flags.set_non_blocking();
      flags.set_alloc_with_old_seqnum();
      flags.set_gc_relocation();
      _target[i] = ZHeap::heap()->alloc_page(ZPageType::small, ZPageSizeSmall, flags, static_cast<ZPageAge>(i));
    }
  }
}

size_t ZFromSpacePool::reset_end() {
  if (_fsp.length()) _stat_to_be_freed_in_bytes.add(_fsp.length() * ZPageSizeSmall - _deferrable_bytes);

  const size_t deferrable_bytes = _deferrable_bytes;
  const size_t evacuated_bytes = _evacuated_bytes;
  const size_t in_placed_bytes = _in_placed_bytes;
  const size_t deferred_bytes = deferrable_bytes - (evacuated_bytes + in_placed_bytes);

  {
    // Clear all in-placed pages
    for (uint i = 0; i < ZPageAgeMax; i++) {
      while (ZPage* const p = _in_placed_pages[i].remove_first()) {}
    }
  }

  {
    // Reset all target pages
    ZLocker<ZLock> guard(&_target_guard);
    reset_target();
  }

  {
    // Remove all pages from free list
    empty_free_list(_shared_free_list);
    for (size_t i = 0; i < ZCPU::count(); ++i) {
      ZFreeList& cpu_free_list = _per_cpu_free_list.get(i);
      empty_free_list(cpu_free_list);
    }
  }

  if (_deferrable_bytes && _fsp_pages) _stat_fsp_percent_evacuated.add((1.0 * evacuated_bytes + _in_placed_bytes) / (_fsp_pages * ZPageSizeSmall));

  log_info(gc)("FSP:Deferrable bytes:  %zd", deferrable_bytes);
  log_info(gc)("FSP:Deferred bytes:    %zd", deferred_bytes);
  log_info(gc)("FSP:Evacuated bytes:   %zd", evacuated_bytes);

  // Reset counters etc for next GC cycle
  _fsp_pages = 0;
  _fsp_start = 0;
  _size_in_bytes = 0;
  _evacuated_bytes = 0;
  _in_placed_bytes = 0;
  _deferrable_bytes = 0;
  _evacuated_page_count = 0;
  _in_placed_page_count = 0;
  _fsp.clear();

  return deferred_bytes;
}

ZFromSpacePool::~ZFromSpacePool() {
  ZLocker<ZLock> guard(&_in_placed_guard);
  for (uint i = 0; i < ZPageAgeMax; i++) {
    while (_in_placed_pages[i].remove_first()) {}
  }
}

// Take a page from FSP, compact it in-place and install it as the new target.
// Existing in-placed pages are used first
ZPage* ZFromSpacePool::install_new_target(ZPageAge age) {
  {
    ZLocker<ZLock> guard(&_in_placed_guard);

    if (!_in_placed_pages[static_cast<uint>(age)].is_empty()) {
      return _in_placed_pages[static_cast<uint>(age)].remove_first();
    }
  }

  if (ZPage* p = claim_and_remove_any_page(age)) {
    ZForwarding* f = p->get_forwarding();
    size_t bytes_in_placed = ZRelocate::compact_in_place(f);
    inc_in_placed_page_count_and_bytes(bytes_in_placed, f);

    assert(f->ref_count() == -1, "Bug!");
    f->release_page();
    f->mark_done();
    assert(f->ref_count() == 0, "");
    p->reset_age(age);
    return p; // Success

  } else {
    return nullptr; // failure
  }
}

void update_if_higher(volatile size_t *field, size_t value) {
  for (size_t old_value = *field;
       value > old_value;) {
    auto actual_value = Atomic::cmpxchg(field, old_value, value);
    if (actual_value == old_value) {
      return;
    } else {
      old_value = actual_value;
    }
  }
}

bool ZFromSpacePool::free_page() {
  if (alloc_page_from_cache(nullptr)) return true;

  if (!fsp_depleted() && !in_phase_mc()) {
    return alloc_page_inner(nullptr);
  } else {
    return false;
  }
}

size_t ZFromSpacePool::evacuate_page(ZForwarding* f, zaddress* livemap_cursor) {
  size_t evacuated_bytes = 0;
  zaddress start_from = livemap_cursor ? *livemap_cursor : zaddress::null;

  const ZPageAge age = f->to_age();
  ZForwardingCursor cursor;

  f->object_iterate_via_livemap([&](const oop obj) {
    const zaddress from_addr = to_zaddress(obj);
    if (from_addr < start_from) return true;

    if (is_null(ZRelocate::lookup(f, from_addr, &cursor))) {
      const size_t unaligned_size = ZUtils::object_size(from_addr);

      zaddress to_addr = alloc_object_atomic(from_addr, unaligned_size, age);

      if (is_null(to_addr)) {
        // Record in the livemap cursor where we stopped evacuating
        if (livemap_cursor) *livemap_cursor = from_addr;
        return false;
      } else {
        ZUtils::object_copy_disjoint(from_addr, to_addr, unaligned_size);
        zaddress final_addr = ZRelocate::insert(f, from_addr, to_addr, &cursor);
        if (final_addr == to_addr) {
          evacuated_bytes += unaligned_size;
        }
      }
    }

    return true;
  });

  return evacuated_bytes;
}

ZForwarding* ZFromSpacePool::try_claim_page(size_t i, bool* update_fsp) {
  ZForwarding* f = _fsp.at(i);

  if (f->is_done()) {
    if (*update_fsp) update_if_higher(&_fsp_start, i + 1);
    return nullptr;
  }

  if (!f->claim2()) {
    *update_fsp = false;
    return nullptr;
  }

  if (!f->retain_page(nullptr, true, true)) {
    assert(f->ref_count() <= 0 || f->is_evacuated(), "");
    f->unclaim2();
    // update_fsp = false; // REALISATION: if the assert above holds, we can still update fsp_start since the page is not eligble
    // ADDENDUM: f can also be evacuated -- what to do about that?
    return nullptr;
  }

  return f;
}

bool ZFromSpacePool::alloc_page_inner(ZPage** result) {
  bool update_fsp = true;
  zaddress livemap_cursor = zaddress::null;

  for (size_t i = _fsp_start; i < _fsp_pages; i = MAX(i + 1, _fsp_start)) {

  retry_before_claimed:
    ZForwarding* f = try_claim_page(i, &update_fsp);
    if (!f) continue;

    const ZPageAge age = f->to_age();

  retry_after_retained:
    ZPage* page_we_evacuate_onto = load_target(age);

    // Evacuate all objects
    size_t evacuated_bytes = evacuate_page(f, &livemap_cursor);

    // If the page is now fully evacuated...
    if (f->inc_evacuated_bytes(evacuated_bytes)) {
      // ...try to free it -- otherwise try again from the top
      if (try_free_if_evacuated_else_release(f, 2, result)) {
        return true;
      } else {
        // If we want a page back, try again -- else return true because a page has been free'd
        if (result) {
          // reset livemap_cursor because we might continue on a different page
          livemap_cursor = zaddress::null;
          continue;
        } else {
          return true;
        }
      }

    } else {
      // We did not succeed in evacuating the page -- should *always* mean alloc_failed
      ZPage* const target = load_target(age);
      // If the target page is not the same we failed to evacuate onto...
      if (target != page_we_evacuate_onto) {
        // keeping livemap_cursor ensures we will continue from where we left off
        goto retry_after_retained;
      } else {
        // Need to "back out" before possibly calling install_new_target
        f->unclaim2();
        // TODO: make sure the page isn't skipped by _fsp_start
        f->release_page();
        // log_info(gc)("lost one possible page to evacuate in FSP!");
        // reset livemap_cursor because we might continue on a different page
        livemap_cursor = zaddress::null;

        {
          // Timer timer("target_guard");
          ZLocker<ZLock> guard(&_target_guard);

          // Someone else installed a new page while we were blocking on the guard
          // ...we can clear the alloc_failed state and try to claim the page again
          if (target != load_target(age)) {
            goto retry_before_claimed;
          }
          // We won the race to install a new page
          // ...we can clear the alloc_failed state and try to claim the page again
          if (auto new_target = install_new_target(age)) {
            store_target(new_target, age);
            goto retry_before_claimed;
          }
        }
        // We don't have a target page -- just give up
        break;
      }
      f->release_page();
    }
  }

  return false;
}

void ZFromSpacePool::free_page(ZForwarding* f, ZPage** result) {
  static ZGenerationYoung* young = ZGeneration::young();

  ZPage* const p = f->page();
  p->mark_as_fsp_current_cycle();
  inc_evacuated_page_count_and_bytes(f->evacuated_bytes(), f);
  // Atomic::inc(&_evacuated_page_count, memory_order_relaxed);
  // Atomic::sub(&_size_in_bytes, f->size() - f->live_bytes(), memory_order_relaxed);

  if (result) {
    *result = p;
  } else {
    ZFreeList& free_list = _per_cpu_free_list.get();
    // ZLocker<ZLock> guard(&free_list._guard);
    if (free_list._guard.try_lock()) {
      guarantee(!p->in_any_pool(), "");
      free_list._list.insert_last(p);
      free_list._guard.unlock();
    } else {
      log_info(gc)("fail free_list._guard");
    }
  }
  f->mark_done();
}

ZPage* ZFromSpacePool::claim_and_remove_any_page(ZPageAge age) {
  assert(!in_phase_mc(), "why didn't you call claim_logically_free_page?");

  for (size_t attempt = 0; attempt < 2; ++attempt) {
    for (size_t i = Atomic::load_acquire(&_fsp_start); i < _fsp_pages; ++i) {
      ZForwarding* f = _fsp.at(i);

      if (attempt == 0 && f->to_age() != age) continue;
      if (f->is_done()) continue;
      // TODO: should we kill the following line -- might lead to starvation?
      if (f->is_claim2()) continue;
      if (f->in_place_relocation_claim_page()) {
        auto res = f->claim();
        assert(res, "");
        return f->page();
      }
    }
  }

  return nullptr;
}
