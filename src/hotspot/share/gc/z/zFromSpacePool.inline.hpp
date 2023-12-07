#ifndef SHARE_GC_Z_ZFROMSPACEPOOL_INLINE_HPP
#define SHARE_GC_Z_ZFROMSPACEPOOL_INLINE_HPP

#include "gc/z/zFromSpacePool.hpp"
#include "gc/z/zGeneration.hpp"
#include "gc/z/zCPU.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"

inline ZFromSpacePool* ZFromSpacePool::pool() {
  return _pool;
}

inline size_t ZFromSpacePool::pages() const {
  return _fsp_pages - Atomic::load_acquire(&_evacuated_page_count) - Atomic::load_acquire(&_in_placed_page_count);
}

inline size_t ZFromSpacePool::to_be_free_in_bytes() const {
  // return Atomic::load_acquire(&_size_in_bytes);

  const double survival_rate = _stat_fsp_percent_evacuated.davg();
  return pages() * ZPageSizeSmall * (1 - survival_rate - _stat_fsp_percent_evacuated.dvariance());
}

inline size_t ZFromSpacePool::reclaimed_avg() const {
  return _stat_to_be_freed_in_bytes.davg();
}

inline bool ZFromSpacePool::fsp_depleted() const {
  return _fsp_pages <= Atomic::load_acquire(&_fsp_start);
}

inline ZPage* ZFromSpacePool::load_target(ZPageAge age) {
  return const_cast<ZPage*>(Atomic::load_acquire(_target+static_cast<uint>(age)));
}

inline void ZFromSpacePool::store_target(volatile ZPage* page, ZPageAge age) {
  Atomic::release_store(_target+static_cast<uint>(age), page);
}

inline size_t ZFromSpacePool::pages_at_relocate_start() {
  return _fsp_pages;
}

inline void ZFromSpacePool::add_page(ZPage* p) {
  assert(p->is_unlinked(), "");
  // p->mark_as_fsp_current_cycle();
  _fsp.append(p->get_forwarding());
  ++_fsp_pages;

  const size_t page_size = p->size();
  const size_t live_bytes = p->live_bytes();
  _size_in_bytes += (page_size - live_bytes);
  _deferrable_bytes += live_bytes;
}

inline void ZFromSpacePool::inc_in_placed_page_count_and_bytes(size_t bytes_in_placed, ZForwarding* f) {
  Atomic::inc(&_in_placed_page_count, memory_order_relaxed);
  Atomic::add(&_in_placed_bytes, bytes_in_placed, memory_order_relaxed);
  Atomic::sub(&_size_in_bytes, f->size() - f->live_bytes(), memory_order_relaxed);
}

inline void ZFromSpacePool::inc_evacuated_page_count_and_bytes(size_t bytes_evacuated, ZForwarding* f) {
  Atomic::inc(&_evacuated_page_count, memory_order_relaxed);
  Atomic::add(&_evacuated_bytes, bytes_evacuated, memory_order_relaxed);
  Atomic::sub(&_size_in_bytes, f->size() - f->live_bytes(), memory_order_relaxed);
}

inline void ZFromSpacePool::append_to_in_placed_pages(ZPage* p, ZPageAge age) {
  ZLocker<ZLock> guard(&_in_placed_guard);
  _in_placed_pages[static_cast<uint>(age)].insert_last(p);
}

// Allocate on target possibly taking a new target if there is no room
inline zaddress ZFromSpacePool::alloc_object_atomic(zaddress from_addr, size_t size, ZPageAge age) {
  ZPage* current_target = load_target(age);
  return current_target ? current_target->alloc_object_atomic(size) : zaddress::null;
}

inline ZPage* ZFromSpacePool::alloc_page() {
  ZPage* page = nullptr;
  if (alloc_page_from_cache(&page)) return page;

  if (!fsp_depleted() && !ZGeneration::young()->is_phase_mark_complete()) {
    alloc_page_inner(&page);
  }

  return page;
}

inline bool ZFromSpacePool::claim_and_remove_specific(ZForwarding* f) {
  assert(!ZGeneration::young()->is_phase_mark_complete(), "Should never need to be called in this phase");

  if (f->in_place_relocation_claim_page(true)) {
    f->claim();
    return true;
  } else {
    return false;
  }
}

inline size_t ZFromSpacePool::cache_size() {
  ZLocker<ZLock> guard(&_in_placed_guard);

  // size_t cache_size =  _shared_free_list._list.size();
  size_t cache_size = 0;
  {
    ZLocker<ZLock> guard(&_shared_free_list._guard);
    cache_size += _shared_free_list._list.size();
  }
  for (size_t i = 0; i < ZCPU::count(); ++i) {
    auto& free_list = _per_cpu_free_list.get();
    ZLocker<ZLock> guard(&free_list._guard);
    cache_size += free_list._list.size();
  }
  return cache_size;
}

#endif
