#pragma once

#include "MemoryAllocator/MemoryAllocator.h"
#include "PlatformUnifiedInterface/platform.h"

struct RuntimeModule {
  void *base;
  char path[1024];
};

#define MEM_PERM_R 0x1
#define MEM_PERM_W 0x2
#define MEM_PERM_X 0x4
struct MemRegion : MemRange {
  int perm;
  bool is_private;
  bool has_path;

  MemRegion(addr_t addr, size_t size, int perm, bool is_private = false, bool has_path = true)
      : MemRange(addr, size), perm(perm), is_private(is_private), has_path(has_path) {
  }
};

class ProcessRuntime {
public:
  static const stl::vector<MemRegion> &getMemoryLayout();

  static const stl::vector<RuntimeModule> &getModuleMap();

  static RuntimeModule getModule(const char *name);
};
