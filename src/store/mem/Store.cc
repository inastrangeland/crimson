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

/// \file mem/Store.cc
/// \brief Fast, in-memory object store
///
/// \author Adam C. Emerson <aemerson@redhat.com>


#include "store/mem/Store.h"

namespace crimson {
  namespace store {
    namespace mem {
      void Store::ref() {
	++ref_cnt;
      }
      void Store::unref() {
	if (--ref_cnt == 0)
	  delete this;
      }

      future<CollectionRef> Store::lookup_collection(string cid) const {
	if (!local())
	  return smp::submit_to(
	    cpu, [this, cid = std::move(cid)]() {
	      return lookup_collection(cid);
	    });

	auto cid_cpu = cpu_for(cid);
	auto find = [cid = std::move(cid), &m = *maps[cid_cpu]] {
	  auto i = m.find(cid);
	  if (i == m.end()) {
	    return make_exception_future<CollectionRef>(
	      std::system_error(errc::no_such_collection));
	  }

	  return make_ready_future<CollectionRef>(
	    CollectionRef(i->second.get()));
	};

	if (cid_cpu == engine().cpu_id())
	  return find();
	else
	  return smp::submit_to(cid_cpu, std::move(find));
      }

      future<CollectionRef> Store::create_collection(string cid) {
	if (!local())
	  return smp::submit_to(
	    cpu, [this, cid = std::move(cid)]() {
	      return create_collection(cid);
	    });

	auto cid_cpu = cpu_for(cid);

	auto maker = [sr = StoreRef(this), cid = std::move(cid),
		      &m = *maps[cid_cpu]]() mutable {
	  auto i = m.find(cid);
	  if (i != m.end()) {
	    return make_exception_future<CollectionRef>(
	      std::system_error(errc::collection_exists));
	  }

	  return make_ready_future<CollectionRef>(
	    CollectionRef(new Collection(std::move(sr), cid, m)));
	};

	if (cid_cpu == engine().cpu_id())
	  return maker();
	else
	  return smp::submit_to(cid_cpu, std::move(maker));
      }

    } // namespace mem
  } // namespace store
} // crimson
