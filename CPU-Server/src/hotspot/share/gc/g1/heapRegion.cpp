/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "code/nmethod.hpp"
#include "gc/g1/g1BlockOffsetTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1HeapRegionTraceType.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionBounds.inline.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/g1/heapRegionTracer.hpp"
#include "gc/shared/genOopClosures.inline.hpp"
#include "gc/shared/space.inline.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/growableArray.hpp"

int    HeapRegion::LogOfHRGrainBytes = 0;
int    HeapRegion::LogOfHRGrainWords = 0;
size_t HeapRegion::GrainBytes        = 0;
size_t HeapRegion::GrainWords        = 0;
size_t HeapRegion::CardsPerRegion    = 0;

size_t HeapRegion::max_region_size() {
  return HeapRegionBounds::max_size();
}

size_t HeapRegion::min_region_size_in_words() {
  return HeapRegionBounds::min_size() >> LogHeapWordSize;
}

void HeapRegion::setup_heap_region_size(size_t initial_heap_size, size_t max_heap_size) {
  size_t region_size = G1HeapRegionSize;
  if (FLAG_IS_DEFAULT(G1HeapRegionSize)) {
    size_t average_heap_size = (initial_heap_size + max_heap_size) / 2;
    region_size = MAX2(average_heap_size / HeapRegionBounds::target_number(),
                       HeapRegionBounds::min_size());
  }

  int region_size_log = log2_long((jlong) region_size);
  // Recalculate the region size to make sure it's a power of
  // 2. This means that region_size is the largest power of 2 that's
  // <= what we've calculated so far.
  region_size = ((size_t)1 << region_size_log);

  // Now make sure that we don't go over or under our limits.
  if (region_size < HeapRegionBounds::min_size()) {
    region_size = HeapRegionBounds::min_size();
  } 
  
  //mhr: modify
  // else if (region_size > HeapRegionBounds::max_size()) {
  //   region_size = HeapRegionBounds::max_size();
  // }

  // And recalculate the log.
  region_size_log = log2_long((jlong) region_size);

  // Now, set up the globals.
  guarantee(LogOfHRGrainBytes == 0, "we should only set it once");
  LogOfHRGrainBytes = region_size_log;

  guarantee(LogOfHRGrainWords == 0, "we should only set it once");
  LogOfHRGrainWords = LogOfHRGrainBytes - LogHeapWordSize;

  guarantee(GrainBytes == 0, "we should only set it once");
  // The cast to int is safe, given that we've bounded region_size by
  // MIN_REGION_SIZE and MAX_REGION_SIZE.
  GrainBytes = region_size;
  log_info(gc, heap)("Heap region size: " SIZE_FORMAT "M", GrainBytes / M);

  guarantee(GrainWords == 0, "we should only set it once");
  GrainWords = GrainBytes >> LogHeapWordSize;
  guarantee((size_t) 1 << LogOfHRGrainWords == GrainWords, "sanity");

  guarantee(CardsPerRegion == 0, "we should only set it once");
  CardsPerRegion = GrainBytes >> G1CardTable::card_shift;

  if (G1HeapRegionSize != GrainBytes) {
    FLAG_SET_ERGO(size_t, G1HeapRegionSize, GrainBytes);
  }
}

void HeapRegion::hr_clear(bool keep_remset, bool clear_space, bool locked) {
  assert(_cpu_to_mem_gc->_humongous_start_region == NULL,
         "we should have already filtered out humongous regions");
  assert(!in_collection_set(),
         "Should not clear heap region %u in the collection set", hrm_index());

  bool _is_young = is_young();
  set_young_index_in_cset(-1);
  uninstall_surv_rate_group();
  set_free();
  reset_pre_dummy_top();

  if (!keep_remset) {
    if (locked) {
      rem_set()->clear_locked();
    } else {
      rem_set()->clear();
    }
  }

  zero_marked_bytes();

  init_top_at_mark_start();
  if (clear_space) clear(SpaceDecorator::Mangle);


  //mhr: modify
  // reset_region_cm_scanned();
  // if(!_is_young && cross_region_ref_update_queue() != NULL) {
  //   if(!keep_remset) {

  //     // [XX] Wait to be corrected. [XX]

  //     // delete _move_to;
  //     // _move_to = NULL;
  //     cross_region_ref_update_queue()->reset();
  //   }
  //   else {
  //     cross_region_ref_update_queue()->reset();
  //   }

  // }
}

void HeapRegion::clear_cardtable() {
  G1CardTable* ct = G1CollectedHeap::heap()->card_table();
  ct->clear(MemRegion(bottom(), end()));
}

void HeapRegion::calc_gc_efficiency() {
  // GC efficiency is the ratio of how much space would be
  // reclaimed over how long we predict it would take to reclaim it.
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  G1Policy* g1p = g1h->g1_policy();

  // Retrieve a prediction of the elapsed time for this region for
  // a mixed gc because the region will only be evacuated during a
  // mixed gc.
  double region_elapsed_time_ms =
    g1p->predict_region_elapsed_time_ms(this, false /* for_young_gc */);
  _gc_efficiency = (double) reclaimable_bytes() / region_elapsed_time_ms;
}

void HeapRegion::set_free() {
  report_region_type_change(G1HeapRegionTraceType::Free);
  _cpu_to_mem_gc->_type.set_free();
}

void HeapRegion::set_eden() {
  report_region_type_change(G1HeapRegionTraceType::Eden);
  _cpu_to_mem_gc->_type.set_eden();
}

void HeapRegion::set_eden_pre_gc() {
  report_region_type_change(G1HeapRegionTraceType::Eden);
  _cpu_to_mem_gc->_type.set_eden_pre_gc();
}

void HeapRegion::set_survivor() {
  report_region_type_change(G1HeapRegionTraceType::Survivor);
  _cpu_to_mem_gc->_type.set_survivor();
}

void HeapRegion::move_to_old() {
  if (_cpu_to_mem_gc->_type.relabel_as_old()) {
    report_region_type_change(G1HeapRegionTraceType::Old);
  }
}

