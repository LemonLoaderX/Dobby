# Agent Note: Preserve writable data on patched POSIX pages

Status: implemented

## Problem

DobbyCodePatch temporarily writes a page and then forces it to RX. Native code
can reside in malloc allocations alongside mutable runtime data. Marking the whole
page read-only then crashes unrelated writes, including CoreCLR interface-dispatch
cache updates. A managed frame at the failure does not identify a bad native class
pointer when the actual instruction is an atomic write to such a shared page.

## Decision

Linux and Android code patching collect readable mappings for every touched page
before changing memory. Temporary publication uses RWX. On success each page keeps
its original read/write access and gains execute access. Pages already made writable
are restored to their original permissions if preparation fails; no bytes are
written until every touched page is ready. Range checks use the last patched byte,
so an exact page boundary does not affect the next page.

Execute access must remain explicit: Android native bridges can expose guest
executable code as a non-executable host mapping in /proc/self/maps. The native and
translated-reservation hook tests exercise that boundary. The patcher does not
create a new allocator or change caller-owned pointer lifetimes.

## Alternatives considered

- Restoring RX keeps code non-writable, but revokes access required by unrelated
  data sharing that page. Code-only allocations must be isolated by their owner.
- Restoring every observed permission exactly preserves native mappings, but a
  bridge's maps view hides guest execute permission and the trampoline stops running.
- Restoring every page to RWX avoids data faults, but leaves originally read-only
  code writable. Only initially writable pages retain write access.

## Consequences

Each patch reads /proc/self/maps and temporarily stores one entry per touched page.
Unreadable or unmapped ranges fail before modification. The executable fixture
checks shared data writes, differing permissions across multiple pages, exact-end
boundaries, rejection of an unreadable neighbor, hook invocation and undo.

This does not serialize concurrent page-permission changes or guarantee atomic
instruction publication during concurrent execution. Existing consumers still
need to coordinate hooks. A failed restoration after bytes are published returns
an error; it cannot promise that the original code remains installed.

## Prior-note Audit

There are no existing agent notes in this fork. PATCHES.md owns the separate
near-allocation and translated-reservation rules; those rules remain unchanged.
