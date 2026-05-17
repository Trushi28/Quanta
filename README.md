# Quanta OS — x86-64 Kernel  v2.0

A clean, modern, well-commented x86-64 operating system kernel written in C
and assembly, booted by Limine 8.7.0.

```
  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗ █████╗
  ██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██╔══██╗
  ██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   ███████║
  ██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   ██╔══██║
  ╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   ██║  ██║
   ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝
  Quanta OS  v2.0.0  (x86_64)
  x2APIC · SMP · VirtIO · VFS · Ring 3 Realms
```

---

## What's New in Phase 4

| Feature | Details |
|---|---|
| **x2APIC** | Requested from Limine; MSR-based LAPIC access (no MMIO) |
| **SMP** | Up to 64 CPUs; Limine AP bringup; per-CPU GDT/TSS/APIC |
| **x86-64 MSR helpers** | `rdmsr`/`wrmsr`, EFER NX+SCE, GS base, TSC |
| **CPUID** | Feature detection: x2APIC, NX, TSC, invariant TSC |
| **ACPI parser** | RSDP → XSDT/RSDT → MADT, HPET, MCFG |
| **Ticket spinlock** | Fair FIFO; IRQ-saving variants for ISR use |
| **Intrusive list** | Zero-allocation doubly-linked list |
| **Kernel heap** | Slab (8–2048 B, 9 caches) + large PMM allocator |
| **Preemptive scheduler** | Round-robin; APIC timer 1ms tick; sleep/yield/exit |
| **Context switch** | 6 callee-saved regs + RIP in `sched_asm.S` |
| **VirtIO-blk 1.1** | Split-ring virtqueue; PCIe ECAM via ACPI MCFG |
| **VFS** | RamFS + DevFS (`/dev/null`, `/dev/zero`) |
| **PS/2 Keyboard** | IRQ1; scancode-set-1; ring buffer; arrow keys |
| **Ring 3 native-init** | Primary user console running in a Native Realm |
| **Realm syscalls** | stdio, VFS open/read/close/readdir/stat, realm id, pages, power |
| **LibOS registry** | Kernel-managed LibOS Realm and runtime module lookup |
| **QAI Shell** | Kernel fallback/debug shell with history and tab-complete |
| **QAI Assistant** | Compiled-in OS knowledge base; keyword scoring |

---

## Project Structure

```
quanta/
├── Makefile                    Full build system (auto-discovers sources)
├── limine.conf                 Limine 8.x boot configuration
│
└── kernel/
    ├── kernel.ld               Higher-half linker script
    ├── include/
    │   └── limine.h            Limine protocol v2 header (all requests)
    │
    └── src/
        ├── kmain.c             Kernel entry — boots into Ring 3 native-init
        │
        ├── boot/
        │   ├── limine_requests.h   Inline accessors for all responses
        │   └── limine_requests.c   Request objects in .requests section
        │
        ├── cpu/
        │   ├── msr.h           rdmsr/wrmsr + well-known MSR addresses
        │   ├── cpuid.h         CPUID feature queries (x2APIC, NX, TSC…)
        │   ├── gdt.h / gdt.c   Per-CPU GDT + TSS (static pool)
        │   ├── idt.h / idt.c   IDT; idt_reload() for APs
        │   ├── isr.h / isr.c   Dispatch + PIC management (masked for APIC)
        │   ├── apic.h / apic.c xAPIC + x2APIC local APIC driver + timer
        │   ├── smp.h / smp.c   AP bringup + per-CPU cpu_local_t (GS base)
        │   ├── isr_stubs.S     Auto-generated 256-entry stub table
        │   └── gen_isr_stubs.py Python generator for isr_stubs.S
        │
        ├── acpi/
        │   ├── acpi.h          Table structures: RSDP, XSDT, MADT, HPET, MCFG
        │   └── acpi.c          Parser + MADT iterator
        │
        ├── mm/
        │   ├── pmm.h / pmm.c   Bitmap allocator (spinlock-protected)
        │   ├── vmm.h / vmm.c   Four-level paging; NX + SCE in EFER
        │   └── heap.h / heap.c Slab + free-list kmalloc/kfree/krealloc
        │
        ├── sched/
        │   ├── sched.h / sched.c   Preemptive round-robin scheduler
        │   └── sched_asm.S     Context switch (callee-saved regs + RIP)
        │
        ├── drivers/
        │   ├── serial.h / serial.c     COM1 UART (38400 baud, 8N1)
        │   ├── framebuffer.h / .c      8×16 pixel terminal (scrolling)
        │   ├── keyboard.h / keyboard.c PS/2 IRQ1 driver + ring buffer
        │   └── virtio/
        │       ├── virtio.h    VirtIO 1.1 structures + API
        │       └── virtio.c    PCIe ECAM scan + virtio-blk driver
        │
        ├── fs/
        │   ├── vfs.h           VFS abstraction layer + FD table
        │   └── vfs.c           RamFS + DevFS implementation
        │
        ├── shell/
        │   ├── shell.h         Shell API + ANSI macros
        │   ├── shell.c         QAI shell (history, tab-complete, 17 cmds)
        │   ├── qai.h           QAI assistant API
        │   └── qai.c           Keyword-scoring knowledge engine
        │
        └── lib/
            ├── string.h / string.c     Freestanding string + memory
            ├── kprintf.h / kprintf.c   Formatted output (%d %x %s %p %llu)
            ├── spinlock.h              Ticket spinlock (IRQ-saving variants)
            └── list.h                  Intrusive doubly-linked circular list
```

