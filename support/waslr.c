#include "waslr.h"

#define SMALL_OBJECT_RANDOM_CHUNKS 4
// TODO: Probably set multiplier to 2, 4 is overkill
#define GROW_MEMORY_MULTIPLIER_LOG_2 2 // when increasing memory, we grow by the number of bytes times a multiplier (power of 2)
#define INITIAL_PAGES_ALLOC 252
#define MINIMUM_NEW_PAGES 128
// 0: unused, 1: for manual allocations (e.g. waslr stackframes)
#define FIRST_USABLE_PAGE_IDX 2
//static size_t doinit = 0;
// easy way to ensure that SO freelist lives at a specific offset
// without having to implement custom lowering in llvm
#define SO_FREELIST_START 75000
#define NUM_KINDS 10
#define LIST_SIZE (NUM_KINDS * sizeof(struct freelist *)) 
#define MAP_LOCATION_END (MAP_LOCATION_START + LIST_SIZE - 1)
#define small_object_freelists ((struct freelist **)(SO_FREELIST_START))

// functions with this attribute will use the original prologue/epilogue
#define NO_WASLR __attribute__((waslr_no_rand,noinline))
// prevent inlining of malloc/free so the special prologue applies
#define NO_INLINE __attribute__((noinline))
#define USED __attribute__((used))

#define STATIC_ASSERT_EQ(a, b) _Static_assert((a) == (b), "eq")

#ifndef NDEBUG
#define ASSERT(x) do { if (!(x)) __builtin_trap(); } while (0)
#else
#define ASSERT(x) do { } while (0)
#endif
#define ASSERT_EQ(a,b) ASSERT((a) == (b))

static inline size_t max(size_t a, size_t b) {
  return a < b ? b : a;
}

static inline size_t min(size_t a, size_t b) {
  return a < b ? a : b;
}

static inline uintptr_t align(uintptr_t val, uintptr_t alignment) {
  return (val + alignment - 1) & ~(alignment - 1);
}
#define ASSERT_ALIGNED(x, y) ASSERT((x) == align((x), y))

#define CHUNK_SIZE 256
#define CHUNK_SIZE_LOG_2 8
#define CHUNK_MASK (CHUNK_SIZE - 1)
STATIC_ASSERT_EQ(CHUNK_SIZE, 1 << CHUNK_SIZE_LOG_2);

#define PAGE_SIZE_LOG_2 16
#define PAGE_MASK (PAGE_SIZE - 1)
STATIC_ASSERT_EQ(PAGE_SIZE, 1 << PAGE_SIZE_LOG_2);

STATIC_ASSERT_EQ(0, PAGE_SIZE % CHUNK_SIZE);
#define CHUNKS_PER_PAGE (PAGE_SIZE / CHUNK_SIZE)
#define CHUNKS_PER_PAGE_LOG2 (PAGE_SIZE_LOG_2 - CHUNK_SIZE_LOG_2)

#define GRANULE_SIZE 8
#define GRANULE_SIZE_LOG_2 3
#define LARGE_OBJECT_THRESHOLD 256
#define LARGE_OBJECT_GRANULE_THRESHOLD 32

STATIC_ASSERT_EQ(GRANULE_SIZE, 1 << GRANULE_SIZE_LOG_2);
STATIC_ASSERT_EQ(LARGE_OBJECT_THRESHOLD,
                 LARGE_OBJECT_GRANULE_THRESHOLD * GRANULE_SIZE);

struct chunk {
    char data[CHUNK_SIZE];
};

#define FOR_EACH_SMALL_OBJECT_GRANULES(M) \
  M(1) M(2) M(4) M(8) M(16) M(32)
  //M(1) M(2) M(3) M(4) M(5) M(6) M(8) M(10) M(16) M(32)
  
enum chunk_kind {
  FREE_CHUNK,
#define DEFINE_SMALL_OBJECT_CHUNK_KIND(i) GRANULES_##i,
  FOR_EACH_SMALL_OBJECT_GRANULES(DEFINE_SMALL_OBJECT_CHUNK_KIND)
#undef DEFINE_SMALL_OBJECT_CHUNK_KIND
  SMALL_OBJECT_CHUNK_KINDS,
  LARGE_OBJECT_START = 254,
  LARGE_OBJECT = 255
};

