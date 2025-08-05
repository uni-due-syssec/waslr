FIRST_USABLE_PAGE = 1

# one page less so that group size is 256 
# 255 pages = 255 chunks on header page 
# is only relevant when we alloc a chunk 
# if we ensure we do not write to the first/last chunk, then there should never be a request to that byte offset, right?
#
CHUNKS_PER_PAGE = 256
CHUNK_SIZE = 256
PAGES_PER_HEADER = CHUNK_SIZE - 1 
CHUNK_SIZE_LOG2 = 8
PAGE_GROUP_SIZE = CHUNK_SIZE

def test_chunk_offset(hpage_idx, chunk_idx, expect_chunk):
    header_page_idx = FIRST_USABLE_PAGE + hpage_idx * (PAGES_PER_HEADER + 1)
    #chunk_idx_page = (FIRST_USABLE_PAGE + 1 + (hpage_idx * PAGES_PER_HEADER)) * CHUNKS_PER_PAGE 
    print(f"HEADER PAGE {hpage_idx} ON PAGE {header_page_idx}")
    # page local chunk idx to global chunk idx 
    global_chunk_idx = ((header_page_idx+1) * CHUNKS_PER_PAGE) + chunk_idx
    print("DATA PAGE IDX:", global_chunk_idx // CHUNKS_PER_PAGE)
    print("DATA PAGE FIRST CHUNK:", (header_page_idx+1)*CHUNKS_PER_PAGE) 
    print("GLOBAL CHUNK INDEX:", global_chunk_idx) 

    offset_alloc = global_chunk_idx * CHUNK_SIZE
    print("OFFSET ALLOC:", offset_alloc)
    print("---------------")
    return offset_alloc

def test_get_headerpage_index_256(ptr):
    my_page_idx = ptr>>16
    my_header_page_idx = (my_page_idx - FIRST_USABLE_PAGE) // 257 
    my_headerpage_pageidx = FIRST_USABLE_PAGE + my_header_page_idx * (PAGES_PER_HEADER+1)
    # 2 -> 1; 257 -> 256 ; 259 -> 258; 771 -> 770
    print(f"MY PAGE: {my_page_idx}; HEADER: {my_header_page_idx}; HEADER GLOBAL: {my_headerpage_pageidx}")

def test_get_headerpage_index_255(ptr):
    page_idx = ptr >> 16
    global_headerpage_idx = 1 + ((page_idx-1) & ~0xFF)
    print(f"PTR {ptr} \n- PAGE: {page_idx} \n- HEADERPAGE: {global_headerpage_idx}")
    headerpage_addr = global_headerpage_idx << 16
    chunk_offset = (ptr- ((global_headerpage_idx + 1) << 16)) >> CHUNK_SIZE_LOG2
    headerbyte_ptr = headerpage_addr + chunk_offset - 1 

    print("- HEADERADDR:", headerpage_addr, "CHUNK OFFS:", chunk_offset)
    print("- HEADERBYTE_ADDR:", headerbyte_ptr)

def test_total_hpages():
    memory_sizes = [2, 256, 257, 258, 512, 513, 514, 768, 769, 770]
    for size in memory_sizes:
        usable_pages = size - FIRST_USABLE_PAGE
        total_hpages = (usable_pages + PAGES_PER_HEADER) >> CHUNK_SIZE_LOG2
        print(f"USABLE PAGES {usable_pages} => HPAGES: {total_hpages}")
        usable_pages_tail = (usable_pages - 1) & 0xFF
        print("- usable tail: ", usable_pages_tail)
    hpage_indices = [0, 1, 2, 3]
    for hpage_idx in hpage_indices:
        global_page_idx = 1 + (hpage_idx << 8)
        print(f"HPAGE IDX {hpage_idx} -> GLOB PAGE IDX: {global_page_idx}")
        page_ptr = global_page_idx << 16
        print(f"GLOB PAGE PTR: ", page_ptr)


if __name__ == "__main__":
    #chunk_start = test_chunk_offset(0, 10, 522)
    #chunk_start2 = test_chunk_offset(0, 65279, 66047)
    #chunk_start3 = test_chunk_offset(1, 0, 66048)
    #chunk_start4 = test_chunk_offset(2, 65279, 197119)

    
    #test_get_headerpage_index_255(chunk_start)
    #test_get_headerpage_index_255(chunk_start2)
    #test_get_headerpage_index_255(chunk_start3)
    #test_get_headerpage_index_255(chunk_start4)
    test_total_hpages()
    # (256-1) = 255 % 256 = 255 => correct
    # (257-2) = 255 % 256 = 255 => wrong, should be 0 
    # (258-2) = 256 % 256 = 0 => wrong, should be 1

    # (256-1) = 255 % 255 = 0 => wrong, should be 255
    # (257-1) = 256 % 255 = 1 => wrong, should be 0
    # (258-1) = 257 % 255 = 2 => wrong, shoul be 1