---

## Architecture Overview

### Boot Sequence (kmain.c)

```
Limine 8.7.0
  │  Sets up: long mode, HHDM, framebuffer, SMP info, RSDP
  ▼
kmain()
  1.  serial_init()              COM1 @ 38400 baud
  2.  limine_verify_requests()   Check framebuffer, memmap, HHDM, RSDP
  3.  fb_init()                  Pixel terminal
  4.  acpi_init()                Parse XSDT → MADT + MCFG (HHDM pre-set)
  5.  gdt_init()                 BSP GDT + TSS
  6.  idt_init()                 256 gates; PIC remapped then masked
  7.  pmm_init()                 Bitmap allocator over all usable RAM
  8.  vmm_init()                 Inherit Limine PML4; enable NX + SCE
  9.  heap_init()                Slab caches (8–2048 B) + large allocator
  10. apic_init()                x2APIC (MSR) or xAPIC (MMIO), SVR set
  11. smp_bsp_early_init()       Set BSP cpu_local_t via GS base MSR
      smp_init()                 Wake each AP → ap_entry()
                                   AP: gdt_init, idt_reload, apic_init,
                                       cpu_local_setup, apic_timer_init
  12. apic_timer_init(1ms)       BSP timer; fires sched_tick() via IRQ
  13. sched_init()               Create idle task; per-CPU run queues
  14. vfs_init()                 ramfs at /; devfs at /dev
  15. virtio_init()              PCIe ECAM scan → virtio-blk 1.1 driver
  16. keyboard_init()            PS/2 IRQ1; scancode-set-1 decoder
  17. syscall_init()             Program SYSCALL/SYSRET MSRs
  18. realm_system_init()        Realm table + LibOS Realm
  19. native-init Realm          Embedded ELF enters Ring 3
  20. QAI shell fallback         Only created if native-init fails
```

### Memory Layout

```
Virtual Address Space (x86-64, 48-bit canonical)

0x0000000000000000 ─ 0x00007FFFFFFFFFFF  Lower half (future user space)
                     ···canonical hole···
0xFFFF800000000000 ─ 0xFFFFFFFF7FFFFFFF  HHDM: all physical RAM
0xFFFFFFFF80000000 ─ 0xFFFFFFFFFFFFFFFF  Kernel image (-2 GB)
```

### x2APIC & SMP

```
BSP boots → Limine SMP response
  flags bit 0: request x2APIC

  x2APIC enabled?
    YES → LAPIC registers via MSRs 0x800–0x83F (32-bit each)
          IPI = single 64-bit wrmsr(0x830, dest<<32 | vector)
    NO  → xAPIC MMIO at HHDM(lapic_base)

  Each AP:
    1. Receives goto_address write (atomic)
    2. Runs ap_entry(smp_info)
    3. gdt_init() — owns its own GDT slot
    4. idt_reload() — shared IDT built by BSP
    5. apic_init() — its own LAPIC
    6. cpu_local_setup() — writes GS_BASE MSR → cpu_local()
    7. apic_timer_init(1) — 1ms periodic tick
    8. Increments g_cpu_count, enables interrupts, halts
```

### Interrupt Flow

```
Hardware / INT instruction
        │
isr_stub_N  (isr_stubs.S — generated)
  PUSH 0 (dummy error) if no hardware error code
  PUSH vector_number
  JMP  isr_common_stub

isr_common_stub
  PUSH r15..rax (15 GP regs)
  CLD
  MOV  rdi, rsp   ← arg0 = registers_t*
  CALL isr_dispatch(r)

isr_dispatch(r)
  v < 32  → CPU exception → kpanic (full register dump)
  v == 3  → Breakpoint → print RIP, continue
  v >= 32 → call irq_handlers[v-32], then apic_eoi()
             (APIC_TIMER_VECTOR = 48 → sched_tick())

Return path: pop r15..rax, add rsp 16, iretq
```

