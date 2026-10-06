#include "dobby.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

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

  const size_t reservation_size = page_size * 3;
  void *reservation = mmap(nullptr, reservation_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) {
    std::fprintf(stderr, "translated reservation mmap failed: %s\n", std::strerror(errno));
    return 7;
  }

  void *translated_target = static_cast<char *>(reservation) + page_size;
  if (mprotect(translated_target, page_size, PROT_READ | PROT_WRITE) != 0) {
    std::fprintf(stderr, "translated target mprotect failed: %s\n", std::strerror(errno));
    return 8;
  }
  const uint32_t target_code[] = {
      0x528000e0u, // mov w0, #7
      0xd65f03c0u, // ret
  };
  std::memcpy(translated_target, target_code, sizeof(target_code));
  __builtin___clear_cache(static_cast<char *>(translated_target),
                          static_cast<char *>(translated_target) + sizeof(target_code));
  if (mprotect(translated_target, page_size, PROT_READ) != 0) {
    std::fprintf(stderr, "translated target read-only mprotect failed: %s\n", std::strerror(errno));
    return 9;
  }

  const uintptr_t translated_address = reinterpret_cast<uintptr_t>(translated_target);
  const uintptr_t compiled_replacement = reinterpret_cast<uintptr_t>(&replacement_target);
  const uintptr_t translated_distance = translated_address > compiled_replacement
                                            ? translated_address - compiled_replacement
                                            : compiled_replacement - translated_address;
  if (translated_distance < (128uLL * 1024u * 1024u)) {
    std::fprintf(stderr, "translated target is not far enough to require a near trampoline\n");
    return 10;
  }

  void *translated_original = nullptr;
  const int translated_status =
      DobbyHook(translated_target, reinterpret_cast<void *>(&replacement_target), &translated_original);
  if (translated_status != 0 || translated_original == nullptr) {
    std::fprintf(stderr, "translated reservation hook failed with status %d\n", translated_status);
    return 11;
  }
  if (mprotect(translated_target, page_size, PROT_READ | PROT_EXEC) != 0) {
    std::fprintf(stderr, "translated target executable mprotect failed: %s\n", std::strerror(errno));
    return 12;
  }

  auto translated_function = reinterpret_cast<int (*)()>(translated_target);
  auto translated_original_function = reinterpret_cast<int (*)()>(translated_original);
  if (translated_function() != 42 || translated_original_function() != 7) {
    std::fprintf(stderr, "translated hook or original trampoline returned an unexpected value\n");
    return 13;
  }
  if (DobbyDestroy(translated_target) != 0 || translated_function() != 7) {
    std::fprintf(stderr, "translated hook restoration failed\n");
    return 14;
  }

  std::puts("Dobby Android near-hook test passed");
  return 0;
}
