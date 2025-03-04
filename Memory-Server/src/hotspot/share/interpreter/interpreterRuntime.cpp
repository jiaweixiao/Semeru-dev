/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/javaClasses.inline.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/linkResolver.hpp"
#include "interpreter/templateTable.hpp"
#include "logging/log.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/constantPool.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/methodData.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/nativeLookup.hpp"
#include "runtime/atomic.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/compilationPolicy.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/fieldDescriptor.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/icache.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/jfieldIDWorkaround.hpp"
#include "runtime/osThread.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/threadCritical.hpp"
#include "utilities/align.hpp"
#include "utilities/copy.hpp"
#include "utilities/events.hpp"
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif

class UnlockFlagSaver {
  private:
    JavaThread* _thread;
    bool _do_not_unlock;
  public:
    UnlockFlagSaver(JavaThread* t) {
      _thread = t;
      _do_not_unlock = t->do_not_unlock_if_synchronized();
      t->set_do_not_unlock_if_synchronized(false);
    }
    ~UnlockFlagSaver() {
      _thread->set_do_not_unlock_if_synchronized(_do_not_unlock);
    }
};

// Helper class to access current interpreter state
class LastFrameAccessor : public StackObj {
  frame _last_frame;
public:
  LastFrameAccessor(JavaThread* thread) {
    assert(thread == Thread::current(), "sanity");
    _last_frame = thread->last_frame();
  }
  bool is_interpreted_frame() const              { return _last_frame.is_interpreted_frame(); }
  Method*   method() const                       { return _last_frame.interpreter_frame_method(); }
  address   bcp() const                          { return _last_frame.interpreter_frame_bcp(); }
  int       bci() const                          { return _last_frame.interpreter_frame_bci(); }
  address   mdp() const                          { return _last_frame.interpreter_frame_mdp(); }

  void      set_bcp(address bcp)                 { _last_frame.interpreter_frame_set_bcp(bcp); }
  void      set_mdp(address dp)                  { _last_frame.interpreter_frame_set_mdp(dp); }

  // pass method to avoid calling unsafe bcp_to_method (partial fix 4926272)
  Bytecodes::Code code() const                   { return Bytecodes::code_at(method(), bcp()); }

  Bytecode  bytecode() const                     { return Bytecode(method(), bcp()); }
  int get_index_u1(Bytecodes::Code bc) const     { return bytecode().get_index_u1(bc); }
  int get_index_u2(Bytecodes::Code bc) const     { return bytecode().get_index_u2(bc); }
  int get_index_u2_cpcache(Bytecodes::Code bc) const
                                                 { return bytecode().get_index_u2_cpcache(bc); }
  int get_index_u4(Bytecodes::Code bc) const     { return bytecode().get_index_u4(bc); }
  int number_of_dimensions() const               { return bcp()[3]; }
  ConstantPoolCacheEntry* cache_entry_at(int i) const
                                                 { return method()->constants()->cache()->entry_at(i); }
  ConstantPoolCacheEntry* cache_entry() const    { return cache_entry_at(Bytes::get_native_u2(bcp() + 1)); }

  oop callee_receiver(Symbol* signature) {
    return _last_frame.interpreter_callee_receiver(signature);
  }
  BasicObjectLock* monitor_begin() const {
    return _last_frame.interpreter_frame_monitor_begin();
  }
  BasicObjectLock* monitor_end() const {
    return _last_frame.interpreter_frame_monitor_end();
  }
  BasicObjectLock* next_monitor(BasicObjectLock* current) const {
    return _last_frame.next_monitor_in_interpreter_frame(current);
  }

  frame& get_frame()                             { return _last_frame; }
};


bool InterpreterRuntime::is_breakpoint(JavaThread *thread) {
  return Bytecodes::code_or_bp_at(LastFrameAccessor(thread).bcp()) == Bytecodes::_breakpoint;
}

//------------------------------------------------------------------------------------------------------------------------
// State accessors

void InterpreterRuntime::set_bcp_and_mdp(address bcp, JavaThread *thread) {
  LastFrameAccessor last_frame(thread);
  last_frame.set_bcp(bcp);
  if (ProfileInterpreter) {
    // ProfileTraps uses MDOs independently of ProfileInterpreter.
    // That is why we must check both ProfileInterpreter and mdo != NULL.
    MethodData* mdo = last_frame.method()->method_data();
    if (mdo != NULL) {
      NEEDS_CLEANUP;
      last_frame.set_mdp(mdo->bci_to_dp(last_frame.bci()));
    }
  }
}

//------------------------------------------------------------------------------------------------------------------------
// Constants


IRT_ENTRY(void, InterpreterRuntime::ldc(JavaThread* thread, bool wide))
  // access constant pool
  LastFrameAccessor last_frame(thread);
  ConstantPool* pool = last_frame.method()->constants();
  int index = wide ? last_frame.get_index_u2(Bytecodes::_ldc_w) : last_frame.get_index_u1(Bytecodes::_ldc);
  constantTag tag = pool->tag_at(index);

  assert (tag.is_unresolved_klass() || tag.is_klass(), "wrong ldc call");
  Klass* klass = pool->klass_at(index, CHECK);
    oop java_class = klass->java_mirror();
    thread->set_vm_result(java_class);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::resolve_ldc(JavaThread* thread, Bytecodes::Code bytecode)) {
  assert(bytecode == Bytecodes::_ldc ||
         bytecode == Bytecodes::_ldc_w ||
         bytecode == Bytecodes::_ldc2_w ||
         bytecode == Bytecodes::_fast_aldc ||
         bytecode == Bytecodes::_fast_aldc_w, "wrong bc");
  ResourceMark rm(thread);
  const bool is_fast_aldc = (bytecode == Bytecodes::_fast_aldc ||
                             bytecode == Bytecodes::_fast_aldc_w);
  LastFrameAccessor last_frame(thread);
  methodHandle m (thread, last_frame.method());
  Bytecode_loadconstant ldc(m, last_frame.bci());

  // Double-check the size.  (Condy can have any type.)
  BasicType type = ldc.result_type();
  switch (type2size[type]) {
  case 2: guarantee(bytecode == Bytecodes::_ldc2_w, ""); break;
  case 1: guarantee(bytecode != Bytecodes::_ldc2_w, ""); break;
  default: ShouldNotReachHere();
  }

  // Resolve the constant.  This does not do unboxing.
  // But it does replace Universe::the_null_sentinel by null.
  oop result = ldc.resolve_constant(CHECK);
  assert(result != NULL || is_fast_aldc, "null result only valid for fast_aldc");

#ifdef ASSERT
  {
    // The bytecode wrappers aren't GC-safe so construct a new one
    Bytecode_loadconstant ldc2(m, last_frame.bci());
    int rindex = ldc2.cache_index();
    if (rindex < 0)
      rindex = m->constants()->cp_to_object_index(ldc2.pool_index());
    if (rindex >= 0) {
      oop coop = m->constants()->resolved_references()->obj_at(rindex);
      oop roop = (result == NULL ? Universe::the_null_sentinel() : result);
      assert(oopDesc::equals(roop, coop), "expected result for assembly code");
    }
  }
#endif
  thread->set_vm_result(result);
  if (!is_fast_aldc) {
    // Tell the interpreter how to unbox the primitive.
    guarantee(java_lang_boxing_object::is_instance(result, type), "");
    int offset = java_lang_boxing_object::value_offset_in_bytes(type);
    intptr_t flags = ((as_TosState(type) << ConstantPoolCacheEntry::tos_state_shift)
                      | (offset & ConstantPoolCacheEntry::field_index_mask));
    thread->set_vm_result_2((Metadata*)flags);
  }
}
IRT_END


//------------------------------------------------------------------------------------------------------------------------
// Allocation

IRT_ENTRY(void, InterpreterRuntime::_new(JavaThread* thread, ConstantPool* pool, int index))
  Klass* k = pool->klass_at(index, CHECK);
  InstanceKlass* klass = InstanceKlass::cast(k);

  // Make sure we are not instantiating an abstract klass
  klass->check_valid_for_instantiation(true, CHECK);

  // Make sure klass is initialized
  klass->initialize(CHECK);

  // At this point the class may not be fully initialized
  // because of recursive initialization. If it is fully
  // initialized & has_finalized is not set, we rewrite
  // it into its fast version (Note: no locking is needed
  // here since this is an atomic byte write and can be
  // done more than once).
  //
  // Note: In case of classes with has_finalized we don't
  //       rewrite since that saves us an extra check in
  //       the fast version which then would call the
  //       slow version anyway (and do a call back into
  //       Java).
  //       If we have a breakpoint, then we don't rewrite
  //       because the _breakpoint bytecode would be lost.
  oop obj = klass->allocate_instance(CHECK);
  thread->set_vm_result(obj);