void HeapRegion::set_old() {
  report_region_type_change(G1HeapRegionTraceType::Old);
  _cpu_to_mem_gc->_type.set_old();
}

void HeapRegion::set_open_archive() {
  report_region_type_change(G1HeapRegionTraceType::OpenArchive);
  _cpu_to_mem_gc->_type.set_open_archive();
}

void HeapRegion::set_closed_archive() {
  report_region_type_change(G1HeapRegionTraceType::ClosedArchive);
  _cpu_to_mem_gc->_type.set_closed_archive();
}

void HeapRegion::set_starts_humongous(HeapWord* obj_top, size_t fill_size) {
  assert(!is_humongous(), "sanity / pre-condition");
  assert(top() == bottom(), "should be empty");

  report_region_type_change(G1HeapRegionTraceType::StartsHumongous);
  _cpu_to_mem_gc->_type.set_starts_humongous();
  _cpu_to_mem_gc->_humongous_start_region = this;

  _sync_mem_cpu->_bot_part.set_for_starts_humongous(obj_top, fill_size);
}

void HeapRegion::set_continues_humongous(HeapRegion* first_hr) {
  assert(!is_humongous(), "sanity / pre-condition");
  assert(top() == bottom(), "should be empty");
  assert(first_hr->is_starts_humongous(), "pre-condition");

  report_region_type_change(G1HeapRegionTraceType::ContinuesHumongous);
  _cpu_to_mem_gc->_type.set_continues_humongous();
  _cpu_to_mem_gc->_humongous_start_region = first_hr;

 _sync_mem_cpu-> _bot_part.set_object_can_span(true);
}

void HeapRegion::clear_humongous() {
  assert(is_humongous(), "pre-condition");

  assert(capacity() == HeapRegion::GrainBytes, "pre-condition");
  _cpu_to_mem_gc->_humongous_start_region = NULL;

  _sync_mem_cpu->_bot_part.set_object_can_span(false);
}

HeapRegion::HeapRegion(uint hrm_index,
                       G1BlockOffsetTable* bot,
                       MemRegion mr) :
    G1ContiguousSpace(bot),
   _cpu_to_mem_init(NULL),
    _cpu_to_mem_gc(NULL),
    _mem_to_cpu_gc(NULL),
    _rem_set(NULL),
    _evacuation_failed(false),
#ifdef ASSERT
    _containing_set(NULL),
#endif
    _prev_marked_bytes(0), _next_marked_bytes(0), _gc_efficiency(0.0),
    _index_in_opt_cset(G1OptionalCSet::InvalidCSetIndex), _young_index_in_cset(-1),
    _surv_rate_group(NULL), _age_index(-1), _age(-1), //mhr: modify
    _prev_top_at_mark_start(NULL), _next_top_at_mark_start(NULL),
    _recorded_rs_length(0), _predicted_elapsed_time_ms(0)
{

  // Semeru
  // Initialize the RDMA meta data space
  _cpu_to_mem_init = new(hrm_index) CPUToMemoryAtInit(hrm_index);
  _cpu_to_mem_gc = new(hrm_index) CPUToMemoryAtGC(hrm_index);
  _mem_to_cpu_gc = new(hrm_index) MemoryToCPUAtGC(hrm_index);
  _sync_mem_cpu = new(hrm_index) SyncBetweenMemoryAndCPU(hrm_index, bot, this);
  _rem_set = new HeapRegionRemSet(bot, this);

  initialize(mr);
}

void HeapRegion::initialize(MemRegion mr, bool clear_space, bool mangle_space) {
  assert(_rem_set->is_empty(), "Remembered set must be empty");

  G1ContiguousSpace::initialize(mr, clear_space, mangle_space);

  hr_clear(false /*par*/, false /*clear_space*/);
  set_top(bottom());

  // if(_target_obj_queue == NULL)
  //   _target_obj_queue = new TargetObjQueue(); 
  // else
  //   _target_obj_queue->reset();

  set_saved_mark_word(NULL);
  reset_bot();
}

void HeapRegion::report_region_type_change(G1HeapRegionTraceType::Type to) {
  HeapRegionTracer::send_region_type_change(_cpu_to_mem_init->_hrm_index,
                                            get_trace_type(),
                                            to,
                                            (uintptr_t)bottom(),
                                            used());
}

void HeapRegion::note_self_forwarding_removal_start(bool during_initial_mark,
                                                    bool during_conc_mark) {
  // We always recreate the prev marking info and we'll explicitly
  // mark all objects we find to be self-forwarded on the prev
  // bitmap. So all objects need to be below PTAMS.
  _prev_marked_bytes = 0;

  if (during_initial_mark) {
    // During initial-mark, we'll also explicitly mark all objects
    // we find to be self-forwarded on the next bitmap. So all
    // objects need to be below NTAMS.
    _next_top_at_mark_start = top();
    _next_marked_bytes = 0;
  } else if (during_conc_mark) {
    // During concurrent mark, all objects in the CSet (including
    // the ones we find to be self-forwarded) are implicitly live.
    // So all objects need to be above NTAMS.
    _next_top_at_mark_start = bottom();
    _next_marked_bytes = 0;
  }
}

void HeapRegion::note_self_forwarding_removal_end(size_t marked_bytes) {
  assert(marked_bytes <= used(),
         "marked: " SIZE_FORMAT " used: " SIZE_FORMAT, marked_bytes, used());
  _prev_top_at_mark_start = top();
  _prev_marked_bytes = marked_bytes;
}

// Code roots support

void HeapRegion::add_strong_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_strong_code_root(nm);
}

void HeapRegion::add_strong_code_root_locked(nmethod* nm) {
  assert_locked_or_safepoint(CodeCache_lock);
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->add_strong_code_root_locked(nm);
}

void HeapRegion::remove_strong_code_root(nmethod* nm) {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->remove_strong_code_root(nm);
}