### Scheduler

```
Per-CPU run_queue (circular linked list of task_t)
APIC timer (1ms) → sched_tick()
  1. Increment global tick counter
  2. Wake sleeping tasks (tick >= wake_tick)
  3. If preemptible(): sched_run_next()

sched_run_next()
  1. Re-queue current task (if RUNNING → RUNNABLE)
  2. pop_front(run_queue) → next
  3. sched_switch(&cur->ctx, next->ctx)
       saves: r15,r14,r13,r12,rbx,rbp,rip
       loads: new task's saved context

Task creation:
  task_create(name, fn, arg, stack_sz)
    allocates task_t + kernel stack via kmalloc
    builds initial stack frame so sched_switch enters task_trampoline
    task_trampoline: calls fn(arg), then sched_exit(0)
```

### VirtIO Block Device

```
ACPI MCFG → PCIe ECAM base address
Bus/device scan → VendorID 0x1AF4, DeviceID 0x1040–0x107F

virtio-blk init:
  1. Reset device (status = 0)
  2. Acknowledge + Driver
  3. Negotiate features (VIRTIO_F_VERSION_1)
  4. Features OK
  5. virtq_init(queue 0)
     alloc desc table + avail ring + used ring (PMM pages)
     write phys addresses to common config BAR
  6. Driver OK

I/O (virtio_blk_read / virtio_blk_write):
  Build 3-descriptor chain:
    [0] req header (type, sector) — device read
    [1] data buffer               — WRITE flag for reads
    [2] status byte               — device writes result
  Add to avail ring, write notify register
  Busy-wait on status byte (≠ 0xFF)
```

### User Consoles

The normal boot path starts `native-init`, a small Ring 3 console loaded as an
ELF into a Native Realm. It uses syscalls for keyboard input, console output,
identity, LibOS lookup, and realm-owned page mapping.

```
r3:/ $ help
commands: help ls cat wasm pid realm libos page game reboot shutdown exit
```

The QAI shell is still present, but it is now the kernel debug fallback rather
than the primary user session.

### QAI Shell

```
shell_run() task:
  register_builtins()  ← 17 commands
  print version banner
  loop:
    print prompt
    read line (kbd_getchar blocking)
    handle: backspace, arrow keys (ESC[A/B/C/D), tab-complete
    execute(line)
      parse_args → argv[]
      lookup in cmd_table → cmd_fn(argc, argv)

QAI assistant (ai <question>):
  For each knowledge-base entry, count keyword hits in question
  Return response from best-scoring entry
  Enrich with live data (pmm_stats, uptime, cpu info)
```

---

## Building

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install gcc-x86_64-linux-gnu binutils-x86_64-linux-gnu \
                 qemu-system-x86 xorriso git python3

# Arch Linux
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils qemu xorriso python
```

### Make Targets

```bash
make                 # Build kernel ELF
make iso             # Build bootable ISO (clones Limine 8.7.0 if needed)
make run             # ISO + QEMU (4 CPUs, virtio-blk, SDL display)
make run-nographic   # Same but serial only (good for CI)
make run-kvm         # KVM-accelerated
make generate_stubs  # Regenerate isr_stubs.S
make clean           # Remove build/
make distclean       # Remove build/ + limine/
```

### QEMU Invocation (`make run`)

```bash
qemu-system-x86_64 -M q35 -m 512M -smp 4,cores=4 \
    -cdrom build/quanta.iso \
    -device virtio-blk-pci,drive=vblk0 \
    -drive id=vblk0,file=build/disk.img,if=none,format=raw \
    -serial stdio -display sdl \
    -no-reboot -no-shutdown
```

---

## What You Should See at Boot

```
  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗ █████╗
  ...
  Quanta OS  v2.0.0  (x86_64)
  x2APIC  ·  SMP  ·  VirtIO  ·  VFS  ·  Ring 3 Realms
====================================================================
  Bootloader : Limine 8.7.0
  Kernel     : phys 0x1000000  virt 0xffffffff80000000
  Boot time  : 1746700000 (Unix)
  Framebuffer: 1280x800  32 bpp  pitch 5120
  HHDM offset: 0xffff800000000000
  CPU        : GenuineIntel  (Intel(R) Core...)
  x2APIC     : supported