IRT_END


IRT_ENTRY(void, InterpreterRuntime::newarray(JavaThread* thread, BasicType type, jint size))
  oop obj = oopFactory::new_typeArray(type, size, CHECK);
  thread->set_vm_result(obj);
IRT_END


/**
 * Entrance of object array allocation, Interpreter.
 * 
 * 1) Get the related klass instance 
 *    [?] Get klass from a Constant Pool ?
 *          Use constant reference to index the klass instance ?
 *          [?] There is only a global ConstantPool ? or it's per-class pool ?
 *        OR, there is only a Symbol,
 *          Get the klass instance from ClassLoaderData->dictionary.
 *        
 *        
 * 2) Request memory from Java heap
 * 3) Initialize the object header 
 * 
 */
IRT_ENTRY(void, InterpreterRuntime::anewarray(JavaThread* thread, ConstantPool* pool, int index, jint size))
  Klass*    klass = pool->klass_at(index, CHECK);     // Why does here go into ConstantPool to search the klass instance ?
  objArrayOop obj = oopFactory::new_objArray(klass, size, CHECK);
  thread->set_vm_result(obj);
IRT_END


IRT_ENTRY(void, InterpreterRuntime::multianewarray(JavaThread* thread, jint* first_size_address))
  // We may want to pass in more arguments - could make this slightly faster
  LastFrameAccessor last_frame(thread);
  ConstantPool* constants = last_frame.method()->constants();
  int          i = last_frame.get_index_u2(Bytecodes::_multianewarray);
  Klass* klass   = constants->klass_at(i, CHECK);
  int   nof_dims = last_frame.number_of_dimensions();
  assert(klass->is_klass(), "not a class");
  assert(nof_dims >= 1, "multianewarray rank must be nonzero");

  // We must create an array of jints to pass to multi_allocate.
  ResourceMark rm(thread);
  const int small_dims = 10;
  jint dim_array[small_dims];
  jint *dims = &dim_array[0];
  if (nof_dims > small_dims) {
    dims = (jint*) NEW_RESOURCE_ARRAY(jint, nof_dims);
  }
  for (int index = 0; index < nof_dims; index++) {
    // offset from first_size_address is addressed as local[index]
    int n = Interpreter::local_offset_in_bytes(index)/jintSize;
    dims[index] = first_size_address[n];
  }
  oop obj = ArrayKlass::cast(klass)->multi_allocate(nof_dims, dims, CHECK);
  thread->set_vm_result(obj);
IRT_END


IRT_ENTRY(void, InterpreterRuntime::register_finalizer(JavaThread* thread, oopDesc* obj))
  assert(oopDesc::is_oop(obj), "must be a valid oop");
  assert(obj->klass()->has_finalizer(), "shouldn't be here otherwise");
  InstanceKlass::register_finalizer(instanceOop(obj), CHECK);
IRT_END


// Quicken instance-of and check-cast bytecodes
IRT_ENTRY(void, InterpreterRuntime::quicken_io_cc(JavaThread* thread))
  // Force resolving; quicken the bytecode
  LastFrameAccessor last_frame(thread);
  int which = last_frame.get_index_u2(Bytecodes::_checkcast);
  ConstantPool* cpool = last_frame.method()->constants();
  // We'd expect to assert that we're only here to quicken bytecodes, but in a multithreaded
  // program we might have seen an unquick'd bytecode in the interpreter but have another
  // thread quicken the bytecode before we get here.
  // assert( cpool->tag_at(which).is_unresolved_klass(), "should only come here to quicken bytecodes" );
  Klass* klass = cpool->klass_at(which, CHECK);
  thread->set_vm_result_2(klass);
IRT_END


//------------------------------------------------------------------------------------------------------------------------
// Exceptions

void InterpreterRuntime::note_trap_inner(JavaThread* thread, int reason,
                                         const methodHandle& trap_method, int trap_bci, TRAPS) {
  if (trap_method.not_null()) {
    MethodData* trap_mdo = trap_method->method_data();
    if (trap_mdo == NULL) {
      Method::build_interpreter_method_data(trap_method, THREAD);
      if (HAS_PENDING_EXCEPTION) {
        assert((PENDING_EXCEPTION->is_a(SystemDictionary::OutOfMemoryError_klass())),
               "we expect only an OOM error here");
        CLEAR_PENDING_EXCEPTION;
      }
      trap_mdo = trap_method->method_data();
      // and fall through...
    }
    if (trap_mdo != NULL) {
      // Update per-method count of trap events.  The interpreter
      // is updating the MDO to simulate the effect of compiler traps.
      Deoptimization::update_method_data_from_interpreter(trap_mdo, trap_bci, reason);
    }
  }
}

// Assume the compiler is (or will be) interested in this event.
// If necessary, create an MDO to hold the information, and record it.
void InterpreterRuntime::note_trap(JavaThread* thread, int reason, TRAPS) {
  assert(ProfileTraps, "call me only if profiling");
  LastFrameAccessor last_frame(thread);
  methodHandle trap_method(thread, last_frame.method());
  int trap_bci = trap_method->bci_from(last_frame.bcp());
  note_trap_inner(thread, reason, trap_method, trap_bci, THREAD);
}

#ifdef CC_INTERP
// As legacy note_trap, but we have more arguments.
IRT_ENTRY(void, InterpreterRuntime::note_trap(JavaThread* thread, int reason, Method *method, int trap_bci))
  methodHandle trap_method(method);
  note_trap_inner(thread, reason, trap_method, trap_bci, THREAD);
IRT_END

// Class Deoptimization is not visible in BytecodeInterpreter, so we need a wrapper
// for each exception.
void InterpreterRuntime::note_nullCheck_trap(JavaThread* thread, Method *method, int trap_bci)
  { if (ProfileTraps) note_trap(thread, Deoptimization::Reason_null_check, method, trap_bci); }
void InterpreterRuntime::note_div0Check_trap(JavaThread* thread, Method *method, int trap_bci)
  { if (ProfileTraps) note_trap(thread, Deoptimization::Reason_div0_check, method, trap_bci); }
void InterpreterRuntime::note_rangeCheck_trap(JavaThread* thread, Method *method, int trap_bci)
  { if (ProfileTraps) note_trap(thread, Deoptimization::Reason_range_check, method, trap_bci); }
void InterpreterRuntime::note_classCheck_trap(JavaThread* thread, Method *method, int trap_bci)
  { if (ProfileTraps) note_trap(thread, Deoptimization::Reason_class_check, method, trap_bci); }
void InterpreterRuntime::note_arrayCheck_trap(JavaThread* thread, Method *method, int trap_bci)
  { if (ProfileTraps) note_trap(thread, Deoptimization::Reason_array_check, method, trap_bci); }
#endif // CC_INTERP


static Handle get_preinitialized_exception(Klass* k, TRAPS) {
  // get klass
  InstanceKlass* klass = InstanceKlass::cast(k);
  assert(klass->is_initialized(),
         "this klass should have been initialized during VM initialization");
  // create instance - do not call constructor since we may have no
  // (java) stack space left (should assert constructor is empty)
  Handle exception;
  oop exception_oop = klass->allocate_instance(CHECK_(exception));
  exception = Handle(THREAD, exception_oop);
  if (StackTraceInThrowable) {
    java_lang_Throwable::fill_in_stack_trace(exception);
  }
  return exception;
}

