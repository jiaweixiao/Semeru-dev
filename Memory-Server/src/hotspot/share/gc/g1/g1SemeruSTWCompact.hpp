/**
 * Do compaction during the CPU server's Stop-The-World window.
 * 
 * Define 
 * 	1) STW Compact Task
 *  2) Summary, Compact closures for this task
 * 
 *  
 */

#ifndef SHARE_VM_GC_G1_SEMERU_G1STWCOMPACT_HPP
#define SHARE_VM_GC_G1_SEMERU_G1STWCOMPACT_HPP

// gc module
#include "gc/g1/g1ConcurrentMarkBitMap.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1RegionMarkStatsCache.hpp"
#include "gc/g1/heapRegionSet.hpp"
#include "gc/shared/taskqueue.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/g1/heapRegion.inline.hpp"


// Full GC structures
#include "gc/g1/g1FullGCCompactionPoint.hpp"

// memory module
#include "memory/resourceArea.hpp"
#include "memory/allocation.hpp"





class ConcurrentGCTimer;
class G1CMOopClosure;
class G1OldTracer;
class G1RegionToSpaceMapper;
class G1SurvivorRegions;

// Semeru
class G1SemeruCollectedHeap;
class G1SemeruConcurrentMark;
class G1SemeruConcurrentMarkThread;
//class G1SemeruSTWTask;
class G1SemeruSTWCompactTask;
class G1SemeruCMCSetRegions;


// Preparation closures



// Adjust pointer closures
class G1SemeruAdjustRegionClosure;
class G1SemeruAdjustLiveClosure;
class G1SemeruAdjustClosure;


// Do object compaction closures







/**
 * Semeru Memory Server - Define the works of MS STW compact here.
 *  
 * 	[?] Why do we need such a structure ?
 * 		=> to contain all the related information here.
 * 			 Can get this information by G1SemeruColelctedHeap->_semeru_sc,
 * 			 All other thread handlers, tasks are created and assigned to this structure.
 * 
 */
class G1SemeruSTWCompact : public CHeapObj<mtGC> {
  friend class G1SemeruConcurrentMarkThread;          // Use the same thread of concurrent marking.
	friend class G1SemeruConcurrentMark;								// Reuse all the concurrent thread resource of CM.
  friend class G1CMBitMapClosure;                     // [?] The data copy is bit closure based.    
  friend class G1SemeruSTWCompactTask;								// STW Compact workGang
  // friend class G1SemeruSTWTask;												// the code task to be executed.			

  // [?] Seems that G1SemeruConcurrentMarkThread is only a manager of all the concurrent threads.
  //     The real concurrent threads are stored in _concurrent_workers.
  G1SemeruConcurrentMarkThread*     _semeru_cm_thread;    // The manager of all the concurrent threads
  G1SemeruCollectedHeap*            _semeru_h;            // The heap
  
  bool                              _completed_initialization; // Set to true when initialization is complete

  G1SemeruCMCSetRegions*   	        _mem_server_cset;		// points to G1SemeruConcurrentMark->_mem_server_cset

	// Thread related fields
	//
  uint                    _max_num_tasks;    		// Maximum number of semeru concurrent tasks
  uint                    _num_active_tasks; 		// Number of tasks currently active

  // [?] If this structure is only defined code task, why do need to keep so many versions ?
	//		 Each worker_id keeps one.
  // G1SemeruSTWTask**       _tasks;         // Task queue array (max_worker_id length)


  // True: marking is concurrent, false: we're in STW Compact.
  // [?] Because we are not interactiving with mutator threads, it's safe to set it as true.
	volatile bool           _concurrent;


  // 2 conditions can stop the Semeru MS compacting:
  //    1) all the scanned Regions are processed.
  //    2) the CPU STW windown is closed. this flag is tested by _has_abortd.
  // Set aborted when the Semeru MS compact is interrupped by CPU server.
  volatile bool           _has_aborted;



  //
  // fields for MS compactions
  //


  G1FullGCCompactionPoint** _compaction_points;  // each thread use one, clear it after the compaction phase.


	//
	// Statistics fields
	//

  ConcurrentGCTimer*      _gc_timer_cm;     // A timer to record the elapsed time for each concurrent phase.

  G1OldTracer*            _gc_tracer_cm;    // [?] G1 Old space logging systems

