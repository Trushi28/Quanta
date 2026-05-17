# Quanta OS — Realm-Oriented Operating System Architecture
### Revision 2.0 — Phase 4 Edition

---

## Abstract

Quanta OS is a realm-oriented operating system that redefines the primary unit of
computation from isolated processes to isolated execution environments called Realms.

Traditional operating systems structure execution around individual processes managed
directly by the kernel under globally fixed semantics. Quanta treats execution as a
routing and orchestration problem. The kernel acts as a minimal hardware substrate and
realm lifecycle manager, while all runtime semantics are delegated to userspace execution
domains running in Ring 3.

A single global Library Operating System (LibOS) service provides compatibility layers
on demand. Realms request what they need from the LibOS. The LibOS fetches, caches, and
maps appropriate runtime implementations into requesting Realms as shared read-only pages.
No compatibility logic ever enters the kernel.

The architecture combines:

- Exokernel-inspired resource management
- Realm-oriented execution isolation
- Global on-demand LibOS runtime delivery
- Two-level scheduling (kernel schedules Realms; Realms schedule their own tasks)
- Shared-memory asynchronous IPC
- Kernel-native realm lifecycle management
- Declarative system composition

Quanta is not intended to be:

- A Linux clone or POSIX-first system
- A traditional microkernel
- A container-oriented operating system
- A virtual machine hypervisor
- A process-centric UNIX clone

It is instead a research platform for execution-environment-centric system design.

---

# Part I — Architectural Philosophy

## 1. Process-Centric vs Realm-Centric Execution

Traditional operating systems model computation as:

```
program → process → kernel scheduler
```

The kernel owns every execution unit. Every binary runs under the same global semantics.
Compatibility is a kernel concern. Runtime policy is globally fixed.

Quanta models computation as:

```
program → binary inspection → realm resolution → execution delegation → realm runtime
```

The kernel resolves what environment a binary belongs to and delegates execution into
that environment. The environment defines its own semantics, its own scheduler, its own
memory policy, and its own compatibility surface.

The primary abstraction boundary shifts from:

```
kernel ↔ process
```

to:

```
kernel ↔ realm
```

---

## 2. Core Principles

### Kernel as Pure Substrate

The kernel's job is narrow and fixed:

- Arbitrate hardware access
- Own physical memory and page tables
- Enforce isolation between Realms
- Manage Realm lifecycle (create, schedule, destroy)
- Validate and route IPC
- Service syscalls from Ring 3

The kernel does not define runtime semantics, implement compatibility layers, provide
allocators to applications, or enforce scheduling policy beyond CPU quota distribution.
All of that belongs to Realms and the global LibOS.

---

### Realms as First-Class Kernel Objects

A Realm is a kernel-managed isolated execution territory. It is a kernel data structure
(`realm_t`) in the same way a task is a kernel data structure (`task_t`). The kernel
creates it, owns it, schedules it, and destroys it. The Realm does not manage itself.

A Realm contains:

- An isolated virtual address space (its own PML4)
- A set of tasks executing inside it
- A type tag (Native, WASM, Linux ELF, Windows PE)
- A capability table (what it is allowed to access)
- A local scheduler (defined by the Realm, not the kernel)
- A memory territory (pages granted by the kernel)

Realms run entirely in Ring 3. Ring 3 is the execution privilege level for Realm code.
There is no Ring 3 management infrastructure — the kernel manages Realms from Ring 0
directly, the same way it manages tasks today.

---

### Execution as Routing

Execution is not a direct kernel operation. It is a routing decision.

When a binary arrives for execution, the kernel inspects its format and routes it to
the appropriate Realm type. The binary then executes inside that Realm under that
Realm's runtime semantics. The kernel has no knowledge of Win32, POSIX, or WASM
internals.

---

### Runtime Specialization Without Kernel Changes

Different Realm types may optimize independently:

- A Gaming Realm may use frame-paced scheduling and CPU pinning
- A Realtime Realm may use deadline scheduling
- A WASM Realm may use cooperative scheduling with a JIT runtime
- A Linux ELF Realm may use POSIX-compatible semantics via musl

None of these require kernel modifications. Each Realm implements its own policy in
Ring 3 and expresses only resource requests to the kernel via syscall.

---

# Part II — System Topology

## 1. Kernel Substrate (Ring 0)

The kernel is the trusted authority. It is not a Realm. It supervises all Realms
from outside.

```
┌─────────────────────────────────────────────────────┐
│                  Kernel (Ring 0)                    │
│                                                     │
│  Hardware Substrate    Realm Manager                │
│  ─────────────────    ──────────────                │
│  x2APIC / IOAPIC      realm_create()                │
│  SMP / per-CPU        realm_destroy()               │
│  PMM (bitmap)         realm_schedule()              │
│  VMM (PML4, NX)       realm_grant_pages()           │
│  Interrupt routing    realm_exec()                  │
│  Slab heap            realm_ipc_setup()             │
│  VFS / KV store       Page fault containment        │
│  VirtIO / NVMe        Capability validation         │
└─────────────────────────────────────────────────────┘
```

### Kernel Responsibilities

**Hardware Arbitration**
- x2APIC and IOAPIC management
- SMP bringup and per-CPU state
- Interrupt routing and IRQ dispatch
- Timer management (PIT-calibrated APIC timer)
- Device coordination (VirtIO, NVMe, PS/2)