void HeapRegion::strong_code_roots_do(CodeBlobClosure* blk) const {
  HeapRegionRemSet* hrrs = rem_set();
  hrrs->strong_code_roots_do(blk);
}

class VerifyStrongCodeRootOopClosure: public OopClosure {
  const HeapRegion* _hr;
  bool _failures;
  bool _has_oops_in_region;

  template <class T> void do_oop_work(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);

      // Note: not all the oops embedded in the nmethod are in the
      // current region. We only look at those which are.
      if (_hr->is_in(obj)) {
        // Object is in the region. Check that its less than top
        if (_hr->top() <= (HeapWord*)obj) {
          // Object is above top
          log_error(gc, verify)("Object " PTR_FORMAT " in region [" PTR_FORMAT ", " PTR_FORMAT ") is above top " PTR_FORMAT,
                               p2i(obj), p2i(_hr->bottom()), p2i(_hr->end()), p2i(_hr->top()));
          _failures = true;
          return;
        }
        // Nmethod has at least one oop in the current region
        _has_oops_in_region = true;
      }
    }
  }

public:
  VerifyStrongCodeRootOopClosure(const HeapRegion* hr):
    _hr(hr), _failures(false), _has_oops_in_region(false) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }

  bool failures()           { return _failures; }
  bool has_oops_in_region() { return _has_oops_in_region; }
};

class VerifyStrongCodeRootCodeBlobClosure: public CodeBlobClosure {
  const HeapRegion* _hr;
  bool _failures;
public:
  VerifyStrongCodeRootCodeBlobClosure(const HeapRegion* hr) :
    _hr(hr), _failures(false) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = (cb == NULL) ? NULL : cb->as_compiled_method()->as_nmethod_or_null();
    if (nm != NULL) {
      // Verify that the nemthod is live
      if (!nm->is_alive()) {
        log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] has dead nmethod " PTR_FORMAT " in its strong code roots",
                              p2i(_hr->bottom()), p2i(_hr->end()), p2i(nm));
        _failures = true;
      } else {
        VerifyStrongCodeRootOopClosure oop_cl(_hr);
        nm->oops_do(&oop_cl);
        if (!oop_cl.has_oops_in_region()) {
          log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] has nmethod " PTR_FORMAT " in its strong code roots with no pointers into region",
                                p2i(_hr->bottom()), p2i(_hr->end()), p2i(nm));
          _failures = true;
        } else if (oop_cl.failures()) {
          log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] has other failures for nmethod " PTR_FORMAT,
                                p2i(_hr->bottom()), p2i(_hr->end()), p2i(nm));
          _failures = true;
        }
      }
    }
  }

  bool failures()       { return _failures; }
};

void HeapRegion::verify_strong_code_roots(VerifyOption vo, bool* failures) const {
  if (!G1VerifyHeapRegionCodeRoots) {
    // We're not verifying code roots.
    return;
  }
  if (vo == VerifyOption_G1UseFullMarking) {
    // Marking verification during a full GC is performed after class
    // unloading, code cache unloading, etc so the strong code roots
    // attached to each heap region are in an inconsistent state. They won't
    // be consistent until the strong code roots are rebuilt after the
    // actual GC. Skip verifying the strong code roots in this particular
    // time.
    assert(VerifyDuringGC, "only way to get here");
    return;
  }

  HeapRegionRemSet* hrrs = rem_set();
  size_t strong_code_roots_length = hrrs->strong_code_roots_list_length();

  // if this region is empty then there should be no entries
  // on its strong code root list
  if (is_empty()) {
    if (strong_code_roots_length > 0) {
      log_error(gc, verify)("region [" PTR_FORMAT "," PTR_FORMAT "] is empty but has " SIZE_FORMAT " code root entries",
                            p2i(bottom()), p2i(end()), strong_code_roots_length);
      *failures = true;
    }
    return;
  }

  if (is_continues_humongous()) {
    if (strong_code_roots_length > 0) {
      log_error(gc, verify)("region " HR_FORMAT " is a continuation of a humongous region but has " SIZE_FORMAT " code root entries",
                            HR_FORMAT_PARAMS(this), strong_code_roots_length);
      *failures = true;
    }
    return;
  }

  VerifyStrongCodeRootCodeBlobClosure cb_cl(this);
  strong_code_roots_do(&cb_cl);

  if (cb_cl.failures()) {
    *failures = true;
  }
}

void HeapRegion::print() const { print_on(tty); }
void HeapRegion::print_on(outputStream* st) const {
  st->print("|%4u", this->_cpu_to_mem_init->_hrm_index);
  st->print("|" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT,
            p2i(bottom()), p2i(top()), p2i(end()));
  st->print("|%3d%%", (int) ((double) used() * 100 / capacity()));
  st->print("|%2s", get_short_type_str());
  if (in_collection_set()) {
    st->print("|CS");
  } else {
    st->print("|  ");
  }
  st->print_cr("|TAMS " PTR_FORMAT ", " PTR_FORMAT "| %s ",
               p2i(prev_top_at_mark_start()), p2i(next_top_at_mark_start()), rem_set()->get_state_str());
}

class G1VerificationClosure : public BasicOopIterateClosure {
protected:
  G1CollectedHeap* _g1h;
  G1CardTable *_ct;
  oop _containing_obj;
  bool _failures;
  int _n_failures;
  VerifyOption _vo;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseFullMarking -> use "next" marking bitmap but no TAMS.
  G1VerificationClosure(G1CollectedHeap* g1h, VerifyOption vo) :
    _g1h(g1h), _ct(g1h->card_table()),
    _containing_obj(NULL), _failures(false), _n_failures(0), _vo(vo) {
  }

  void set_containing_obj(oop obj) {
    _containing_obj = obj;
  }

  bool failures() { return _failures; }
  int n_failures() { return _n_failures; }

