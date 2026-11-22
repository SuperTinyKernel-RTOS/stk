/* Updated for RP2350 4MB Flash - TrustZone Secure World */

/* Secure Flash: Where the Secure Manager/TEE resides (2MB) */
FLASH(rx)       : ORIGIN = 0x10000000, LENGTH = 2048k 

/* Non-Secure Callable (NSC): Gateway for Non-Secure calls (4KB) */
/* IMPORTANT: Both ORIGIN and LENGTH must satisfy SAU constraints:
   - ORIGIN must be 256-byte aligned (already satisfied: 0x10200000)
   - LENGTH must be a power of 2 (4096 = 0x1000 satisfies this)
   Do NOT change LENGTH to a non-power-of-2 value. */
FLASH_NSC(rx)   : ORIGIN = 0x10200000, LENGTH = 4k 

/* Non-Secure Flash: Used by the NS image (linked separately).
   Starts immediately after NSC region at 0x10201000.
   __ns_flash_start symbol (exported from memmap_tz.ld) must be used
   as ORIGIN in the Non-Secure image's linker script to enforce this boundary.
   Remaining size: 4096k - 2048k - 4k = 2044k */
FLASH_NS(rx)    : ORIGIN = 0x10201000, LENGTH = 2044k