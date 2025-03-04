/*
 * Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
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
#include "gc/shared/suspendibleThreadSet.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/semaphore.hpp"
#include "runtime/thread.inline.hpp"

uint   SuspendibleThreadSet::_nthreads          = 0;
uint   SuspendibleThreadSet::_nthreads_stopped  = 0;
bool   SuspendibleThreadSet::_suspend_all       = false;
double SuspendibleThreadSet::_suspend_all_start = 0.0;

static Semaphore* _synchronize_wakeup = NULL;

void SuspendibleThreadSet_init() {
  assert(_synchronize_wakeup == NULL, "STS already initialized");
  _synchronize_wakeup = new Semaphore();
}
// This is the last thread ?
bool SuspendibleThreadSet::is_synchronized() {  
  assert_lock_strong(STS_lock);
  assert(_nthreads_stopped <= _nthreads, "invariant");
  return _nthreads_stopped == _nthreads;
}

void SuspendibleThreadSet::join() {
  assert(!Thread::current()->is_suspendible_thread(), "Thread already joined");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  while (_suspend_all) {
    ml.wait(Mutex::_no_safepoint_check_flag);
  }
  _nthreads++;
  DEBUG_ONLY(Thread::current()->set_suspendible_thread();)
}

void SuspendibleThreadSet::leave() {
  assert(Thread::current()->is_suspendible_thread(), "Thread not joined");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_nthreads > 0, "Invalid");
  DEBUG_ONLY(Thread::current()->clear_suspendible_thread();)
  _nthreads--;
  if (_suspend_all && is_synchronized()) {
    // This leave completes a request, so inform the requestor.
    _synchronize_wakeup->signal();
  }
}

void SuspendibleThreadSet::yield() {
  assert(Thread::current()->is_suspendible_thread(), "Must have joined");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  if (_suspend_all) {
    _nthreads_stopped++;
    if (is_synchronized()) {
      if (ConcGCYieldTimeout > 0) {
        double now = os::elapsedTime();
        guarantee((now - _suspend_all_start) * 1000.0 < (double)ConcGCYieldTimeout, "Long delay");
      }
      // This yield completes the request, so inform the requestor.
      _synchronize_wakeup->signal();  // #1 if this is the last thread, wakeup other threads
    }

    // If other thread wants to wakeup this thread, need to set _suspend_all to false.
    while (_suspend_all) {
      ml.wait(Mutex::_no_safepoint_check_flag);  // #2, threads may wait here until wakenup by other threads.
    }
    assert(_nthreads_stopped > 0, "Invalid");
    _nthreads_stopped--;                         // if the thread is wakenup, decrease the _nthreads_stopped counter.
  }
}


/**
 * Tag : Semeru - VM Thread sets environments to suspend mutators for STW GC.
 *  
 *  The mutators need to check some Semaphore or (protected) page to see 
 *  if they need to yield to GC Threads at their safepoints.
 *  1) For here, seems VM_thread uses a Mutex, _suspend_all, to protect a bool variable, _suspend_all ?
 *  2) And then it uses the semaphore, _synchronize_wakeup, to suspend, wakeup other mutator threads.
 *  
 *  [?] GC threads, e.g. WorkGang can also by synchronized() by this function ?
 * 
 *  [?] The check instructions are inserted by compiler, e.g. JIT or interpreter ?
 * 
 */
void SuspendibleThreadSet::synchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");
  if (ConcGCYieldTimeout > 0) {
    _suspend_all_start = os::elapsedTime();
  }

  {
    // Prepare to suspend the mutators in their safepoint.
    // Mutators need to proactively check this signal and yied yo the GC threads at their safepoint.
    MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
    assert(!_suspend_all, "Only one at a time");
    _suspend_all = true;
    if (is_synchronized()) {
      return;
    }
  } // Release lock before semaphore wait.

  // Semaphore initial count is zero.  To reach here, there must be at
  // least one not yielded thread in the set, e.g. is_synchronized()
  // was false before the lock was released.  A thread in the set will
  // signal the semaphore iff it is the last to yield or leave while
  // there is an active suspend request.  So there will be exactly one
  // signal, which will increment the semaphore count to one, which
  // will then be consumed by this wait, returning it to zero.  No
  // thread can exit yield or enter the set until desynchronize is
  // called, so there are no further opportunities for the semaphore
  // being signaled until we get back here again for some later
  // synchronize call.  Hence, there is no need to re-check for
  // is_synchronized after the wait; it will always be true there.
  _synchronize_wakeup->wait();

#ifdef ASSERT
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_suspend_all, "STS not synchronizing");
  assert(is_synchronized(), "STS not synchronized");
#endif
}

void SuspendibleThreadSet::desynchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_suspend_all, "STS not synchronizing");
  assert(is_synchronized(), "STS not synchronized");
  _suspend_all = false;
  ml.notify_all();
}