// rand returns int
// min must always be smaller than max. Our implementation ensures that so we do not explicitly check
// max range is RAND_INT, which is more than enough for our uses
size_t rand_in_range(size_t min, size_t max) {
    return min + (rand() % (max - min + 1));
}

// functions to convert between chunk_kind and granules
NO_WASLR static enum chunk_kind granules_to_chunk_kind(unsigned granules) {
#define TEST_GRANULE_SIZE(i) if (granules <= i) return GRANULES_##i;
  FOR_EACH_SMALL_OBJECT_GRANULES(TEST_GRANULE_SIZE);
#undef TEST_GRANULE_SIZE
  return LARGE_OBJECT;
}

NO_WASLR static unsigned chunk_kind_to_granules(enum chunk_kind kind) {
  if (kind == 0) return 0;
  return 1U << (kind-1);
}

// these are all not needed as they will always have the same value as other already defined variables
// we use these for better readability
#define PAGE_HEADER_SIZE CHUNKS_PER_PAGE
#define PAGE_HEADER_LOG2 CHUNKS_PER_PAGE_LOG2
#define HEADERS_TOTAL_SIZE (PAGE_SIZE - PAGE_HEADER_SIZE)
#define DATA_PAGES_PER_GROUP (CHUNK_SIZE-1) // - 1 so the group size is always a power of 2
#define PAGE_GROUP_LOG2 CHUNK_SIZE_LOG_2
#define PAGES_PER_GROUP (DATA_PAGES_PER_GROUP + 1) // or CHUNK_SIZE, but this is more readable
#define PAGES_PER_GROUP_MASK (PAGES_PER_GROUP - 1) // also the same as CHUNK_MASK
#define PAGE_GROUP_SIZE_MASK ((PAGES_PER_GROUP << PAGE_SIZE_LOG_2) - 1)

#define MAX_ALLOC_SIZE (PAGES_PER_GROUP * PAGE_SIZE)

struct header_page {
    uint8_t meta[PAGE_HEADER_SIZE]; // currently unused
    uint8_t headers[HEADERS_TOTAL_SIZE];
};

struct page {
    struct chunk chunks[CHUNKS_PER_PAGE];
};

NO_WASLR static struct header_page* get_headerpage(void *ptr) {
  size_t bias = FIRST_USABLE_PAGE_IDX << PAGE_SIZE_LOG_2;
  size_t headerpage_ptr = (((uintptr_t)ptr - bias) & ~PAGE_GROUP_SIZE_MASK) + bias;
  return (struct header_page*) headerpage_ptr;
}

NO_WASLR static unsigned get_header_chunk_index(struct header_page *headerpage, void *ptr) {
  // skip header page
  uintptr_t data_start = (uintptr_t)headerpage + PAGE_SIZE;
  // calc relative offset of ptr within the data pages
  uintptr_t rel_offset = (uintptr_t)ptr - data_start;
  // convert to offset in chunks
  return rel_offset >> CHUNK_SIZE_LOG_2;
}

struct freelist {
    struct freelist *next;
};

char* TESTSTR = "Hello World!";
int TESTINT = 200;

//static struct freelist *small_object_freelists[SMALL_OBJECT_CHUNK_KINDS];

struct search_result {
  size_t chunks_left;
  size_t next_group_idx;
};

// currently kind of unnecessary but more readable
NO_WASLR size_t grow_wasm_memory(size_t numPages){
  debug_early(888);
  debug_early(__builtin_wasm_memory_size(0));
  int grow = __builtin_wasm_memory_grow(0, numPages);
  debug_early(__builtin_wasm_memory_size(0));
  debug_early(889);
  if (grow == -1) {
    return 0;
  }
  return 1;
}

NO_WASLR static void allocate_chunks(struct header_page *page, unsigned idx, enum chunk_kind kind, size_t num_chunks) {
  size_t offset = idx;
  size_t i = 0;
  if(kind == LARGE_OBJECT) {
    // Large objects require the start byte to be another value
    page->headers[offset] = LARGE_OBJECT_START;
    i++;
  }
  for (; i<num_chunks; i++) {
    offset = idx + i;
    uint8_t current = page->headers[offset];
    /*if (current!=FREE_CHUNK){
      alloc_error((uintptr_t)page, idx, current, kind);
    }*/
    page->headers[offset] = kind;
  }
  //console_uintptr("ALLOCATED ON HP:", (uintptr_t) page);
  //console_uintptr("ALLOCATED CHUNKS:", i);
}