**Physical Resource Ownership**
- PMM owns all physical pages
- VMM owns all page tables
- CPU arbitration via global scheduler
- Memory territory grants to Realms

**Realm Lifecycle Management**
- `realm_create(type, name)` — allocate realm_t, new address space
- `realm_destroy(id)` — reclaim all resources
- `realm_exec(realm, binary, size)` — load binary and enter Ring 3
- `realm_grant_pages(realm, phys, n, flags)` — extend Realm territory
- `realm_schedule()` — CPU quota distribution across active Realms

**Stability Enforcement**
- Privilege level enforcement (Ring 0 / Ring 3)
- MMU isolation between Realm address spaces
- Page fault containment (user faults kill the faulting task; kernel faults panic)
- Capability table validation on every syscall
- IPC permission checks before establishing shared regions

---

## 2. Global LibOS (Ring 3, Kernel-Managed)

The LibOS is a single global service. It is itself a kernel-managed Realm with
elevated mapping privileges. There is one LibOS instance for the entire system.

Its responsibilities:

- Maintain a registry of all compatibility implementations
- Detect what library surface a binary requires
- Fetch implementation modules from persistent storage on demand
- Map those modules as shared read-only pages into requesting Realm address spaces
- Cache frequently used modules to avoid repeated disk reads
- Translate API calls through to Quanta kernel syscalls

**The LibOS is not inside each Realm. Realms request from it. The LibOS maps
implementations into requesting Realm address spaces as shared read-only pages —
the same physical pages are mapped into every Realm that requests the same module.
No duplication. No per-Realm overhead.**

### LibOS Module Registry

```
/libos/
├── native/
│   └── libquanta.so        Quanta native Realm API
│
├── wasm/
│   └── wasi_runtime.so     WASM System Interface runtime (wasm3-based)
│
├── linux/
│   ├── libc.so             musl libc — POSIX compatible
│   ├── libpthread.so       POSIX threads
│   └── libm.so             Math library
│
└── win32/
    ├── ntdll.so            NT layer — heap, loader, basic NT primitives
    ├── kernel32.so         Core Win32 — files, processes, memory, sync
    ├── user32.so           Windowing and input API
    └── gdi32.so            Graphics device interface
```

### LibOS Fetch Flow

```
Realm starts, binary has Import Directory needing kernel32.dll
          ↓
Realm issues SYS_LIBOS_FETCH("win32", "kernel32")
          ↓
LibOS checks module cache
  hit  → return existing mapped address
  miss → read /libos/win32/kernel32.so from VFS
         allocate physical pages, load module
         add to cache
          ↓
LibOS maps pages read-only into requesting Realm address space
LibOS returns base address of mapped module
          ↓
Realm patches binary Import Address Table with LibOS function pointers
          ↓
Binary executes — Win32 calls land in LibOS implementations
LibOS translates to Quanta syscalls where kernel resources are needed
```

### What The LibOS Absorbs

Most API calls never reach the kernel. The LibOS handles them entirely in Ring 3:

| Call Type | Handled By | Kernel Involved |
|---|---|---|
| `HeapAlloc` | LibOS local allocator | No |
| `CreateThread` | LibOS → Realm task | No |
| `QueryPerformanceCounter` | LibOS reads TSC | No |
| `CreateFileW` | LibOS → `SYS_WRITE`/`SYS_READ` | Yes (VFS) |
| `VirtualAlloc` | LibOS → `SYS_PAGE_REQUEST` | Yes (PMM/VMM) |
| `CreateWindow` | LibOS → IPC to Display Realm | Yes (IPC) |
| `malloc` (musl) | LibOS allocator | No |
| `open` (musl) | LibOS → `SYS_READ`/`SYS_WRITE` | Yes (VFS) |
| `wasm fd_write` | LibOS → `SYS_WRITE` | Yes (minimal) |

The kernel path is the exception, not the norm.

---

## 3. Application Realms (Ring 3)

Application Realms run all user binaries. They are created and managed by the kernel.
A Realm does not manage other Realms — it simply executes its workload.

Each Realm:

- Has its own isolated virtual address space
- Runs its tasks in Ring 3
- Requests library implementations from the global LibOS
- Communicates with other Realms only through kernel-validated IPC
- Has a local scheduler it defines and controls
- Manages its own heap using allocators it implements internally

---

## 4. Realm Types

### Native Realm

Executes Quanta-native binaries. Uses the native Quanta LibOS API (`libquanta.so`).
The reference execution environment for new software written for Quanta.

---

### WASM Runtime Realm

Executes WebAssembly binaries via the WASI runtime embedded in the LibOS.

- Binary format: WASM magic `\0asm`
- LibOS module: `wasi_runtime.so` (wasm3-based, MIT licensed)
- Interface: WASI snapshot preview 1
- Scheduling: cooperative by default, preemptive optional

This is the first and simplest compatibility target due to the open spec, minimal
binary format, and existing lightweight runtime implementations.

---

### Linux ELF Realm

Executes Linux ELF64 binaries via a POSIX-compatible LibOS layer.

- Binary format: ELF magic `\x7FELF`, class 64, OS/ABI 0 (System V)
- LibOS modules: `libc.so` (musl), `libpthread.so`, `libm.so`
- Interface: Subset of Linux syscall table (translated through LibOS)
- Scheduling: POSIX-compatible, CFS-like policy optional

