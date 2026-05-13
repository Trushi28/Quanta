#pragma once
// ============================================================
//  cpu/power.h — ACPI-backed power management
//
//  Provides safe reboot and shutdown for both QEMU and bare
//  metal.  Must call power_init() after acpi_init() so the
//  FADT and DSDT can be parsed once at boot.
//
//  Reboot chain (tries each in order until CPU resets):
//    1. ACPI FADT reset register (ACPI 2.0+ hardware reset)
//    2. PS/2 keyboard-controller pulse (port 0x64 ← 0xFE)
//    3. Port 0xCF9 write (PCIe standard warm reset)
//    4. Triple-fault (last resort; always works on x86)
//
//  Shutdown chain:
//    1. ACPI S5 sleep state via PM1a/PM1b control block
//    2. QEMU PIIX4 legacy port 0x0604 (value 0x3400)
//    3. Old QEMU legacy port 0xB004 (value 0x2000)
//    4. CPU halt with "safe to power off" message
// ============================================================

#include <stdint.h>

// Parse ACPI FADT and DSDT to find:
//   - reset register address and value
//   - PM1a/PM1b control block I/O ports
//   - S5 sleep type bytes for ACPI shutdown
// Safe to call even if no ACPI tables are present (silently skips).
void power_init(void);

// Reboot the system.  Never returns.
__attribute__((noreturn)) void power_reboot(void);

// Power off / shutdown the system.  Never returns.
__attribute__((noreturn)) void power_shutdown(void);