// Special handling for stack overflow: since we don't have any (java) stack
// space left we use the pre-allocated & pre-initialized StackOverflowError
// klass to create an stack overflow error instance.  We do not call its
// constructor for the same reason (it is empty, anyway).
IRT_ENTRY(void, InterpreterRuntime::throw_StackOverflowError(JavaThread* thread))
  Handle exception = get_preinitialized_exception(
                                 SystemDictionary::StackOverflowError_klass(),
                                 CHECK);
  // Increment counter for hs_err file reporting
  Atomic::inc(&Exceptions::_stack_overflow_errors);
  THROW_HANDLE(exception);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::throw_delayed_StackOverflowError(JavaThread* thread))
  Handle exception = get_preinitialized_exception(
                                 SystemDictionary::StackOverflowError_klass(),
                                 CHECK);
  java_lang_Throwable::set_message(exception(),
          Universe::delayed_stack_overflow_error_message());
  // Increment counter for hs_err file reporting
  Atomic::inc(&Exceptions::_stack_overflow_errors);
  THROW_HANDLE(exception);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::create_exception(JavaThread* thread, char* name, char* message))
  // lookup exception klass
  TempNewSymbol s = SymbolTable::new_symbol(name, CHECK);
  if (ProfileTraps) {
    if (s == vmSymbols::java_lang_ArithmeticException()) {
      note_trap(thread, Deoptimization::Reason_div0_check, CHECK);
    } else if (s == vmSymbols::java_lang_NullPointerException()) {
      note_trap(thread, Deoptimization::Reason_null_check, CHECK);
    }
  }
  // create exception
  Handle exception = Exceptions::new_exception(thread, s, message);
  thread->set_vm_result(exception());
IRT_END


IRT_ENTRY(void, InterpreterRuntime::create_klass_exception(JavaThread* thread, char* name, oopDesc* obj))
  // Produce the error message first because note_trap can safepoint
  ResourceMark rm(thread);
  const char* klass_name = obj->klass()->external_name();
  // lookup exception klass
  TempNewSymbol s = SymbolTable::new_symbol(name, CHECK);
  if (ProfileTraps) {
    note_trap(thread, Deoptimization::Reason_class_check, CHECK);
  }
  // create exception, with klass name as detail message
  Handle exception = Exceptions::new_exception(thread, s, klass_name);
  thread->set_vm_result(exception());
IRT_END

IRT_ENTRY(void, InterpreterRuntime::throw_ArrayIndexOutOfBoundsException(JavaThread* thread, arrayOopDesc* a, jint index))
  // Produce the error message first because note_trap can safepoint
  ResourceMark rm(thread);
  stringStream ss;
  ss.print("Index %d out of bounds for length %d", index, a->length());

  if (ProfileTraps) {
    note_trap(thread, Deoptimization::Reason_range_check, CHECK);
  }

  THROW_MSG(vmSymbols::java_lang_ArrayIndexOutOfBoundsException(), ss.as_string());
IRT_END

IRT_ENTRY(void, InterpreterRuntime::throw_ClassCastException(
  JavaThread* thread, oopDesc* obj))

  // Produce the error message first because note_trap can safepoint
  ResourceMark rm(thread);
  char* message = SharedRuntime::generate_class_cast_message(
    thread, obj->klass());

  if (ProfileTraps) {
    note_trap(thread, Deoptimization::Reason_class_check, CHECK);
  }

  // create exception
  THROW_MSG(vmSymbols::java_lang_ClassCastException(), message);
IRT_END

// exception_handler_for_exception(...) returns the continuation address,
// the exception oop (via TLS) and sets the bci/bcp for the continuation.
// The exception oop is returned to make sure it is preserved over GC (it
// is only on the stack if the exception was thrown explicitly via athrow).
// During this operation, the expression stack contains the values for the
// bci where the exception happened. If the exception was propagated back
// from a call, the expression stack contains the values for the bci at the
// invoke w/o arguments (i.e., as if one were inside the call).
IRT_ENTRY(address, InterpreterRuntime::exception_handler_for_exception(JavaThread* thread, oopDesc* exception))

  LastFrameAccessor last_frame(thread);
  Handle             h_exception(thread, exception);
  methodHandle       h_method   (thread, last_frame.method());
  constantPoolHandle h_constants(thread, h_method->constants());
  bool               should_repeat;
  int                handler_bci;
  int                current_bci = last_frame.bci();

  if (thread->frames_to_pop_failed_realloc() > 0) {
    // Allocation of scalar replaced object used in this frame
    // failed. Unconditionally pop the frame.
    thread->dec_frames_to_pop_failed_realloc();
    thread->set_vm_result(h_exception());
    // If the method is synchronized we already unlocked the monitor
    // during deoptimization so the interpreter needs to skip it when
    // the frame is popped.
    thread->set_do_not_unlock_if_synchronized(true);
#ifdef CC_INTERP
    return (address) -1;
#else
    return Interpreter::remove_activation_entry();
#endif
  }

  // Need to do this check first since when _do_not_unlock_if_synchronized
  // is set, we don't want to trigger any classloading which may make calls
  // into java, or surprisingly find a matching exception handler for bci 0
  // since at this moment the method hasn't been "officially" entered yet.
  if (thread->do_not_unlock_if_synchronized()) {
    ResourceMark rm;
    assert(current_bci == 0,  "bci isn't zero for do_not_unlock_if_synchronized");
    thread->set_vm_result(exception);
#ifdef CC_INTERP
    return (address) -1;
#else
    return Interpreter::remove_activation_entry();
#endif
  }

  do {
    should_repeat = false;

    // assertions
#ifdef ASSERT
    assert(h_exception.not_null(), "NULL exceptions should be handled by athrow");
    // Check that exception is a subclass of Throwable, otherwise we have a VerifyError
    if (!(h_exception->is_a(SystemDictionary::Throwable_klass()))) {
      if (ExitVMOnVerifyError) vm_exit(-1);
      ShouldNotReachHere();
    }
#endif

    // tracing
    if (log_is_enabled(Info, exceptions)) {
      ResourceMark rm(thread);
      stringStream tempst;
      tempst.print("interpreter method <%s>\n"
                   " at bci %d for thread " INTPTR_FORMAT " (%s)",
                   h_method->print_value_string(), current_bci, p2i(thread), thread->name());
      Exceptions::log_exception(h_exception, tempst);
    }
// Don't go paging in something which won't be used.
//     else if (extable->length() == 0) {
//       // disabled for now - interpreter is not using shortcut yet
//       // (shortcut is not to call runtime if we have no exception handlers)
//       // warning("performance bug: should not call runtime if method has no exception handlers");
//     }
    // for AbortVMOnException flag
    Exceptions::debug_check_abort(h_exception);

    // exception handler lookup
    Klass* klass = h_exception->klass();
    handler_bci = Method::fast_exception_handler_bci_for(h_method, klass, current_bci, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      // We threw an exception while trying to find the exception handler.
      // Transfer the new exception to the exception handle which will
      // be set into thread local storage, and do another lookup for an
      // exception handler for this exception, this time starting at the
      // BCI of the exception handler which caused the exception to be
      // thrown (bug 4307310).
      h_exception = Handle(THREAD, PENDING_EXCEPTION);
      CLEAR_PENDING_EXCEPTION;
      if (handler_bci >= 0) {
        current_bci = handler_bci;
        should_repeat = true;
      }
    }
  } while (should_repeat == true);

#if INCLUDE_JVMCI
  if (EnableJVMCI && h_method->method_data() != NULL) {
    ResourceMark rm(thread);
    ProfileData* pdata = h_method->method_data()->allocate_bci_to_data(current_bci, NULL);
    if (pdata != NULL && pdata->is_BitData()) {
      BitData* bit_data = (BitData*) pdata;
      bit_data->set_exception_seen();
    }
  }
#endif

  // notify JVMTI of an exception throw; JVMTI will detect if this is a first
  // time throw or a stack unwinding throw and accordingly notify the debugger
  if (JvmtiExport::can_post_on_exceptions()) {
    JvmtiExport::post_exception_throw(thread, h_method(), last_frame.bcp(), h_exception());
  }

#ifdef CC_INTERP
  address continuation = (address)(intptr_t) handler_bci;
#else
  address continuation = NULL;
#endif
  address handler_pc = NULL;
  if (handler_bci < 0 || !thread->reguard_stack((address) &continuation)) {
    // Forward exception to callee (leaving bci/bcp untouched) because (a) no
    // handler in this method, or (b) after a stack overflow there is not yet
    // enough stack space available to reprotect the stack.
#ifndef CC_INTERP
    continuation = Interpreter::remove_activation_entry();
#endif
#if COMPILER2_OR_JVMCI
    // Count this for compilation purposes
    h_method->interpreter_throwout_increment(THREAD);
#endif
  } else {
    // handler in this method => change bci/bcp to handler bci/bcp and continue there
    handler_pc = h_method->code_base() + handler_bci;
#ifndef CC_INTERP
    set_bcp_and_mdp(handler_pc, thread);
    continuation = Interpreter::dispatch_table(vtos)[*handler_pc];
#endif
  }
  // notify debugger of an exception catch
  // (this is good for exceptions caught in native methods as well)
  if (JvmtiExport::can_post_on_exceptions()) {
    JvmtiExport::notice_unwind_due_to_exception(thread, h_method(), handler_pc, h_exception(), (handler_pc != NULL));
  }

  thread->set_vm_result(h_exception());
  return continuation;