NO_WASLR static void free_consecutive_chunks(struct header_page *page, unsigned idx, enum chunk_kind kind) {
  // Free the first byte manually to deal with the different start byte of LOs
  size_t offset = 1;
  page->headers[idx] = FREE_CHUNK;

  while(idx+offset < (HEADERS_TOTAL_SIZE-1) && page->headers[idx+offset] == kind) {
    page->headers[idx+offset] = FREE_CHUNK;
    offset += 1;
  }
}

NO_WASLR static size_t allocate_pages(size_t payload_size) {
  // grow memory atleast by MINIMUM_NEW_PAGES pages, or by enough pages to store the payload size * 4, whichever is larger
  size_t pages_to_alloc = max(MINIMUM_NEW_PAGES, align(payload_size << GROW_MEMORY_MULTIPLIER_LOG_2, PAGE_SIZE) >> PAGE_SIZE_LOG_2);
  if (!grow_wasm_memory(pages_to_alloc)) {
    return 0;
  }

  return pages_to_alloc;
}

static void fisher_yates_shuffle(size_t *array, size_t size) {
  for (size_t i = size - 1; i > 0; i--) {
    size_t j = rand() % (i + 1);
    size_t temp = array[i];
    array[i] = array[j];
    array[j] = temp;
  }
}

// can either export or mark as used to force retention of the symbol until linking
// TODO: Change export to used when no longer needed for testing
NO_WASLR void WASM_EXPORT(__waslr_init)(unsigned int seed) {
  srand(seed);
  grow_wasm_memory(INITIAL_PAGES_ALLOC);
}

NO_WASLR static inline size_t size_to_granules(size_t size) {
  return (size + GRANULE_SIZE - 1) >> GRANULE_SIZE_LOG_2;
}

NO_WASLR static struct freelist** get_small_object_freelist(enum chunk_kind kind) {
  ASSERT(kind < SMALL_OBJECT_CHUNK_KINDS);
  return &small_object_freelists[kind];
}

// for debugging only
NO_WASLR void check_headerpage(struct header_page* headerpage, size_t size_chunks, size_t start_at_group) {
  size_t usable_pages = __builtin_wasm_memory_size(0) - FIRST_USABLE_PAGE_IDX;
  size_t total_groups = ((usable_pages + DATA_PAGES_PER_GROUP) >> PAGE_GROUP_LOG2);
  size_t total_groups_to_search = total_groups - start_at_group; 
  // data pages represented by the last header page. -1 for the header page itself
  size_t last_group_data_pages = (usable_pages - 1) & PAGES_PER_GROUP_MASK;

  size_t selected_group_idx = (((uintptr_t)headerpage / PAGE_SIZE) - 2) / 256;
  size_t usable_pages_in_group = selected_group_idx == total_groups - 1 ? last_group_data_pages : DATA_PAGES_PER_GROUP;

  size_t max_bytes_to_check = usable_pages_in_group << PAGE_HEADER_LOG2;

  size_t prev_idx = 0;
  size_t idx = 0;
  size_t found = 0;
  size_t free_chunks = 0;
  size_t non_free_chunks = 0;
  size_t current = 0; // 0 = free; 1 = not free
  for (; idx < max_bytes_to_check; idx++) {
    if (headerpage->headers[idx] == FREE_CHUNK) {
      if (current == 1) {
        // switch
        console_uintptr("NOT FREE COUNT:", found);
        found = 0;
        current = 0;
      }
      found++;
      free_chunks++;
    } else {
      if (current == 0) {
        // switch
        console_uintptr("FREE COUNT:", found);
        if (found >= size_chunks) {
          console_uintptr("alloc could fit here size:", size_chunks);
        }
        found = 0;
        current = 1;
      } 
      found++;
      non_free_chunks++;
    }
  }
  if (current == 1) {
    console_uintptr("NOT FREE COUNT:", found);
  } else {
    console_uintptr("FREE COUNT:", found);
    if (found >= size_chunks) {
      console_uintptr("alloc could fit here size:", size_chunks);
    }
  }
  //debug_early(idx);
  //debug_early(non_free_chunks);
  //debug_early(free_chunks);

}