musl libc (MIT licensed) provides the POSIX surface. Syscall translation occurs
inside the LibOS — a Linux `open(2)` call routes to the Quanta VFS through a thin
translation layer entirely in Ring 3.

---

### Windows PE Realm

Executes Windows PE32+ binaries via a Win32-compatible LibOS layer.

- Binary format: MZ/PE magic, machine type `0x8664` (AMD64)
- LibOS modules: `ntdll.so`, `kernel32.so`, `user32.so`, `gdi32.so`
- Interface: Win32 API surface (Wine-derived implementations, LGPL)
- IAT patching: Realm PE loader replaces real DLL imports with LibOS pointers

When a PE binary is loaded, the Realm's PE loader walks the Import Directory Table,
requests each needed DLL from the LibOS, receives the mapped pages, and patches
the Import Address Table. The binary then calls Win32 APIs that land directly in the
LibOS implementation without any kernel involvement for non-resource operations.

---

### Specialized Realms (Future)

| Realm Type | Purpose | Key Property |
|---|---|---|
| Gaming Realm | Low-latency interactive applications | Frame-paced scheduler, GPU priority |
| Secure Sandbox | Untrusted code execution | Minimal capability table, no IPC |
| Realtime Realm | Deterministic timing guarantees | Deadline scheduler, pinned CPU |
| AI Inference Realm | Neural network execution | Large memory territory, batch scheduling |

---

# Part III — Kernel Syscall Interface

## Design Principle

The kernel exposes the minimum possible surface to Ring 3. All higher-level semantics
live in the LibOS or Realm internals. A Realm only crosses into Ring 0 when it needs
raw hardware resources.

## Syscall Mechanism

SYSCALL/SYSRET (x86-64 fast syscall path):

```
Ring 3 SYSCALL instruction
    → CPU saves RIP → RCX, RFLAGS → R11
    → CPU loads kernel CS/SS from STAR MSR
    → CPU jumps to LSTAR MSR (syscall_entry trampoline)
    → trampoline: swapgs, switch to kernel stack (cpu_local→kernel_stack_top)
    → save user RSP in cpu_local→user_rsp_scratch
    → push full register frame
    → call syscall_dispatch(num, a1, a2, a3, a4, a5)
    → restore registers
    → swapgs
    → SYSRETQ → Ring 3, RIP from RCX
```

Argument convention (follows Linux x86-64 ABI):

| Register | Role |
|---|---|
| RAX | Syscall number |
| RDI | Argument 1 |
| RSI | Argument 2 |
| RDX | Argument 3 |
| R10 | Argument 4 (not RCX — clobbered by SYSCALL) |
| R8 | Argument 5 |
| R9 | Argument 6 |
| RAX (return) | Return value (negative = error) |

## Syscall Table

### Core I/O

| Number | Name | Signature | Description |
|---|---|---|---|
| 0 | `SYS_READ` | `read(fd, buf, len)` | Read from file descriptor |
| 1 | `SYS_WRITE` | `write(fd, buf, len)` | Write to file descriptor |
| 2 | `SYS_OPEN` | `open(path, flags)` | Open a VFS path; descriptors start at 3 |
| 3 | `SYS_CLOSE` | `close(fd)` | Close a VFS descriptor |
| 4 | `SYS_STAT` | `stat(path, stat_buf)` | Copy VFS metadata to Ring 3 |
| 24 | `SYS_YIELD` | `yield()` | Voluntarily yield CPU |
| 35 | `SYS_SLEEP` | `sleep_ms(ms)` | Sleep N milliseconds |
| 39 | `SYS_GETPID` | `getpid()` | Return current task PID |
| 60 | `SYS_EXIT` | `exit(code)` | Terminate current task |

### Memory Management

| Number | Name | Signature | Description |
|---|---|---|---|
| 200 | `SYS_PAGE_REQUEST` | `page_request(vaddr, n, flags)` | Map N pages at vaddr |
| 201 | `SYS_PAGE_RELEASE` | `page_release(vaddr, n)` | Unmap and return N pages |

### Inter-Realm Communication

| Number | Name | Signature | Description |
|---|---|---|---|
| 202 | `SYS_IPC_SEND` | `ipc_send(realm_id, buf, len)` | Send message to Realm |
| 203 | `SYS_IPC_RECV` | `ipc_recv(realm_id, buf, len)` | Receive message (blocking) |

### Realm Operations

| Number | Name | Signature | Description |
|---|---|---|---|
| 204 | `SYS_REALM_ID` | `realm_id()` | Return current Realm ID |
| 205 | `SYS_REALM_EXIT` | `realm_exit(code)` | Terminate entire Realm |
| 206 | `SYS_LIBOS_FETCH` | `libos_fetch(type, name, len)` | Request LibOS module mapping |

### Power Operations

| Number | Name | Signature | Description |
|---|---|---|---|
| 207 | `SYS_REBOOT` | `reboot()` | Reboot system; requires `CAP_POWER` |
| 208 | `SYS_SHUTDOWN` | `shutdown()` | Power off system; requires `CAP_POWER` |
| 209 | `SYS_READDIR` | `readdir(fd, idx, name_buf)` | Read a directory entry name |

**Total: 19 syscalls.** Everything else is handled in Ring 3 by the LibOS.

---

# Part IV — Kernel Realm Manager

## realm_t as a Kernel Object

