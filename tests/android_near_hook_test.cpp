#include "dobby.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

__attribute__((noinline)) static int hook_target() {
  return 7;
}

int main() {
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void *replacement = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
  const uintptr_t distance = target_address > replacement_address
                                 ? target_address - replacement_address
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