  // Timing statistics. All of them are in ms
  NumberSeq _init_times;
  NumberSeq _remark_times;
  NumberSeq _remark_mark_times;
  NumberSeq _remark_weak_ref_times;
  NumberSeq _cleanup_times;
  double    _total_cleanup_time;

  double*   _accum_task_vtime;   // Accumulated task vtime

  WorkGang* _concurrent_workers;     // [x] The real threads to execute the workload. Execute the G1SemeruCMTask.
  uint      _num_concurrent_workers; // The number of marking worker threads we're using
  uint      _max_concurrent_workers; // Maximum number of marking worker threads




	//
	// Function declaration
	//

private:


	// fast reclamation of a Region.
  void reclaim_empty_regions();

  // Called to indicate how many threads are currently active.
  void set_concurrency(uint active_tasks);

  // Should be called to indicate which phase we're in (concurrent
  // mark or remark) and how many threads are currently active.
  void set_concurrency_and_phase(uint active_tasks, bool concurrent);


  bool                    concurrent()       { return _concurrent; }
  uint                    active_tasks()     { return _num_active_tasks; }
  //ParallelTaskTerminator* terminator() const { return _terminator.terminator(); }

  // Claims the next available region to be scanned by a marking
  // task/thread. It might return NULL if the next region is empty or
  // we have run out of regions. In the latter case, out_of_regions()
  // determines whether we've really run out of regions or the task
  // should call claim_region() again. This might seem a bit
  // awkward. Originally, the code was written so that claim_region()
  // either successfully returned with a non-empty region or there
  // were no more regions to be claimed. The problem with this was
  // that, in certain circumstances, it iterated over large chunks of
  // the heap finding only empty regions and, while it was working, it
  // was preventing the calling task to call its regular clock
  // method. So, this way, each task will spend very little time in
  // claim_region() and is allowed to call the regular clock method
  // frequently.
  //HeapRegion* claim_region(uint worker_id);

	// Claim a Scanned Region.
 	HeapRegion* claim_region_for_comapct(uint worker_id, HeapRegion* prev_compact);




  // Semeru Memory Server
  // If the regions in memory server CSet are all processed.
  bool out_of_scanned_cset()  {
   // return mem_server_cset()->is_compact_finished();

    // TO BE DONE
    tty->print("%s, not finished. always true. \n", __func__);
    return true;
  }

  // duplicated functions
  bool out_of_regions() { return out_of_scanned_cset(); }



  // // Returns the task with the given id
  // G1SemeruSTWTask* task(uint id) {
  //   // During initial mark we use the parallel gc threads to do some work, so
  //   // we can only compare against _max_num_tasks.
  //   assert(id < _max_num_tasks, "Task id %u not within bounds up to %u", id, _max_num_tasks);
  //   return _tasks[id];
  // }


  // Reliam the entire bitmap of current HeapRegion after the compaction is done.
  void clear_bitmap(G1CMBitMap* bitmap, WorkGang* workers, bool may_yield);


public:



  // Constructor
  G1SemeruSTWCompact(G1SemeruCollectedHeap* g1h, G1SemeruConcurrentMark* 	semeru_cm);

  ~G1SemeruSTWCompact();

  G1SemeruConcurrentMarkThread* semeru_cm_thread() { return _semeru_cm_thread; }   //[?] Change to Semeru CM thread




  // Notification for eagerly reclaimed regions to clean up.
  void humongous_object_eagerly_reclaimed(HeapRegion* r);


  // Transfer content from G1SemeruCollectedHeap->_recv_mem_server_cset
  // The structure of G1SemeruCMCSetRegions support Multiple-Thread safe.
  G1SemeruCMCSetRegions*  mem_server_scanned_cset()   { return _mem_server_cset;  }

  void concurrent_cycle_start();
  // Abandon current marking iteration due to a Full GC.
  void concurrent_cycle_abort();
  void concurrent_cycle_end();




  //
  // Semeru Memory Server functions
  //

	// The main function of STW compaction.
  // Enter the Semeru MS compact tasks.
  void 				semeru_stw_compact();


	// to check if current STW compaction is interrupped by the CPU server.
  inline bool do_interrupt_check();
  bool 				has_aborted()      { return _has_aborted; }



  // Returns true if initialization was successfully completed.
  bool	completed_initialization() const {
    return _completed_initialization;
  }