A Realm is a first-class kernel object, identical in ownership model to `task_t`:

```
Kernel manages task_t  → tasks run in Ring 0 (current)
Kernel manages realm_t → realms run in Ring 3
```

The parallel is exact. The kernel creates, schedules, and destroys Realms from Ring 0.
A Realm never manages another Realm. A Realm never sees another Realm's address space.

## Realm Lifecycle

```
realm_create(type, name)
    kernel allocates realm_t on kernel heap
    kernel calls vmm_new_space() → isolated PML4
    kernel registers realm in global realm table
    realm state → REALM_CREATED
          ↓
realm_exec(realm, binary_buf, size)
    kernel inspects binary magic → confirm type matches
    kernel calls loader for this realm type (ELF / PE / WASM)
    loader maps binary sections into realm address space
    loader issues SYS_LIBOS_FETCH for required modules
    loader patches import table with LibOS function addresses
    kernel creates initial task with Ring 3 RIP = binary entry point
    kernel creates user stack (mapped at fixed high-user address)
    realm state → REALM_RUNNING
          ↓
realm running (Ring 3)
    tasks execute, syscalls cross via SYSCALL/SYSRET
    kernel global scheduler gives realm CPU time
    realm local scheduler distributes time among its tasks
          ↓
realm_destroy(id)
    kernel walks realm task list, terminates all tasks
    kernel unmaps all realm pages, returns to PMM
    kernel frees realm_t
    realm state → REALM_DEAD
```

## Page Fault Handling

The page fault handler (vector 14) distinguishes fault origin:

```
page fault fires
    check error code bit 2 (U/S flag)
          ↓
    bit 2 = 0 → kernel-mode fault
        full register dump → kernel panic
        (this should never happen in correct kernel code)

    bit 2 = 1 → user-mode fault (Realm task)
        log fault: realm_id, faulting address, RIP
        terminate faulting task cleanly
        if realm has no remaining tasks → realm_destroy()
        schedule next runnable task
        (kernel continues running normally)
```

User faults are contained. They cannot crash the kernel or affect other Realms.

---

# Part V — Two-Level Scheduling

## Level 1: Kernel Global Scheduler (Ring 0)

The kernel scheduler treats Realms as the primary scheduling entities.

Responsibilities:

- Distribute CPU time across all active Realms
- Enforce fairness — no Realm starves others
- Handle core assignment in SMP configurations
- Manage Realm sleep and wake events
- Perform preemption at 1ms APIC timer ticks

The kernel does not know or care how a Realm uses its allocated CPU time internally.
It grants time slices to Realms. Realms spend those slices as they choose.

## Level 2: Realm-Local Schedulers (Ring 3)

Within its allocated CPU time, each Realm runs its own scheduler for its internal tasks.

This allows:

| Realm Type | Scheduling Strategy | Why |
|---|---|---|
| Native Realm | Round-robin (inherits kernel style) | General purpose |
| WASM Realm | Cooperative | WASM execution model |
| Linux ELF Realm | CFS-like fair scheduling | POSIX compatibility |
| Gaming Realm | Frame-paced, priority | Latency sensitivity |
| Realtime Realm | EDF (Earliest Deadline First) | Timing guarantees |

The kernel scheduler and Realm-local schedulers are fully independent. Changing one
does not require touching the other.

---

# Part VI — Memory Architecture

## Virtual Address Space Layout

```
0x0000_0000_0000_0000 ─── 0x0000_7FFF_FFFF_FFFF   User space (Realm territory)
                          ··· canonical hole ···
0xFFFF_8000_0000_0000 ─── 0xFFFF_FFFF_7FFF_FFFF   HHDM (all physical RAM)
0xFFFF_FFFF_8000_0000 ─── 0xFFFF_FFFF_FFFF_FFFF   Kernel image (−2 GiB)
```

Each Realm has its own PML4. Kernel entries (PML4 indices 256–511) are shared
read-only across all address spaces. Realm entries (indices 0–255) are private.

## Exokernel-Inspired Territory Grants

The kernel does not expose a `malloc`. It grants pages.

When a Realm is created:

- Kernel allocates initial territory: N pages at a fixed base address
- Kernel maps binary sections with appropriate permissions (RX for code, RW for data)
- Kernel maps user stack (RW, top of user address space)
- LibOS pages mapped read-only (shared physical pages, no copy)

When a Realm needs more memory:

```
Realm issues SYS_PAGE_REQUEST(vaddr, n_pages, flags)
    ↓
Kernel validates: vaddr in user range, no overlap with existing mappings
Kernel calls pmm_alloc_n(n_pages)
Kernel calls vmm_map_page() for each page with requested flags
Returns 0 on success, negative on failure
    ↓
Realm manages its own heap on top of these pages
```

Policy (slab, arena, buddy, etc.) is entirely the Realm's choice. The kernel only
grants raw pages.

## LibOS Shared Mappings

LibOS module pages are physical pages owned by the LibOS Realm. When a second Realm
requests the same module, the kernel maps the same physical pages into its address
space read-only. No copying occurs.

```
Win32 kernel32.so physical pages (one copy in PMM)
    ↓ mapped RO into
    Windows Realm A  (read-only, shared)
    Windows Realm B  (read-only, shared)
    Windows Realm C  (read-only, shared)
```

This is identical to how Linux maps shared libraries. Memory usage grows by one copy
regardless of how many Realms use the same module.

---

