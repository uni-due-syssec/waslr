// Randomize Granules within a Chunk
#define RAND_GRANULES
#define SMALL_OBJECT_RANDOM_CHUNKS 4
#define MINIMUM_NEW_PAGES 128
#define FIRST_USABLE_PAGE_IDX 1

// prevent inlining of malloc/free so the special prologue applies
#define NO_WASLR_INLINE __attribute__((waslr_no_rand,noinline))
// all other function can be inlined if necessary, but they should use the original prologue
#define NO_WASLR __attribute__((waslr_no_rand))

#include "common.h"
#include "waslr.h"

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

#define MEMORY_START 1024

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
  M(1) M(2) M(3) M(4) M(5) M(6) M(8) M(10) M(16) M(32)

enum chunk_kind {
  FREE_CHUNK,
#define DEFINE_SMALL_OBJECT_CHUNK_KIND(i) GRANULES_##i,
  FOR_EACH_SMALL_OBJECT_GRANULES(DEFINE_SMALL_OBJECT_CHUNK_KIND)
#undef DEFINE_SMALL_OBJECT_CHUNK_KIND
  SMALL_OBJECT_CHUNK_KINDS,
  FREE_LARGE_OBJECT = 254,
  LARGE_OBJECT = 255
};

static const uint8_t small_object_granule_sizes[] = 
{
#define SMALL_OBJECT_GRANULE_SIZE(i) i,
  FOR_EACH_SMALL_OBJECT_GRANULES(SMALL_OBJECT_GRANULE_SIZE)
#undef SMALL_OBJECT_GRANULE_SIZE
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
  switch (kind) {
#define CHUNK_KIND_GRANULE_SIZE(i) case GRANULES_##i: return i;
  FOR_EACH_SMALL_OBJECT_GRANULES(CHUNK_KIND_GRANULE_SIZE);
#undef CHUNK_KIND_GRANULE_SIZE
    default:
      return -1;
  }
}

// these are all not needed as they will always have the same value as other already defined variables
// we use these for better readability
#define PAGE_HEADER_SIZE CHUNKS_PER_PAGE
#define PAGE_HEADER_LOG2 CHUNKS_PER_PAGE_LOG2
#define PAGES_PER_HEADER_PAGE (CHUNK_SIZE-1) // - 1 so the group size is always a power of 2
#define PAGE_GROUP_SIZE (PAGES_PER_HEADER_PAGE + 1) // or CHUNK_SIZE, but this is more readable
#define PAGES_PER_GROUP_MASK (PAGE_GROUP_SIZE - 1) // also the same as CHUNK_MASK
#define PAGE_GROUP_LOG2 CHUNK_SIZE_LOG_2

// hard to make dynamic since it depends on the value we want to divide by
#define FAST_MOD_PAGE(x) ((x) & PAGE_MASK)
#define FAST_MOD_CHUNK(x) ((x) & CHUNK_MASK)

struct header_page {
    uint8_t meta[PAGE_HEADER_SIZE]; // currently unused
    uint8_t headers[PAGE_SIZE-PAGE_HEADER_SIZE];
};

struct page {
    struct chunk chunks[CHUNKS_PER_PAGE];
};

struct page_group {
  struct header_page headerpage;
  struct page data_pages[PAGES_PER_HEADER_PAGE];
};

NO_WASLR static struct page* get_page(void *ptr) {
  return (struct page*) (char*) (((uintptr_t) ptr) & ~PAGE_MASK);
}
NO_WASLR static unsigned get_chunk_index(void *ptr) {
  return (((uintptr_t) ptr) & PAGE_MASK) >> CHUNK_SIZE_LOG_2;
}

struct freelist {
    struct freelist *next;
};

static struct freelist *small_object_freelists[SMALL_OBJECT_CHUNK_KINDS];

struct search_result {
  size_t chunks_left;
  size_t next_hpage_idx;
};

NO_WASLR size_t grow_wasm_memory(size_t numPages){
  if (__builtin_wasm_memory_grow(0, numPages) == -1) {
    return 0;
  }
  return 1;
}

NO_WASLR static void allocate_chunk(struct header_page *page, unsigned idx, enum chunk_kind kind) {
  page->headers[idx] = kind;
}

NO_WASLR static void allocate_pages(size_t payload_size) {
  // grow memory atleast by 128 pages, or by enough pages to store the payload size * 4, whichever is larger
  size_t pages_to_alloc = max(MINIMUM_NEW_PAGES, align(payload_size << 2, PAGE_SIZE) >> PAGE_SIZE_LOG_2);
  // TODO
  // return something? maybe the number of new header pages? 
}

void fisher_yates_shuffle(size_t *array, size_t size) {
  for (size_t i = size - 1; i > 0; i--) {
    size_t j = rand() % (i + 1);
    size_t temp = array[i];
    array[i] = array[j];
    array[j] = temp;
  }
}

NO_WASLR void WASM_EXPORT(waslr_init)(unsigned int seed) {
  srand(seed);
  grow_wasm_memory(300);
}