IRT_END


IRT_ENTRY(void, InterpreterRuntime::throw_pending_exception(JavaThread* thread))
  assert(thread->has_pending_exception(), "must only ne called if there's an exception pending");
  // nothing to do - eventually we should remove this code entirely (see comments @ call sites)
IRT_END


IRT_ENTRY(void, InterpreterRuntime::throw_AbstractMethodError(JavaThread* thread))
  THROW(vmSymbols::java_lang_AbstractMethodError());
IRT_END

// This method is called from the "abstract_entry" of the interpreter.
// At that point, the arguments have already been removed from the stack
// and therefore we don't have the receiver object at our fingertips. (Though,
// on some platforms the receiver still resides in a register...). Thus,
// we have no choice but print an error message not containing the receiver
// type.
IRT_ENTRY(void, InterpreterRuntime::throw_AbstractMethodErrorWithMethod(JavaThread* thread,
                                                                        Method* missingMethod))
  ResourceMark rm(thread);
  assert(missingMethod != NULL, "sanity");
  methodHandle m(thread, missingMethod);
  LinkResolver::throw_abstract_method_error(m, THREAD);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::throw_AbstractMethodErrorVerbose(JavaThread* thread,
                                                                     Klass* recvKlass,
                                                                     Method* missingMethod))
  ResourceMark rm(thread);
  methodHandle mh = methodHandle(thread, missingMethod);
  LinkResolver::throw_abstract_method_error(mh, recvKlass, THREAD);
IRT_END


IRT_ENTRY(void, InterpreterRuntime::throw_IncompatibleClassChangeError(JavaThread* thread))
  THROW(vmSymbols::java_lang_IncompatibleClassChangeError());
IRT_END

IRT_ENTRY(void, InterpreterRuntime::throw_IncompatibleClassChangeErrorVerbose(JavaThread* thread,
                                                                              Klass* recvKlass,
                                                                              Klass* interfaceKlass))
  ResourceMark rm(thread);
  char buf[1000];
  buf[0] = '\0';
  jio_snprintf(buf, sizeof(buf),
               "Class %s does not implement the requested interface %s",
               recvKlass ? recvKlass->external_name() : "NULL",
               interfaceKlass ? interfaceKlass->external_name() : "NULL");
  THROW_MSG(vmSymbols::java_lang_IncompatibleClassChangeError(), buf);
IRT_END

//------------------------------------------------------------------------------------------------------------------------
// Fields
//

void InterpreterRuntime::resolve_get_put(JavaThread* thread, Bytecodes::Code bytecode) {
  Thread* THREAD = thread;
  // resolve field
  fieldDescriptor info;
  LastFrameAccessor last_frame(thread);
  constantPoolHandle pool(thread, last_frame.method()->constants());
  methodHandle m(thread, last_frame.method());
  bool is_put    = (bytecode == Bytecodes::_putfield  || bytecode == Bytecodes::_nofast_putfield ||
                    bytecode == Bytecodes::_putstatic);
  bool is_static = (bytecode == Bytecodes::_getstatic || bytecode == Bytecodes::_putstatic);

  {
    JvmtiHideSingleStepping jhss(thread);
    LinkResolver::resolve_field_access(info, pool, last_frame.get_index_u2_cpcache(bytecode),
                                       m, bytecode, CHECK);
  } // end JvmtiHideSingleStepping

  // check if link resolution caused cpCache to be updated
  ConstantPoolCacheEntry* cp_cache_entry = last_frame.cache_entry();
  if (cp_cache_entry->is_resolved(bytecode)) return;

  // compute auxiliary field attributes
  TosState state  = as_TosState(info.field_type());

  // Resolution of put instructions on final fields is delayed. That is required so that
  // exceptions are thrown at the correct place (when the instruction is actually invoked).
  // If we do not resolve an instruction in the current pass, leaving the put_code
  // set to zero will cause the next put instruction to the same field to reresolve.

  // Resolution of put instructions to final instance fields with invalid updates (i.e.,
  // to final instance fields with updates originating from a method different than <init>)
  // is inhibited. A putfield instruction targeting an instance final field must throw
  // an IllegalAccessError if the instruction is not in an instance
  // initializer method <init>. If resolution were not inhibited, a putfield
  // in an initializer method could be resolved in the initializer. Subsequent
  // putfield instructions to the same field would then use cached information.
  // As a result, those instructions would not pass through the VM. That is,
  // checks in resolve_field_access() would not be executed for those instructions
  // and the required IllegalAccessError would not be thrown.
  //
  // Also, we need to delay resolving getstatic and putstatic instructions until the
  // class is initialized.  This is required so that access to the static
  // field will call the initialization function every time until the class
  // is completely initialized ala. in 2.17.5 in JVM Specification.
  InstanceKlass* klass = info.field_holder();
  bool uninitialized_static = is_static && !klass->is_initialized();
  bool has_initialized_final_update = info.field_holder()->major_version() >= 53 &&
                                      info.has_initialized_final_update();
  assert(!(has_initialized_final_update && !info.access_flags().is_final()), "Fields with initialized final updates must be final");

  Bytecodes::Code get_code = (Bytecodes::Code)0;
  Bytecodes::Code put_code = (Bytecodes::Code)0;
  if (!uninitialized_static) {
    get_code = ((is_static) ? Bytecodes::_getstatic : Bytecodes::_getfield);
    if ((is_put && !has_initialized_final_update) || !info.access_flags().is_final()) {
      put_code = ((is_static) ? Bytecodes::_putstatic : Bytecodes::_putfield);
    }
  }

  cp_cache_entry->set_field(
    get_code,
    put_code,
    info.field_holder(),
    info.index(),
    info.offset(),
    state,
    info.access_flags().is_final(),
    info.access_flags().is_volatile(),
    pool->pool_holder()
  );
}


//------------------------------------------------------------------------------------------------------------------------
// Synchronization
//
// The interpreter's synchronization code is factored out so that it can
// be shared by method invocation and synchronized blocks.
//%note synchronization_3

//%note monitor_1
IRT_ENTRY_NO_ASYNC(void, InterpreterRuntime::monitorenter(JavaThread* thread, BasicObjectLock* elem))
#ifdef ASSERT
  thread->last_frame().interpreter_frame_verify_monitor(elem);
#endif
  if (PrintBiasedLockingStatistics) {
    Atomic::inc(BiasedLocking::slow_path_entry_count_addr());
  }
  Handle h_obj(thread, elem->obj());
  assert(Universe::heap()->is_in_reserved_or_null(h_obj()),
         "must be NULL or an object");
  if (UseBiasedLocking) {
    // Retry fast entry if bias is revoked to avoid unnecessary inflation
    ObjectSynchronizer::fast_enter(h_obj, elem->lock(), true, CHECK);
  } else {
    ObjectSynchronizer::slow_enter(h_obj, elem->lock(), CHECK);
  }
  assert(Universe::heap()->is_in_reserved_or_null(elem->obj()),
         "must be NULL or an object");
#ifdef ASSERT
  thread->last_frame().interpreter_frame_verify_monitor(elem);
#endif
IRT_END


//%note monitor_1
IRT_ENTRY_NO_ASYNC(void, InterpreterRuntime::monitorexit(JavaThread* thread, BasicObjectLock* elem))
#ifdef ASSERT
  thread->last_frame().interpreter_frame_verify_monitor(elem);
#endif
  Handle h_obj(thread, elem->obj());
  assert(Universe::heap()->is_in_reserved_or_null(h_obj()),
         "must be NULL or an object");
  if (elem == NULL || h_obj()->is_unlocked()) {
    THROW(vmSymbols::java_lang_IllegalMonitorStateException());
  }
  ObjectSynchronizer::slow_exit(h_obj(), elem->lock(), thread);
  // Free entry. This must be done here, since a pending exception might be installed on
  // exit. If it is not cleared, the exception handling code will try to unlock the monitor again.
  elem->set_obj(NULL);
#ifdef ASSERT
  thread->last_frame().interpreter_frame_verify_monitor(elem);
#endif
IRT_END


