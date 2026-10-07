# Agent Note: Prepare near capacity without claiming foreign storage

Status: implemented

## Problem

Zero bytes in mapped code or a shared heap page do not identify unused storage.
Anonymous private PROT_NONE mappings also have owners: replacing them can destroy
another allocator's reservation or guard. A readable, non-executable anchor does
not establish a native-bridge ownership transfer. A layout snapshot can become
stale before allocation.

The initial reservation API allocates and discards 16 bytes. If an owned page has
exactly one relay left, it reports success after consuming that last relay.

## Decision

Near allocation uses its own registered pages, non-replacing allocation into gaps,
or the caller's explicit custom allocator. Neither mapped zeros nor PROT_NONE
appearance establishes ownership. POSIX exact allocation uses a mmap hint, unmaps
a different result and reports failure. There is no replacement overload.

The capacity helper finds or acquires an allocator with enough remaining space.
Ordinary allocation consumes the requested bytes; dobby_reserve_near_trampoline
leaves at least 16 bytes available at return. Repetition does not consume relays.
Gap pages are ordered by distance from the anchor; without an anchor they retain
address order. Existing owned pages remain the first source.

Reservation and hook generation share the externally linked inline allocator.
Internal static instances in separate translation units cannot share free space.
The API does not patch its target, return an exclusive token, promise coverage of
other targets or serialize callers. Allocators remain process-scoped for hooks.

## Alternatives considered

- Restricting zero scans to read-only or file-backed code preserves more hooks,
  but still cannot prove free space. Recording scanned ranges prevents duplicate
  allocations within Dobby without protecting the original owner.
- Keeping translated PROT_NONE takeover helps crowded bridge layouts, but there
  is no owner protocol. A real ownership-transfer API would be needed to revisit it.
- A token tied to each target offers exclusive capacity, but adds lifetime and
  hook-consumption state that early shared capacity preparation does not require.
- MAP_FIXED_NOREPLACE provides atomic refusal on newer kernels. Hint plus exact
  result verification also avoids replacement on older supported Android kernels.
- Nearest placement alone cannot help once the entire branch window is occupied;
  early preparation improves availability without making a guarantee.

## Consequences

Hooks that depended on unowned space now fail; required-near failure remains
preferable to corrupting neighboring code or data. Custom callbacks retain their
own storage/lifetime contract. Candidate sorting allocates only after owned capacity
is exhausted. Concurrent permission changes and execution still require coordination.

The Android near-hook fixture checks foreign reservation sentinels, occupied mmap
addresses, mapped zeros, nearest selection, shared allocator visibility, repeated
reservation with exactly 16 bytes left, exhaustion, target preservation and native
hook/original/undo. These are allocator regressions, not attribution of a game crash.

## Prior-note audit

This absorbs the initial 2026-10-06 near-allocation ownership note, including its
zero-scan rationale and shared-allocator contract. Its retained translated takeover
and consuming reservation are superseded by the explicit ownership/capacity policy
above. The [permissions note](2026-10-06-posix-code-page-permissions.md) remains
separate: it owns shared-page write access rather than storage ownership.