# Part VII — Inter-Realm Communication

## Isolation Guarantee

Realms cannot access each other's address spaces. There is no shared mutable state
between Realms by default. All communication is explicit and kernel-validated.

## Shared Memory IPC

To establish a communication channel:

```
Realm A issues SYS_IPC_SEND(realm_b_id, setup_descriptor, len)
    ↓
Kernel validates: Realm A has IPC capability for Realm B
Kernel allocates shared physical pages
Kernel maps pages RW into Realm A address space
Kernel maps pages RW into Realm B address space
Both Realms receive the shared region base address
```

Both Realms then communicate through this shared region directly. The kernel is not
involved in individual messages — only in the initial establishment and teardown.

## Ring Buffer Convention

Shared regions conventionally contain lock-free ring buffers:

```
┌──────────────────────────────────┐
│ header: producer_idx, consumer_idx│
├──────────────────────────────────┤
│ entry[0]  ... entry[N-1]         │
└──────────────────────────────────┘
```

Producers write to `entries[producer_idx % N]`, advance index. Consumers read,
advance their index. No kernel involvement after establishment.

## Asynchronous Doorbell

When a Realm produces new data and wants to wake a sleeping Realm:

```
Realm A writes to shared buffer
Realm A issues SYS_IPC_SEND(realm_b_id, DOORBELL, 0)
    ↓
Kernel finds Realm B tasks in SLEEPING state
Kernel wakes them → RUNNABLE
Kernel returns to Realm A immediately
    ↓
Realm B tasks wake and process buffer contents
```

The kernel acts only as the signal dispatcher. The data path is zero-copy in
shared memory.

---

# Part VIII — Global LibOS Model

## Ownership Clarification

The LibOS is a single global Ring 3 service. It is a kernel-managed Realm with
elevated page-mapping privileges granted at boot. There is exactly one LibOS instance.

Realms do not own a LibOS. Realms request from the global LibOS.

This is the correct model — not "each Realm has its own LibOS."

## Module Lifecycle

```
LibOS module lifecycle:

NOT_LOADED
    → SYS_LIBOS_FETCH request arrives
    → LibOS reads module from /libos/<layer>/<module>.so
    → LibOS allocates physical pages, loads module
    → LibOS maps module into requesting Realm (read-only)
LOADED + CACHED
    → Subsequent requests for same module return cached mapping
    → Same physical pages mapped into additional Realms at no extra cost
EVICTED (future)
    → Under memory pressure, LibOS may unmap unused modules
    → Reload on next request
```

## What The LibOS Is Not

The LibOS is not:

- A process manager
- A filesystem server
- A device driver host
- A kernel in userspace
- Per-Realm private code

It is solely a compatibility library delivery and mapping service.

## LibOS Per-Layer Responsibilities

### Win32 Layer

Provides Windows API surface to PE binaries. Translates to Quanta primitives:

- File operations → `SYS_READ` / `SYS_WRITE` via VFS
- Memory operations → `SYS_PAGE_REQUEST` / `SYS_PAGE_RELEASE`
- Thread creation → Realm-local task creation (no kernel involvement)
- Window management → IPC to Display Realm
- Heap management → LibOS-internal allocator (no kernel involvement)
- Synchronization primitives → LibOS-internal (futex-like, no kernel)

Implementation basis: Wine (LGPL) — legally safe via interoperability provisions
in both US (DMCA §1201(f)) and EU (Directive 2009/24/EC Article 6).

### Linux POSIX Layer

Provides POSIX surface to ELF64 binaries. Implementation basis: musl (MIT).

- `open/read/write/close` → Quanta VFS syscalls
- `mmap/munmap` → `SYS_PAGE_REQUEST` / `SYS_PAGE_RELEASE`
- `pthread_create` → Realm-local task
- `malloc/free` → musl allocator (internal)

### WASM Runtime Layer

Provides WASI execution environment. Implementation basis: wasm3 (MIT, ~7k lines).

- `wasi_fd_write` → `SYS_WRITE`
- `wasi_path_open` → VFS via `SYS_READ`
- `wasi_proc_exit` → `SYS_REALM_EXIT`
- Memory handled by WASM linear memory model internally

---

# Part IX — Execution Routing

## Binary Detection

When a binary is submitted for execution, the kernel or Realm loader inspects
its header to determine the correct Realm type:

| Magic Bytes | Format | Realm Type |
|---|---|---|
| `\x7FELF` + class=2 + type=2/3 + OS/ABI=3 | Linux ELF64 | Linux ELF Realm |
| `\x7FELF` + class=2 + type=2/3 + OS/ABI=0 | Native ELF64 | Native Realm |
| `MZ` + PE signature `PE\0\0` + machine=0x8664 | Windows PE32+ | Windows PE Realm |
| `\0asm` + version=1 | WebAssembly | WASM Runtime Realm |

## Routing Pipeline

```
binary submitted
        ↓
inspect first 64 bytes for magic
        ↓
look up Realm type in kernel Realm registry
        ↓
realm_exec(matching_realm, binary, size)
        ↓
format-specific loader:
  ELF loader  → parse PT_LOAD segments, map RX/RW/RO
  PE loader   → parse sections, map, patch IAT via LibOS
  WASM loader → pass to wasm3 runtime in LibOS
        ↓
LibOS fetch for required modules
        ↓
task created with Ring 3 entry point
        ↓
binary executes in Ring 3 under Realm runtime
```