IRT_ENTRY(void, InterpreterRuntime::throw_illegal_monitor_state_exception(JavaThread* thread))
  THROW(vmSymbols::java_lang_IllegalMonitorStateException());
IRT_END


IRT_ENTRY(void, InterpreterRuntime::new_illegal_monitor_state_exception(JavaThread* thread))
  // Returns an illegal exception to install into the current thread. The
  // pending_exception flag is cleared so normal exception handling does not
  // trigger. Any current installed exception will be overwritten. This
  // method will be called during an exception unwind.

  assert(!HAS_PENDING_EXCEPTION, "no pending exception");
  Handle exception(thread, thread->vm_result());
  assert(exception() != NULL, "vm result should be set");
  thread->set_vm_result(NULL); // clear vm result before continuing (may cause memory leaks and assert failures)
  if (!exception->is_a(SystemDictionary::ThreadDeath_klass())) {
    exception = get_preinitialized_exception(
                       SystemDictionary::IllegalMonitorStateException_klass(),
                       CATCH);
  }
  thread->set_vm_result(exception());
IRT_END


//------------------------------------------------------------------------------------------------------------------------
// Invokes

IRT_ENTRY(Bytecodes::Code, InterpreterRuntime::get_original_bytecode_at(JavaThread* thread, Method* method, address bcp))
  return method->orig_bytecode_at(method->bci_from(bcp));
IRT_END

IRT_ENTRY(void, InterpreterRuntime::set_original_bytecode_at(JavaThread* thread, Method* method, address bcp, Bytecodes::Code new_code))
  method->set_orig_bytecode_at(method->bci_from(bcp), new_code);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::_breakpoint(JavaThread* thread, Method* method, address bcp))
  JvmtiExport::post_raw_breakpoint(thread, method, bcp);
IRT_END

void InterpreterRuntime::resolve_invoke(JavaThread* thread, Bytecodes::Code bytecode) {
  Thread* THREAD = thread;
  LastFrameAccessor last_frame(thread);
  // extract receiver from the outgoing argument list if necessary
  Handle receiver(thread, NULL);
  if (bytecode == Bytecodes::_invokevirtual || bytecode == Bytecodes::_invokeinterface ||
      bytecode == Bytecodes::_invokespecial) {
    ResourceMark rm(thread);
    methodHandle m (thread, last_frame.method());
    Bytecode_invoke call(m, last_frame.bci());
    Symbol* signature = call.signature();
    receiver = Handle(thread, last_frame.callee_receiver(signature));

    assert(Universe::heap()->is_in_reserved_or_null(receiver()),
           "sanity check");
    assert(receiver.is_null() ||
           !Universe::heap()->is_in_reserved(receiver->klass()),
           "sanity check");
  }

  // resolve method
  CallInfo info;
  constantPoolHandle pool(thread, last_frame.method()->constants());

  {
    JvmtiHideSingleStepping jhss(thread);
    LinkResolver::resolve_invoke(info, receiver, pool,
                                 last_frame.get_index_u2_cpcache(bytecode), bytecode,
                                 CHECK);
    if (JvmtiExport::can_hotswap_or_post_breakpoint()) {
      int retry_count = 0;
      while (info.resolved_method()->is_old()) {
        // It is very unlikely that method is redefined more than 100 times
        // in the middle of resolve. If it is looping here more than 100 times
        // means then there could be a bug here.
        guarantee((retry_count++ < 100),
                  "Could not resolve to latest version of redefined method");
        // method is redefined in the middle of resolve so re-try.
        LinkResolver::resolve_invoke(info, receiver, pool,
                                     last_frame.get_index_u2_cpcache(bytecode), bytecode,
                                     CHECK);
      }
    }
  } // end JvmtiHideSingleStepping

  // check if link resolution caused cpCache to be updated
  ConstantPoolCacheEntry* cp_cache_entry = last_frame.cache_entry();
  if (cp_cache_entry->is_resolved(bytecode)) return;

#ifdef ASSERT
  if (bytecode == Bytecodes::_invokeinterface) {
    if (info.resolved_method()->method_holder() ==
                                            SystemDictionary::Object_klass()) {
      // NOTE: THIS IS A FIX FOR A CORNER CASE in the JVM spec
      // (see also CallInfo::set_interface for details)
      assert(info.call_kind() == CallInfo::vtable_call ||
             info.call_kind() == CallInfo::direct_call, "");
      methodHandle rm = info.resolved_method();
      assert(rm->is_final() || info.has_vtable_index(),
             "should have been set already");
    } else if (!info.resolved_method()->has_itable_index()) {
      // Resolved something like CharSequence.toString.  Use vtable not itable.
      assert(info.call_kind() != CallInfo::itable_call, "");
    } else {
      // Setup itable entry
      assert(info.call_kind() == CallInfo::itable_call, "");
      int index = info.resolved_method()->itable_index();
      assert(info.itable_index() == index, "");
    }
  } else if (bytecode == Bytecodes::_invokespecial) {
    assert(info.call_kind() == CallInfo::direct_call, "must be direct call");
  } else {
    assert(info.call_kind() == CallInfo::direct_call ||
           info.call_kind() == CallInfo::vtable_call, "");
  }
#endif
  // Get sender or sender's unsafe_anonymous_host, and only set cpCache entry to resolved if
  // it is not an interface.  The receiver for invokespecial calls within interface
  // methods must be checked for every call.
  InstanceKlass* sender = pool->pool_holder();
  sender = sender->is_unsafe_anonymous() ? sender->unsafe_anonymous_host() : sender;

  switch (info.call_kind()) {
  case CallInfo::direct_call:
    cp_cache_entry->set_direct_call(
      bytecode,
      info.resolved_method(),
      sender->is_interface());
    break;
  case CallInfo::vtable_call:
    cp_cache_entry->set_vtable_call(
      bytecode,
      info.resolved_method(),
      info.vtable_index());
    break;
  case CallInfo::itable_call:
    cp_cache_entry->set_itable_call(
      bytecode,
      info.resolved_klass(),
      info.resolved_method(),
      info.itable_index());
    break;
  default:  ShouldNotReachHere();
  }
}


// First time execution:  Resolve symbols, create a permanent MethodType object.
void InterpreterRuntime::resolve_invokehandle(JavaThread* thread) {
  Thread* THREAD = thread;
  const Bytecodes::Code bytecode = Bytecodes::_invokehandle;
  LastFrameAccessor last_frame(thread);

  // resolve method
  CallInfo info;
  constantPoolHandle pool(thread, last_frame.method()->constants());
  {
    JvmtiHideSingleStepping jhss(thread);
    LinkResolver::resolve_invoke(info, Handle(), pool,
                                 last_frame.get_index_u2_cpcache(bytecode), bytecode,
                                 CHECK);
  } // end JvmtiHideSingleStepping

  ConstantPoolCacheEntry* cp_cache_entry = last_frame.cache_entry();
  cp_cache_entry->set_method_handle(pool, info);
}

// First time execution:  Resolve symbols, create a permanent CallSite object.
void InterpreterRuntime::resolve_invokedynamic(JavaThread* thread) {
  Thread* THREAD = thread;
  LastFrameAccessor last_frame(thread);
  const Bytecodes::Code bytecode = Bytecodes::_invokedynamic;

  //TO DO: consider passing BCI to Java.
  //  int caller_bci = last_frame.method()->bci_from(last_frame.bcp());

  // resolve method
  CallInfo info;
  constantPoolHandle pool(thread, last_frame.method()->constants());
  int index = last_frame.get_index_u4(bytecode);
  {
    JvmtiHideSingleStepping jhss(thread);
    LinkResolver::resolve_invoke(info, Handle(), pool,
                                 index, bytecode, CHECK);
  } // end JvmtiHideSingleStepping

  ConstantPoolCacheEntry* cp_cache_entry = pool->invokedynamic_cp_cache_entry_at(index);
  cp_cache_entry->set_dynamic_call(pool, info);
}

// This function is the interface to the assembly code. It returns the resolved
// cpCache entry.  This doesn't safepoint, but the helper routines safepoint.
// This function will check for redefinition!
IRT_ENTRY(void, InterpreterRuntime::resolve_from_cache(JavaThread* thread, Bytecodes::Code bytecode)) {
  switch (bytecode) {
  case Bytecodes::_getstatic:
  case Bytecodes::_putstatic:
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
    resolve_get_put(thread, bytecode);
    break;
  case Bytecodes::_invokevirtual:
  case Bytecodes::_invokespecial:
  case Bytecodes::_invokestatic:
  case Bytecodes::_invokeinterface:
    resolve_invoke(thread, bytecode);
    break;
  case Bytecodes::_invokehandle:
    resolve_invokehandle(thread);
    break;
  case Bytecodes::_invokedynamic:
    resolve_invokedynamic(thread);
    break;
  default:
    fatal("unexpected bytecode: %s", Bytecodes::name(bytecode));
    break;
  }
}
IRT_END

