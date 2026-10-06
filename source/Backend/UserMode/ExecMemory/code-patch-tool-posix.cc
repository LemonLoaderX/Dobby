
#include "dobby/dobby_internal.h"

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <vector>

namespace {
struct PatchPage {
  uintptr_t address;
  int protection;
};

bool get_patch_pages(uintptr_t first, uintptr_t last, size_t page_size, std::vector<PatchPage> &pages) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return false;
  char line[2048];
  uintptr_t next = first;
  while (fgets(line, sizeof(line), maps)) {
    uintptr_t start, end;
    char permissions[5];
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, permissions) != 3 ||
        next < start || next >= end)
      continue;
    const int protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                           (permissions[1] == 'w' ? PROT_WRITE : 0) |
                           (permissions[2] == 'x' ? PROT_EXEC : 0);
    if (!(protection & PROT_READ))
      break;
    while (next < end) {
      pages.push_back({next, protection});
      if (next == last) {
        fclose(maps);
        return true;
      }
      next += page_size;
    }
  }
  fclose(maps);
  return false;
}
}

#if !defined(__APPLE__)
PUBLIC int DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
#if defined(__ANDROID__) || defined(__linux__)
  if (!address || !buffer || buffer_size == 0)
    return -1;
  const long page_size = sysconf(_SC_PAGESIZE);
  const uintptr_t start = reinterpret_cast<uintptr_t>(address);
  if (page_size <= 0 || start > UINTPTR_MAX - (buffer_size - 1))
    return -1;
  std::vector<PatchPage> pages;
  if (!get_patch_pages(ALIGN_FLOOR(start, page_size), ALIGN_FLOOR(start + buffer_size - 1, page_size),
                       page_size, pages))
    return -1;

  size_t writable = 0;
  for (; writable < pages.size(); ++writable) {
    const auto &page = pages[writable];
    if (mprotect(reinterpret_cast<void *>(page.address), page_size,
                 page.protection | PROT_WRITE | PROT_EXEC) != 0)
      break;
  }
  const bool patched = writable == pages.size();
  if (patched) {
    memcpy(address, buffer, buffer_size);
    ClearCache(address, reinterpret_cast<void *>(start + buffer_size));
  }
  bool restored = true;
  while (writable > 0) {
    const auto &page = pages[--writable];
    // Native bridges omit guest execute permission from /proc/self/maps. Code
    // publication still needs execute access, but must retain shared data writes.
    const int protection = patched ? page.protection | PROT_EXEC : page.protection;
    if (mprotect(reinterpret_cast<void *>(page.address), page_size, protection) != 0)
      restored = false;
  }
  return patched && restored ? 0 : -1;
#endif
  return 0;
}

#endif