// max return value is 65535 and we also want to return negative values to indicate errors
NO_WASLR static int32_t search_headerpage(struct header_page *headerpage, size_t total_hpages, size_t usable_pages_group, size_t num_chunks){
  // we only want to check one byte per usable chunk that follows the header page
  size_t max_bytes_to_check = usable_pages_group << PAGE_HEADER_LOG2;

  // pick a random offset within the header page, which is the header byte for some data chunk
  size_t header_byte_for_chunk = rand_in_range(0, max_bytes_to_check - 1);
  // search for free chunk on the pages represented by the header page
  size_t bytes_searched = 0;

  console_uintptr("MAX TO SEARCH:", max_bytes_to_check);
  console_uintptr("START SEARCH AT:", header_byte_for_chunk);
  
  if (num_chunks == 1) {
    // faster search for a single chunk
    for (; bytes_searched<max_bytes_to_check; bytes_searched++) {
      if (headerpage->headers[header_byte_for_chunk] == FREE_CHUNK) {
        break;
      }
      // do this instead of modulo. Should be faster
      if (++header_byte_for_chunk >= max_bytes_to_check) {
        header_byte_for_chunk = 0;
      }
    }
  } else {
    // find num_chunks of consecutive free chunks
    size_t free_chunks_counter = 0;
    for (; bytes_searched < max_bytes_to_check; bytes_searched++) {
      if (headerpage->headers[header_byte_for_chunk] == FREE_CHUNK) {
        if (++free_chunks_counter == num_chunks) {
          // we want the start of the free sequence, not the end
          header_byte_for_chunk -= (num_chunks -1);
          break;
        }
      } else {
        free_chunks_counter = 0;
      }

      if (++header_byte_for_chunk >= max_bytes_to_check) {
        header_byte_for_chunk = 0;
        // if we reach the end of the header page, we have to reset the counter too!
        free_chunks_counter = 0;
      }
    }
  }

  if (bytes_searched == max_bytes_to_check) {
    // we did not find any free chunk on this header page
    return -1;
  }
  console_uintptr("FOUND FREE CHUNK START:", header_byte_for_chunk);
  return (int32_t)header_byte_for_chunk;
}