  void print_object(outputStream* out, oop obj) {
#ifdef PRODUCT
    Klass* k = obj->klass();
    const char* class_name = k->external_name();
    out->print_cr("class name %s", class_name);
#else // PRODUCT
    obj->print_on(out);
#endif // PRODUCT
  }

  // This closure provides its own oop verification code.
  debug_only(virtual bool should_verify_oops() { return false; })
};

class VerifyLiveClosure : public G1VerificationClosure {
public:
  VerifyLiveClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_liveness(p);
  }

  template <class T>
  void verify_liveness(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    Log(gc, verify) log;
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      bool failed = false;
      if (!_g1h->is_in_closed_subset(obj) || _g1h->is_obj_dead_cond(obj, _vo)) {
        MutexLockerEx x(ParGCRareEvent_lock,
          Mutex::_no_safepoint_check_flag);

        if (!_failures) {
          log.error("----------");
        }
        ResourceMark rm;
        if (!_g1h->is_in_closed_subset(obj)) {
          HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p);
          log.error("Field " PTR_FORMAT " of live obj " PTR_FORMAT " in region [" PTR_FORMAT ", " PTR_FORMAT ")",
            p2i(p), p2i(_containing_obj), p2i(from->bottom()), p2i(from->end()));
          LogStream ls(log.error());
          print_object(&ls, _containing_obj);
          HeapRegion* const to = _g1h->heap_region_containing(obj);
          log.error("points to obj " PTR_FORMAT " in region " HR_FORMAT " remset %s", p2i(obj), HR_FORMAT_PARAMS(to), to->rem_set()->get_state_str());
        } else {
          HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p);
          HeapRegion* to = _g1h->heap_region_containing((HeapWord*)obj);
          log.error("Field " PTR_FORMAT " of live obj " PTR_FORMAT " in region [" PTR_FORMAT ", " PTR_FORMAT ")",
            p2i(p), p2i(_containing_obj), p2i(from->bottom()), p2i(from->end()));
          LogStream ls(log.error());
          print_object(&ls, _containing_obj);
          log.error("points to dead obj " PTR_FORMAT " in region [" PTR_FORMAT ", " PTR_FORMAT ")",
            p2i(obj), p2i(to->bottom()), p2i(to->end()));
          print_object(&ls, obj);
        }
        log.error("----------");
        _failures = true;
        failed = true;
        _n_failures++;
      }
    }
  }
};

class VerifyRemSetClosure : public G1VerificationClosure {
public:
  VerifyRemSetClosure(G1CollectedHeap* g1h, VerifyOption vo) : G1VerificationClosure(g1h, vo) {}
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p) { do_oop_work(p); }

  template <class T>
  void do_oop_work(T* p) {
    assert(_containing_obj != NULL, "Precondition");
    assert(!_g1h->is_obj_dead_cond(_containing_obj, _vo),
      "Precondition");
    verify_remembered_set(p);
  }

  template <class T>
  void verify_remembered_set(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    Log(gc, verify) log;
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      HeapRegion* from = _g1h->heap_region_containing((HeapWord*)p);
      HeapRegion* to = _g1h->heap_region_containing(obj);
      if (from != NULL && to != NULL &&
        from != to &&
        !to->is_pinned() &&
        to->rem_set()->is_complete()) {
        jbyte cv_obj = *_ct->byte_for_const(_containing_obj);
        jbyte cv_field = *_ct->byte_for_const(p);
        const jbyte dirty = G1CardTable::dirty_card_val();

        bool is_bad = !(from->is_young()
          || to->rem_set()->contains_reference(p)
          || (_containing_obj->is_objArray() ?
                cv_field == dirty :
                cv_obj == dirty || cv_field == dirty));
        if (is_bad) {
          MutexLockerEx x(ParGCRareEvent_lock,
            Mutex::_no_safepoint_check_flag);

          if (!_failures) {
            log.error("----------");
          }
          log.error("Missing rem set entry:");
          log.error("Field " PTR_FORMAT " of obj " PTR_FORMAT ", in region " HR_FORMAT,
            p2i(p), p2i(_containing_obj), HR_FORMAT_PARAMS(from));
          ResourceMark rm;
          LogStream ls(log.error());
          _containing_obj->print_on(&ls);
          log.error("points to obj " PTR_FORMAT " in region " HR_FORMAT " remset %s", p2i(obj), HR_FORMAT_PARAMS(to), to->rem_set()->get_state_str());
          if (oopDesc::is_oop(obj)) {
            obj->print_on(&ls);
          }
          log.error("Obj head CTE = %d, field CTE = %d.", cv_obj, cv_field);
          log.error("----------");
          _failures = true;
          _n_failures++;
        }
      }
    }
  }
};

// Closure that applies the given two closures in sequence.
class G1Mux2Closure : public BasicOopIterateClosure {
  OopClosure* _c1;
  OopClosure* _c2;
public:
  G1Mux2Closure(OopClosure *c1, OopClosure *c2) { _c1 = c1; _c2 = c2; }
  template <class T> inline void do_oop_work(T* p) {
    // Apply first closure; then apply the second.
    _c1->do_oop(p);
    _c2->do_oop(p);
  }
  virtual inline void do_oop(oop* p) { do_oop_work(p); }
  virtual inline void do_oop(narrowOop* p) { do_oop_work(p); }

  // This closure provides its own oop verification code.
  debug_only(virtual bool should_verify_oops() { return false; })
};

// This really ought to be commoned up into OffsetTableContigSpace somehow.
// We would need a mechanism to make that code skip dead objects.