NO_WASLR static inline size_t size_to_granules(size_t size) {
  return (size + GRANULE_SIZE - 1) >> GRANULE_SIZE_LOG_2;
}

NO_WASLR static struct freelist** get_small_object_freelist(enum chunk_kind kind) {
  ASSERT(kind < SMALL_OBJECT_CHUNK_KINDS);
  return &small_object_freelists[kind];
}

NO_WASLR static struct search_result find_free_chunks(size_t num_chunks, enum chunk_kind kind, size_t start_at, size_t *found_chunks) {
  // since we request multiple free chunks most times, if we find that one page is full, then we should prevent searching that page for the next chunks in this request
  size_t usable_pages = __builtin_wasm_memory_size(0) - FIRST_USABLE_PAGE_IDX;
  size_t total_hpages_to_search = ((usable_pages + PAGES_PER_HEADER_PAGE) >> PAGE_GROUP_LOG2) - start_at; // Shift equal to div by (PAGES_PER_HEADER+1) with PAGES_PER_HEADER := CHUNK_SIZE-1
  // pages represented by the last header page
  size_t usable_pages_tail = (usable_pages - 1) & PAGES_PER_GROUP_MASK;

  /*
    trick to randomize selection of header pages while preventing pages that were already searched to be searched again
    1) array of header page indices; max_index := num of headers - 1
    2) generate random index between 0 and max_index
    3) if the page index stored at header_pages[i] was searched (and is full), swap the element to max_index and reduce max_index by 1
    => the elements from 0..max_index are always unsearched header page indices
    => allows us to pick a random index efficiently while excluding searched pages
  */
  size_t header_pages[total_hpages_to_search];
  size_t max_index = total_hpages_to_search - 1;

  if (total_hpages_to_search > 1) {
    for (size_t i = 0; i<total_hpages_to_search; i++) {
      header_pages[i] = start_at + i;
    }
  } 

  size_t chunks_left = num_chunks;
  while (chunks_left > 0) {
    // the index of the header page within all header pages
    size_t selected_header_idx = max_index > 0 ? header_pages[rand_in_range(0, max_index)] : start_at;
    // the global page index of the selected header page 
    size_t global_headerpage_idx = 1 + (selected_header_idx << PAGE_GROUP_LOG2);
    
    console_uintptr("SELECTED HEADER PAGE:", selected_header_idx);
    console_uintptr("GLOBAL HEADER PAGE IDX:", global_headerpage_idx);
    struct header_page *headerpage = (struct header_page *) (global_headerpage_idx << PAGE_SIZE_LOG_2);

    size_t hpage_usable_pages = selected_header_idx == total_hpages_to_search-1 ? usable_pages_tail : PAGES_PER_HEADER_PAGE;
    // we only want to check one byte per usable page that follows the header page
    size_t max_bytes_to_check = hpage_usable_pages << PAGE_HEADER_LOG2;

    // pick a random offset within the headers, which is the header byte for some data chunk
    size_t header_idx_for_chunk = rand_in_range(0, min(max_bytes_to_check, PAGE_SIZE) - 1);
    console_uintptr("RAND_START:", header_idx_for_chunk);
    // search for free chunk on the pages represented by the header page
    size_t attempts = 0;
    while (headerpage->headers[header_idx_for_chunk] != FREE_CHUNK && attempts < max_bytes_to_check) {
      // TODO: instead of mod each loop, would it be more efficient to do 2 separate for loops (from rand_idx..end and 0..rand_idx)?
      header_idx_for_chunk = (header_idx_for_chunk+1) % max_bytes_to_check;
      attempts++;
    }

    if (attempts == max_bytes_to_check) {
      // no free chunk found

      // ensure we do not search this headerpage again in this request by swapping elements
      // if the index is the last valid page, no need to swap
      if (max_index != (selected_header_idx-start_at)) {
        size_t temp = header_pages[max_index];
        header_pages[max_index] = header_pages[selected_header_idx];
        header_pages[selected_header_idx] = temp;
      }


      if (max_index == 0) {
        // we searched all available header pages
        // if the last header page has space for newly allocated pages, we should search it again after allocating new pages
        //  - last page has 255 usable pages => allocate new pages and start search at last_page_idx+1
        //  - last page has <255 usable pages => allocate new pages and start search at last_page_idx
        size_t next_hpage_idx = usable_pages_tail == PAGES_PER_HEADER_PAGE ? total_hpages_to_search : total_hpages_to_search-1;

        struct search_result res = {chunks_left, next_hpage_idx};
        return res;
      } 
      
      max_index--;

      // no free chunk found, but there are still header pages left to search. Go to next header page
      continue;
    } 

    // found a free chunk
    chunks_left--;
    // save the pointer to the chunk
    found_chunks[chunks_left] = (((global_headerpage_idx+1) << CHUNKS_PER_PAGE_LOG2) + header_idx_for_chunk) << CHUNK_SIZE_LOG_2;

    allocate_chunk(headerpage, header_idx_for_chunk, kind);
  }
  // next hpage is never used in this case
  struct search_result res = {chunks_left, total_hpages_to_search-1};
  return res;
}