// function to search for both large and small allocations
// num_chunks = size of the allocation in chunks
// num_picks = number of allocations (of size num_chunks*CHUNK_SIZE bytes)
NO_WASLR static struct search_result find_free_chunks(size_t *found_chunks, size_t num_chunks, size_t num_picks, enum chunk_kind kind, size_t start_at_group) {
  size_t usable_pages = __builtin_wasm_memory_size(0) - FIRST_USABLE_PAGE_IDX;
  size_t total_groups = ((usable_pages + DATA_PAGES_PER_GROUP) >> PAGE_GROUP_LOG2);
  // number of header pages to consider in this search (excludes pages we already searched for this request)
  size_t total_groups_to_search = total_groups - start_at_group;
  // allocated data pages of the last group. -1 for the header page 
  size_t last_group_data_pages = (usable_pages - 1) & PAGES_PER_GROUP_MASK;

  /*
    trick to randomize selection of header pages while preventing pages that were already searched to be searched again
    1) array of header page indices; max_index := num of headers - 1
    2) generate random index between 0 and max_index
    3) if the header page of page_groups[i] was searched (and is full), swap the element to max_index and reduce max_index by 1
    => the elements from 0..max_index are always unsearched header page indices
    => allows us to pick a random index somewhat efficiently while excluding searched pages
  */
  size_t page_groups[total_groups_to_search];
  size_t max_index = total_groups_to_search - 1;

  for (size_t i = 0; i<total_groups_to_search; i++) {
    page_groups[i] = start_at_group + i;
  }

  size_t chunks_left = num_picks;
  while (chunks_left > 0) {
    size_t group_array_idx = max_index > 0 ? rand_in_range(0, max_index) : 0;
    // the index of the header page within all header pages
    size_t selected_group_idx = page_groups[group_array_idx];
    console_uintptr("GROUP IDX SEARCH:", selected_group_idx);
  
    // the global page index of the header page from the selected group
    size_t global_headerpage_idx = FIRST_USABLE_PAGE_IDX + (selected_group_idx << PAGE_GROUP_LOG2);
    struct header_page *headerpage = (struct header_page *) (global_headerpage_idx << PAGE_SIZE_LOG_2);

    console_uintptr("GROUPS TO SEARCH", total_groups_to_search);
    size_t usable_pages_in_group = selected_group_idx == total_groups- 1 ? last_group_data_pages : DATA_PAGES_PER_GROUP;

    console_uintptr("--- SEARCHING HP:", (uintptr_t) headerpage);
    check_headerpage(headerpage, num_chunks, start_at_group);
    int32_t found_chunk_idx = search_headerpage(headerpage, total_groups_to_search, usable_pages_in_group, num_chunks);

    if (found_chunk_idx == -1) {
      // no free chunk found

      // ensure we do not search this headerpage again in this request by swapping elements
      // if the index is the last valid page, no need to swap
      if (group_array_idx != max_index) {
        size_t temp = page_groups[max_index];
        page_groups[max_index] = selected_group_idx;
        page_groups[group_array_idx] = temp;
      }


      if (max_index == 0) {
        // we searched all available header pages. Determine which header page we search next after allocating more space
        size_t next_group_idx;
        if (last_group_data_pages == DATA_PAGES_PER_GROUP) {
          // the last page group is fully allocated (header page represents DATA_PAGES_PER_GROUP allocated data pages)
          // => we begin searching on the newly created header page after allocating more space
          next_group_idx = total_groups_to_search;
        } else {
          // the last header page's group can fit some more data pages
          // => we will search the current header page again after allocating more space
          size_t unused_pages = DATA_PAGES_PER_GROUP - last_group_data_pages;
          if (((num_chunks + CHUNKS_PER_PAGE -1) >> 8) > unused_pages) {
            // if the unused pages cannot even fit the allocation (number of chunks to be allocated consecutively), then we do not need to search the current header page again
            next_group_idx = total_groups_to_search;
          } else{
            // search the last header page again since we will add more data pages to it
            next_group_idx = total_groups_to_search - 1;
          }
        }
        
        struct search_result res = {chunks_left, next_group_idx};
        return res;
      } 
      
      max_index--;

      // no free chunk found, but there are still header pages left to search. Go to next header page
      continue;
    } 

    // found a free chunk
    chunks_left--;
    // save the pointer to the chunk
    found_chunks[chunks_left] = (((global_headerpage_idx+1) << CHUNKS_PER_PAGE_LOG2) + found_chunk_idx) << CHUNK_SIZE_LOG_2;

    allocate_chunks(headerpage, found_chunk_idx, kind, num_chunks);
  }
  // next hpage is never used in this case
  struct search_result res = {chunks_left, total_groups_to_search-1};
  return res;
}


NO_WASLR static struct freelist* obtain_small_objects(enum chunk_kind kind) {
  // populate whole_chunk_freelist with N random chunks 
  size_t found_chunks[SMALL_OBJECT_RANDOM_CHUNKS];
  // search from header page idx 0 
  struct search_result res = find_free_chunks(found_chunks, 1, SMALL_OBJECT_RANDOM_CHUNKS, kind, 0);
  if (res.chunks_left > 0) {
    // could not find space for all chunks requested, allocate more 
    size_t new_pages = allocate_pages(SMALL_OBJECT_RANDOM_CHUNKS << CHUNK_SIZE_LOG_2);
    if (!new_pages) {
      return NULL;
    }
    // continue searching taking into account the pages we already identified as full
    res = find_free_chunks(found_chunks, 1, res.chunks_left, kind, res.next_group_idx);
    if (res.chunks_left > 0) {
      // should not happen since we allocated enough new space before
      return NULL;
    }
  }
  // we have an array of allocated chunks
  struct freelist *next = NULL;
  size_t size_granules = chunk_kind_to_granules(kind) << GRANULE_SIZE_LOG_2;
  // Example: GRANULES_1 => 32 x 8 byte ptrs per chunk, 4 different chunks => 128 ptrs total 
  size_t num_elements_per_chunk = CHUNK_SIZE / size_granules; 
  size_t numElements = num_elements_per_chunk * SMALL_OBJECT_RANDOM_CHUNKS;

  size_t indices[numElements];
    for (size_t i=0; i<numElements; i++) {
    indices[i] = i;
  }
  fisher_yates_shuffle(indices, numElements);

  for (size_t idx = 0; idx < numElements; idx++) {
    size_t i = indices[idx];
    size_t found_chunk_idx = i / num_elements_per_chunk;
    size_t chunk_local_idx = i % num_elements_per_chunk;
    struct freelist *head = (struct freelist*) (found_chunks[found_chunk_idx] + (chunk_local_idx*size_granules));
  
    head->next = next;
    next = head;
  }

  return next;  
}