  //
  // Compaction related functions
  //
  G1FullGCCompactionPoint* compaction_point(uint id) { return _compaction_points[id]; }



	//
	// Statistics functions
	//

	void update_accum_task_vtime(int i, double vtime) {
    _accum_task_vtime[i] += vtime;
  }

  double all_task_accum_vtime() {
    double ret = 0.0;
    for (uint i = 0; i < _max_num_tasks; ++i)
      ret += _accum_task_vtime[i];
    return ret;
  }


  ConcurrentGCTimer* gc_timer_cm() const { return _gc_timer_cm; }
  G1OldTracer* gc_tracer_cm() const { return _gc_tracer_cm; }



	//
	// Debug functions
	//
  void print_stats();

};





// /** 
//  * 	[XX] At this time, we are not using this structure now.
//  * 			 We define the code and run the G1SemeruSTWCompactTask->work() directly.
//  * 			
//  * 
//  *  The real code to be executed for the STW compact.
//  *  
//  *  Task isn't a Thread. Task is the computation to be executed by ONE thread.
//  *  Contents of the G1SemeruCMTask:
//  *    1) The attadched concurrent thread, recorded by _worker_id, G1SemeruConcurrentMark->_concurrent_workers[_worker_id]
//  *    2) Evacualte the scanned Region, pointed by _curr_region.
//  * 
//  * [?] Why do we need such a task ?
//  * 		 How about implementing all the things in G1SemeruSTWCompact ?
//  * 
//  */
// class G1SemeruSTWTask : public TerminatorTerminator {
// private:

//   uint                              _worker_id;     // [?] Only one concurrent Thread can cliam this Region.
//                                                     // Let other available concurrent threads to steal work from here ?
// 	G1SemeruCollectedHeap*						_semeru_h;
//   G1SemeruSTWCompact*           		_semeru_sc;			// point the STW compact handler.
//   G1CMBitMap*                       _alive_bitmap;  // points to the evacutating Region's alive_bitmap.
 
//   // Region this task is scanning, NULL if we're not scanning any
//   HeapRegion*                 _curr_compacting_region;



//   // If true, then the task has aborted for some reason
//   bool                        _has_aborted;
//   // Set when the task aborts because it has met its time quota
//   bool                        _has_timed_out;



// 	//
// 	// Functions declaration.
// 	//


  
// public:

//   // Constructor
//   G1SemeruSTWTask(uint worker_id,	G1SemeruSTWCompact* sc);
// 	~G1SemeruSTWTask() { }


//   G1CMBitMap*    alive_bitmap()  { return _alive_bitmap; }

//   // // Mark an object alive in current scanning region, pointed by _curr_region.
//   // inline bool mark_in_alive_bitmap(uint const worker_id, oop const obj);


//   // Returns the worker ID associated with this task.
//   uint worker_id() { return _worker_id; }

//   // From TerminatorTerminator. It determines whether this task should
//   // exit the termination protocol after it's entered it.
//   virtual bool should_exit_termination();

//   // // Resets the local region fields after a task has finished scanning a
//   // // region; or when they have become stale as a result of the region
//   // // being evacuated.
//   // void giveup_current_region();


//   /**
//    * [x] What's the difference between _has_aborted and _should_terminated
//    *    G1SemeruCMTask->_has_aborted, abort a CMTask, which is executed by a worker.
//    *        => abort the G1SemeruCMTask::do_semeru_marking_step()
//    *    G1SemeruConcurrentMark->_has_aborted, abort the current mark's all tasks ?
//    *      
//    *    ConcurrentMarkThread->_should_terminated, abort the thread handler.
//    *        => terminate the G1SemeruConcurrentMarkThread::run_service().
//    */
//   bool has_aborted()            { return _has_aborted; }
//   void set_has_aborted()        { _has_aborted = true; }
//   void clear_has_aborted()      { _has_aborted = false; }

//   //void set_cm_oop_closure(G1SemeruCMOopClosure* cm_oop_closure);




// 	//
// 	// Statistics fields
// 	//

// 	// Number sequence of past step times
//   NumberSeq                   _step_times_ms;
//   // Elapsed time of this task
//   double                      _elapsed_time_ms;
//   // Termination time of this task
//   double                      _termination_time_ms;
//   // When this task got into the termination protocol
//   double                      _termination_start_time_ms;
//   // Number of calls to this task
//   uint                        _calls;
//   // When the virtual timer reaches this time, the marking step should exit
//   double                      _time_target_ms;
//   // Start time of the current marking step
//   double                      _start_time_ms;