NO_WASLR static struct freelist* obtain_small_objects(enum chunk_kind kind) {
  // populate whole_chunk_freelist with N random chunks 
  size_t found_chunks[SMALL_OBJECT_RANDOM_CHUNKS];
  // search from header page idx 0 
  struct search_result res = find_free_chunks(SMALL_OBJECT_RANDOM_CHUNKS, kind, 0, found_chunks);
  if (res.chunks_left > 0) {
    // could not find space for all chunks requested, allocate more 
    allocate_pages(SMALL_OBJECT_RANDOM_CHUNKS << CHUNK_SIZE_LOG_2);
    // continue searching taking into account the pages we already identified as full
    res = find_free_chunks(res.chunks_left, kind, res.next_hpage_idx, found_chunks);
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

  #ifdef RAND_GRANULES
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
    console_uintptr("CHUNK:", found_chunk_idx);
    console_uintptr("GLOBALPTR:", (uintptr_t)head);

    head->next = next;
    next = head;
  }

  #else

  for (size_t idx = 0; idx < numElements; idx++) {
    size_t found_chunk_idx = idx / num_elements_per_chunk;
    size_t chunk_local_idx = idx % num_elements_per_chunk;
    struct freelist *head = (struct freelist*) (found_chunks[found_chunk_idx] + (chunk_local_idx*size_granules));
    head->next = next;
    next = head;
  }

  #endif

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
    return (void *) ret;
}

// for testing, delete later
NO_WASLR void WASM_EXPORT(find_space)(size_t size) {
  size_t num_chunks;
  enum chunk_kind kind;

  if (size > CHUNK_SIZE) {
    // Large object
    num_chunks = align(size, CHUNK_SIZE) / CHUNK_SIZE;
    kind = LARGE_OBJECT;
  } else {
    // Small object
    size_t granules = size_to_granules(size);
    num_chunks = SMALL_OBJECT_RANDOM_CHUNKS;
    kind = granules_to_chunk_kind(granules);
  }

  // TODO: dont need this array in large object case
  // TODO: remember large objects should be able to span multiple pages
  //    - LOs cannot go across page group boundaries due to the header page inbetween.
  size_t found_chunks[num_chunks];
  // search from header page idx 0 
  struct search_result res = find_free_chunks(num_chunks, kind, 0, found_chunks);
  if (res.chunks_left > 0) {
    // could not find space for all chunks requested
    allocate_pages(size);
    // continue searching taking into account the pages we already identified as full
    find_free_chunks(res.chunks_left, kind, res.next_hpage_idx, found_chunks);
  }

  // for testing
  console_uintarray("CHUNKS: ", (uint32_t *)found_chunks, SMALL_OBJECT_RANDOM_CHUNKS);
  
  for (int i=0; i<4; i++) {
    size_t ptr = found_chunks[i] << 8;
    console_uintptr("PTR", ptr);
    console_uintptr("page", ptr >> 16);
  }
  // TODO ideally build freelist here
  //  - pass a pointer to an array of size num_chunks into find_free_chunks
  //  - when a chunk if found, store the chunk offset at arr[chunks_left]
  // shuffle all elements from <num> chunks
}

NO_WASLR_INLINE void* WASM_EXPORT(malloc)(size_t size) {
    size_t granules = size_to_granules(size);
    enum chunk_kind kind = granules_to_chunk_kind(granules);
    return (kind == LARGE_OBJECT) ? NULL : allocate_small(kind);
}

NO_WASLR_INLINE void free(void *ptr) {

}



/*

page 0: untouched
page 1: header page
page 2: first data page
  - first 8000 bytes for walloc stack




*/

/*

page 1 skip for walloc stack frames
- alloc large obj of size 8000



normally:

page0
0: page_header
256: chunk1
512: chunk2
...



8 byte alloc
freelistfreelist[8] [8] : 32 * ptr(8), 32 * ptr(8), 32 * ptr(8), 32 * ptr(8)

256 byte header
123 
124
125

page
LO 65536

256
256
LO




freelist[GRANULES_1 (8 bytes)]:
    - 32 entries

freelist[8] : 32 * ptr(8)
freelist[16] : 16 * ptr(16)

each freelist has 1-32 entries, depending on chunk_kind

only one page is considered for allocations at a time

=> how can we consider many pages without blowing up the freelist?

A) instead of populating the freelist with only one chunk:
- get N (4?) random chunks, and reserve them for this class
    - shuffle and put in a freelist
- maybe reduce the chunk size and increase the number of random chunks


B) freelists per page?

header_page
0: page1
256: page2
512: page3
...

page1
0: chunk0
256: chunk1
512: chunk2
...

page2
0: chunk257
256: chunk258
512: chunk259




*/