//------------------------------------------------------------------------------------------------------------------------
// Miscellaneous


nmethod* InterpreterRuntime::frequency_counter_overflow(JavaThread* thread, address branch_bcp) {
  nmethod* nm = frequency_counter_overflow_inner(thread, branch_bcp);
  assert(branch_bcp != NULL || nm == NULL, "always returns null for non OSR requests");
  if (branch_bcp != NULL && nm != NULL) {
    // This was a successful request for an OSR nmethod.  Because
    // frequency_counter_overflow_inner ends with a safepoint check,
    // nm could have been unloaded so look it up again.  It's unsafe
    // to examine nm directly since it might have been freed and used
    // for something else.
    LastFrameAccessor last_frame(thread);
    Method* method =  last_frame.method();
    int bci = method->bci_from(last_frame.bcp());
    nm = method->lookup_osr_nmethod_for(bci, CompLevel_none, false);
    BarrierSetNMethod* bs_nm = BarrierSet::barrier_set()->barrier_set_nmethod();
    if (nm != NULL && bs_nm != NULL) {
      // in case the transition passed a safepoint we need to barrier this again
      if (!bs_nm->nmethod_osr_entry_barrier(nm)) {
        nm = NULL;
      }
    }
  }
  if (nm != NULL && thread->is_interp_only_mode()) {
    // Normally we never get an nm if is_interp_only_mode() is true, because
    // policy()->event has a check for this and won't compile the method when
    // true. However, it's possible for is_interp_only_mode() to become true
    // during the compilation. We don't want to return the nm in that case
    // because we want to continue to execute interpreted.
    nm = NULL;
  }
#ifndef PRODUCT
  if (TraceOnStackReplacement) {
    if (nm != NULL) {
      tty->print("OSR entry @ pc: " INTPTR_FORMAT ": ", p2i(nm->osr_entry()));
      nm->print();
    }
  }
#endif
  return nm;
}

IRT_ENTRY(nmethod*,
          InterpreterRuntime::frequency_counter_overflow_inner(JavaThread* thread, address branch_bcp))
  // use UnlockFlagSaver to clear and restore the _do_not_unlock_if_synchronized
  // flag, in case this method triggers classloading which will call into Java.
  UnlockFlagSaver fs(thread);

  LastFrameAccessor last_frame(thread);
  assert(last_frame.is_interpreted_frame(), "must come from interpreter");
  methodHandle method(thread, last_frame.method());
  const int branch_bci = branch_bcp != NULL ? method->bci_from(branch_bcp) : InvocationEntryBci;
  const int bci = branch_bcp != NULL ? method->bci_from(last_frame.bcp()) : InvocationEntryBci;

  assert(!HAS_PENDING_EXCEPTION, "Should not have any exceptions pending");
  nmethod* osr_nm = CompilationPolicy::policy()->event(method, method, branch_bci, bci, CompLevel_none, NULL, thread);
  assert(!HAS_PENDING_EXCEPTION, "Event handler should not throw any exceptions");

  BarrierSetNMethod* bs_nm = BarrierSet::barrier_set()->barrier_set_nmethod();
  if (osr_nm != NULL && bs_nm != NULL) {
    if (!bs_nm->nmethod_osr_entry_barrier(osr_nm)) {
      osr_nm = NULL;
    }
  }

  if (osr_nm != NULL) {
    // We may need to do on-stack replacement which requires that no
    // monitors in the activation are biased because their
    // BasicObjectLocks will need to migrate during OSR. Force
    // unbiasing of all monitors in the activation now (even though
    // the OSR nmethod might be invalidated) because we don't have a
    // safepoint opportunity later once the migration begins.
    if (UseBiasedLocking) {
      ResourceMark rm;
      GrowableArray<Handle>* objects_to_revoke = new GrowableArray<Handle>();
      for( BasicObjectLock *kptr = last_frame.monitor_end();
           kptr < last_frame.monitor_begin();
           kptr = last_frame.next_monitor(kptr) ) {
        if( kptr->obj() != NULL ) {
          objects_to_revoke->append(Handle(THREAD, kptr->obj()));
        }
      }
      BiasedLocking::revoke(objects_to_revoke);
    }
  }
  return osr_nm;
IRT_END

IRT_LEAF(jint, InterpreterRuntime::bcp_to_di(Method* method, address cur_bcp))
  assert(ProfileInterpreter, "must be profiling interpreter");
  int bci = method->bci_from(cur_bcp);
  MethodData* mdo = method->method_data();
  if (mdo == NULL)  return 0;
  return mdo->bci_to_di(bci);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::profile_method(JavaThread* thread))
  // use UnlockFlagSaver to clear and restore the _do_not_unlock_if_synchronized
  // flag, in case this method triggers classloading which will call into Java.
  UnlockFlagSaver fs(thread);

  assert(ProfileInterpreter, "must be profiling interpreter");
  LastFrameAccessor last_frame(thread);
  assert(last_frame.is_interpreted_frame(), "must come from interpreter");
  methodHandle method(thread, last_frame.method());
  Method::build_interpreter_method_data(method, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    assert((PENDING_EXCEPTION->is_a(SystemDictionary::OutOfMemoryError_klass())), "we expect only an OOM error here");
    CLEAR_PENDING_EXCEPTION;
    // and fall through...
  }
IRT_END


#ifdef ASSERT
IRT_LEAF(void, InterpreterRuntime::verify_mdp(Method* method, address bcp, address mdp))
  assert(ProfileInterpreter, "must be profiling interpreter");

  MethodData* mdo = method->method_data();
  assert(mdo != NULL, "must not be null");

  int bci = method->bci_from(bcp);

  address mdp2 = mdo->bci_to_dp(bci);
  if (mdp != mdp2) {
    ResourceMark rm;
    ResetNoHandleMark rnm; // In a LEAF entry.
    HandleMark hm;
    tty->print_cr("FAILED verify : actual mdp %p   expected mdp %p @ bci %d", mdp, mdp2, bci);
    int current_di = mdo->dp_to_di(mdp);
    int expected_di  = mdo->dp_to_di(mdp2);
    tty->print_cr("  actual di %d   expected di %d", current_di, expected_di);
    int expected_approx_bci = mdo->data_at(expected_di)->bci();
    int approx_bci = -1;
    if (current_di >= 0) {
      approx_bci = mdo->data_at(current_di)->bci();
    }
    tty->print_cr("  actual bci is %d  expected bci %d", approx_bci, expected_approx_bci);
    mdo->print_on(tty);
    method->print_codes();
  }
  assert(mdp == mdp2, "wrong mdp");
IRT_END
#endif // ASSERT

IRT_ENTRY(void, InterpreterRuntime::update_mdp_for_ret(JavaThread* thread, int return_bci))
  assert(ProfileInterpreter, "must be profiling interpreter");
  ResourceMark rm(thread);
  HandleMark hm(thread);
  LastFrameAccessor last_frame(thread);
  assert(last_frame.is_interpreted_frame(), "must come from interpreter");
  MethodData* h_mdo = last_frame.method()->method_data();

  // Grab a lock to ensure atomic access to setting the return bci and
  // the displacement.  This can block and GC, invalidating all naked oops.
  MutexLocker ml(RetData_lock);

  // ProfileData is essentially a wrapper around a derived oop, so we
  // need to take the lock before making any ProfileData structures.
  ProfileData* data = h_mdo->data_at(h_mdo->dp_to_di(last_frame.mdp()));
  guarantee(data != NULL, "profile data must be valid");
  RetData* rdata = data->as_RetData();
  address new_mdp = rdata->fixup_ret(return_bci, h_mdo);
  last_frame.set_mdp(new_mdp);
IRT_END

IRT_ENTRY(MethodCounters*, InterpreterRuntime::build_method_counters(JavaThread* thread, Method* m))
  MethodCounters* mcs = Method::build_method_counters(m, thread);
  if (HAS_PENDING_EXCEPTION) {
    assert((PENDING_EXCEPTION->is_a(SystemDictionary::OutOfMemoryError_klass())), "we expect only an OOM error here");
    CLEAR_PENDING_EXCEPTION;
  }
  return mcs;
