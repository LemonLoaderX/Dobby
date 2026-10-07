# Android ARM64 patch set

This branch extends upstream commit
`c82d31d5e43f4069edb7a74f4e0665f161673129` with Android build support and ARM64
hook-safety fixes.

## Reproducible embedding

The CMake project exposes stable shared and static targets, derives its embedded
version from the checked-out source, and provides standalone Android build
scripts. Build outputs and manifests remain generated artifacts.

## Near-trampoline safety

Short ARM64 functions cannot safely accept a larger inline patch when a near
relay cannot be allocated. Embedders can require near trampolines explicitly;
failure then leaves the original instructions unchanged and returns an error.
Closure trampoline register handling also follows the Android ARM64 ABI.

Near allocation uses owned allocator pages or explicitly acquired mappings. It
never treats zero bytes in an existing executable mapping as free storage: those
bytes can belong to live runtime data or code. Exhaustion returns failure rather
than overwriting another owner's memory. Custom allocation callbacks still own
their storage and lifetime contract.

Ordinary POSIX exact-address allocation never replaces an existing mapping. It
uses an address hint and rejects a different returned address, including on older
Android kernels. Anonymous private `PROT_NONE` mappings remain owned by their
original allocator; a readable, non-executable target cannot authorize replacement.
Gap pages are selected nearest the hook anchor. Embedders can call
`dobby_reserve_near_trampoline` before other runtimes fill the address space; it
leaves at least 16 bytes of owned capacity available without patching the target or
consuming a relay. Repeated requests reuse available capacity. Other hooks may
consume it, so this is preparation, not an exclusive target reservation or proof
that a later hook can succeed. Callers serialize reservation and hook installation.
Reservation and hook generation share one inline allocator across translation
units; internal static copies cannot share ownership or free-space accounting.

Validate both successful hooks and forced near-allocation failure before
updating consumers. A failed hook must not modify the target function.

Run the regression on an attached ARM64 device. It covers hook/original/undo,
nearest gaps, cross-translation-unit sharing, repeated reservation with only one
relay left, exhaustion, live zero-filled mappings and foreign `PROT_NONE` sentinels:

```powershell
./scripts/test-android-near-hook.ps1 -DeviceSerial <serial>
```

## POSIX code-page permissions

Code patching preserves each touched page's existing read/write access while
publishing executable code. It does not force shared malloc pages to read-only,
which can crash unrelated runtime data writes. Readable mappings are collected
before mutation; preparation failures leave instructions unchanged. Execute
access remains explicit for native bridges whose maps view hides guest execution.
The same Android regression covers multiple pages, shared writes, exact boundaries
and unreadable-neighbor rejection. Concurrent patching still requires coordination.
