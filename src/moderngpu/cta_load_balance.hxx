// moderngpu copyright (c) 2016, Sean Baxter http://www.moderngpu.com
#pragma once
#include "cta_merge.hxx"
#include "operators.hxx"
#include "tuple.hxx"

BEGIN_MGPU_NAMESPACE

struct lbs_placement_t {
  merge_range_t range;    // The merge range of *loaded* values. 
                          // May extend b_range one element in each direction.
  int a_index;            // Starting A index for merge.
  int b_index;            // Starting B index for merge.
};

template<int nt, int vt, typename segments_it>
MGPU_DEVICE lbs_placement_t cta_load_balance_place(int tid, 
  merge_range_t range, int count, segments_it segments, int num_segments,
  int* b_shared) {

  // We want to know the value of the segment ID for the segment starting
  // this tile. Load it by decrementing range.b_begin.
  int load_preceding = 0 < range.b_begin;
  range.b_begin -= load_preceding;

  // Load a trailing member of the segment ID array. This lets us read one past
  // the last member: b_key = b_shared[++b0]. Note the use of prefix increment,
  // which gets the beginning of the next identifier, not the current one.
  if(range.b_end < num_segments && range.a_end < count)
    ++range.b_end;

  int load_count = range.b_count();
  int fill_count = nt * vt + 1 + load_preceding - load_count - range.a_count();

  // Fill the end of the array with dest_count.
  for(int i = tid; i < fill_count; i += nt)
    b_shared[load_count + i] = count;

  // Load the segments descriptors into the front of the indices array.
  // TODO: SUBTRACT OUT A_BEGIN FROM B_BEGIN SO WE CAN DO 32-BIT COMPARISONS!
  for(int i = tid; i < load_count; i += nt)
    b_shared[i] = segments[range.b_begin + i];
  __syncthreads();

  // Run a merge path search to find the start of the serial merge for
  // each thread. If we loaded a preceding value from B, increment the 
  // cross-diagonal so that we don't redundantly process it.
  int diag = vt * tid + load_preceding;
  int mp = merge_path<bounds_upper>(counting_iterator_t<int>(range.a_begin),
    range.a_count(), b_shared, load_count + fill_count, diag, less_t<int>());
  __syncthreads();

  // Get the starting points for the merge for A and B. Why do we subtract 1
  // from B? At the start of the array, we are pointing to output 0 and 
  // segment 0. But we don't really start merging A until we've encountered
  // its start flag at B. That is, the first iteration should increment b_index
  // to 0, then start merging from the first segment of A, so b_index needs to
  // start at -1.
  int a_index = range.a_begin + mp;
  int b_index = range.b_begin + (diag - mp) - 1;

  return lbs_placement_t {
    range, a_index, b_index
  };
}

template<int nt, int vt>
struct cta_load_balance_t {
  enum { nv = nt * vt };
  struct storage_t {
    int indices[nv + 2];
  };

  struct result_t {
    lbs_placement_t placement;
    merge_range_t merge_range;

    // thread-order data.
    int merge_flags;

    // strided-order data.
    array_t<int, vt> indices;
    array_t<int, vt> segments;
    array_t<int, vt> ranks;
  };

  template<typename segments_it, typename partition_it>
  MGPU_DEVICE result_t load_balance(int count, segments_it segments, 
    int num_segments, int tid, int cta, partition_it partitions, 
    storage_t& storage) const {

    int mp0 = partitions[cta];
    int mp1 = partitions[cta + 1];

    merge_range_t range = compute_merge_range(count, num_segments, cta, 
      nv, mp0, mp1);

    int* a_shared = storage.indices - range.a_begin;
    int* b_shared = storage.indices + range.a_count();

    lbs_placement_t placement = cta_load_balance_place<nt, vt>(tid, range, 
      count, segments, num_segments, b_shared);

    // Adjust the b pointer by the loaded b_begin. This lets us dereference it
    // directly with the segment index.
    b_shared -= placement.range.b_begin;

    // Store the segment of each element in A.
    int cur_item = placement.a_index;
    int cur_segment = placement.b_index;
    int merge_flags = 0;

    // Fill shared memory with the segment IDs of the in-range values.
    iterate<vt + 1>([&](int i) {
      // Compare the output index to the starting position of the next segment.
      bool p = cur_item < b_shared[cur_segment + 1];
      if(p && i < vt) // Advance A (the needle). 
        a_shared[cur_item++] = cur_segment;
      else  // Advance B (the haystack)
        ++cur_segment;
      merge_flags |= (int)p<< i;
    });
    __syncthreads();

    // Load the segment indices in strided order. Use the segment ID to compute
    // rank of each element. These strided-order (index, seg, rank) tuples
    // will be passed to the lbs functor.
    array_t<int, vt> indices, seg, ranks;
    iterate<vt>([&](int i) {
      int j = nt * i + tid;
      indices[i] = range.a_begin + j;
      if(j < range.a_count()) {
        seg[i] = storage.indices[j];
        ranks[i] = indices[i] - b_shared[seg[i]];
      } else {
        seg[i] = range.b_begin;
        ranks[i] = -1;
      }
    });
    __syncthreads();

    return result_t { 
      placement, range, merge_flags,
      indices, seg, ranks
    };
  }
};


namespace detail {

template<int nv, typename value_t>
struct cached_segment_load_storage_t {
  enum { size = tuple_union_size_t<value_t>::value };
  char bytes[size * (nv + 1)];
};

template<int i, int nt, int vt, typename tpl_t, 
  int size = tuple_size<tpl_t>::value>
struct cached_segment_load_t {
  typedef typename tuple_iterator_value_t<tpl_t>::type_t value_t;
  typedef cached_segment_load_storage_t<nt * vt, value_t> storage_t;

  MGPU_DEVICE static void load(int tid, range_t range,
    array_t<int, vt> segments, storage_t& storage, tpl_t iterators, 
    array_t<value_t, vt>& values) {

    typedef typename tuple_element<i, value_t>::type type_t;
    type_t* shared = (type_t*)storage.bytes;

    // Cooperatively load the values into shared memory.
    shared -= range.begin;
    auto it_i = get<i>(iterators);

    for(int j = range.begin + tid; j < range.end; j += nt)
      shared[j] = it_i[j];
    __syncthreads();

    // Load the values into register.
    iterate<vt>([&](int j) {
      get<i>(values[j]) = shared[segments[j]];
    });
    __syncthreads();

    cached_segment_load_t<i + 1, nt, vt, tpl_t, size>::load(tid, range, 
      segments, storage, iterators, values);
  }
};

template<int nt, int vt, typename tpl_t, int size>
struct cached_segment_load_t<size, nt, vt, tpl_t, size> {
  struct storage_t { };
  template<typename value_t, typename dummy_t>
  MGPU_DEVICE static void load(int tid, range_t range, 
    array_t<int, vt> segments, dummy_t& storage, tpl_t iterators, 
    array_t<value_t, vt>& values) { }
};

} // namespace detail 

END_MGPU_NAMESPACE