IRT_END


IRT_ENTRY(void, InterpreterRuntime::at_safepoint(JavaThread* thread))
  // We used to need an explict preserve_arguments here for invoke bytecodes. However,
  // stack traversal automatically takes care of preserving arguments for invoke, so
  // this is no longer needed.

  // IRT_END does an implicit safepoint check, hence we are guaranteed to block
  // if this is called during a safepoint

  if (JvmtiExport::should_post_single_step()) {
    // We are called during regular safepoints and when the VM is
    // single stepping. If any thread is marked for single stepping,
    // then we may have JVMTI work to do.
    LastFrameAccessor last_frame(thread);
    JvmtiExport::at_single_stepping_point(thread, last_frame.method(), last_frame.bcp());
  }
IRT_END

IRT_ENTRY(void, InterpreterRuntime::post_field_access(JavaThread *thread, oopDesc* obj,
ConstantPoolCacheEntry *cp_entry))

  // check the access_flags for the field in the klass

  InstanceKlass* ik = InstanceKlass::cast(cp_entry->f1_as_klass());
  int index = cp_entry->field_index();
  if ((ik->field_access_flags(index) & JVM_ACC_FIELD_ACCESS_WATCHED) == 0) return;

  bool is_static = (obj == NULL);
  HandleMark hm(thread);

  Handle h_obj;
  if (!is_static) {
    // non-static field accessors have an object, but we need a handle
    h_obj = Handle(thread, obj);
  }
  InstanceKlass* cp_entry_f1 = InstanceKlass::cast(cp_entry->f1_as_klass());
  jfieldID fid = jfieldIDWorkaround::to_jfieldID(cp_entry_f1, cp_entry->f2_as_index(), is_static);
  LastFrameAccessor last_frame(thread);
  JvmtiExport::post_field_access(thread, last_frame.method(), last_frame.bcp(), cp_entry_f1, h_obj, fid);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::post_field_modification(JavaThread *thread,
  oopDesc* obj, ConstantPoolCacheEntry *cp_entry, jvalue *value))

  Klass* k = cp_entry->f1_as_klass();

  // check the access_flags for the field in the klass
  InstanceKlass* ik = InstanceKlass::cast(k);
  int index = cp_entry->field_index();
  // bail out if field modifications are not watched
  if ((ik->field_access_flags(index) & JVM_ACC_FIELD_MODIFICATION_WATCHED) == 0) return;

  char sig_type = '\0';

  switch(cp_entry->flag_state()) {
    case btos: sig_type = 'B'; break;
    case ztos: sig_type = 'Z'; break;
    case ctos: sig_type = 'C'; break;
    case stos: sig_type = 'S'; break;
    case itos: sig_type = 'I'; break;
    case ftos: sig_type = 'F'; break;
    case atos: sig_type = 'L'; break;
    case ltos: sig_type = 'J'; break;
    case dtos: sig_type = 'D'; break;
    default:  ShouldNotReachHere(); return;
  }
  bool is_static = (obj == NULL);

  HandleMark hm(thread);
  jfieldID fid = jfieldIDWorkaround::to_jfieldID(ik, cp_entry->f2_as_index(), is_static);
  jvalue fvalue;
#ifdef _LP64
  fvalue = *value;
#else
  // Long/double values are stored unaligned and also noncontiguously with
  // tagged stacks.  We can't just do a simple assignment even in the non-
  // J/D cases because a C++ compiler is allowed to assume that a jvalue is
  // 8-byte aligned, and interpreter stack slots are only 4-byte aligned.
  // We assume that the two halves of longs/doubles are stored in interpreter
  // stack slots in platform-endian order.
  jlong_accessor u;
  jint* newval = (jint*)value;
  u.words[0] = newval[0];
  u.words[1] = newval[Interpreter::stackElementWords]; // skip if tag
  fvalue.j = u.long_value;
#endif // _LP64

  Handle h_obj;
  if (!is_static) {
    // non-static field accessors have an object, but we need a handle
    h_obj = Handle(thread, obj);
  }

  LastFrameAccessor last_frame(thread);
  JvmtiExport::post_raw_field_modification(thread, last_frame.method(), last_frame.bcp(), ik, h_obj,
                                           fid, sig_type, &fvalue);
IRT_END

IRT_ENTRY(void, InterpreterRuntime::post_method_entry(JavaThread *thread))
  LastFrameAccessor last_frame(thread);
  JvmtiExport::post_method_entry(thread, last_frame.method(), last_frame.get_frame());
IRT_END


IRT_ENTRY(void, InterpreterRuntime::post_method_exit(JavaThread *thread))
  LastFrameAccessor last_frame(thread);
  JvmtiExport::post_method_exit(thread, last_frame.method(), last_frame.get_frame());
IRT_END

IRT_LEAF(int, InterpreterRuntime::interpreter_contains(address pc))
{
  return (Interpreter::contains(pc) ? 1 : 0);
}
IRT_END


// Implementation of SignatureHandlerLibrary

#ifndef SHARING_FAST_NATIVE_FINGERPRINTS
// Dummy definition (else normalization method is defined in CPU
// dependant code)
uint64_t InterpreterRuntime::normalize_fast_native_fingerprint(uint64_t fingerprint) {
  return fingerprint;
}
#endif

address SignatureHandlerLibrary::set_handler_blob() {
  BufferBlob* handler_blob = BufferBlob::create("native signature handlers", blob_size);
  if (handler_blob == NULL) {
    return NULL;
  }
  address handler = handler_blob->code_begin();
  _handler_blob = handler_blob;
  _handler = handler;
  return handler;
}

void SignatureHandlerLibrary::initialize() {
  if (_fingerprints != NULL) {
    return;
  }
  if (set_handler_blob() == NULL) {
    vm_exit_out_of_memory(blob_size, OOM_MALLOC_ERROR, "native signature handlers");
  }

  BufferBlob* bb = BufferBlob::create("Signature Handler Temp Buffer",
                                      SignatureHandlerLibrary::buffer_size);
  _buffer = bb->code_begin();

  _fingerprints = new(ResourceObj::C_HEAP, mtCode)GrowableArray<uint64_t>(32, true);
  _handlers     = new(ResourceObj::C_HEAP, mtCode)GrowableArray<address>(32, true);
}

address SignatureHandlerLibrary::set_handler(CodeBuffer* buffer) {
  address handler   = _handler;
  int     insts_size = buffer->pure_insts_size();
  if (handler + insts_size > _handler_blob->code_end()) {
    // get a new handler blob
    handler = set_handler_blob();
  }
  if (handler != NULL) {
    memcpy(handler, buffer->insts_begin(), insts_size);
    pd_set_handler(handler);
    ICache::invalidate_range(handler, insts_size);
    _handler = handler + insts_size;
  }
  return handler;
}

