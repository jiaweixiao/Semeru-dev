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

#ifndef SHARE_VM_GC_G1_SEMERU_HEAPREGION_INLINE_HPP
#define SHARE_VM_GC_G1_SEMERU_HEAPREGION_INLINE_HPP

//#include "gc/g1/g1BlockOffsetTable.inline.hpp"
//#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentMarkBitMap.inline.hpp"
//#include "gc/g1/heapRegion.hpp"
#include "gc/shared/space.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/align.hpp"


// Semeru
#include "gc/g1/heapRegion.inline.hpp"		// Use some functions defined in here.
#include "gc/g1/SemeruHeapRegion.hpp"
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/g1SemeruBlockOffsetTable.inline.hpp"


//
// Functions for G1SemeruContiguousSpace
//





// Degrade functions from G1SemeruContiguousSpace to sub-class, G1SmeruHeapRegion.
//

inline HeapWord* SemeruHeapRegion::block_start(const void* p) {
	return _sync_mem_cpu->_bot_part.block_start(p);
}

inline HeapWord* SemeruHeapRegion::block_start_const(const void* p) const {
	return _sync_mem_cpu->_bot_part.block_start_const(p);
}





/**
 * Tag : allocate object into a G1 Heap region
 * 
 * [?] Is this region thread local ??
 * 
 * Can't find any lock ?
 * 
 * 
 * x.Region Architecture:
 * G1SemeruContiguousSpace   ----> CompactibleSpace  ---> Space  --> CHeapObj
 *  =>top(Allocate pointer)                          =>_bottom
 *                                                   =>_end
 * 
 */
inline HeapWord* SemeruHeapRegion::allocate_impl(size_t min_word_size,
																									size_t desired_word_size,
																									size_t* actual_size) {
	HeapWord* obj = top();
	size_t available = pointer_delta(end(), obj);
	size_t want_to_allocate = MIN2(available, desired_word_size);
	if (want_to_allocate >= min_word_size) {
		HeapWord* new_top = obj + want_to_allocate;
		set_top(new_top);
		assert(is_aligned(obj) && is_aligned(new_top), "checking alignment");
		*actual_size = want_to_allocate;
		return obj;
	} else {
		return NULL;
	}
}


/**
 * Tag : concurrent object allocation in a G1 Heap Region
 * 		=> Allocate into current Region, or return NULL.
 * While loop + Atomic::cmpxchg(new_val, mem/dest_var, cmp/old_val)
 * 
 */
inline HeapWord* SemeruHeapRegion::par_allocate_impl(size_t min_word_size,
																											size_t desired_word_size,
																											size_t* actual_size) {
	do {
		HeapWord* obj = top();
		size_t available = pointer_delta(end(), obj);
		size_t want_to_allocate = MIN2(available, desired_word_size);
		if (want_to_allocate >= min_word_size) {
			HeapWord* new_top = obj + want_to_allocate;
			HeapWord* result = Atomic::cmpxchg(new_top, top_addr(), obj);
			// result can be one of two:
			//  the old top value: the exchange succeeded
			//  otherwise: the new value of the top is returned.
			if (result == obj) {
				assert(is_aligned(obj) && is_aligned(new_top), "checking alignment");
				*actual_size = want_to_allocate;
				return obj;
			}
		} else {
			return NULL;
		}
	} while (true);
}

inline HeapWord* SemeruHeapRegion::allocate(size_t min_word_size,
																						 size_t desired_word_size,
																						 size_t* actual_size) {
	HeapWord* res = allocate_impl(min_word_size, desired_word_size, actual_size);
	if (res != NULL) {
		_sync_mem_cpu->_bot_part.alloc_block(res, *actual_size);
	}
	return res;
}

inline HeapWord* SemeruHeapRegion::allocate(size_t word_size) {
	size_t temp;
	return allocate(word_size, word_size, &temp);
}

inline HeapWord* SemeruHeapRegion::par_allocate(size_t word_size) {
	size_t temp;
	return par_allocate(word_size, word_size, &temp);
}

