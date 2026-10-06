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

Validate both successful hooks and forced near-allocation failure before
updating consumers. A failed hook must not modify the target function.

Android native bridges may expose translated ARM code as readable guest mappings
and reserve the surrounding guest address space with anonymous `PROT_NONE`
mappings. Near allocation recognizes that layout only when the hook target is
readable but non-executable, then replaces one private anonymous reservation page
with executable trampoline storage. Native ARM targets retain the conservative
unmapped-gap allocator.

Run the native and synthetic translated-reservation hook regressions on an
attached ARM64 device or native-bridge emulator:

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
