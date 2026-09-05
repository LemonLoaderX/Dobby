#pragma once

#include "dobby/common.h"
#include "MemoryAllocator.h"
#include "PlatformUtil/ProcessRuntime.h"
#include <stdint.h>

#define KB (1024uLL)
#define MB (1024uLL * KB)
#define GB (1024uLL * MB)

// memmem impl
inline void *memmem_impl(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
  if (!haystack || !needle) {
    return (void *)haystack;
  } else {
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    size_t l = needlelen;
    const char *r = h;
    while (l && (l <= haystacklen)) {
      if (*n++ != *h++) {
        r = h;
        n = (const char *)needle;
        l = needlelen;
      } else {
        --l;
      }
      --haystacklen;
    }
    return l ? nullptr : (void *)r;
  }
}

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
    // step-1: search from allocators first
    auto allocators = is_exec ? code_page_allocators : data_page_allocators;
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

      auto result = allocator->alloc(in_size);
      DEBUG_LOG("step-1 allocator: %p, size: %d", (void *)result, in_size);
      return {(addr_t)result, (size_t)in_size};
    }

    // step-2: search from unused page between regions
    auto regions = ProcessRuntime::getMemoryLayout();

#if defined(__ANDROID__)
    // Native bridges can expose translated ARM code as readable, non-executable
    // guest mappings while reserving the surrounding guest address space with
    // anonymous PROT_NONE mappings. Reuse one page only when the hook target is
    // itself such translated code; native ARM processes keep the conservative
    // unmapped-gap path below.
    bool translated_code = false;
    if (is_exec && anchor) {
      for (const auto &region : regions) {
        if (anchor >= region.start() && anchor < region.end()) {
          translated_code = (region.perm & MEM_PERM_R) && !(region.perm & MEM_PERM_X);
          break;
        }
      }
    }
    if (translated_code) {
      const size_t page_size = OSMemory::PageSize();
      for (const auto &region : regions) {
        if (region.perm != kNoAccess || !region.is_private || region.has_path)
          continue;

        auto intersect = search_range.intersect(region);
        auto page_addr = ALIGN_CEIL(intersect.addr(), page_size);
        if (intersect.size < page_size || page_addr + page_size > intersect.end())
          continue;

        auto page = OSMemory::Allocate(page_size, kNoAccess, (void *)page_addr);
        if (page != (void *)page_addr)
          continue;
        if (!OSMemory::SetPermission(page, page_size, kReadExecute)) {
          OSMemory::Free(page, page_size);
          continue;
        }

        DEBUG_LOG("step-2 translated reservation: %p", page);
        auto page_allocator = new simple_linear_allocator_t((uint8_t *)page, page_size);
        code_page_allocators.push_back(page_allocator);
        auto result = page_allocator->alloc(in_size);
        return {(addr_t)result, (size_t)in_size};
      }
    }
#endif

    for (int i = 0; i < regions.size(); ++i) {
      auto *region = &regions[i];
      auto *next_region = i < regions.size() - 1 ? &regions[i + 1] : nullptr;
      if (!next_region)
        break;

      auto unused_region_start = region->end();
      auto unused_region_size = next_region->addr() - region->end();
      MemRegion unused_region(unused_region_start, unused_region_size, kNoAccess);
      auto intersect = search_range.intersect(unused_region);
      const size_t page_size = OSMemory::PageSize();
      auto unused_page = ALIGN_CEIL(intersect.addr(), page_size);
      if (intersect.size < page_size || unused_page + page_size > intersect.end())
        continue;

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
      auto result = page_allocator->alloc(in_size);
      return {(addr_t)result, (size_t)in_size};
    }

    // step-3 for exec only
    if (!is_exec) {
      return {};
    }

    // step-3: search unused code gap in regions
    const uint8_t invalid_code_seq[0x1000] = {0};
    for (int i = 0; i < regions.size(); ++i) {
      auto *region = &regions[i];
      if (!(region->perm & MEM_PERM_X))
        continue;

      auto intersect = search_range.intersect(*region);
      if (intersect.size < in_size)
        continue;

      auto search_start = intersect.addr();
      auto search_size = intersect.size;

      auto alignmemt = 4;
      auto unused_code_gap =
          memmem_impl((void *)search_start, search_size, invalid_code_seq, in_size + (alignmemt - 1));
      if (!unused_code_gap)
        continue;
      unused_code_gap = (void *)ALIGN_CEIL(unused_code_gap, alignmemt);
      DEBUG_LOG("step-3 unused code gap: %p, size: %d", unused_code_gap, in_size);
      return {(addr_t)unused_code_gap, (size_t)in_size};
    }

    return {};
  }
};

inline static NearMemoryAllocator gNearMemoryAllocator;
NearMemoryAllocator *NearMemoryAllocator::Shared() {
  return &gNearMemoryAllocator;
}