// Because of the requirement of keeping "_offsets" up to date with the
// allocations, we sequentialize these with a lock.  Therefore, best if
// this is used for larger LAB allocations only.
inline HeapWord* SemeruHeapRegion::par_allocate(size_t min_word_size,
																								 size_t desired_word_size,
																								 size_t* actual_size) {
	MutexLocker x(&_par_alloc_lock);
	return allocate(min_word_size, desired_word_size, actual_size);
}



//
// Functions for SemeruHeapRegion
//



inline bool SemeruHeapRegion::is_obj_dead_with_size(const oop obj, const G1CMBitMap* const prev_bitmap, size_t* size) const {
	HeapWord* addr = (HeapWord*) obj;

	assert(addr < top(), "must be");
	assert(!is_closed_archive(),
				 "Closed archive regions should not have references into other regions");
	assert(!is_humongous(), "Humongous objects not handled here");
	bool obj_is_dead = is_obj_dead(obj, prev_bitmap);

	if (ClassUnloadingWithConcurrentMark && obj_is_dead) {
		assert(!block_is_obj(addr), "must be");
		*size = block_size_using_bitmap(addr, prev_bitmap);
	} else {
		assert(block_is_obj(addr), "must be");
		*size = obj->size();
	}
	return obj_is_dead;
}

inline bool
SemeruHeapRegion::block_is_obj(const HeapWord* p) const {
	G1SemeruCollectedHeap* g1h = G1SemeruCollectedHeap::heap();

	if (!this->is_in(p)) {
		assert(is_continues_humongous(), "This case can only happen for humongous regions");
		return (p == humongous_start_region()->bottom());
	}
	if (ClassUnloadingWithConcurrentMark) {
		return !g1h->is_obj_dead(oop(p), this);
	}
	return p < top();
}

inline size_t SemeruHeapRegion::block_size_using_bitmap(const HeapWord* addr, const G1CMBitMap* const prev_bitmap) const {
	assert(ClassUnloadingWithConcurrentMark,
				 "All blocks should be objects if class unloading isn't used, so this method should not be called. "
				 "HR: [" PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT ") "
				 "addr: " PTR_FORMAT,
				 p2i(bottom()), p2i(top()), p2i(end()), p2i(addr));

	// Old regions' dead objects may have dead classes
	// We need to find the next live object using the bitmap
	HeapWord* next = prev_bitmap->get_next_marked_addr(addr, prev_top_at_mark_start());

	assert(next > addr, "must get the next live object");
	return pointer_delta(next, addr);
}

inline bool SemeruHeapRegion::is_obj_dead(const oop obj, const G1CMBitMap* const prev_bitmap) const {
	assert(is_in_reserved(obj), "Object " PTR_FORMAT " must be in region", p2i(obj));
	return !obj_allocated_since_prev_marking(obj) &&
				 !prev_bitmap->is_marked((HeapWord*)obj) &&
				 !is_open_archive();
}

inline size_t SemeruHeapRegion::block_size(const HeapWord *addr) const {
	if (addr == top()) {
		return pointer_delta(end(), addr);
	}

	if (block_is_obj(addr)) {
		return oop(addr)->size();
	}

	return block_size_using_bitmap(addr, G1SemeruCollectedHeap::heap()->concurrent_mark()->prev_mark_bitmap());
}

inline void SemeruHeapRegion::complete_compaction() {
	// Reset space and bot after compaction is complete if needed.
	reset_after_compaction();  // _top to _compaction_top.
	if (used_region().is_empty()) {
		reset_bot();
	}

	// After a compaction the mark bitmap is invalid, so we must
	// treat all objects as being inside the unmarked area.
	zero_marked_bytes();
	init_top_at_mark_start();

	// Clear unused heap memory in debug builds.
	if (ZapUnusedHeapArea) {
		mangle_unused_area();
	}
}



/**
 * Semeru MS : Apply a Closure to the target Regions.
 * e.g.
 *  The colusre can be :
 * 	1)  G1SemeruPrepareCompactLiveClosure, put forwarding pointer at the MarkOop of the alive objects.
 * 	2) Evacuate alive objects to desetination according to the bitmap.
 * 			 This phase is purely object data copy. no pointer adjustment, no RemSet update. 
 * 
 * 
 * 	[?] Destination choosing ? 
 * 		=> defiend in Full GC's another phase. And already put the object's destination in its markOop.
 * 	[?] If this is the case, how to do parallel copy. Because it may overwrite the forwarding pointer ?
 * 
 * 
 * [x] How to asign value to the <ApplyToMarkedClosure>
 * 		=> This function will be inlined the caller.
 * 			 So the <typename ApplyToMarkedClosure> will be asigned the parameter, closure, 's class type.
 */
