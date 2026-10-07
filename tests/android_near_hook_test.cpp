#include "dobby.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "MemoryAllocator/NearMemoryAllocator.h"

static int test_live_zero_page(size_t page_size) {
  void *page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return 30;

  NearMemoryAllocator allocator;
  // The entire search range belongs to this live allocation; there is no free gap.
  auto block = allocator.allocNearBlock(16, MemRange(reinterpret_cast<addr_t>(page), page_size));
  munmap(page, page_size);
  if (block.addr() != 0) {
    std::fputs("Near allocator reused a live zero-filled mapping\n", stderr);
    return 31;
  }
  return 0;
}

static int test_occupied_allocation_address(size_t page_size) {
  auto *page = static_cast<uint8_t *>(mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (page == MAP_FAILED) return 32;
  page[0] = 0x5a;
  // Models a mapping acquired after the allocator's memory-layout snapshot.
  void *allocation = OSMemory::Allocate(page_size, kReadWrite, page);
  const bool overwritten = page[0] != 0x5a;
  if (allocation && allocation != page) OSMemory::Free(allocation, page_size);
  munmap(page, page_size);
  if (allocation || overwritten) {
    std::fputs("Near page allocation replaced an occupied mapping\n", stderr);
    return 33;
  }
  return 0;
}

static int test_nearest_gap(size_t page_size) {
  auto *pages = static_cast<uint8_t *>(mmap(nullptr, page_size * 8, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (pages == MAP_FAILED) return 38;
  munmap(pages + page_size, page_size);
  munmap(pages + page_size * 5, page_size);
  NearMemoryAllocator allocator;
  const addr_t anchor = reinterpret_cast<addr_t>(pages + page_size * 4);
  auto block = allocator.allocNearBlock(16, MemRange(reinterpret_cast<addr_t>(pages), page_size * 8), true, anchor);
  const bool nearest = block.addr() == reinterpret_cast<addr_t>(pages + page_size * 5);
  for (auto page_allocator : allocator.code_page_allocators) delete page_allocator;
  munmap(pages, page_size * 8);
  if (!nearest) {
    std::fputs("Near allocator selected a distant gap before the nearest gap\n", stderr);
    return 39;
  }
  return 0;
}

static int test_foreign_reservation(size_t page_size) {
  auto *pages = static_cast<uint8_t *>(mmap(nullptr, page_size * 3, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (pages == MAP_FAILED) return 42;
  pages[page_size] = 0x5a;
  if (mprotect(pages, page_size, PROT_READ) != 0 ||
      mprotect(pages + page_size, page_size, PROT_NONE) != 0) return 43;
  NearMemoryAllocator allocator;
  // A readable, non-executable anchor does not grant ownership of its neighbor.
  auto block = allocator.allocNearBlock(16, MemRange(reinterpret_cast<addr_t>(pages + page_size), page_size),
                                       true, reinterpret_cast<addr_t>(pages));
  if (mprotect(pages + page_size, page_size, PROT_READ | PROT_WRITE) != 0) return 44;
  const bool preserved = !block.addr() && pages[page_size] == 0x5a;
  munmap(pages, page_size * 3);
  if (!preserved) std::fputs("Near allocator took over a foreign reservation\n", stderr);
  return preserved ? 0 : 45;
}

static int test_reservation_capacity(size_t page_size) {
  constexpr size_t range = 128uLL * 1024u * 1024u;
  auto *region = static_cast<uint8_t *>(mmap(nullptr, range * 2, PROT_NONE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (region == MAP_FAILED) return 46;
  if (mprotect(region, page_size, PROT_READ | PROT_EXEC) != 0) return 47;
  auto *allocator = new simple_linear_allocator_t(region, page_size);
  allocator->alloc(page_size - 16);
  gNearMemoryAllocator.code_page_allocators.push_back(allocator);
  const addr_t anchor = reinterpret_cast<addr_t>(region + range);
  const int first = dobby_reserve_near_trampoline(reinterpret_cast<void *>(anchor));
  const int second = dobby_reserve_near_trampoline(reinterpret_cast<void *>(anchor));
  const bool available = allocator->capacity - allocator->size == 16;
  auto relay = gNearMemoryAllocator.allocNearCodeBlock(16, anchor, range);
  const bool consumed = relay.addr() == reinterpret_cast<addr_t>(region + page_size - 16) &&
                        dobby_reserve_near_trampoline(reinterpret_cast<void *>(anchor)) == -1;
  gNearMemoryAllocator.code_page_allocators.pop_back();
  delete allocator;
  munmap(region, range * 2);
  if (first != 0 || second != 0 || !available || !consumed) {
    std::fputs("Near reservation consumed capacity or reported exhausted storage as available\n", stderr);
    return 48;
  }
  return 0;
}

static int page_protection(void *address) {
  FILE *maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return -1;
  char line[1024];
  while (std::fgets(line, sizeof(line), maps)) {
    uintptr_t start, end;
    char permissions[5];
    if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, permissions) == 3 &&
        reinterpret_cast<uintptr_t>(address) >= start && reinterpret_cast<uintptr_t>(address) < end) {
      std::fclose(maps);
      return (permissions[0] == 'r' ? PROT_READ : 0) | (permissions[1] == 'w' ? PROT_WRITE : 0) |
             (permissions[2] == 'x' ? PROT_EXEC : 0);
    }
  }
  std::fclose(maps);
  return -1;
}

static int test_patch_permissions(size_t page_size) {
  auto *pages = static_cast<uint8_t *>(mmap(nullptr, page_size * 3, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (pages == MAP_FAILED) return 20;
  const uint8_t patch[] = {1, 2, 3, 4};
  if (mprotect(pages + page_size, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ||
      mprotect(pages + page_size * 2, page_size, PROT_READ) != 0) return 21;
  // Native bridges can omit guest execute permission from the host maps view.
  const int data_permissions = PROT_READ | PROT_WRITE;
  if (DobbyCodePatch(pages, const_cast<uint8_t *>(patch), sizeof(patch)) != 0 ||
      (page_protection(pages) & data_permissions) != data_permissions) {
    std::fputs("CodePatch removed write permission from a shared data page\n", stderr);
    return 22;
  }
  if (DobbyCodePatch(pages + page_size - 2, const_cast<uint8_t *>(patch), sizeof(patch)) != 0 ||
      (page_protection(pages) & data_permissions) != data_permissions ||
      (page_protection(pages + page_size) & data_permissions) != data_permissions) {
    std::fprintf(stderr, "Cross-page data/code protection mismatch: %d/%d\n",
                 page_protection(pages), page_protection(pages + page_size));
    return 23;
  }
  if (DobbyCodePatch(pages + page_size * 2 - 2, const_cast<uint8_t *>(patch), sizeof(patch)) != 0 ||
      (page_protection(pages + page_size) & data_permissions) != data_permissions ||
      (page_protection(pages + page_size * 2) & data_permissions) != PROT_READ) return 24;
  std::vector<uint8_t> large_patch(page_size * 2 + 4, 0x5a);
  if (DobbyCodePatch(pages, large_patch.data(), large_patch.size()) != 0 ||
      std::memcmp(pages, large_patch.data(), large_patch.size()) != 0 ||
      (page_protection(pages) & data_permissions) != data_permissions ||
      (page_protection(pages + page_size) & data_permissions) != data_permissions ||
      (page_protection(pages + page_size * 2) & data_permissions) != PROT_READ) return 28;
  if (mprotect(pages + page_size * 2, page_size, PROT_NONE) != 0) return 25;
  uint8_t original[2];
  std::memcpy(original, pages + page_size * 2 - 2, sizeof(original));
  if (DobbyCodePatch(pages + page_size * 2 - 2, const_cast<uint8_t *>(patch), sizeof(patch)) == 0 ||
      std::memcmp(original, pages + page_size * 2 - 2, sizeof(original)) != 0 ||
      page_protection(pages + page_size * 2) != PROT_NONE) return 26;
  if (DobbyCodePatch(pages + page_size * 2 - sizeof(patch), const_cast<uint8_t *>(patch), sizeof(patch)) != 0)
    return 27; // An exact page boundary must not touch the next unreadable page.
  pages[64] = 42;
  munmap(pages, page_size * 3);
  return 0;
}

__attribute__((noinline)) static int hook_target() {
  return 7;
}

__attribute__((noinline)) static int replacement_target() {
  return 42;
}

int main() {
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const int zero_page_status = test_live_zero_page(page_size);
  if (zero_page_status != 0) return zero_page_status;
  const int occupied_page_status = test_occupied_allocation_address(page_size);
  if (occupied_page_status != 0) return occupied_page_status;
  const int nearest_gap_status = test_nearest_gap(page_size);
  if (nearest_gap_status != 0) return nearest_gap_status;
  const int foreign_status = test_foreign_reservation(page_size);
  if (foreign_status != 0) return foreign_status;
  const int capacity_status = test_reservation_capacity(page_size);
  if (capacity_status != 0) return capacity_status;
  const int permission_status = test_patch_permissions(page_size);
  if (permission_status != 0) return permission_status;
  void *replacement = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (replacement == MAP_FAILED) {
    std::fprintf(stderr, "replacement mmap failed: %s\n", std::strerror(errno));
    return 1;
  }

  const uint32_t replacement_code[] = {
      0x52800540u, // mov w0, #42
      0xd65f03c0u, // ret
  };
  std::memcpy(replacement, replacement_code, sizeof(replacement_code));
  __builtin___clear_cache(static_cast<char *>(replacement),
                          static_cast<char *>(replacement) + sizeof(replacement_code));
  if (mprotect(replacement, page_size, PROT_READ | PROT_EXEC) != 0) {
    std::fprintf(stderr, "replacement mprotect failed: %s\n", std::strerror(errno));
    return 2;
  }

  void *target = reinterpret_cast<void *>(&hook_target);
  const uint32_t initial_instruction = *static_cast<uint32_t *>(target);
  if (dobby_reserve_near_trampoline(nullptr) != -1 || dobby_reserve_near_trampoline(target) != 0 ||
      *static_cast<uint32_t *>(target) != initial_instruction) return 40;
  bool owns_reservation = false;
  for (auto allocator : gNearMemoryAllocator.code_page_allocators) {
    const auto cursor = reinterpret_cast<uintptr_t>(allocator->cursor());
    const auto target_address = reinterpret_cast<uintptr_t>(target);
    const auto distance = cursor > target_address ? cursor - target_address : target_address - cursor;
    owns_reservation |= distance < 128uLL * 1024u * 1024u;
  }
  if (!owns_reservation) {
    std::fputs("Near reservation is invisible across translation units\n", stderr);
    return 41;
  }
  const uintptr_t target_address = reinterpret_cast<uintptr_t>(target);
  const uintptr_t replacement_address = reinterpret_cast<uintptr_t>(replacement);
  const uintptr_t distance = target_address > replacement_address ? target_address - replacement_address
                                                                  : replacement_address - target_address;
  if (distance < (128uLL * 1024u * 1024u)) {
    std::fprintf(stderr, "replacement is not far enough to exercise near allocation\n");
    return 3;
  }

  dobby_set_near_trampoline_required(true);
  void *original = nullptr;
  const int hook_status = DobbyHook(target, replacement, &original);
  if (hook_status != 0 || original == nullptr) {
    std::fprintf(stderr, "required near hook failed with status %d\n", hook_status);
    return 4;
  }

  int (*volatile target_function)() = &hook_target;
  auto original_function = reinterpret_cast<int (*)()>(original);
  if (target_function() != 42 || original_function() != 7) {
    std::fprintf(stderr, "hook or original trampoline returned an unexpected value\n");
    return 5;
  }

  if (DobbyDestroy(target) != 0 || target_function() != 7) {
    std::fprintf(stderr, "hook restoration failed\n");
    return 6;
  }

  munmap(replacement, page_size);

  std::puts("Dobby Android near-hook test passed");
  return 0;
}
