/*
 * Copyright (c) 2016, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_G1_G1COLLECTIONSET_HPP
#define SHARE_VM_GC_G1_G1COLLECTIONSET_HPP

#include "gc/g1/collectionSetChooser.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class G1CollectedHeap;
class G1CollectorState;
class G1GCPhaseTimes;
class G1ParScanThreadStateSet;
class G1Policy;
class G1SurvivorRegions;
class HeapRegion;

class G1CollectionSet {
  G1CollectedHeap* _g1h;
  G1Policy* _policy;

  CollectionSetChooser* _cset_chooser;

  uint _eden_region_length;
  uint _survivor_region_length;

  //mhr: modify
public:
  uint _old_region_length;

  // The actual collection set as a set of region indices.
  // All entries in _collection_set_regions below _collection_set_cur_length are
  // assumed to be valid entries.
  // We assume that at any time there is at most only one writer and (one or more)
  // concurrent readers. This means we are good with using storestore and loadload
  // barriers on the writer and reader respectively only.
  uint* _collection_set_regions;
  volatile size_t _collection_set_cur_length;
  size_t _collection_set_max_length;
  size_t _rebuild_set_length;

  // When doing mixed collections we can add old regions to the collection, which
  // can be collected if there is enough time. We call these optional regions and
  // the pointer to these regions are stored in the array below.
  HeapRegion** _optional_regions;
  uint _optional_region_length;
  uint _optional_region_max_length;
private:
  // The number of bytes in the collection set before the pause. Set from
  // the incrementally built collection set at the start of an evacuation
  // pause, and incremented in finalize_old_part() when adding old regions
  // (if any) to the collection set.
  size_t _bytes_used_before;

  size_t _recorded_rs_lengths;

  // The associated information that is maintained while the incremental
  // collection set is being built with young regions. Used to populate
  // the recorded info for the evacuation pause.

  enum CSetBuildType {
    Active,             // We are actively building the collection set
    Inactive            // We are not actively building the collection set
  };

  CSetBuildType _inc_build_state;

  // The number of bytes in the incrementally built collection set.
  // Used to set _collection_set_bytes_used_before at the start of
  // an evacuation pause.
  size_t _inc_bytes_used_before;

  // The RSet lengths recorded for regions in the CSet. It is updated
  // by the thread that adds a new region to the CSet. We assume that
  // only one thread can be allocating a new CSet region (currently,
  // it does so after taking the Heap_lock) hence no need to
  // synchronize updates to this field.
  size_t _inc_recorded_rs_lengths;

  // A concurrent refinement thread periodically samples the young
  // region RSets and needs to update _inc_recorded_rs_lengths as
  // the RSets grow. Instead of having to synchronize updates to that
  // field we accumulate them in this field and add it to
  // _inc_recorded_rs_lengths_diffs at the start of a GC.
  ssize_t _inc_recorded_rs_lengths_diffs;

  // The predicted elapsed time it will take to collect the regions in
  // the CSet. This is updated by the thread that adds a new region to
  // the CSet. See the comment for _inc_recorded_rs_lengths about
  // MT-safety assumptions.
  double _inc_predicted_elapsed_time_ms;

  // See the comment for _inc_recorded_rs_lengths_diffs.
  double _inc_predicted_elapsed_time_ms_diffs;

  G1CollectorState* collector_state();
  G1GCPhaseTimes* phase_times();

  void verify_young_cset_indices() const NOT_DEBUG_RETURN;
  void add_as_optional(HeapRegion* hr);
  void add_as_old(HeapRegion* hr);
  bool optional_is_full();

  //mhr: modify
  size_t cache_ratio_pages(HeapRegion* hr);

public:
  G1CollectionSet(G1CollectedHeap* g1h, G1Policy* policy);
  ~G1CollectionSet();

  //mhr: modify
  //records the survivor regions which may not be collected
  uint* _original_survivor_regions;
  volatile size_t _survivor_set_cur_length;


  // Initializes the collection set giving the maximum possible length of the collection set.
  void initialize(uint max_region_length);
  void initialize_optional(uint max_length);
  void free_optional_regions();

  CollectionSetChooser* cset_chooser();

  void init_region_lengths(uint eden_cset_region_length,
                           uint survivor_cset_region_length);

  void set_recorded_rs_lengths(size_t rs_lengths);

  uint region_length() const       { return young_region_length() +
                                            old_region_length(); }
  uint young_region_length() const { return eden_region_length() +
                                            survivor_region_length(); }

  uint eden_region_length() const     { return _eden_region_length;     }
  uint survivor_region_length() const { return _survivor_region_length; }
  uint old_region_length() const      { return _old_region_length;      }
  uint optional_region_length() const { return _optional_region_length; }

  // Incremental collection set support

  // Initialize incremental collection set info.
  void start_incremental_building();

  // Perform any final calculations on the incremental collection set fields
  // before we can use them.
  void finalize_incremental_building();

  // Reset the contents of the collection set.
  void clear();

  // Iterate over the collection set, applying the given HeapRegionClosure on all of them.
  // If may_be_aborted is true, iteration may be aborted using the return value of the
  // called closure method.
  void iterate(HeapRegionClosure* cl) const;

  // Iterate over the collection set, applying the given HeapRegionClosure on all of them,
  // trying to optimally spread out starting position of total_workers workers given the
  // caller's worker_id.
  void iterate_from(HeapRegionClosure* cl, uint worker_id, uint total_workers) const;

  // Stop adding regions to the incremental collection set.
  void stop_incremental_building() { _inc_build_state = Inactive; }

  size_t recorded_rs_lengths() { return _recorded_rs_lengths; }

  size_t bytes_used_before() const {
    return _bytes_used_before;
  }

  void reset_bytes_used_before() {
    _bytes_used_before = 0;
  }

  // Choose a new collection set.  Marks the chosen regions as being
  // "in_collection_set".
  double finalize_young_part(double target_pause_time_ms, G1SurvivorRegions* survivors);
  void finalize_old_part(double time_remaining_ms);

  //mhr: modify
  //mhr: new
  void semeru_finalize_parts(G1SurvivorRegions* survivors);
  //void finalize_parts_with_ratio(G1SurvivorRegions* survivors);

  // Add old region "hr" to the collection set.
  void add_old_region(HeapRegion* hr);

  // Add old region "hr" to optional collection set.
  void add_optional_region(HeapRegion* hr);

  // Update information about hr in the aggregated information for
  // the incrementally built collection set.
  void update_young_region_prediction(HeapRegion* hr, size_t new_rs_length);

  // Add eden region to the collection set.
  void add_eden_region(HeapRegion* hr);

  // Add survivor region to the collection set.
  void add_survivor_regions(HeapRegion* hr);

#ifndef PRODUCT
  bool verify_young_ages();

  void print(outputStream* st);
#endif // !PRODUCT

  double predict_region_elapsed_time_ms(HeapRegion* hr);

  void clear_optional_region(const HeapRegion* hr);

  HeapRegion* optional_region_at(uint i) const {
    assert(_optional_regions != NULL, "Not yet initialized");
    assert(i < _optional_region_length, "index %u out of bounds (%u)", i, _optional_region_length);
    return _optional_regions[i];
  }

  HeapRegion* remove_last_optional_region() {
    assert(_optional_regions != NULL, "Not yet initialized");
    assert(_optional_region_length != 0, "No region to remove");
    _optional_region_length--;
    HeapRegion* removed = _optional_regions[_optional_region_length];
    _optional_regions[_optional_region_length] = NULL;
    return removed;
  }

  //mhr: modify
  static int compare_region_ages(const HeapRegion* a, const HeapRegion* b);

private:
  // Update the incremental collection set information when adding a region.
  void add_young_region_common(HeapRegion* hr);
};

// Helper class to manage the optional regions in a Mixed collection.
class G1OptionalCSet : public StackObj {
private:
  G1CollectionSet* _cset;
  G1ParScanThreadStateSet* _pset;
  uint _current_index;
  uint _current_limit;
  bool _prepare_failed;
  bool _evacuation_failed;

  void prepare_to_evacuate_optional_region(HeapRegion* hr);

public:
  static const uint InvalidCSetIndex = UINT_MAX;

  G1OptionalCSet(G1CollectionSet* cset, G1ParScanThreadStateSet* pset) :
    _cset(cset),
    _pset(pset),
    _current_index(0),
    _current_limit(0),
    _prepare_failed(false),
    _evacuation_failed(false) { }
  // The destructor returns regions to the cset-chooser and
  // frees the optional structure in the cset.
  ~G1OptionalCSet();

  uint current_index() { return _current_index; }
  uint current_limit() { return _current_limit; }

  uint size();
  bool is_empty();

  HeapRegion* region_at(uint index);

  // Prepare a set of regions for optional evacuation.
  void prepare_evacuation(double time_left_ms);
  bool prepare_failed();

  // Complete the evacuation of the previously prepared
  // regions by updating their state and check for failures.
  void complete_evacuation();
  bool evacuation_failed();
};

#endif // SHARE_VM_GC_G1_G1COLLECTIONSET_HPP