template<typename ApplyToMarkedClosure>
inline void SemeruHeapRegion::apply_to_marked_objects(G1CMBitMap* bitmap, ApplyToMarkedClosure* closure) {
	HeapWord* limit = scan_limit();		// current Region top
	HeapWord* next_addr = bottom();		// from Region start

	while (next_addr < limit) {
		Prefetch::write(next_addr, PrefetchScanIntervalInBytes);  
		// This explicit is_marked check is a way to avoid
		// some extra work done by get_next_marked_addr for
		// the case where next_addr is marked.
		if (bitmap->is_marked(next_addr)) {  // oop is marked, means this object is alive during the tracing ?
			oop current = oop(next_addr);
			next_addr += closure->apply(current);		// the apply() function is defiend by the parameter, closure.
		} else {
			next_addr = bitmap->get_next_marked_addr(next_addr, limit);  // next alive objects. 
		}
	}

	assert(next_addr == limit, "Should stop the scan at the limit.");
}




inline HeapWord* SemeruHeapRegion::par_allocate_no_bot_updates(size_t min_word_size,
																												 size_t desired_word_size,
																												 size_t* actual_word_size) {
	assert(is_young(), "we can only skip BOT updates on young regions");
	return par_allocate_impl(min_word_size, desired_word_size, actual_word_size);
}

inline HeapWord* SemeruHeapRegion::allocate_no_bot_updates(size_t word_size) {
	size_t temp;
	return allocate_no_bot_updates(word_size, word_size, &temp);
}

inline HeapWord* SemeruHeapRegion::allocate_no_bot_updates(size_t min_word_size,
																										 size_t desired_word_size,
																										 size_t* actual_word_size) {
	assert(is_young(), "we can only skip BOT updates on young regions");
	return allocate_impl(min_word_size, desired_word_size, actual_word_size);
}

inline void SemeruHeapRegion::note_start_of_marking() {
	_next_marked_bytes = 0;
	_next_top_at_mark_start = top();
}

inline void SemeruHeapRegion::note_end_of_marking() {
	_prev_top_at_mark_start = _next_top_at_mark_start;
	_next_top_at_mark_start = bottom();
	_prev_marked_bytes = _next_marked_bytes;
	_next_marked_bytes = 0;
}

inline bool SemeruHeapRegion::in_collection_set() const {
	return G1SemeruCollectedHeap::heap()->is_in_cset(this);
}

// Test if this object is in current Region.
// 1) The value of oop is the start address of the object, count in HeapWords.
// 2) The object can't span 2 SemeruHeapRegion in Semeru.
// 3) Check if the SemeruHeapRegion->_bottom and obj are in same SemeruHeapRegion.
// inline bool SemeruHeapRegion::is_in_heapregion(oop obj){
//   return (((uintptr_t)this->bottom() ^ cast_from_oop<uintptr_t>(obj)  ) >> LogOfHRGrainWords) == 0;
// }

// Use the function heapRegion->is_in_reserved(void* oop)