void HeapRegion::verify(VerifyOption vo,
                        bool* failures) const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyLiveClosure vl_cl(g1h, vo);
  VerifyRemSetClosure vr_cl(g1h, vo);
  bool is_region_humongous = is_humongous();
  size_t object_num = 0;
  while (p < top()) {
    oop obj = oop(p);
    size_t obj_size = block_size(p);
    object_num += 1;

    if (!g1h->is_obj_dead_cond(obj, this, vo)) {
      if (oopDesc::is_oop(obj)) {
        Klass* klass = obj->klass();
        bool is_metaspace_object = Metaspace::contains(klass);
        if (!is_metaspace_object) {
          log_error(gc, verify)("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                "not metadata", p2i(klass), p2i(obj));
          *failures = true;
          return;
        } else if (!klass->is_klass()) {
          log_error(gc, verify)("klass " PTR_FORMAT " of object " PTR_FORMAT " "
                                "not a klass", p2i(klass), p2i(obj));
          *failures = true;
          return;
        } else {
          vl_cl.set_containing_obj(obj);
          if (!g1h->collector_state()->in_full_gc() || G1VerifyRSetsDuringFullGC) {
            // verify liveness and rem_set
            vr_cl.set_containing_obj(obj);
            G1Mux2Closure mux(&vl_cl, &vr_cl);
            obj->oop_iterate(&mux);

            if (vr_cl.failures()) {
              *failures = true;
            }
            if (G1MaxVerifyFailures >= 0 &&
              vr_cl.n_failures() >= G1MaxVerifyFailures) {
              return;
            }
          } else {
            // verify only liveness
            obj->oop_iterate(&vl_cl);
          }
          if (vl_cl.failures()) {
            *failures = true;
          }
          if (G1MaxVerifyFailures >= 0 &&
              vl_cl.n_failures() >= G1MaxVerifyFailures) {
            return;
          }
        }
      } else {
        log_error(gc, verify)(PTR_FORMAT " not an oop", p2i(obj));
        *failures = true;
        return;
      }
    }
    prev_p = p;
    p += obj_size;
  }

  if (!is_young() && !is_empty()) {
    _sync_mem_cpu->_bot_part.verify();
  }

  if (is_region_humongous) {
    oop obj = oop(this->humongous_start_region()->bottom());
    if ((HeapWord*)obj > bottom() || (HeapWord*)obj + obj->size() < bottom()) {
      log_error(gc, verify)("this humongous region is not part of its' humongous object " PTR_FORMAT, p2i(obj));
      *failures = true;
      return;
    }
  }

  if (!is_region_humongous && p != top()) {
    log_error(gc, verify)("end of last object " PTR_FORMAT " "
                          "does not match top " PTR_FORMAT, p2i(p), p2i(top()));
    *failures = true;
    return;
  }

  HeapWord* the_end = end();
  // Do some extra BOT consistency checking for addresses in the
  // range [top, end). BOT look-ups in this range should yield
  // top. No point in doing that if top == end (there's nothing there).
  if (p < the_end) {
    // Look up top
    HeapWord* addr_1 = p;
    HeapWord* b_start_1 = _sync_mem_cpu->_bot_part.block_start_const(addr_1);
    if (b_start_1 != p) {
      log_error(gc, verify)("BOT look up for top: " PTR_FORMAT " "
                            " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                            p2i(addr_1), p2i(b_start_1), p2i(p));
      *failures = true;
      return;
    }

    // Look up top + 1
    HeapWord* addr_2 = p + 1;
    if (addr_2 < the_end) {
      HeapWord* b_start_2 = _sync_mem_cpu->_bot_part.block_start_const(addr_2);
      if (b_start_2 != p) {
        log_error(gc, verify)("BOT look up for top + 1: " PTR_FORMAT " "
                              " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                              p2i(addr_2), p2i(b_start_2), p2i(p));
        *failures = true;
        return;
      }
    }

    // Look up an address between top and end
    size_t diff = pointer_delta(the_end, p) / 2;
    HeapWord* addr_3 = p + diff;
    if (addr_3 < the_end) {
      HeapWord* b_start_3 = _sync_mem_cpu->_bot_part.block_start_const(addr_3);
      if (b_start_3 != p) {
        log_error(gc, verify)("BOT look up for top + diff: " PTR_FORMAT " "
                              " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                              p2i(addr_3), p2i(b_start_3), p2i(p));
        *failures = true;
        return;
      }
    }

    // Look up end - 1
    HeapWord* addr_4 = the_end - 1;
    HeapWord* b_start_4 = _sync_mem_cpu->_bot_part.block_start_const(addr_4);
    if (b_start_4 != p) {
      log_error(gc, verify)("BOT look up for end - 1: " PTR_FORMAT " "
                            " yielded " PTR_FORMAT ", expecting " PTR_FORMAT,
                            p2i(addr_4), p2i(b_start_4), p2i(p));
      *failures = true;
      return;
    }
  }

  verify_strong_code_roots(vo, failures);
}

void HeapRegion::verify() const {
  bool dummy = false;
  verify(VerifyOption_G1UsePrevMarking, /* failures */ &dummy);
}

void HeapRegion::verify_rem_set(VerifyOption vo, bool* failures) const {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *failures = false;
  HeapWord* p = bottom();
  HeapWord* prev_p = NULL;
  VerifyRemSetClosure vr_cl(g1h, vo);
  while (p < top()) {
    oop obj = oop(p);
    size_t obj_size = block_size(p);

    if (!g1h->is_obj_dead_cond(obj, this, vo)) {
      if (oopDesc::is_oop(obj)) {
        vr_cl.set_containing_obj(obj);
        obj->oop_iterate(&vr_cl);

        if (vr_cl.failures()) {
          *failures = true;
        }
        if (G1MaxVerifyFailures >= 0 &&
          vr_cl.n_failures() >= G1MaxVerifyFailures) {
          return;
        }
      } else {
        log_error(gc, verify)(PTR_FORMAT " not an oop", p2i(obj));
        *failures = true;
        return;
      }
    }

    prev_p = p;
    p += obj_size;
  }
}

void HeapRegion::verify_rem_set() const {
  bool failures = false;
  verify_rem_set(VerifyOption_G1UsePrevMarking, &failures);
  guarantee(!failures, "HeapRegion RemSet verification failed");
}

void HeapRegion::prepare_for_compaction(CompactPoint* cp) {
  // Not used for G1 anymore, but pure virtual in Space.
  ShouldNotReachHere();
}



//
// G1ContiguousSpace
//

G1ContiguousSpace::G1ContiguousSpace(G1BlockOffsetTable* bot) :
  //_bot_part(bot, this),
  _par_alloc_lock(Mutex::leaf, "OffsetTableContigSpace par alloc lock", true)
{
}

void G1ContiguousSpace::initialize(MemRegion mr, bool clear_space, bool mangle_space) {
  CompactibleSpace::initialize(mr, clear_space, mangle_space);
  //_top = bottom();
  set_saved_mark_word(NULL);
  //reset_bot();
}





/**
 * Semeru
 *  
 * Added by Chenxi.
 */


// void HeapRegion::allocate_init_target_oop_queue(uint hrm_index){
  
//   #ifdef ASSERT
//   // In general, the heap region number can't exceed 128.
//   // Because the instane also costs some space, reserve 8 target object queues' space.
//   if(hrm_index >= TARGET_OBJ_SIZE_BYTE/(TARGET_OBJ_QUEUE_SIZE * HeapWordSize) -8 ){
//     tty->print("Error in %s, too many regions[0x%x] , limitation is 0x%lx. Give up current allocation. \n", 
//                                             __func__, 
//                                             hrm_index, 
//                                             TARGET_OBJ_SIZE_BYTE/(TARGET_OBJ_QUEUE_SIZE * HeapWordSize) -8 );
//     return;
//   }
//   #endif

//   // CHeapRDMAObj::new(instance_size(asigned by new), element_legnth, q_index, alloc_type )
//   _cpu_to_mem_gc->_target_obj_queue = new (TARGET_OBJ_QUEUE_SIZE, hrm_index) TargetObjQueue();   // The instance should be allocated in RDMA Meta space.
//   _cpu_to_mem_gc->_target_obj_queue->initialize((size_t)hrm_index);
// }


/**
 * get the memory server id for the region.
 * 
 * !! Fix Me !! 
 * To support multiple memory servers.
 * 
 */
int HeapRegion::region_to_memory_server_mapping(){
  int target_mem_id = -1;

  if( (size_t)this->end() < (size_t)MEMORY_SERVER_1_START_ADDR ){
    target_mem_id = 0;
  }else{
    target_mem_id = 1;
  }

  return target_mem_id;
}



void HeapRegion::allocate_init_cross_region_ref_update_queue(uint hrm_index){
  
  // CHeapRDMAObj::new(instance_size(asigned by new), element_legnth, q_index, alloc_type )
  //_sync_mem_cpu->_cross_region_ref_update_queue = new (CROSS_REGION_REF_UPDATE_Q_LEN, hrm_index) HashQueue();   // The instance should be allocated in RDMA Meta space.
  
  //_sync_mem_cpu->_cross_region_ref_update_queue = new (CROSS_REGION_REF_UPDATE_Q_LEN, hrm_index) HashQueue(G1CollectedHeap::heap()->_queue_bitmap);   // The instance should be allocated in RDMA Meta space.

  // _sync_mem_cpu->_cross_region_ref_update_queue->initialize((size_t)hrm_index, bottom());
	// log_debug(semeru,alloc)("%s,Region[0x%x] cross_region_ref_update_queue [0x%lx, 0x%lx) ", __func__, hrm_index, (size_t)_sync_mem_cpu->_cross_region_ref_update_queue, (size_t)CHeapRDMAObj<ElemPair, CROSS_REGION_REF_UPDATE_QUEUE_ALLOCTYPE>::_alloc_ptr );
  _sync_mem_cpu->_cross_region_ref_target_queue = new (CROSS_REGION_REF_TARGET_Q_LEN, hrm_index) BitQueue(GrainWords);   // The instance should be allocated in RDMA Meta space.
  _sync_mem_cpu->_cross_region_ref_target_queue->initialize((size_t)hrm_index, bottom());
  log_debug(semeru,alloc)("%s,Region[0x%x] cross_region_ref_target_queue [0x%lx, 0x%lx) ", __func__, 
                                                                  hrm_index, 
                                                                  (size_t)_sync_mem_cpu->_cross_region_ref_target_queue, 
                                                                  (size_t)CHeapRDMAObj<size_t, ALLOC_TARGET_OBJ_QUEUE_ALLOCTYPE>::_alloc_ptr );

}


//
// Degrade these G1Congtiguous functions to sub-class, HeapRegion.
//


// G1OffsetTableContigSpace code; copied from space.cpp.  Hope this can go
// away eventually.

void HeapRegion::clear(bool mangle_space) {
  set_top(bottom());
  CompactibleSpace::clear(mangle_space);
  reset_bot();
}
#ifndef PRODUCT
void HeapRegion::mangle_unused_area() {
  mangle_unused_area_complete();
}

void HeapRegion::mangle_unused_area_complete() {
  SpaceMangler::mangle_region(MemRegion(top(), end()));
}
#endif

// void HeapRegion::print() const {
//   print_short();
//   tty->print_cr(" [" INTPTR_FORMAT ", " INTPTR_FORMAT ", "
//                 INTPTR_FORMAT ", " INTPTR_FORMAT ")",
//                 p2i(bottom()), p2i(top()), p2i(_sync_mem_cpu->_bot_part.threshold()), p2i(end()));
// }

HeapWord* HeapRegion::initialize_threshold() {
  return _sync_mem_cpu->_bot_part.initialize_threshold();
}

HeapWord* HeapRegion::cross_threshold(HeapWord* start,
                                                    HeapWord* end) {
  _sync_mem_cpu->_bot_part.alloc_block(start, end);
  return _sync_mem_cpu->_bot_part.threshold();
}

void HeapRegion::safe_object_iterate(ObjectClosure* blk) {
  object_iterate(blk);
}

void HeapRegion::object_iterate(ObjectClosure* blk) {
  HeapWord* p = bottom();
  while (p < top()) {
    if (block_is_obj(p)) {
      blk->do_object(oop(p));
    }
    p += block_size(p);
  }
}


//mhr: modify
void HeapRegion::send_info_at_gc(){
  
  int target_mem_id;

  target_mem_id = region_to_memory_server_mapping();

  // 1) Region basi information
  log_debug(semeru,rdma)("Write CPUToMemoryAtGC 0x%lx , class size 0x%lx to Memory Server[%d] ", 
                            (size_t)_cpu_to_mem_gc , (size_t)(sizeof(CPUToMemoryAtGC)), target_mem_id );
  syscall(RDMA_WRITE, target_mem_id, _cpu_to_mem_gc, sizeof(CPUToMemoryAtGC));
    
  // 2) Control the Memory server gc behavior. e.g. reset the  _cm_scanned to enable GC.
  log_debug(semeru,rdma)("Write MemoryToCPUAtGC 0x%lx , class size 0x%lx to Memory Server[%d] ", 
                            (size_t)_mem_to_cpu_gc , (size_t)(sizeof(MemoryToCPUAtGC)), target_mem_id );
  syscall(RDMA_WRITE, target_mem_id, _mem_to_cpu_gc, sizeof(MemoryToCPUAtGC));
  
  // 3) e.g. Region usage and allocation information
  log_debug(semeru,rdma)("Write SyncBetweenMemoryAndCPU 0x%lx , class size 0x%lx to Memory Server[%d]", 
                            (size_t)_sync_mem_cpu , (size_t)(sizeof(SyncBetweenMemoryAndCPU)), target_mem_id );
  syscall(RDMA_WRITE, target_mem_id, _sync_mem_cpu, sizeof(SyncBetweenMemoryAndCPU));

    // Send the offset array of _sync_mem_cpu->_bot_part->_offset_array_part
    // 1 byte for a card, 512 bytes
  log_debug(semeru,rdma)("  Write SyncBetweenMemoryAndCPU->_bot_part->_offset_array_part 0x%lx, size 0x%lx \n", 
                                                                                    (size_t)_sync_mem_cpu->_bot_part.offset_array_part(), 
                                                                                    _sync_mem_cpu->_bot_part.offset_array_part_length() );
  syscall(RDMA_WRITE, target_mem_id, _sync_mem_cpu->_bot_part.offset_array_part(), _sync_mem_cpu->_bot_part.offset_array_part_length());
	
}

//mhr: modify
void HeapRegion::send_remset_at_gc(){

	//mhr: TODO
}
//mhr: modify
void HeapRegion::send_target_queue_at_gc(){

  int target_mem_id = region_to_memory_server_mapping();


  BitQueue* tq = _sync_mem_cpu->_cross_region_ref_target_queue;
	log_debug(semeru,rdma)("Write CrossRegionTargetQueue 0x%lx , size 0x%lx to Memory Server[%d]", 
	 																  (size_t)_sync_mem_cpu->_cross_region_ref_target_queue , 
                                    (size_t)(align_up(sizeof(BitQueue), PAGE_SIZE)+CROSS_REGION_REF_TARGET_Q_LEN*sizeof(size_t)),
                                    target_mem_id );
  
  // log_debug(semeru,rdma)("CrossRegionTarQueue[0x%lx]  size: 0x%lx",tq->_region_index,  tq->_length );





  // Debug
  //check_cross_region_reg_queue(this, "Check Region[0x7] before send.");
	// Chenxi
	#ifdef ASSERT
	// Valid the content of the TargetObjQueue
	// pop an item and then push it back.
	// StarTask ref;
	// if(tq->pop_local(ref, 0)) { 
	// 	log_debug(semeru,rdma)("%s, StarTask._holder : 0x%lx \n", __func__, (size_t)(oop*)ref );
	// 	oop const obj = RawAccess<MO_VOLATILE>::oop_load((oop*)ref);
	// 	if(obj!= NULL){
	// 		log_debug(semeru,rdma)("%s, obj 0x%lx, klass 0x%lx, layout_helper 0x%x. is TypeArray ? %d \n",
	// 									__func__, (size_t)obj, (size_t)obj->klass(), obj->klass()->layout_helper(), obj->is_typeArray()  );
	// 	}

	// 	tq->push(ref); //push back
	// }
	#endif
  //printf("TargetObjQueue size: %u\n", tq->bottom());



  syscall(RDMA_WRITE,target_mem_id, _sync_mem_cpu->_cross_region_ref_target_queue, align_up(sizeof(BitQueue), PAGE_SIZE)+CROSS_REGION_REF_TARGET_Q_LEN*sizeof(size_t));
}


//Chenxi
void HeapRegion::read_info_at_gc(){
  ShouldNotReachHere();

  // #1 read _sync_mem_cpu
	log_debug(semeru,rdma)("Read SyncBetweenMemoryAndCPU 0x%lx , class size 0x%lx to Memory Server", (size_t)_sync_mem_cpu , (size_t)(sizeof(SyncBetweenMemoryAndCPU)) );
	syscall(RDMA_READ, 0, _sync_mem_cpu, sizeof(SyncBetweenMemoryAndCPU));


  //#2, read cross_region_ref queue -- useless now. For Memory server compation.
	// log_debug(semeru,rdma)("Read CrossRegionRegQueue 0x%lx , size 0x%lx to Memory Server", 
	// 																(size_t)_sync_mem_cpu->_cross_region_ref_update_queue , (size_t)(align_up(sizeof(HashQueue), PAGE_SIZE)+CROSS_REGION_REF_UPDATE_Q_LEN*24) );
  //TargetObjQueue* tq = _cpu_to_mem_gc->_target_obj_queue;
  // HashQueue* tq = _sync_mem_cpu->_cross_region_ref_update_queue;
  // log_debug(semeru,rdma)("CrossRegionRegQueue[0x%lx]  size: 0x%lx",tq->_region_index,  tq->_length );

  // [?] Useless for now?
  // //printf("TargetObjQueue size: %u\n", tq->bottom());
  //syscall(RDMA_READ, _sync_mem_cpu->_cross_region_ref_update_queue, align_up(sizeof(HashQueue), PAGE_SIZE)+CROSS_REGION_REF_UPDATE_Q_LEN*24);


  //check_cross_region_reg_queue(this, "Check after read.");
}


//mhr: modify
void HeapRegion::read_info_before_gc(){

  int target_mem_id = region_to_memory_server_mapping();

  // #1 read _mem_to_cpu
	log_debug(semeru,rdma)("Read MemoryToCPUAtGC 0x%lx , class size 0x%lx to Memory Server[%d]", 
                              (size_t)_mem_to_cpu_gc , (size_t)(sizeof(MemoryToCPUAtGC)), target_mem_id);
  syscall(RDMA_READ, target_mem_id, _mem_to_cpu_gc, sizeof(MemoryToCPUAtGC));

}



//mhr: modify
// [?] Each Region can only be flushed by one thread, 
// Should be flushed by gc threads ? Mutators must be suspended ?
void HeapRegion::flush_data(){
  int ret = 0;
  int target_mem_id = region_to_memory_server_mapping();

	log_debug(semeru,rdma)("Write Region[%u] , addr 0x%lx, sent size 0x%lx to Memory Server[%d]", 
                        this->hrm_index(),  (size_t)bottom() , (size_t)GrainBytes, target_mem_id );
  
  //debug
  //check_sync_between_memory_and_cpu("Check Region before sent");
  // [?]Run Control Path with Data Path together can cause CPU server crash.
  //    And multiple QP can lead to a much higher posibility ??
  ret = syscall(RDMA_WRITE, target_mem_id, bottom(), GrainBytes);  
  if(ret){
    tty->print("%s, RDMA write for region[%u] to memory server[%d] failed. Crash here. \n", __func__, this->hrm_index(),target_mem_id);
    guarantee(false," RDMA write failed." );
  }

}








/**
 * Just print, not pop any items. 
 */
void HeapRegion::check_cross_region_reg_queue( HeapRegion* hr,  const char* message){
	size_t length = hr->cross_region_ref_update_queue()->length();
	HashQueue* cross_region_ref_update_queue = hr->cross_region_ref_update_queue();
	ElemPair* q_iter;

	tty->print("%s,check_cross_region_reg_queue, Start for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

	// for(i=0; i < length; i++){
	// 	q_iter = cross_region_reg_queue->retrieve_item(i);
	// 	if(q_iter->from != NULL){
	// 		if(hr->is_in_reserved(q_iter->from) == false ){
	// 			tty->print("	Wong obj 0x%lx in Region[0x%lx]'s cross_region_reg_queue \n", (size_t)q_iter->from , (size_t)hr->hrm_index() );
	// 		}
  //     //printf("Send obj 0x%lx in region 0x%lx to memory server\n",  (size_t)q_iter->from, (size_t)hr->hrm_index());
  //     if(q_iter->from->size()<2) {
  //       printf("Size < 2!\n");
  //     }
  //     // else{
	// 		// 	tty->print("	non-null item[0x%lx] from 0x%lx, to 0x%lx \n", i, (size_t)q_iter->from, (size_t)q_iter->to );
	// 		// }
	// 	}

	// }

  BitQueue* cross_region_ref_target_queue = hr->cross_region_ref_target_queue();
	
  for(size_t i = 0; i < cross_region_ref_target_queue->_heap_words/64; i ++){
    size_t val, p;
    size_t bitmap_st = cross_region_ref_update_queue->bitmap_st;
    val = cross_region_ref_update_queue->g1hbitmap[bitmap_st + i];
    p = cross_region_ref_target_queue->_target_bitmap[i];
    // for(uint j = 0 ; j < 64; j += 2){
    //   p = p|((val&(1<<j))>>(j/2));
    // }
    // val = cross_region_ref_update_queue->g1hbitmap[bitmap_st + i*2 + 1];
    // for(uint j = 0 ; j < 64; j += 2){
    //   p = p | ( ((val&(1<<j)>>(j/2))) << 32 );
    // }
    if(((val ^ p) & p) != (val ^ p) ) {
      tty->print("	Wrong here in Region[0x%lx]'s cross_region_queue, values are 0x%lx, 0x%lx, %lx\n", (size_t)hr->hrm_index(), val, cross_region_ref_target_queue->_target_bitmap[i], (val ^ p));
    }
  }

	tty->print("%s,check_cross_region_reg_queue, End for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

}



void  HeapRegion::check_sync_between_memory_and_cpu(const char* message){
  ShouldNotReachHere();
  HeapRegion* hr = this;

	tty->print("%s,check_sync_between_memory_and_cpu, Start for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

  // Check each fields
  tty->print("  addr of HeapRegion: 0x%lx: \n", (size_t)hr);

  // Filed of sync_between_memory_and_cpu
  tty->print("  addr of HeapRegion->_sync_mem_cpu: 0x%lx \n", (size_t)hr->_sync_mem_cpu);
  tty->print(" addr of _top 0x%lx, value of _top 0x%lx \n", (size_t)&(hr->_sync_mem_cpu->_top), (size_t)hr->_sync_mem_cpu->_top);
  tty->print(" addr of _bot_part 0x%lx, value of _bot_part->_bot 0x%lx, value of _bot_part->space 0x%lx. \n", 
                        (size_t)&(hr->_sync_mem_cpu->_bot_part),  (size_t)hr->_sync_mem_cpu->_bot_part._bot, (size_t)hr->_sync_mem_cpu->_bot_part._space);
  tty->print(" addr of _cross_region_ref_update_queue 0x%lx, value of _cross_region_ref_update_queue 0x%lx \n", 
                                  (size_t)&(hr->_sync_mem_cpu->_cross_region_ref_update_queue), (size_t)hr->_sync_mem_cpu->_cross_region_ref_update_queue);

  tty->print("%s,check_sync_between_memory_and_cpu, End for Region[0x%lx] \n", message, (size_t)hr->hrm_index() );

}