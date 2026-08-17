#pragma once

#include "dobby/common.h"
#include "InterceptRouting/InterceptRouting.h"
#include "TrampolineBridge/ClosureTrampolineBridge/ClosureTrampoline.h"

struct InstrumentRouting : InterceptRouting {
  ClosureTrampoline *instrument_tramp = nullptr;

  InstrumentRouting(Interceptor::Entry *entry, dobby_instrument_callback_t pre_handler) : InterceptRouting(entry) {
  }

  ~InstrumentRouting() {
    if (instrument_tramp) {
      // TODO: free code block
      delete instrument_tramp;
    }
  }

  addr_t TrampolineTarget() override {
    return instrument_tramp->addr();
  }

  void GenerateInstrumentClosureTrampoline() {
    __FUNC_CALL_TRACE__();
    instrument_tramp = ::GenerateInstrumentClosureTrampoline(entry);
  }

  void BuildRouting() {
    __FUNC_CALL_TRACE__();
    GenerateInstrumentClosureTrampoline();

    if (!GenerateTrampoline())
      return;

    GenerateRelocatedCode();

    BackupOriginCode();
  }
};

PUBLIC inline int DobbyInstrument(void *address, dobby_instrument_callback_t pre_handler) {
  __FUNC_CALL_TRACE__();
  if (!address) {
    ERROR_LOG("address is 0x0.");
    return -1;
  }

  features::apple::arm64e_pac_strip(address);
  features::android::make_memory_readable(address, 4);

  DEBUG_LOG("----- [DobbyInstrument: %p] -----", address);

  auto entry = gInterceptor.find((addr_t)address);
  if (entry) {
    ERROR_LOG("%s already been instrumented.", address);
    return -1;
  }

  entry = new Interceptor::Entry((addr_t)address);
  entry->pre_handler = pre_handler;

  auto routing = new InstrumentRouting(entry, pre_handler);
  routing->BuildRouting();
  if (routing->error) {
    ERROR_LOG("build routing error.");
    delete routing;
    delete entry;
    return -1;
  }
  routing->Active();
  if (routing->error) {
    ERROR_LOG("activate routing error.");
    entry->restore_orig_code();
    delete routing;
    delete entry;
    return -1;
  }
  entry->routing = routing;

  gInterceptor.add(entry);

  return 0;
}