template <class Closure, bool is_gc_active>
bool SemeruHeapRegion::do_oops_on_card_in_humongous(MemRegion mr,
																							Closure* cl,
																							G1SemeruCollectedHeap* g1h) {
	assert(is_humongous(), "precondition");
	SemeruHeapRegion* sr = humongous_start_region();
	oop obj = oop(sr->bottom());

	// If concurrent and klass_or_null is NULL, then space has been
	// allocated but the object has not yet been published by setting
	// the klass.  That can only happen if the card is stale.  However,
	// we've already set the card clean, so we must return failure,
	// since the allocating thread could have performed a write to the
	// card that might be missed otherwise.
	if (!is_gc_active && (obj->klass_or_null_acquire() == NULL)) {
		return false;
	}

	// We have a well-formed humongous object at the start of sr.
	// Only filler objects follow a humongous object in the containing
	// regions, and we can ignore those.  So only process the one
	// humongous object.
	if (!g1h->is_obj_dead(obj, sr)) {
		if (obj->is_objArray() || (sr->bottom() < mr.start())) {
			// objArrays are always marked precisely, so limit processing
			// with mr.  Non-objArrays might be precisely marked, and since
			// it's humongous it's worthwhile avoiding full processing.
			// However, the card could be stale and only cover filler
			// objects.  That should be rare, so not worth checking for;
			// instead let it fall out from the bounded iteration.
			obj->oop_iterate(cl, mr);
		} else {
			// If obj is not an objArray and mr contains the start of the
			// obj, then this could be an imprecise mark, and we need to
			// process the entire object.
			obj->oop_iterate(cl);
		}
	}
	return true;
}


/**
 * Tag : Scan a specific card(Dirty Card) of this SemeruHeapRegion
 *  
 * The real action of handling the oop is based on the Closure definition.
 * 
 * [x] Confirm this heap region follow the rules:
 * 		parsable, // not stale
 * 		old region,
 * 		card is valid,
 * 				=> Not exceed SemeruHeapRegion->scan_limit
 * 				=> card is dirty
 */
template <bool is_gc_active, class Closure>
bool SemeruHeapRegion::oops_on_card_seq_iterate_careful(MemRegion mr,
																									Closure* cl) {
	assert(MemRegion(bottom(), end()).contains(mr), "Card region not in heap region");
	G1SemeruCollectedHeap* g1h = G1SemeruCollectedHeap::heap();

	// Special handling for humongous regions.
	if (is_humongous()) {
		return do_oops_on_card_in_humongous<Closure, is_gc_active>(mr, cl, g1h);
	}
	assert(is_old() || is_archive(), "Wrongly trying to iterate over region %u type %s", _cpu_to_mem_init->_hrm_index, get_type_str());

	// Because mr has been trimmed to what's been allocated in this
	// region, the parts of the heap that are examined here are always
	// parsable; there's no need to use klass_or_null to detect
	// in-progress allocation.

	// Cache the boundaries of the memory region in some const locals
	HeapWord* const start = mr.start();
	HeapWord* const end = mr.end();

	// Find the obj that extends onto mr.start().
	// Update BOT as needed while finding start of (possibly dead)
	// object containing the start of the region.
	HeapWord* cur = block_start(start);

#ifdef ASSERT
	{
		assert(cur <= start,
					 "cur: " PTR_FORMAT ", start: " PTR_FORMAT, p2i(cur), p2i(start));
		HeapWord* next = cur + block_size(cur);
		assert(start < next,
					 "start: " PTR_FORMAT ", next: " PTR_FORMAT, p2i(start), p2i(next));
	}
#endif
	//[?] use pre_mark_bitmap to detect if the object is dead ?
	//    pre_mark_bitmap is the results of last cm marking ?
	const G1CMBitMap* const bitmap = g1h->concurrent_mark()->prev_mark_bitmap(); 
	do {
		oop obj = oop(cur);
		assert(oopDesc::is_oop(obj, true), "Not an oop at " PTR_FORMAT, p2i(cur));   // Debug, confirm this card isn't stale. oop are parsable.
		assert(obj->klass_or_null() != NULL,
					 "Unparsable heap at " PTR_FORMAT, p2i(cur));

		size_t size;
		bool is_dead = is_obj_dead_with_size(obj, bitmap, &size);

		cur += size;
		if (!is_dead) {
			// Process live object's references.

			// Non-objArrays are usually marked imprecise at the object
			// start, in which case we need to iterate over them in full.
			// objArrays are precisely marked, but can still be iterated
			// over in full if completely covered.
			if (!obj->is_objArray() || (((HeapWord*)obj) >= start && cur <= end)) {
				obj->oop_iterate(cl);
			} else {
				obj->oop_iterate(cl, mr);
			}
		}
	} while (cur < end);

	return true;
}

#endif // SHARE_VM_GC_G1_HEAPREGION_INLINE_HPP