// 	// These two calls start and stop the timer
//   void record_start_time() {
//     _elapsed_time_ms = os::elapsedTime() * 1000.0;
//   }

//   void record_end_time() {
//     _elapsed_time_ms = os::elapsedTime() * 1000.0 - _elapsed_time_ms;
//   }

//   void print_stats();

// };









/**
 * Semeru Memory Server - the Compact WorkGang tasks.
 * 
 * There are 2 phases in current task. 
 * 	1) Do the compact.
 *  2) Do fields update (between servers)
 * 
 * More Explanation:
 * 	[x] Only concurrent GC threads can execute this task.
 * 	[x] Execute this task in STW mode.  
 * 
 * 
 * [?] How to stop or interrupt this work ??
 * 	=> This worker is scheduled to run by G1SemeruConcurrentMarkThread.
 * 		 After finish the executing of function G1SemeruCMConcurrentMarkingTask::work(),
 * 		 this thread will finished automaticaly.
 * 
 * 
 */
class G1SemeruSTWCompactTask : public AbstractGangTask {

  //phase#1 preparation
  friend class G1SemeruCalculatePointersClosure;
  friend class G1SemeruPrepareCompactLiveClosure;

  // Phase#2 Pointer adjustment

  // phase#3 Compaction
  friend class G1SemeruCompactRegionClosure;


  G1SemeruSTWCompact*		_semeru_sc;			// [x] Reuse the STW Compact use the same structure and Thread handler.
  
  // initialized in function, work()
  uint _worker_id;
  G1FullGCCompactionPoint* _cp;         // Compaction Point for this task.
  

  // The statistics data in each Closure are stateless, 
  // Pass these statistic data to them.
  uint _humongous_regions_removed;

public:

	// Constructor 
	G1SemeruSTWCompactTask(G1SemeruSTWCompact* semeru_sc) :
			AbstractGangTask("Semeru MS STW Compact Worker"), 
      _semeru_sc(semeru_sc),
      _worker_id(0),
      _cp(NULL),
      _humongous_regions_removed(0) { }



	// Deconstructor
	~G1SemeruSTWCompactTask() { }

  // [x] The entry point of current Worker.
  //     This is executed by G1SemeruSTWCompact::semeru_stw_compact in a synchronized way.
	void work(uint worker_id);				// Virutal function, The actual work for this 

	// The phases of this task's work.

  // Phase 1, 
  // 1) calculate the destination address for alive objects
  //    Put forwarding pointer in the markOop
  void phase1_prepare_for_compact(HeapRegion* hr);

  // Phase 2,
  // adjust the pointer
  // [?] The pointer can be inter-Region and intra-Region.
  //     How to handle the inter-Region ?
  //     Need to record these objects with inter-Region, scan and update their fields at the end of the phase 4?
  //     We can reuse the alive_bitmap for the compacted Region to record the objects having inter-Region references to be updated.
  void phase2_adjust_intra_region_pointer(HeapRegion* hr);

  
  // Phase 3,
	void phase3_compact_region(HeapRegion* hr);  // Compact a single HeapRegion.

  // Phase 4,
  // Need to share data between CPU server and other Memory servers.
	int phase4_inter_region_pointer();		// Inter-Region fields update ? Intra-Region reference is done during compaction.


  



};


  //
  // Closures for G1SemeruSTWCompactTask.
  //




  // Preparation Phase #1, calculate the destination Region for each source Region.
  // [X] This closure is only for a single Region. 
  //     All its stateless structures should get from the G1SemeruSTWCompactTask.
  //
class G1SemeruCalculatePointersClosure : public HeapRegionClosure {
protected:
  //  G1CollectedHeap* _g1h;
    G1SemeruSTWCompact* _semeru_sc;
    G1CMBitMap* _bitmap;
    G1FullGCCompactionPoint* _cp;       // The destination Region, for Semeru MS, each Region compact to itself.
    uint* _humongous_regions_removed;   // stateless,  pointed to G1SemeruSTWCompactTask->_humongous_regions_removed