The kernel has no knowledge of what the binary does after routing. Execution
semantics belong entirely to the Realm and LibOS.

---

# Part X — Capability and Security Model

## Kernel as Root Authority

The kernel holds root authority over all hardware resources. No Realm can access
hardware directly — all access is mediated by capability tokens.

## Capability Types

| Capability | Grants Access To | Default |
|---|---|---|
| `CAP_VFS` | VFS read/write operations | Yes (all Realms) |
| `CAP_IPC` | IPC to specific Realm IDs | Configurable |
| `CAP_PAGES` | Physical page allocation | Yes (quota limited) |
| `CAP_IRQ` | Specific IRQ lines | Drivers only |
| `CAP_GPU` | GPU command submission | Granted per policy |
| `CAP_NETWORK` | Network device access | Granted per policy |
| `CAP_LIBOS_MAP` | Map pages into other Realms | LibOS only |
| `CAP_REALM_CREATE` | Create new Realms | Native Realm, init only |
| `CAP_POWER` | Reboot / shutdown authority | Native init only |

## Enforcement

Every syscall that accesses a resource checks the calling Realm's capability table:

```
syscall_dispatch(SYS_IPC_SEND, target_realm_id, buf, len, ...)
    ↓
check caller's cap_table for CAP_IPC to target_realm_id
    has cap → proceed
    no cap  → return EQUANTA_PERM (-4)
```

Capabilities are granted at Realm creation time by the kernel based on Realm type
and configuration. They cannot be self-escalated.

---

# Part XI — Boot Sequence

## Stage 1 — Bootloader (Limine 8.7.0)

Limine loads the kernel ELF and provides via response structs:

- Framebuffer (pixel-mode display)
- Physical memory map (usable / reserved regions)
- HHDM offset (virtual base for all physical RAM)
- Kernel physical/virtual load addresses
- RSDP pointer (ACPI root)
- SMP descriptors (per-AP goto_address + x2APIC opt-in)
- Boot timestamp (Unix epoch)

---

## Stage 2 — Kernel Hardware Initialization

```
serial_init()           COM1 @ 38400 baud — early debug output
fb_init()               Framebuffer terminal + boot splash
acpi_init()             RSDP → XSDT → MADT, HPET, MCFG
power_init()            FADT reset register + DSDT _S5_ for shutdown
gdt_init()              BSP GDT (null, kcode, kdata, udata, ucode, TSS)
                        Note: user data before user code — required for SYSRET
idt_init()              256 gates, PIC remapped and masked, IOAPIC takes over
pmm_init()              Bitmap allocator over all usable RAM
vmm_init()              Inherit Limine PML4, enable NX + SCE in EFER
heap_init()             Slab allocator (9 caches: 8–2048 bytes)
apic_init()             x2APIC (MSR) or xAPIC (MMIO)
ioapic_init()           Locate via MADT, mask all entries, redirect IRQ1
smp_bsp_early_init()    BSP cpu_local_t via GS base MSR
smp_init()              Wake APs: gdt_init, idt_reload, apic_init, cpu_local_setup
apic_timer_init(1ms)    PIT-calibrated periodic tick
```

---

## Stage 3 — Kernel Service Initialization

```
syscall_init()          Program STAR, LSTAR, FMASK MSRs on BSP
                        APs call syscall_init() during their bringup
sched_init()            Preemptive round-robin, per-CPU queues, idle tasks
vfs_init()              RamFS at /, DevFS at /dev (/dev/null, /dev/zero)
virtio_init()           PCIe ECAM scan via ACPI MCFG → virtio-blk 1.1
kv_init()               Persistent KV store on virtio-blk sector 2048
keyboard_init()         PS/2 IRQ1 via IOAPIC
```

---

## Stage 4 — Realm System Initialization

```
realm_system_init()     Initialize global realm table, realm ID allocator
libos_init()            Create LibOS Realm, grant CAP_LIBOS_MAP
                        Mount /libos/ on VFS
                        Pre-load native libquanta.so into LibOS cache
realm_create_init()     Create initial Native Realm
                        Load embedded native-init ELF
                        Enter Ring 3 native console
                        Kernel QAI shell is debug fallback only
```

---

## Stage 5 — Ring 3 Execution

After Stage 4, the kernel's boot path hands off to the native-init Realm. Normal
user-facing activity happens in Ring 3 inside Realms. The kernel QAI shell is kept
only as a debug fallback if native-init cannot be created. The kernel otherwise runs
only when:

- A syscall is issued (SYSCALL/SYSRET crossing)
- An interrupt fires (timer tick, keyboard, device)
- A page fault occurs (user fault → handled; kernel fault → panic)

---

# Part XII — GDT Layout for SYSRET Compatibility

The SYSCALL/SYSRET mechanism requires a specific GDT segment ordering.
SYSRETQ loads selectors as:

```
CS = STAR[63:48] + 16  (with RPL forced to 3)
SS = STAR[63:48] + 8   (with RPL forced to 3)
```

For this to work correctly with Quanta's segment layout:

```
Index   Selector   Descriptor       Notes
──────────────────────────────────────────────────────
  0     0x00       Null             Required
  1     0x08       Kernel Code      Ring 0, 64-bit, SYSCALL CS
  2     0x10       Kernel Data      Ring 0, SYSCALL SS (0x08 + 8)
  3     0x18       User Data        Ring 3, SYSRET SS (STAR_base + 8)
  4     0x20       User Code        Ring 3, 64-bit, SYSRET CS (STAR_base + 16)
  5     0x28       TSS low          128-bit TSS descriptor
  6     0x30       TSS high
```

