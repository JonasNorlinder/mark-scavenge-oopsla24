#ifndef SHARE_GC_Z_ZFROMSPACEPOOL_HPP
#define SHARE_GC_Z_ZFROMSPACEPOOL_HPP

#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zForwarding.hpp"
#include "gc/z/zLiveMap.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zPage.hpp"
#include "gc/z/zThread.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zValue.hpp"
#include "utilities/debug.hpp"
#include "logging/log.hpp"
#include "utilities/numberSeq.hpp"
#include <cstdint>

class Timer {
private:
  size_t       _start;
  const char*  _name;
  double       _ignore_below_s;
public:
  Timer(const char* name, double ignore = GCTimerIgnore) :
    _start(os::elapsed_counter()),
    _name(name),
    _ignore_below_s(ignore) {};

  ~Timer() {
    double duration_ns = os::elapsed_counter() - _start;
    double duration_s = duration_ns / os::elapsed_frequency();
    if ((_ignore_below_s == 0) || (duration_s >= _ignore_below_s)) {
      log_info(gc)("[Timer] %s: %f", _name, duration_s);
    }
  };
};

class ZFreeList {
public:
  ZLock        _guard;
  ZList<ZPage> _list;
  ZFreeList() : _guard(), _list() {
  }
};
class ZPageAllocator;
class ZFromSpacePool {
  friend class ZPageAllocator;
private:
  static ZFromSpacePool*   _pool;

  // From space pool is an array of pointers to FW objects since
  // page objects may be destroyed when freeing them, and we can't
  // afford to lock the FSP to coordinate iteration with deletion.
  ZArray<ZForwarding*> _fsp;
  size_t            _fsp_pages;

  // To avoid looking up pages from the start, we keep track of the
  // smallest index below which all pages have been evacuated or
  // in-placed.
  volatile size_t   _fsp_start;
  // Because in-placed pages are guaranteed to survive, we can keep
  // in-placed pages in a list. Compacting a page in-place adds it
  // to this list and install_new_page takes pages from this list
  // if it is non-empty.
  ZList<ZPage>      _in_placed_pages[ZPageAgeMax];
  ZLock             _in_placed_guard;
  // The page used as targed for evacuation and its lock.
  ZPage volatile *   _target[ZPageAgeMax] = { nullptr };
  ZLock              _target_guard;

  ZPerCPU<ZFreeList> _per_cpu_free_list;
  ZFreeList          _shared_free_list;
  // We keep track of the FSP size in bytes so that ZDirector can
  // get an updated number to use for adjusting the start of the
  // next GC cycle.
  volatile size_t   _size_in_bytes;
  volatile size_t   _evacuated_bytes;
  volatile size_t   _in_placed_bytes;
  size_t            _deferrable_bytes;
  volatile size_t   _evacuated_page_count;
  volatile size_t   _in_placed_page_count;
  NumberSeq         _stat_evacuated_pages;
  NumberSeq         _stat_fsp_percent_evacuated;
  NumberSeq         _stat_to_be_freed_in_bytes;


  // Install a new target page for remove and clear page to evacuate onto.
  ZPage* install_new_target(ZPageAge age);
  // Used inside remove and clear page to allocate on the target page.
  zaddress alloc_object_atomic(zaddress from_addr, size_t size, ZPageAge age);

  // Acquires the write-lock for a specific page. This is required to
  // in-place compact it or free it.
  bool claim_and_remove_specific(ZForwarding* page);
  // Acquires the write-lock for the sparsest page with RC=1 and returns it.
  ZPage* claim_and_remove_any_page(ZPageAge age);
  // Tries to acquire the read-lock for a page if it has RC=1 and returns it.
  ZForwarding* try_retain_and_remove_page(size_t _fsp_index);
  // Free's a page whose RC=0 and is claimed (ZForwarding::claim()).
  // Subtracts from _size_in_bytes and marks the forwarding as done.
  void free_page(ZForwarding* f, ZPage** result);
  // Used internally to add an in-place compacted page to _in_placed_pages.
  void append_to_in_placed_pages(ZPage* page, ZPageAge age);
  void inc_in_placed_page_count_and_bytes(size_t bytes, ZForwarding* f);
  void inc_evacuated_page_count_and_bytes(size_t bytes, ZForwarding* f);

  ZPage* load_target(ZPageAge age);
  void store_target(volatile ZPage * page, ZPageAge age);
  void reset_target();
  bool alloc_page_inner(ZPage** result);
  bool alloc_page_from_cache(ZPage** result);
  size_t evacuate_page(ZForwarding* f, zaddress* livemap_cursor = nullptr);
  void update_fsp_start(size_t new_fsp_start);
  ZForwarding* try_claim_page(size_t i, bool* update_fsp);

public:
  ZFromSpacePool();
  ~ZFromSpacePool();

  // Static method to obtain the FSP singleton
  static ZFromSpacePool* pool();

  // Returns the number of pages in FSP
  size_t pages() const;
  // Returns the number of bytes in FSP
  size_t to_be_free_in_bytes() const;
  size_t reclaimed_avg() const;

  // For readability -- used in install_new_target to indicate that FSP is empty
  bool fsp_depleted() const;

  //////////////////////////////////////////////////////////////
  // Methods that should ONLY be called during MC
  //////////////////////////////////////////////////////////////

  // Resets the FSP for the next cycle
  size_t reset_end();
  void reset_start();
  // Add a page to the FSP
  void add_page(ZPage* page);

  size_t pages_at_relocate_start();

  //////////////////////////////////////////////////////////////
  // Method that should NEVER be called during MC
  //////////////////////////////////////////////////////////////

  // Compacts a page in-place
  void compact_in_place(ZForwarding* forwarding);
  // If the page is evacuated and has RC=2, it will be freed,
  // and the method returns true.
  // Else it's RC will be decreased by 1 and return false.
  bool try_free_if_evacuated_else_release(ZForwarding* f, size_t from_rc, ZPage** result = nullptr);
  // Take a page from FSP, evacuated it, and return the page
  // FIXME: page is currently free'd internally
  ZPage* alloc_page();
  bool free_page();

  size_t cache_size();
};

#endif
