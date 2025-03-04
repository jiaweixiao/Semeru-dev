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

#ifndef SHARE_VM_OOPS_OOP_HPP
#define SHARE_VM_OOPS_OOP_HPP

#include "memory/iterator.hpp"
#include "memory/memRegion.hpp"
#include "oops/access.hpp"
#include "oops/metadata.hpp"
#include "runtime/atomic.hpp"
#include "utilities/macros.hpp"

// oopDesc is the top baseclass for objects classes. The {name}Desc classes describe
// the format of Java objects so the fields can be accessed from C++.
// oopDesc is abstract.
// (see oopHierarchy for complete oop class hierarchy)
//
// no virtual functions allowed

extern bool always_do_update_barrier;

// Forward declarations.
class OopClosure;
class ScanClosure;
class FastScanClosure;
class FilteringClosure;
class CMSIsAliveClosure;

class PSPromotionManager;
class ParCompactionManager;

// Points to an object header.
class oopDesc {
  friend class VMStructs;
  friend class JVMCIVMStructs;
  
// private:
public:  
  volatile  markOop _mark;     // first 8 bytes, 
  volatile union _metadata {
    Klass*      _klass;
    narrowKlass _compressed_klass;
  } _metadata;                // second 8 bytes,

public:
  inline markOop  mark()          const;
  inline markOop  mark_raw()      const;
  inline markOop* mark_addr_raw() const;

  inline void set_mark(volatile markOop m);
  inline void set_mark_raw(volatile markOop m);
  static inline void set_mark_raw(HeapWord* mem, markOop m);

  inline void release_set_mark(markOop m);
  inline markOop cas_set_mark(markOop new_mark, markOop old_mark);
  inline markOop cas_set_mark_raw(markOop new_mark, markOop old_mark, atomic_memory_order order = memory_order_conservative);

  // Used only to re-initialize the mark word (e.g., of promoted
  // objects during a GC) -- requires a valid klass pointer
  inline void init_mark();
  inline void init_mark_raw();

  inline Klass* klass() const;
  inline Klass* klass_or_null() const volatile;
  inline Klass* klass_or_null_acquire() const volatile;
  static inline Klass** klass_addr(HeapWord* mem);
  static inline narrowKlass* compressed_klass_addr(HeapWord* mem);
  inline Klass** klass_addr();
  inline narrowKlass* compressed_klass_addr();

  inline void set_klass(Klass* k);
  static inline void release_set_klass(HeapWord* mem, Klass* klass);

  // For klass field compression
  inline int klass_gap() const;
  inline void set_klass_gap(int z);
  static inline void set_klass_gap(HeapWord* mem, int z);
  // For when the klass pointer is being used as a linked list "next" field.
  inline void set_klass_to_list_ptr(oop k);
  inline oop list_ptr_from_klass();

  // size of object header, aligned to platform wordSize
  static int header_size() { return sizeof(oopDesc)/HeapWordSize; }

  // Returns whether this is an instance of k or an instance of a subclass of k
  inline bool is_a(Klass* k) const;

  // Returns the actual oop size of the object
  inline int size();

  // Sometimes (for complicated concurrency-related reasons), it is useful
  // to be able to figure out the size of an object knowing its klass.
  inline int size_given_klass(Klass* klass);

  // type test operations (inlined in oop.inline.hpp)
  inline bool is_instance()            const;
  inline bool is_array()               const;
  inline bool is_objArray()            const;
  inline bool is_typeArray()           const;

  // type test operations that don't require inclusion of oop.inline.hpp.
  bool is_instance_noinline()          const;
  bool is_array_noinline()             const;
  bool is_objArray_noinline()          const;
  bool is_typeArray_noinline()         const;

protected:
  inline oop        as_oop() const { return const_cast<oopDesc*>(this); }

public:

	// Semeru
	//
	inline int semeru_oop_size(oop obj, Klass* klass);

  inline bool is_klass_valid(Klass* klass);
	//
	// End of Semeru

  // field addresses in oop
  inline void* field_addr(int offset)     const;
  inline void* field_addr_raw(int offset) const;

  // Need this as public for garbage collection.
  template <class T> inline T* obj_field_addr_raw(int offset) const;

  template <typename T> inline size_t field_offset(T* p) const;

  // Standard compare function returns negative value if o1 < o2
  //                                   0              if o1 == o2
  //                                   positive value if o1 > o2
  inline static int  compare(oop o1, oop o2) {
    void* o1_addr = (void*)o1;
    void* o2_addr = (void*)o2;
    if (o1_addr < o2_addr) {
      return -1;
    } else if (o1_addr > o2_addr) {
      return 1;
    } else {
      return 0;
    }
  }

  inline static bool equals(oop o1, oop o2) { return Access<>::equals(o1, o2); }

  inline static bool equals_raw(oop o1, oop o2) { return RawAccess<>::equals(o1, o2); }

  // Access to fields in a instanceOop through these methods.
  template <DecoratorSet decorator>
  oop obj_field_access(int offset) const;
  oop obj_field(int offset) const;
  void obj_field_put(int offset, oop value);
  void obj_field_put_raw(int offset, oop value);
  void obj_field_put_volatile(int offset, oop value);

  Metadata* metadata_field(int offset) const;
  Metadata* metadata_field_raw(int offset) const;
  void metadata_field_put(int offset, Metadata* value);

  Metadata* metadata_field_acquire(int offset) const;
  void release_metadata_field_put(int offset, Metadata* value);

  jbyte byte_field(int offset) const;
  void byte_field_put(int offset, jbyte contents);

  jchar char_field(int offset) const;
  void char_field_put(int offset, jchar contents);

