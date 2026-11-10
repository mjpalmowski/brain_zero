/* Match PlatformIO's static RAM accounting. 35% of 320 KiB is 114688 bytes. */
ASSERT(SIZEOF(.dram0.data) + SIZEOF(.dram0.bss) + SIZEOF(.noinit) < 114688,
       "brain_zero static RAM must be below 35% of 320 KiB")
