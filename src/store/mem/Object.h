// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

// Crimson: a prototype high performance OSD

// Copyright (C) 2015 Adam C. Emerson <aemerson@redhat.com
//
// Based on MemStore work by Casey Bodley <cbodley@redhat.com>
// and the Ceph object store which is Copyright (C) 2004-2006
// Sage Weil <sage@newdream.net>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2.1 of
// the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
// 02110-1301 USA

/// \file mem/Object.h
/// \brief Fast, in-memory objects
///
/// \author Adam C. Emerson <aemerson@redhat.com>

#ifndef CRIMSON_STORE_MEM_OBJECT_H
#define CRIMSON_STORE_MEM_OBJECT_H

#include <array>
#include <utility>

#include "store/Object.h"

#include "common/xxHash.h"
#include "PageSet.h"


namespace crimson {
  /// Storage interface
  namespace store {
    /// Memory based object store
    namespace mem {
      class Object : crimson::store::Object {
	const unsigned cpu;
	_::PageSet data;
	Length data_len;
	std::array<std::map<string,lw_shared_ptr<string>>,
		   (unsigned)attr_ns::END> attarray;
	string omap_header;

	bool in_range(const Range& range) const {
	  return (range.offset + range.length <= data_len);
	}

	bool local() const {
	  return engine().cpu_id() == cpu;
	}

	Object(CollectionRef&& _coll, string&& _oid)
	  : crimson::store::Object(std::move(_coll), std::move(_oid)),
	    cpu(xxHash()(oid) % smp::count) {}

	/// Read data from an object
	future<IovecRef> read(const Range& r) const override;
	/// Write data to an offset within an object
	future<> write(IovecRef&& iov) override;
	/// Zero out the indicated byte range within an object.
	///
	/// \note For the memstore, zero is equivalent to hole_punch,
	/// except that zero will not throw store::errc::out_of_range
	future<> zero(const Range& range) override;
	/// Punch a hole in the object of the given dimension
	future<> hole_punch(const Range& range) override;
	/// Truncate an object.
	future<> truncate(const Length length) override;
	/// XXX Implement this when the Collection is better developed
	future<> remove() = 0;
	/// Get a single attribute value
	future<const_buffer> getattr(
	  attr_ns ns, string&& attr) const override;
	/// Get some attribute values
	future<std::vector<const_buffer>> getattrs(
	  attr_ns ns, std::vector<string>&& attrs) const override;
	/// Set a single attribute
	future<> setattr(attr_ns ns, string&& attr,
			 const_buffer&& val) override;
	/// Sets attributes
	future<> setattrs(
	  attr_ns ns,
	  std::vector<pair<string, const_buffer>>&& attrpairs) override;
	/// Remove an attribute
	virtual future<> rmattr(attr_ns ns, string&& attr) override;
	/// Remove several attributes
	future<> rmattrs(attr_ns ns, std::vector<string>&& attr) override;
	/// Remove attributes in an overcomplicated way
	///
	/// Not supported for memstore right now
	future<> rmattr_range(attr_ns ns,
			      AttrCursorRef lb,
			      AttrCursorRef ub) override {
	  throw std::system_error(errc::operation_not_supported);
	}
	/// Enumerate attributes (just the names)
	future<std::vector<string>, optional<AttrCursorRef>>
	  enumerate_attr_keys(attr_ns ns, optional<AttrCursorRef> cursor,
			      size_t to_return) const override;
	/// Enumerate attributes (key/value)
	future<std::vector<pair<string, const_buffer>>,
	       optional<AttrCursorRef>> enumerate_attr_kvs(
		 attr_ns ns, optional<AttrCursorRef> cursor,
		 size_t to_return) const override;

	/// Get cursor for attribute key
	///
	/// This function gives a cursor that will continue an
	/// enumeration as if a previous enumeration had ended
	/// just before returning `attr`.
	///
	/// \note Not supported on stores without a well-defined
	/// enumeration order for attributes.
	virtual future<AttrCursorRef> attr_cursor(attr_ns ns, string attr) const;

	/// Clone this object into another object
	///
	/// Low-cost (e.g., O(1)) cloning (if supported) is best, but
	/// fallback to an O(n) copy is allowed.  All object data are
	/// cloned.
	///
	/// This clones everything, attributes, omap header, etc.
	///
	/// \param[in] dest Destination object
	///
	/// \note Objects must be in the same collection.
	///
	/// \see clone_range
	virtual future<> clone(Object& dest_obj) const = 0;
	/// Clone a byte range from one object to another
	///
	/// This only affects the data portion of the destination object.
	///
	/// \see clone
	virtual future<> clone_range(Range src_range,
				     Object& dest,
				     Offset dest_offset) const = 0;

	/// Inform store of future allocation plans
	///
	/// @param[in] obj_size   Expected total size of object
	/// @param[in] write_size Expected size of write operations
	virtual future<>set_alloc_hint(Length obj_size,
				       Length write_size) = 0;

	/// Get the object "header"
	///
	/// Ceph object stores have an additional piece of data, an
	/// 'OMAP Header' that is read or written in its entirety in a
	/// single operation.
	///
	/// \see set_header
	virtual future<const_buffer> get_header() const = 0;
	/// Set the object "header"
	///
	/// param[in] header Header to set
	/// \see get_header
	virtual future<> set_header(const_buffer header) = 0;
	/// Get allocated extents within a range
	///
	/// Return a list of extents that contain actual data within a
	/// range.
	///
	/// \note Is there a better interface for this?
	///
	/// \param[in] range Range of object to query
	///
	/// \see hole_punch
	virtual future<std::vector<Range>> get_extents(const Range& range) const = 0;
	/// Move object from one collection to another
	///
	/// \note This is used by Ceph's recovery logic, to move objects
	/// out of Temporary Collections.
	///
	/// \warning The same concerns apply as for
	/// split_collection.
	///
	/// \warning It's very likely that this call invalidates
	/// outstanding handles to the object it moves. In some
	/// implementations, this call might function as a barrier,
	/// preventing the acquisition of new handles on the object in
	/// question and waiting for outstanding handles to be
	/// released. If this is the case, the Store should make sure
	/// not to deadlock itself.
	///
	/// \param[in] dest_coll Collection to which to move the object
	/// \param[in] dest_oid  OID it should have there
	///
	/// \see split_collection
	virtual future<>move_to_collection(Collection& dest_coll,
					   const string& dest_oid) = 0;
	/// Commit all outstanding modifications on this object
	///
	/// This function acts as a barrier. It will complete when all
	/// outstanding operations are complete and written to stable storage.
	virtual future<> commit() = 0;
      };
    } // namespace mem
  } // namespace store
} // namespace crimson

#endif // CRIMSON_STORE_MEM_OBJECT_H