NO_WASLR static void* allocate_small(enum chunk_kind kind) {
    struct freelist **loc = get_small_object_freelist(kind);
    if (!*loc) {
        // freelist empty
        struct freelist *freelist = obtain_small_objects(kind);
        if (!freelist) {
            return NULL;
        }
        *loc = freelist;
    }

    struct freelist *ret = *loc;
    *loc = ret->next;

    debug_early(102);
    debug_early(kind);
    debug_early((uintptr_t)ret);
    debug_early(22222);
    return (void *) ret;
}

NO_WASLR static void* allocate_large(size_t size) {
  size_t size_in_chunks = (size + CHUNK_MASK) >> CHUNK_SIZE_LOG_2;
  size_t found_chunks[1]; // array of size 1 so we do not need extra handling in find_free_chunks
  struct search_result res = find_free_chunks(found_chunks, size_in_chunks, 1, LARGE_OBJECT, 0);
  
  if (res.chunks_left > 0) {
    debug_early(7770);
    // could not find space for all chunks requested, allocate more 
    size_t new_pages = allocate_pages(size);
    if (!new_pages) {
      debug_early(22220);
      return NULL;
    }
    // continue searching taking into account the pages we already identified as full
    res = find_free_chunks(found_chunks, size_in_chunks, 1, LARGE_OBJECT, res.next_group_idx);
    if (res.chunks_left > 0) {
      debug_early(22221);
      // should not happen since we allocated enough new space before
      return NULL;
    }
  }
  // we have a pointer to "size_in_chunks" consecutive chunks at index 0 of found_chunks
  debug_early(101);
  debug_early(found_chunks[0]);
  debug_early(size);
  debug_early(size_in_chunks);
  size_t memory_size_bytes = __builtin_wasm_memory_size(0) * PAGE_SIZE;
  size_t alloc_end = found_chunks[0] + size;
  if (alloc_end > memory_size_bytes) {
    debug_early(1919191919);
    debug_early(memory_size_bytes);
    debug_early(alloc_end);
  }
  debug_early(22222);
  //check_headerpage((void*) found_chunks[0]);
  return (void*) found_chunks[0];
}

NO_WASLR NO_INLINE void* WASM_EXPORT(calloc)(size_t num, size_t size) {
    debug_early(999);
    size_t total_size = num*size;

    void* ptr = malloc(total_size);
    if(!ptr) return NULL;

    uint8_t* bptr = (uint8_t*)ptr;
    for(size_t i=0; i<total_size; i++) {
      bptr[i] = 0;
    }
    return ptr;
}

NO_WASLR NO_INLINE void* WASM_EXPORT(malloc)(size_t size) {
    //debug_early(1);
    //debug_early(size);
    console_uintptr("MALLOC SZ:", size);
    /*if(doinit == 0) {
      __waslr_init(2222);
      doinit = 1;
    }*/
    // TODO: need to assert max allocation size here?
    size_t granules = size_to_granules(size);
    enum chunk_kind kind = granules_to_chunk_kind(granules);
    return (kind == LARGE_OBJECT) ? allocate_large(size) : allocate_small(kind);
}

NO_WASLR NO_INLINE void WASM_EXPORT(free)(void *ptr) {
  debug_early(2);
  debug_early((uintptr_t)ptr);
  if (!ptr) return;
  // get header page for pointer
  struct header_page *headerpage = get_headerpage(ptr);
  console_uintptr(TESTSTR, (uintptr_t)headerpage);
  // get header byte that represents the ptr's chunk
  unsigned chunk = get_header_chunk_index(headerpage, ptr);
  console_uintptr("CHUNK: ", (uintptr_t)chunk);

  uint8_t kind = headerpage->headers[chunk];
  if (kind == LARGE_OBJECT_START) {
    console_uintptr("FREE LARGE OBJECT", 0);
    free_consecutive_chunks(headerpage, chunk, LARGE_OBJECT);
  } else {
    console_uintptr("FREE SMALL OBJECT KIND", kind);
    // freed object is put at the front of the freelist
    // TODO: Check if it is necessary to randomize this too
    struct freelist **loc = get_small_object_freelist(kind);
    struct freelist *obj = ptr;
    obj->next = *loc;
    *loc = obj;
  } 
  debug_early(5);
}