void SignatureHandlerLibrary::add(const methodHandle& method) {
  if (method->signature_handler() == NULL) {
    // use slow signature handler if we can't do better
    int handler_index = -1;
    // check if we can use customized (fast) signature handler
    if (UseFastSignatureHandlers && method->size_of_parameters() <= Fingerprinter::max_size_of_parameters) {
      // use customized signature handler
      MutexLocker mu(SignatureHandlerLibrary_lock);
      // make sure data structure is initialized
      initialize();
      // lookup method signature's fingerprint
      uint64_t fingerprint = Fingerprinter(method).fingerprint();
      // allow CPU dependant code to optimize the fingerprints for the fast handler
      fingerprint = InterpreterRuntime::normalize_fast_native_fingerprint(fingerprint);
      handler_index = _fingerprints->find(fingerprint);
      // create handler if necessary
      if (handler_index < 0) {
        ResourceMark rm;
        ptrdiff_t align_offset = align_up(_buffer, CodeEntryAlignment) - (address)_buffer;
        CodeBuffer buffer((address)(_buffer + align_offset),
                          SignatureHandlerLibrary::buffer_size - align_offset);
        InterpreterRuntime::SignatureHandlerGenerator(method, &buffer).generate(fingerprint);
        // copy into code heap
        address handler = set_handler(&buffer);
        if (handler == NULL) {
          // use slow signature handler (without memorizing it in the fingerprints)
        } else {
          // debugging suppport
          if (PrintSignatureHandlers && (handler != Interpreter::slow_signature_handler())) {
            ttyLocker ttyl;
            tty->cr();
            tty->print_cr("argument handler #%d for: %s %s (fingerprint = " UINT64_FORMAT ", %d bytes generated)",
                          _handlers->length(),
                          (method->is_static() ? "static" : "receiver"),
                          method->name_and_sig_as_C_string(),
                          fingerprint,
                          buffer.insts_size());
            if (buffer.insts_size() > 0) {
              Disassembler::decode(handler, handler + buffer.insts_size());
            }
#ifndef PRODUCT
            address rh_begin = Interpreter::result_handler(method()->result_type());
            if (CodeCache::contains(rh_begin)) {
              // else it might be special platform dependent values
              tty->print_cr(" --- associated result handler ---");
              address rh_end = rh_begin;
              while (*(int*)rh_end != 0) {
                rh_end += sizeof(int);
              }
              Disassembler::decode(rh_begin, rh_end);
            } else {
              tty->print_cr(" associated result handler: " PTR_FORMAT, p2i(rh_begin));
            }
#endif
          }
          // add handler to library
          _fingerprints->append(fingerprint);
          _handlers->append(handler);
          // set handler index
          assert(_fingerprints->length() == _handlers->length(), "sanity check");
          handler_index = _fingerprints->length() - 1;
        }
      }
      // Set handler under SignatureHandlerLibrary_lock
      if (handler_index < 0) {
        // use generic signature handler
        method->set_signature_handler(Interpreter::slow_signature_handler());
      } else {
        // set handler
        method->set_signature_handler(_handlers->at(handler_index));
      }
    } else {
      CHECK_UNHANDLED_OOPS_ONLY(Thread::current()->clear_unhandled_oops());
      // use generic signature handler
      method->set_signature_handler(Interpreter::slow_signature_handler());
    }
  }
#ifdef ASSERT
  int handler_index = -1;
  int fingerprint_index = -2;
  {
    // '_handlers' and '_fingerprints' are 'GrowableArray's and are NOT synchronized
    // in any way if accessed from multiple threads. To avoid races with another
    // thread which may change the arrays in the above, mutex protected block, we
    // have to protect this read access here with the same mutex as well!
    MutexLocker mu(SignatureHandlerLibrary_lock);
    if (_handlers != NULL) {
      handler_index = _handlers->find(method->signature_handler());
      uint64_t fingerprint = Fingerprinter(method).fingerprint();
      fingerprint = InterpreterRuntime::normalize_fast_native_fingerprint(fingerprint);
      fingerprint_index = _fingerprints->find(fingerprint);
    }
  }
  assert(method->signature_handler() == Interpreter::slow_signature_handler() ||
         handler_index == fingerprint_index, "sanity check");
#endif // ASSERT
}

void SignatureHandlerLibrary::add(uint64_t fingerprint, address handler) {
  int handler_index = -1;
  // use customized signature handler
  MutexLocker mu(SignatureHandlerLibrary_lock);
  // make sure data structure is initialized
  initialize();
  fingerprint = InterpreterRuntime::normalize_fast_native_fingerprint(fingerprint);
  handler_index = _fingerprints->find(fingerprint);
  // create handler if necessary
  if (handler_index < 0) {
    if (PrintSignatureHandlers && (handler != Interpreter::slow_signature_handler())) {
      tty->cr();
      tty->print_cr("argument handler #%d at " PTR_FORMAT " for fingerprint " UINT64_FORMAT,
                    _handlers->length(),
                    p2i(handler),
                    fingerprint);
    }
    _fingerprints->append(fingerprint);
    _handlers->append(handler);
  } else {
    if (PrintSignatureHandlers) {
      tty->cr();
      tty->print_cr("duplicate argument handler #%d for fingerprint " UINT64_FORMAT "(old: " PTR_FORMAT ", new : " PTR_FORMAT ")",
                    _handlers->length(),
                    fingerprint,
                    p2i(_handlers->at(handler_index)),
                    p2i(handler));
    }
  }
}


BufferBlob*              SignatureHandlerLibrary::_handler_blob = NULL;
address                  SignatureHandlerLibrary::_handler      = NULL;
GrowableArray<uint64_t>* SignatureHandlerLibrary::_fingerprints = NULL;
GrowableArray<address>*  SignatureHandlerLibrary::_handlers     = NULL;
address                  SignatureHandlerLibrary::_buffer       = NULL;


IRT_ENTRY(void, InterpreterRuntime::prepare_native_call(JavaThread* thread, Method* method))
  methodHandle m(thread, method);
  assert(m->is_native(), "sanity check");
  // lookup native function entry point if it doesn't exist
  bool in_base_library;
  if (!m->has_native_function()) {
    NativeLookup::lookup(m, in_base_library, CHECK);
  }
  // make sure signature handler is installed
  SignatureHandlerLibrary::add(m);
  // The interpreter entry point checks the signature handler first,
  // before trying to fetch the native entry point and klass mirror.
  // We must set the signature handler last, so that multiple processors
  // preparing the same method will be sure to see non-null entry & mirror.
IRT_END

#if defined(IA32) || defined(AMD64) || defined(ARM)
IRT_LEAF(void, InterpreterRuntime::popframe_move_outgoing_args(JavaThread* thread, void* src_address, void* dest_address))
  if (src_address == dest_address) {
    return;
  }
  ResetNoHandleMark rnm; // In a LEAF entry.
  HandleMark hm;
  ResourceMark rm;
  LastFrameAccessor last_frame(thread);
  assert(last_frame.is_interpreted_frame(), "");
  jint bci = last_frame.bci();
  methodHandle mh(thread, last_frame.method());
  Bytecode_invoke invoke(mh, bci);
  ArgumentSizeComputer asc(invoke.signature());
  int size_of_arguments = (asc.size() + (invoke.has_receiver() ? 1 : 0)); // receiver
  Copy::conjoint_jbytes(src_address, dest_address,
                       size_of_arguments * Interpreter::stackElementSize);
IRT_END
#endif

#if INCLUDE_JVMTI
// This is a support of the JVMTI PopFrame interface.
// Make sure it is an invokestatic of a polymorphic intrinsic that has a member_name argument
// and return it as a vm_result so that it can be reloaded in the list of invokestatic parameters.
// The member_name argument is a saved reference (in local#0) to the member_name.
// For backward compatibility with some JDK versions (7, 8) it can also be a direct method handle.
// FIXME: remove DMH case after j.l.i.InvokerBytecodeGenerator code shape is updated.
IRT_ENTRY(void, InterpreterRuntime::member_name_arg_or_null(JavaThread* thread, address member_name,
                                                            Method* method, address bcp))
  Bytecodes::Code code = Bytecodes::code_at(method, bcp);
  if (code != Bytecodes::_invokestatic) {
    return;
  }
  ConstantPool* cpool = method->constants();
  int cp_index = Bytes::get_native_u2(bcp + 1) + ConstantPool::CPCACHE_INDEX_TAG;
  Symbol* cname = cpool->klass_name_at(cpool->klass_ref_index_at(cp_index));
  Symbol* mname = cpool->name_ref_at(cp_index);

  if (MethodHandles::has_member_arg(cname, mname)) {
    oop member_name_oop = (oop) member_name;
    if (java_lang_invoke_DirectMethodHandle::is_instance(member_name_oop)) {
      // FIXME: remove after j.l.i.InvokerBytecodeGenerator code shape is updated.
      member_name_oop = java_lang_invoke_DirectMethodHandle::member(member_name_oop);
    }
    thread->set_vm_result(member_name_oop);
  } else {
    thread->set_vm_result(NULL);
  }
IRT_END
#endif // INCLUDE_JVMTI

#ifndef PRODUCT
// This must be a IRT_LEAF function because the interpreter must save registers on x86 to
// call this, which changes rsp and makes the interpreter's expression stack not walkable.
// The generated code still uses call_VM because that will set up the frame pointer for
// bcp and method.
IRT_LEAF(intptr_t, InterpreterRuntime::trace_bytecode(JavaThread* thread, intptr_t preserve_this_value, intptr_t tos, intptr_t tos2))
  LastFrameAccessor last_frame(thread);
  assert(last_frame.is_interpreted_frame(), "must be an interpreted frame");
  methodHandle mh(thread, last_frame.method());
  BytecodeTracer::trace(mh, last_frame.bcp(), tos, tos2);
  return preserve_this_value;
IRT_END
#endif // !PRODUCT