--------------------------------------------------------------------
[ACPI] XSDT at phys 0x...
[GDT] Initialising...
[IDT] Initialising...
[PMM] bitmap @ phys 0x...  total=131072 pages  free=496 MiB
[PMM] Total:512 MiB  Used:16 MiB  Free:496 MiB
[VMM] PML4 phys=0x...  NX+SCE enabled
[HEAP] Slab allocator ready  (9 caches)
[APIC] Mode: x2APIC  LAPIC-ID: 0
[SMP] 4 CPU(s) detected  BSP-LAPIC=0  x2APIC
[SMP] CPU 1 online  (LAPIC 2  x2APIC)
[SMP] CPU 2 online  (LAPIC 4  x2APIC)
[SMP] CPU 3 online  (LAPIC 6  x2APIC)
[SMP] 4 CPU(s) online
[VFS] Mounted ramfs at /  devfs at /dev
[VirtIO] Found device ID=2 at 00:03.0
[VirtIO-blk] Ready  capacity=131072 sectors (64 MiB)
[KBD] Initialising PS/2 keyboard...
====================================================================
  Quanta OS initialised successfully.
  4 CPU(s) online  |  x2APIC  |  VFS ready  |  Realm ready
====================================================================

[Ring3 Init] Quanta Native Realm console online.
Type 'help' for user-mode commands.
r3:/ $ _
```

---

## QAI Shell Commands

| Command | Description |
|---|---|
| `help` | List all commands |
| `version` | Show Quanta ASCII banner |
| `clear` | Clear the screen |
| `echo <text>` | Print arguments |
| `cpuinfo` | CPU vendor, brand, features |
| `mem` | PMM + heap statistics |
| `uptime` | System uptime (h/m/s/ms) |
| `tasks` | Show running tasks |
| `ls [path]` | List directory (colour-coded) |
| `cat <file>` | Print a file |
| `write <file> <text>` | Write text to a file |
| `stat <path>` | Show file/directory metadata |
| `sleep <ms>` | Sleep N milliseconds |
| `disk` | VirtIO block device info |
| `history` | Command history |
| `reboot` | Triple-fault reboot |
| `ai <question>` | Ask the QAI assistant |

### QAI Topics

```
ai what is quanta
ai how does x2apic work
ai explain smp bringup
ai how does the scheduler work
ai explain paging
ai how does virtio work
ai what is the vfs
ai how does kmalloc work
ai explain the idt
ai what acpi tables does quanta use
```

---

## Key Design Decisions

**x2APIC by default** — Requested from Limine via `LIMINE_SMP_X2APIC` flag.
x2APIC uses MSR writes instead of MMIO, which is faster and required on
systems with more than 255 logical processors.

**Ticket spinlocks** — Fair FIFO ordering prevents starvation. IRQ-saving
variants (`spinlock_irq_acquire`) ensure correctness when a lock can be
acquired from both task context and interrupt handlers.

**ACPI before PMM** — ACPI table parsing only needs `phys_to_virt()`, which
uses the HHDM offset. Quanta sets `hhdm_off_early` from the Limine response
before `pmm_init()` so ACPI structures are readable immediately.

**Intrusive linked list** — `list.h` embeds the list node directly in
structures (no separate heap node per element). The scheduler run queue,
all-tasks list, and VFS directory children all use this.

**VirtIO PCIe ECAM** — Rather than legacy PCI config I/O cycles, Quanta
uses PCIe Memory-Mapped ECAM (from ACPI MCFG). This is the correct approach
for QEMU q35 which exposes VirtIO devices over PCIe.

**Slab allocator** — 9 fixed-size slab caches (8 B – 2048 B) cover the
common case. Each allocation has a 16-byte header with magic cookie
`0xA110C8A7` to catch double-free and corruption in `kfree`.

**QAI is compiled-in** — No network, no external model. The knowledge base
is a constant array of keyword→response entries. Best-match scoring gives
contextual answers enriched with live kernel data (PMM stats, uptime, CPU).

---

## Roadmap (Phase 3+)

1. **IOAPIC driver** — Route external IRQs via the I/O APIC properly
2. **HPET driver** — High-precision timer; replace calibration loop
3. **Syscall interface** — `SYSCALL`/`SYSRET` (STAR/LSTAR MSRs already enabled)
4. **ELF loader** — Parse + map ELF64 binaries into user address spaces
5. **User space** — Ring-3 tasks; syscall gateway; user stack
6. **FAT32 driver** — Read the virtio-blk disk (replace ramfs persistence)
7. **NVMe driver** — PCIe NVMe admin + I/O queues
8. **Network** — virtio-net driver + minimal IP/UDP stack
9. **SMP load balancing** — Work-stealing across per-CPU run queues
10. **Demand paging** — Page fault handler + copy-on-write
