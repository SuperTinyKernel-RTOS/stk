/* Updated for RP2350 4MB Flash - TrustZone Non-Secure World */

/* Non-Secure Flash: Starts immediately after the NSC region.
   ORIGIN matches __ns_flash_start exported from the Secure image
   (memmap_tz_sc.ld / pico_flash_region_tz_sc.ld).
   Remaining size: 4096k - 2048k - 4k = 2044k */
FLASH_NS(rx)    : ORIGIN = 0x10201000, LENGTH = 2044k
