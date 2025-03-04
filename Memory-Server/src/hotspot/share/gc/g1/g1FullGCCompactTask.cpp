/*
 * Copyright (c) 2017, 2018 Oracle and/or its affiliates. All rights reserved.
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
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
#include "gc/g1/g1FullCollector.hpp"
#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/g1FullGCCompactTask.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "logging/log.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/ticks.hpp"

class G1ResetHumongousClosure : public HeapRegionClosure {
  G1CMBitMap* _bitmap;

public:
  G1ResetHumongousClosure(G1CMBitMap* bitmap) :
      _bitmap(bitmap) { }

  bool do_heap_region(HeapRegion* current) {
    if (current->is_humongous()) {
      if (current->is_starts_humongous()) {
        oop obj = oop(current->bottom());
        if (_bitmap->is_marked(obj)) {
          // Clear bitmap and fix mark word.
          _bitmap->clear(obj);
          obj->init_mark_raw();   // [?] What's the purpose for this marking ?
        } else {
          assert(current->is_empty(), "Should have been cleared in phase 2.");
        }
      }
      current->reset_during_compaction();   // [?] purpose ?
    }
    return false;
  }
};


/**
 * Tag : Full-GC uses the forwarding pointer to do the object copy ??
 *        Put the destination into alive object's header as forwarding pointer. 
 * 
 *  [?] When and where put the forwarding value ?
 *    
 *  [?] Is this MT safe ?
 * 
 */
size_t G1FullGCCompactTask::G1CompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  HeapWord* destination = (HeapWord*)obj->forwardee();   // [?] already moved
  if (destination == NULL) {
    // Object not moving
    return size;
  }

  // copy object and reinit its mark
  HeapWord* obj_addr = (HeapWord*) obj;
  assert(obj_addr != destination, "everything in this pass should be moving");
  Copy::aligned_conjoint_words(obj_addr, destination, size);      // 4 bytes alignment copy ?
  oop(destination)->init_mark_raw();    // initialize the MarkOop.
  assert(oop(destination)->klass() != NULL, "should have a class");

  return size;
}

/**
 * The main entry of compacting a Region.
 *   
 *  [?] Use the bitmap to do Region based compaction. 
 * 
 * 
 */
void G1FullGCCompactTask::compact_region(HeapRegion* hr) {
  assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");
  G1CompactRegionClosure compact(collector()->mark_bitmap());
  hr->apply_to_marked_objects(collector()->mark_bitmap(), &compact);  // Do the evacuation according to the next_mark_bitmap.
  // Once all objects have been moved the liveness information
  // needs be cleared.
  collector()->mark_bitmap()->clear_region(hr);
  hr->complete_compaction();
}

void G1FullGCCompactTask::work(uint worker_id) {
  Ticks start = Ticks::now();
  GrowableArray<HeapRegion*>* compaction_queue = collector()->compaction_point(worker_id)->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }

  G1ResetHumongousClosure hc(collector()->mark_bitmap());
  G1CollectedHeap::heap()->heap_region_par_iterate_from_worker_offset(&hc, &_claimer, worker_id);
  log_task("Compaction task", worker_id, start);
}

void G1FullGCCompactTask::serial_compaction() {
  GCTraceTime(Debug, gc, phases) tm("Phase 4: Serial Compaction", collector()->scope()->timer());
  GrowableArray<HeapRegion*>* compaction_queue = collector()->serial_compaction_point()->regions();
  for (GrowableArrayIterator<HeapRegion*> it = compaction_queue->begin();
       it != compaction_queue->end();
       ++it) {
    compact_region(*it);
  }
}