User data (0x18) must appear before user code (0x20). This is the opposite of
the Phase 1-3 layout and must be corrected in Phase 4.

STAR MSR configuration:

```
STAR[47:32] = 0x0008   SYSCALL → CS=0x08 (kernel code), SS=0x10 (kernel data)
STAR[63:48] = 0x0010   SYSRET  → SS=0x18 (user data),   CS=0x20 (user code)
LSTAR       = &syscall_entry   Kernel entry point
FMASK       = 0x200            Clear IF on syscall entry (disable interrupts)
```

---

# Part XIII — Implementation Status

## Completed (Phases 1–3)

| Subsystem | Status | Notes |
|---|---|---|
| Serial / UART | ✅ Done | COM1, 38400 baud, 8N1 |
| Framebuffer terminal | ✅ Done | 8×16 font, ANSI, scroll, splash |
| ACPI parser | ✅ Done | RSDP, XSDT, MADT, HPET, MCFG |
| Power management | ✅ Done | ACPI S5, KBC reset, CF9 fallback |
| GDT + TSS | ✅ Done | Per-CPU, static pool, 64 CPUs |
| IDT | ✅ Done | 256 gates, auto-generated stubs |
| PMM | ✅ Done | Bitmap, spinlock, contiguous alloc |
| VMM | ✅ Done | 4-level paging, NX, SCE |
| Slab heap | ✅ Done | 9 caches (8–2048 B), large PMM |
| x2APIC / xAPIC | ✅ Done | MSR mode + MMIO fallback |
| IOAPIC | ✅ Done | MADT lookup, redirect, mask/unmask |
| SMP | ✅ Done | 64 CPUs, GS base, per-CPU locals |
| Ticket spinlock | ✅ Done | IRQ-saving variants |
| Scheduler | ✅ Done | Preemptive round-robin, per-CPU |
| Context switch | ✅ Done | Assembly, callee-saved + RIP |
| VirtIO-blk 1.1 | ✅ Done | PCIe ECAM, split-ring virtqueue |
| VFS | ✅ Done | RamFS + DevFS, Phase 3 metadata |
| Persistent KV store | ✅ Done | Single sector, virtio-blk |
| PS/2 keyboard | ✅ Done | IRQ1 via IOAPIC, ring buffer |
| QAI shell | ✅ Done | 40+ commands, editor, calc, grep |
| QAI assistant | ✅ Done | Keyword KB, live system data |

## Phase 4 — Complete

| Component | Status | Depends On |
|---|---|---|
| GDT fix (udata↔ucode swap) | Done | — |
| Page fault handler (user faults) | Done | GDT fix |
| SYSCALL/SYSRET trampoline | Done | GDT fix |
| syscall_dispatch (C handler) | Done | Trampoline |
| `realm_t` kernel object | Done | Heap |
| `realm_create / destroy` | Done | VMM, PMM |
| User address space builder | Done | VMM |
| ELF64 segment loader | Done | VMM, realm_t |
| LibOS Realm foundation | Done | realm_t |
| `SYS_LIBOS_FETCH` | Done | LibOS + VFS |
| `SYS_REBOOT` / `SYS_SHUTDOWN` | Done | CAP_POWER |
| Ring 3 native init console | Done | All above |

---

# Part XIV — Roadmap

## Phase 4 — Ring 3 Foundation (Complete)

Establish the kernel↔Ring 3 boundary. Quanta now boots into a persistent
native-init console running in Ring 3, not the Ring 0 QAI shell. The console uses
the syscall ABI for output, keyboard input, identity, LibOS lookup, and realm-owned
page mapping.

Deliverables:
- SYSCALL/SYSRET trampoline + syscall_dispatch
- `realm_t` as kernel object, lifecycle management, and user page cleanup
- User address space construction and stack mapping
- ELF64 binary loader (PT_LOAD segments, RX/RW/RO)
- Page fault handler: user faults contained, kernel faults panic
- LibOS Realm created at boot with CAP_LIBOS_MAP
- `SYS_READ(0)` routes PS/2 keyboard input into Ring 3 user buffers
- VFS-backed Ring 3 `open`, `close`, `stat`, and `readdir`; fd 0/1/2 are reserved for stdio
- `SYS_LIBOS_FETCH` resolves registered LibOS runtime modules
- Capability-gated `SYS_REBOOT` and `SYS_SHUTDOWN` for native-init
- Native Ring 3 init console: `help`, `ls`, `cat`, `wasm`, `kernel`, `fs`, `qfs`, `pid`, `realm`, `libos`, `page`, `game`, `reboot`, `shutdown`, `exit`
- Kernel QAI shell remains available only as a debug fallback if native-init fails

---

## Phase 5 — WASM Runtime Realm (Started)

First working compatibility Realm. Chosen because WASM has the simplest
binary format, a fully open spec, and lightweight existing runtimes.

Deliverables:
- LibOS module registry and `SYS_LIBOS_FETCH` route (done)
- WASM binary detection and routing from Ring 3 native-init (done)
- Seed `/apps/hello.wasm` in VFS for routing tests (done)
- wasm3 embedded as WASM LibOS module
- WASI snapshot preview 1 syscall surface
- Running a "hello world" WASM binary from VFS

