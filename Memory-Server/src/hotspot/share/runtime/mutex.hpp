/*
 * Copyright (c) 1998, 2018, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_RUNTIME_MUTEX_HPP
#define SHARE_VM_RUNTIME_MUTEX_HPP

#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/histogram.hpp"

// The SplitWord construct allows us to colocate the contention queue
// (cxq) with the lock-byte.  The queue elements are ParkEvents, which are
// always aligned on 256-byte addresses - the least significant byte of
// a ParkEvent is always 0.  Colocating the lock-byte with the queue
// allows us to easily avoid what would otherwise be a race in lock()
// if we were to use two completely separate fields for the contention queue
// and the lock indicator.  Specifically, colocation renders us immune
// from the race where a thread might enqueue itself in the lock() slow-path
// immediately after the lock holder drops the outer lock in the unlock()
// fast-path.
//
// Colocation allows us to use a fast-path unlock() form that uses
// A MEMBAR instead of a CAS.  MEMBAR has lower local latency than CAS
// on many platforms.
//
// See:
// +  http://blogs.sun.com/dave/entry/biased_locking_in_hotspot
// +  http://blogs.sun.com/dave/resource/synchronization-public2.pdf
//
// Note that we're *not* using word-tearing the classic sense.
// The lock() fast-path will CAS the lockword and the unlock()
// fast-path will store into the lock-byte colocated within the lockword.
// We depend on the fact that all our reference platforms have
// coherent and atomic byte accesses.  More precisely, byte stores
// interoperate in a safe, sane, and expected manner with respect to
// CAS, ST and LDs to the full-word containing the byte.
// If you're porting HotSpot to a platform where that isn't the case
// then you'll want change the unlock() fast path from:
//    STB;MEMBAR #storeload; LDN
// to a full-word CAS of the lockword.


union SplitWord {   // full-word with separately addressable LSB
  volatile intptr_t FullWord ;
  volatile void * Address ;
  volatile jbyte Bytes [sizeof(intptr_t)] ;
} ;

class ParkEvent ;

// See orderAccess.hpp.  We assume throughout the VM that mutex lock and
// try_lock do fence-lock-acquire, and that unlock does a release-unlock,
// *in that order*.  If their implementations change such that these
// assumptions are violated, a whole lot of code will break.

// The default length of monitor name was originally chosen to be 64 to avoid
// false sharing. Now, PaddedMonitor is available for this purpose.
// TODO: Check if _name[MONITOR_NAME_LEN] should better get replaced by const char*.
static const int MONITOR_NAME_LEN = 64;

class Monitor : public CHeapObj<mtInternal> {

 public:
  // A special lock: Is a lock where you are guaranteed not to block while you are
  // holding it, i.e., no vm operation can happen, taking other (blocking) locks, etc.
  // The rank 'access' is similar to 'special' and has the same restrictions on usage.
  // It is reserved for locks that may be required in order to perform memory accesses
  // that require special barriers, e.g. SATB GC barriers, that in turn uses locks.
  // The rank 'tty' is also similar to 'special' and has the same restrictions.
  // It is reserved for the tty_lock.
  // Since memory accesses should be able to be performed pretty much anywhere
  // in the code, that requires locks required for performing accesses being
  // inherently a bit more special than even locks of the 'special' rank.
  // NOTE: It is critical that the rank 'special' be the lowest (earliest)
  // (except for "event" and "access") for the deadlock detection to work correctly.
  // The rank native is only for use in Mutex's created by JVM_RawMonitorCreate,
  // which being external to the VM are not subject to deadlock detection.
  // The rank safepoint is used only for synchronization in reaching a
  // safepoint and leaving a safepoint.  It is only used for the Safepoint_lock
  // currently.  While at a safepoint no mutexes of rank safepoint are held
  // by any thread.
  // The rank named "leaf" is probably historical (and should
  // be changed) -- mutexes of this rank aren't really leaf mutexes
  // at all.
  enum lock_types {
       event,
       access         = event          +   1,
       tty            = access         +   2,
       special        = tty            +   1,
       suspend_resume = special        +   1,
       vmweak         = suspend_resume +   2,
       leaf           = vmweak         +   2,
       safepoint      = leaf           +  10,
       barrier        = safepoint      +   1,
       nonleaf        = barrier        +   1,
       max_nonleaf    = nonleaf        + 900,
       native         = max_nonleaf    +   1
  };

  // The WaitSet and EntryList linked lists are composed of ParkEvents.
  // I use ParkEvent instead of threads as ParkEvents are immortal and
  // type-stable, meaning we can safely unpark() a possibly stale
  // list element in the unlock()-path.

 protected:                              // Monitor-Mutex metadata
  SplitWord _LockWord ;                  // Contention queue (cxq) colocated with Lock-byte
  Thread * volatile _owner;              // The owner of the lock
                                         // Consider sequestering _owner on its own $line
                                         // to aid future synchronization mechanisms.
  ParkEvent * volatile _EntryList ;      // List of threads waiting for entry
  ParkEvent * volatile _OnDeck ;         // heir-presumptive
  volatile intptr_t _WaitLock [1] ;      // Protects _WaitSet
  ParkEvent * volatile  _WaitSet ;       // LL of ParkEvents. Points to the ParkEvent of the Threads waiting on the Mutex.
  volatile bool     _snuck;              // Used for sneaky locking (evil).
  char _name[MONITOR_NAME_LEN];          // Name of mutex

  // Debugging fields for naming, deadlock detection, etc. (some only used in debug mode)
#ifndef PRODUCT
  bool      _allow_vm_block;
  DEBUG_ONLY(int _rank;)                 // rank (to avoid/detect potential deadlocks)
  DEBUG_ONLY(Monitor * _next;)           // Used by a Thread to link up owned locks
  DEBUG_ONLY(Thread* _last_owner;)       // the last thread to own the lock
  DEBUG_ONLY(static bool contains(Monitor * locks, Monitor * lock);)
  DEBUG_ONLY(static Monitor * get_least_ranked_lock(Monitor * locks);)
  DEBUG_ONLY(Monitor * get_least_ranked_lock_besides_this(Monitor * locks);)
#endif

  void set_owner_implementation(Thread* owner)                        PRODUCT_RETURN;
  void check_prelock_state     (Thread* thread, bool safepoint_check) PRODUCT_RETURN;
  void check_block_state       (Thread* thread)                       PRODUCT_RETURN;

  // platform-dependent support code can go here (in os_<os_family>.cpp)
 public:
  enum {
    _no_safepoint_check_flag    = true,
    _allow_vm_block_flag        = true,
    _as_suspend_equivalent_flag = true
  };

  // Locks can be acquired with or without safepoint check.
  // Monitor::lock and Monitor::lock_without_safepoint_check
  // checks these flags when acquiring a lock to ensure
  // consistent checking for each lock.
  // A few existing locks will sometimes have a safepoint check and
  // sometimes not, but these locks are set up in such a way to avoid deadlocks.
  enum SafepointCheckRequired {
    _safepoint_check_never,       // Monitors with this value will cause errors
                                  // when acquired with a safepoint check.
    _safepoint_check_sometimes,   // Certain locks are called sometimes with and
                                  // sometimes without safepoint checks. These
                                  // locks will not produce errors when locked.
    _safepoint_check_always       // Causes error if locked without a safepoint
                                  // check.
  };

  NOT_PRODUCT(SafepointCheckRequired _safepoint_check_required;)

  enum WaitResults {
    CONDVAR_EVENT,         // Wait returned because of condition variable notification
    INTERRUPT_EVENT,       // Wait returned because waiting thread was interrupted
    NUMBER_WAIT_RESULTS
  };

 private:
   int  TrySpin (Thread * Self) ;
   int  TryLock () ;
   int  TryFast () ;
   int  AcquireOrPush (ParkEvent * ev) ;
   void IUnlock (bool RelaxAssert) ;
   void ILock (Thread * Self) ;
   int  IWait (Thread * Self, jlong timo);
   int  ILocked () ;

 protected:
   static void ClearMonitor (Monitor * m, const char* name = NULL) ;
   Monitor() ;

 public:
  Monitor(int rank, const char *name, bool allow_vm_block = false,
          SafepointCheckRequired safepoint_check_required = _safepoint_check_always);
  ~Monitor();

  // Wait until monitor is notified (or times out).
  // Defaults are to make safepoint checks, wait time is forever (i.e.,
  // zero), and not a suspend-equivalent condition. Returns true if wait
  // times out; otherwise returns false.
  bool wait(bool no_safepoint_check = !_no_safepoint_check_flag,
            long timeout = 0,
            bool as_suspend_equivalent = !_as_suspend_equivalent_flag);
  bool notify();
  bool notify_all();


  void lock(); // prints out warning if VM thread blocks
  void lock(Thread *thread); // overloaded with current thread
  void unlock();
  bool is_locked() const                     { return _owner != NULL; }

  bool try_lock(); // Like lock(), but unblocking. It returns false instead

  // Lock without safepoint check. Should ONLY be used by safepoint code and other code
  // that is guaranteed not to block while running inside the VM.
  void lock_without_safepoint_check();
  void lock_without_safepoint_check (Thread * Self) ;

  // Current owner - not not MT-safe. Can only be used to guarantee that
  // the current running thread owns the lock
  Thread* owner() const         { return _owner; }
  bool owned_by_self() const;

  // Support for JVM_RawMonitorEnter & JVM_RawMonitorExit. These can be called by
  // non-Java thread. (We should really have a RawMonitor abstraction)
  void jvm_raw_lock();
  void jvm_raw_unlock();
  const char *name() const                  { return _name; }

  void print_on_error(outputStream* st) const;

  #ifndef PRODUCT
    void print_on(outputStream* st) const;
    void print() const                      { print_on(::tty); }
    DEBUG_ONLY(int    rank() const          { return _rank; })
    bool   allow_vm_block()                 { return _allow_vm_block; }

    DEBUG_ONLY(Monitor *next()  const         { return _next; })
    DEBUG_ONLY(void   set_next(Monitor *next) { _next = next; })
  #endif

  void set_owner(Thread* owner) {
  #ifndef PRODUCT
    set_owner_implementation(owner);
    DEBUG_ONLY(void verify_Monitor(Thread* thr);)
  #else
    _owner = owner;
  #endif
  }

};

class PaddedMonitor : public Monitor {
  enum {
    CACHE_LINE_PADDING = (int)DEFAULT_CACHE_LINE_SIZE - (int)sizeof(Monitor),
    PADDING_LEN = CACHE_LINE_PADDING > 0 ? CACHE_LINE_PADDING : 1
  };
  char _padding[PADDING_LEN];
 public:
  PaddedMonitor(int rank, const char *name, bool allow_vm_block = false,
               SafepointCheckRequired safepoint_check_required = _safepoint_check_always) :
    Monitor(rank, name, allow_vm_block, safepoint_check_required) {};
};

// Normally we'd expect Monitor to extend Mutex in the sense that a monitor
// constructed from pthreads primitives might extend a mutex by adding
// a condvar and some extra metadata.  In fact this was the case until J2SE7.
//
// Currently, however, the base object is a monitor.  Monitor contains all the
// logic for wait(), notify(), etc.   Mutex extends monitor and restricts the
// visibility of wait(), notify(), and notify_all().
//
// Another viable alternative would have been to have Monitor extend Mutex and
// implement all the normal mutex and wait()-notify() logic in Mutex base class.
// The wait()-notify() facility would be exposed via special protected member functions
// (e.g., _Wait() and _Notify()) in Mutex.  Monitor would extend Mutex and expose wait()
// as a call to _Wait().  That is, the public wait() would be a wrapper for the protected
// _Wait().
//
// An even better alternative is to simply eliminate Mutex:: and use Monitor:: instead.
// After all, monitors are sufficient for Java-level synchronization.   At one point in time
// there may have been some benefit to having distinct mutexes and monitors, but that time
// has past.
//
// The Mutex/Monitor design parallels that of Java-monitors, being based on
// thread-specific park-unpark platform-specific primitives.


class Mutex : public Monitor {      // degenerate Monitor
 public:
   Mutex(int rank, const char *name, bool allow_vm_block = false,
         SafepointCheckRequired safepoint_check_required = _safepoint_check_always);
  // default destructor
 private:
   bool notify ()    { ShouldNotReachHere(); return false; }
   bool notify_all() { ShouldNotReachHere(); return false; }
   bool wait (bool no_safepoint_check, long timeout, bool as_suspend_equivalent) {
     ShouldNotReachHere() ;
     return false ;
   }
};

class PaddedMutex : public Mutex {
  enum {
    CACHE_LINE_PADDING = (int)DEFAULT_CACHE_LINE_SIZE - (int)sizeof(Mutex),
    PADDING_LEN = CACHE_LINE_PADDING > 0 ? CACHE_LINE_PADDING : 1
  };
  char _padding[PADDING_LEN];
public:
  PaddedMutex(int rank, const char *name, bool allow_vm_block = false,
              SafepointCheckRequired safepoint_check_required = _safepoint_check_always) :
    Mutex(rank, name, allow_vm_block, safepoint_check_required) {};
};

#endif // SHARE_VM_RUNTIME_MUTEX_HPP