    virtual void prepare_for_compaction(HeapRegion* hr);
    void prepare_for_compaction_work(G1FullGCCompactionPoint* cp, HeapRegion* hr);
    void free_humongous_region(HeapRegion* hr);
    void reset_region_metadata(HeapRegion* hr);

public:
    G1SemeruCalculatePointersClosure( G1SemeruSTWCompact* semeru_sc,
                                      G1CMBitMap* bitmap,
                                      G1FullGCCompactionPoint* cp,
                                      uint* humongous_regions_removed);

    void update_sets();       // [?] Young, Old, Humongous set ?
    bool do_heap_region(HeapRegion* hr); // Main Entry :  Claim and process a Region.
    bool freed_regions();
};




  // preparation Phase #1, calculate the destination for each alive object withn a source Region.
  //
class G1SemeruPrepareCompactLiveClosure : public StackObj {
    G1FullGCCompactionPoint* _cp;     // This source Region's compaction/destination Region.

public:
    G1SemeruPrepareCompactLiveClosure(G1FullGCCompactionPoint* cp);
    size_t apply(oop object);
};




//
// Phase#2, adjust the heap Region's Intra-Region references.
//





/**
 * Adjust the pointer for a single field.
 * 
 *  if this is a intra-Region reference
 *      adjust it.
 *  else if inter-Region reference
 *      Record the object &&
 *      Delay the processing to phase#4.
 * 
 * More Explanation
 *    The processing not exceed a single Region.
 * 
 */
class G1SemeruAdjustClosure : public BasicOopIterateClosure {
  HeapRegion* _curr_region;   // Current compacting Region.

  template <class T> static inline void adjust_intra_region_pointer(T* p, HeapRegion* hr);
public:
  G1SemeruAdjustClosure(HeapRegion* curr_region) : _curr_region(curr_region) { }

  template <class T> void do_oop_work(T* p) { adjust_intra_region_pointer(p , _curr_region); }
  virtual void do_oop(oop* p);
  virtual void do_oop(narrowOop* p);

  virtual ReferenceIterationMode reference_iteration_mode() { return DO_FIELDS; }
};





/**
 * Adjust pointer for a single object
 *  
 */
class G1SemeruAdjustLiveClosure : public StackObj {
  G1SemeruAdjustClosure* _adjust_pointer;
public:
  G1SemeruAdjustLiveClosure(G1SemeruAdjustClosure* cl) :
    _adjust_pointer(cl) { }

  size_t apply(oop object) {
    return object->oop_iterate_size(_adjust_pointer);
  }
};





/**
 * Adjust intra-Region references for a single Region.
 * 
 */
class G1SemeruAdjustRegionClosure : public HeapRegionClosure {
  G1SemeruSTWCompact* _semeru_sc;
  G1CMBitMap* _bitmap;
  //uint _worker_id;

 public:
  G1SemeruAdjustRegionClosure(G1SemeruSTWCompact* semeru_sc, G1CMBitMap* bitmap) :
    _semeru_sc(semeru_sc),
    _bitmap(bitmap) { }

  bool do_heap_region(HeapRegion* r) {
    G1SemeruAdjustClosure adjust_pointer(r);
    if (r->is_humongous()) {
      oop obj = oop(r->humongous_start_region()->bottom());  // get the humongous object
      obj->oop_iterate(&adjust_pointer, MemRegion(r->bottom(), r->top()));    // traverse the humongous object's fields.
    } else if (r->is_open_archive()) {
      // Only adjust the open archive regions, the closed ones
      // never change.
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
      // Open archive regions will not be compacted and the marking information is
      // no longer needed. Clear it here to avoid having to do it later.
      _bitmap->clear_region(r);
    } else {
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
    }
    return false;
  }


};




  //
  // Preparation Phase #3, Copy the alive objects to destination.
  // [X] This closure is only for a single Region. 
  //     All its stateless structures should get from the G1SemeruSTWCompactTask.
  //


	// Closure to compact a single object.
	// Define all the behaviors of how to evacuate an alive object.
	// srouce, destination, do the copy action.
	class G1SemeruCompactRegionClosure : public StackObj {

  public:
    G1SemeruCompactRegionClosure() {}

    size_t apply(oop object);		// [?] The closure is applied to an object ? not to a Region ??
  };






//
// Phase#4, Update Inter-Region poitners
//







#endif // SHARE_VM_GC_G1_SEMERU_G1STWCOMPACT_HPP



