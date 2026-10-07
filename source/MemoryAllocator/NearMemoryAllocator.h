#pragma once

#include "dobby/common.h"
#include "MemoryAllocator.h"
#include "PlatformUtil/ProcessRuntime.h"
#include <algorithm>
#include <stdint.h>

#define KB (1024uLL)
#define MB (1024uLL * KB)
#define GB (1024uLL * MB)

inline dobby_alloc_near_code_callback_t custom_alloc_near_code_handler = nullptr;
PUBLIC inline void dobby_register_alloc_near_code_callback(dobby_alloc_near_code_callback_t handler) {
  features::apple::arm64e_pac_strip_and_sign(handler);
  custom_alloc_near_code_handler = handler;
}

struct NearMemoryAllocator {
  stl::vector<simple_linear_allocator_t*> code_page_allocators;
  stl::vector<simple_linear_allocator_t*> data_page_allocators;

  inline static NearMemoryAllocator *Shared();

  MemBlock allocNearCodeBlock(uint32_t in_size, addr_t pos, size_t range) {
    if (custom_alloc_near_code_handler) {
      auto addr = custom_alloc_near_code_handler(in_size, pos, range);
      if (addr)
        return {addr, in_size};
    } else {
      auto search_range = MemRange(pos - range, range * 2);
      return allocNearBlock(in_size, search_range, true, pos);
    }
    return {};
  }

  MemBlock allocNearDataBlock(uint32_t in_size, addr_t pos, size_t range) {
    auto search_range = MemRange(pos - range, range * 2);
    return allocNearBlock(in_size, search_range, false);
  }

  MemBlock allocNearBlock(uint32_t in_size, MemRange search_range, bool is_exec = true, addr_t anchor = 0) {
    auto allocator = ensureNearCapacity(in_size, search_range, is_exec, anchor);
    if (!allocator)
      return {};
    return {(addr_t)allocator->alloc(in_size), (size_t)in_size};
  }

  // Leaves in_size bytes available for the next allocation; does not consume a relay.
  bool reserveNearCode(uint32_t in_size, MemRange search_range, addr_t anchor) {
    return ensureNearCapacity(in_size, search_range, true, anchor) != nullptr;
  }

private:
  simple_linear_allocator_t *ensureNearCapacity(uint32_t in_size, MemRange search_range, bool is_exec, addr_t anchor) {
    if (in_size == 0 || in_size > OSMemory::PageSize())
      return nullptr;
    // step-1: search from allocators first
    const auto &allocators = is_exec ? code_page_allocators : data_page_allocators;
    for (auto allocator : allocators) {
      auto cursor = allocator->cursor();
      auto unused_size = allocator->capacity - allocator->size;
      auto unused_range = MemRange((addr_t)cursor, unused_size);
      auto intersect = search_range.intersect(unused_range);
      if (intersect.size < in_size)
        continue;

      auto gap_size = intersect.addr() - (addr_t)cursor;
      if (gap_size) {
        allocator->alloc(gap_size);
      }

      return allocator;
    }

    // step-2: search from unused page between regions
    auto regions = ProcessRuntime::getMemoryLayout();

    stl::vector<addr_t> candidates;
    const size_t page_size = OSMemory::PageSize();
    for (int i = 0; i < regions.size(); ++i) {
      auto *region = &regions[i];
      auto *next_region = i < regions.size() - 1 ? &regions[i + 1] : nullptr;
      if (!next_region)
        break;

      auto unused_region_start = region->end();
      auto unused_region_size = next_region->addr() - region->end();
      MemRegion unused_region(unused_region_start, unused_region_size, kNoAccess);
      auto intersect = search_range.intersect(unused_region);
      auto unused_page = ALIGN_CEIL(intersect.addr(), page_size);
      if (intersect.size < page_size || unused_page + page_size > intersect.end())
        continue;

      const auto last_page = ALIGN_FLOOR(intersect.end() - page_size, page_size);
      if (anchor)
        unused_page = std::min(std::max((addr_t)ALIGN_FLOOR(anchor, page_size), unused_page), last_page);
      candidates.push_back(unused_page);
    }
    std::sort(candidates.begin(), candidates.end(), [anchor](addr_t left, addr_t right) {
      return (left > anchor ? left - anchor : anchor - left) <
             (right > anchor ? right - anchor : anchor - right);
    });
    for (auto unused_page : candidates) {
      auto page = OSMemory::Allocate(page_size, kNoAccess, (void *)unused_page);
      if (page != (void *)unused_page)
        continue;
      if (!OSMemory::SetPermission(page, page_size, is_exec ? kReadExecute : kReadWrite)) {
        OSMemory::Free(page, page_size);
        continue;
      }

      DEBUG_LOG("step-2 unused page: %p", page);
      auto page_allocator = new simple_linear_allocator_t((uint8_t *)page, page_size);
      if (is_exec)
        code_page_allocators.push_back(page_allocator);
      else
        data_page_allocators.push_back(page_allocator);
      return page_allocator;
    }

    // Zero bytes in an existing mapping do not establish ownership or free space.
    return {};
  }
};

inline NearMemoryAllocator gNearMemoryAllocator;
NearMemoryAllocator *NearMemoryAllocator::Shared() {
  return &gNearMemoryAllocator;
}