  jboolean bool_field(int offset) const;
  void bool_field_put(int offset, jboolean contents);

  jint int_field(int offset) const;
  jint int_field_raw(int offset) const;
  void int_field_put(int offset, jint contents);

  jshort short_field(int offset) const;
  void short_field_put(int offset, jshort contents);

  jlong long_field(int offset) const;
  void long_field_put(int offset, jlong contents);

  jfloat float_field(int offset) const;
  void float_field_put(int offset, jfloat contents);

  jdouble double_field(int offset) const;
  void double_field_put(int offset, jdouble contents);

  address address_field(int offset) const;
  void address_field_put(int offset, address contents);

  oop obj_field_acquire(int offset) const;
  void release_obj_field_put(int offset, oop value);

  jbyte byte_field_acquire(int offset) const;
  void release_byte_field_put(int offset, jbyte contents);

  jchar char_field_acquire(int offset) const;
  void release_char_field_put(int offset, jchar contents);

  jboolean bool_field_acquire(int offset) const;
  void release_bool_field_put(int offset, jboolean contents);

  jint int_field_acquire(int offset) const;
  void release_int_field_put(int offset, jint contents);

  jshort short_field_acquire(int offset) const;
  void release_short_field_put(int offset, jshort contents);

  jlong long_field_acquire(int offset) const;
  void release_long_field_put(int offset, jlong contents);

  jfloat float_field_acquire(int offset) const;
  void release_float_field_put(int offset, jfloat contents);

  jdouble double_field_acquire(int offset) const;
  void release_double_field_put(int offset, jdouble contents);

  address address_field_acquire(int offset) const;
  void release_address_field_put(int offset, address contents);

  // printing functions for VM debugging
  void print_on(outputStream* st) const;        // First level print
  void print_value_on(outputStream* st) const;  // Second level print.
  void print_address_on(outputStream* st) const; // Address printing

  // printing on default output stream
  void print();
  void print_value();
  void print_address();

  // return the print strings
  char* print_string();
  char* print_value_string();

  // verification operations
  static void verify_on(outputStream* st, oopDesc* oop_desc);
  static void verify(oopDesc* oopDesc);

  // locking operations
  inline bool is_locked()   const;
  inline bool is_unlocked() const;
  inline bool has_bias_pattern() const;
  inline bool has_bias_pattern_raw() const;

  // asserts and guarantees
  static bool is_oop(oop obj, bool ignore_mark_word = false);
  static bool is_oop_or_null(oop obj, bool ignore_mark_word = false);

  // semeru
  static bool semeru_is_oop(oop obj, bool ignore_mark_word = false);

#ifndef PRODUCT
  inline bool is_unlocked_oop() const;
  static bool is_archived_object(oop p) NOT_CDS_JAVA_HEAP_RETURN_(false);
#endif

  // garbage collection
  inline bool is_gc_marked() const;

  // Forward pointer operations for scavenge
  inline bool is_forwarded() const;

  inline void forward_to(oop p);
  inline bool cas_forward_to(oop p, markOop compare, atomic_memory_order order = memory_order_conservative);

  // Like "forward_to", but inserts the forwarding pointer atomically.
  // Exactly one thread succeeds in inserting the forwarding pointer, and
  // this call returns "NULL" for that thread; any other thread has the
  // value of the forwarding pointer returned and does not modify "this".
  inline oop forward_to_atomic(oop p, markOop compare, atomic_memory_order order = memory_order_conservative);

  inline oop forwardee() const;
  inline oop forwardee_acquire() const;

  // Age of object during scavenge
  inline uint age() const;
  inline void incr_age();

  // mark-sweep support
  void follow_body(int begin, int end);

  template <typename OopClosureType>
  inline void oop_iterate(OopClosureType* cl);

  template <typename OopClosureType>
  inline void oop_iterate(OopClosureType* cl, MemRegion mr);

  template <typename OopClosureType>
  inline int oop_iterate_size(OopClosureType* cl);

  template <typename OopClosureType>
  inline int oop_iterate_size(OopClosureType* cl, MemRegion mr);

  template <typename OopClosureType>
  inline void oop_iterate_backwards(OopClosureType* cl);

  inline static bool is_instanceof_or_null(oop obj, Klass* klass);

  // identity hash; returns the identity hash key (computes it if necessary)
  // NOTE with the introduction of UseBiasedLocking that identity_hash() might reach a
  // safepoint if called on a biased object. Calling code must be aware of that.
  inline intptr_t identity_hash();
  intptr_t slow_identity_hash();

  // Alternate hashing code if string table is rehashed
  unsigned int new_hash(juint seed);

  // marks are forwarded to stack when object is locked
  inline bool    has_displaced_mark_raw() const;
  inline markOop displaced_mark_raw() const;
  inline void    set_displaced_mark_raw(markOop m);

  static bool has_klass_gap();

  // for code generation
  static int mark_offset_in_bytes()      { return offset_of(oopDesc, _mark); }
  static int klass_offset_in_bytes()     { return offset_of(oopDesc, _metadata._klass); }
  static int klass_gap_offset_in_bytes() {
    assert(has_klass_gap(), "only applicable to compressed klass pointers");
    return klass_offset_in_bytes() + sizeof(narrowKlass);
  }

  // for error reporting
  static oop   decode_oop_raw(narrowOop narrow_oop);
  static void* load_klass_raw(oop obj);
  static void* load_oop_raw(oop obj, int offset);
  static bool  is_valid(oop obj);
  static oop   oop_or_null(address addr);
};

#endif // SHARE_VM_OOPS_OOP_HPP