---

## Phase 6 — QuantaFS-Weave (Started)

Create a Quanta-native filesystem instead of cloning FAT32, ext4, or Btrfs.
The design treats files as projections over a graph of immutable capsules,
relations, tags, and append-only namespace epochs. The current tranche seeds
the namespace on top of RamFS; persistence on virtio-blk comes after the model
stabilizes.

Deliverables:
- `/system`, `/apps`, and `/qfs` seeded at boot (done)
- `/system/kernel.elf` ELF64 descriptor for the loaded Ring 0 kernel, readable from Ring 3 (done)
- `/system/qfs.plan` design document visible through the Ring 3 `fs` command (done)
- `/qfs/capsules`, `/qfs/tags`, `/qfs/timeline`, and generated view paths (done)
- Sealed capsule manifests with FNV-1a content IDs for boot files (done)
- Ring 3 `qfs` command for superblock, capsule catalog, and storage epoch (done)
- Storage checkpoint writes a QFS journal sector when virtio-blk is available (done)
- Capsule allocator with content identifiers
- Append-only epoch journal on virtio-blk
- View compiler that projects graph objects into path namespaces
- Load LibOS modules and Realm binaries from QuantaFS-Weave storage

---

## Phase 7 — Linux ELF Realm

POSIX-compatible execution environment using musl as the LibOS layer.

Deliverables:
- musl libc as `/libos/linux/libc.so`
- POSIX syscall translation table (open, read, write, mmap, exit, etc.)
- ELF64 dynamic linker (resolve `.so` imports against LibOS)
- Running a statically-linked Linux ELF binary

---

## Phase 8 — NVMe Driver

PCIe NVMe storage for faster, larger disk access.

Deliverables:
- NVMe admin queue initialization
- NVMe I/O queue setup
- Identify namespace, read/write commands
- Mount NVMe partition under VFS alongside virtio-blk

---

## Phase 9 — Realm-Local Schedulers

Allow Realms to implement their own scheduling policy within their CPU quota.

Deliverables:
- Realm scheduler interface (realm provides a `schedule_next()` callback)
- Gaming Realm scheduler: frame-paced, fixed interval
- Cooperative WASM scheduler
- Kernel global scheduler remains unchanged

---

## Phase 10 — Windows PE Realm

Win32-compatible execution environment using Wine-derived LibOS modules.

Deliverables:
- PE32+ loader (parse sections, map, handle relocations)
- Import Address Table patching via LibOS
- `ntdll.so`, `kernel32.so` as LibOS modules (Wine LGPL derivation)
- Running a simple Win32 console binary (no GDI dependency)
- `user32.so` + IPC to Display Realm for windowing

---

## Phase 11 — IPC Optimization + SMP Load Balancing

Performance and scalability improvements.

Deliverables:
- Lock-free ring buffer IPC implementation
- Work-stealing across per-CPU Realm run queues
- NUMA-aware page allocation (if multiple memory nodes present)
- Doorbell coalescing (batch multiple notifications)

---

## Phase 12 — Capability System Hardening

Replace implicit capability grants with explicit token-based system.

Deliverables:
- `cap_table_t` as kernel object per Realm
- Capability token issuance at realm_create
- Per-syscall capability check (CAP_VFS, CAP_IPC, CAP_PAGES, etc.)
- Secure Sandbox Realm: minimal capabilities, no IPC, no network

---

# Part XV — Non-Goals

Quanta does not aim to become:

- A Linux distribution or replacement
- A POSIX-first general purpose OS
- A pure microkernel (IPC cost is acceptable for bulk transfers)
- A container runtime (Realms are stronger isolation than containers)
- A VM hypervisor (Realms are not VMs — they share the kernel)
- A process-centric UNIX clone

These are not failures. They are design choices. The goal is to explore
execution-environment-centric computing, not to replicate existing systems.

---

# Part XVI — Summary

Quanta OS is a realm-oriented operating system where:

- **The kernel is a substrate**, not a runtime. It owns hardware, manages Realm
  lifecycle, enforces isolation, and services minimal syscalls. It does not define
  how binaries execute.

- **Realms are kernel objects**, managed from Ring 0 like tasks. They run in Ring 3.
  There is no Ring 3 Realm management infrastructure — the kernel manages Realms
  directly.

- **A single global LibOS** delivers compatibility libraries on demand. Realms request
  what they need. The LibOS maps shared read-only pages into requesting Realms.
  No compatibility logic enters the kernel.

- **Execution is routing.** Binary format determines Realm type. The Realm provides
  runtime semantics. The kernel routes, not executes.

- **Scheduling is two-level.** The kernel distributes CPU time to Realms. Each Realm
  schedules its own tasks. Policy is per-Realm, not global.

- **Memory is territorial.** The kernel grants raw pages. Realms implement their
  own allocators. LibOS modules are shared physical pages, not copies.

- **IPC is shared memory.** The kernel establishes shared regions. The data path
  is zero-copy between Realms. The kernel signals only.

The primary boundary of the system is:

```
kernel substrate  ↔  realm execution territory
```

Everything that matters about what software does happens on the right side of
that boundary.

---

*Quanta OS — Revision 2.0 — Phase 4 Edition*
*Architecture document. Implementation in kernel/ directory.*
