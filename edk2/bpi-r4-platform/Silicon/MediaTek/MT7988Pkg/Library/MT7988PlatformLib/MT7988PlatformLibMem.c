/** @file
  Memory map for MediaTek MT7988.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <MT7988.h>

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  4

#define PROBE_PATTERN  0x5AA5F00Fu

/**
  Check whether DramBase + Offset is backed by distinct DRAM, rather than
  aliasing DramBase the way the EMI wraps accesses past the end of DRAM.

  Runs with the MMU off, so every access goes straight to the bus.
  Both locations are restored afterwards.

  @param[in]  DramBase  Start of DRAM
  @param[in]  Offset    Byte offset to test

  @return TRUE if DramBase + Offset holds separate memory

**/
STATIC
BOOLEAN
IsDistinctDram (
  IN UINT64  DramBase,
  IN UINT64  Offset
  )
{
  UINTN    Low;
  UINTN    High;
  UINT32   SavedLow;
  UINT32   SavedHigh;
  BOOLEAN  Distinct;

  Low  = (UINTN)DramBase;
  High = (UINTN)(DramBase + Offset);

  SavedLow  = MmioRead32 (Low);
  SavedHigh = MmioRead32 (High);

  MmioWrite32 (Low, 0);
  MmioWrite32 (High, PROBE_PATTERN);
  Distinct = (MmioRead32 (Low) == 0) && (MmioRead32 (High) == PROBE_PATTERN);

  MmioWrite32 (Low, ~0u);
  MmioWrite32 (High, ~PROBE_PATTERN);
  Distinct = Distinct && (MmioRead32 (Low) == ~0u) &&
             (MmioRead32 (High) == ~PROBE_PATTERN);

  // Restore High first: if the two alias, Low is the real cell and must win.
  MmioWrite32 (High, SavedHigh);
  MmioWrite32 (Low, SavedLow);

  return Distinct;
}

/**
  Find the installed DRAM size, the same way U-Boot's get_ram_size() does:
  halve the candidate size until the upper half no longer aliases the lower.

  The BPI-R4 has 4 GB or 8 GB, so the probe never reports less than 4 GB.

  @return DRAM size in bytes

**/
STATIC
UINT64
ProbeDramSize (
  VOID
  )
{
  UINT64  DramBase;
  UINT64  Size;

  DramBase = FixedPcdGet64 (PcdSystemMemoryBase);

  for (Size = FixedPcdGet64 (PcdSystemMemoryMaxSize); Size > SIZE_4GB; Size >>= 1) {
    if (IsDistinctDram (DramBase, Size >> 1)) {
      return Size;
    }
  }

  return SIZE_4GB;
}

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU on your platform.

  The default MemoryInitPeiLib declares the always-present DRAM below 4 GB
  (PcdSystemMemoryBase/Size). DRAM above that is probed and declared here,
  along with the secure monitor carve-out.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR describing a Physical-to-
                                    Virtual Memory mapping. This array must be ended by a zero-filled
                                    entry

**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;
  UINT64                        DramBase;
  UINT64                        DramSize;
  UINT64                        LowSize;
  UINTN                         Index;

  ASSERT (VirtualMemoryMap != NULL);

  VirtualMemoryTable = AllocatePages (
                         EFI_SIZE_TO_PAGES (
                           sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
                           MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS
                           )
                         );
  if (VirtualMemoryTable == NULL) {
    return;
  }

  DramBase = FixedPcdGet64 (PcdSystemMemoryBase);
  DramSize = ProbeDramSize ();
  LowSize  = FixedPcdGet64 (PcdSystemMemorySize);

  DEBUG ((DEBUG_INFO, "MT7988: %Lu MB DRAM at 0x%Lx\n", DramSize >> 20, DramBase));

  if (DramSize > LowSize) {
    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      EFI_RESOURCE_ATTRIBUTE_PRESENT |
      EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_TESTED,
      DramBase + LowSize,
      DramSize - LowSize
      );
  }

  // BL31 lives here; neither UEFI nor the OS may touch it.
  BuildMemoryAllocationHob (
    FixedPcdGet64 (PcdSecureMonitorBase),
    FixedPcdGet64 (PcdSecureMonitorSize),
    EfiReservedMemoryType
    );

  Index = 0;

  // DRAM
  VirtualMemoryTable[Index].PhysicalBase = DramBase;
  VirtualMemoryTable[Index].VirtualBase  = DramBase;
  VirtualMemoryTable[Index].Length       = DramSize;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  // GICv3
  VirtualMemoryTable[++Index].PhysicalBase = MT7988_GIC_BASE;
  VirtualMemoryTable[Index].VirtualBase    = MT7988_GIC_BASE;
  VirtualMemoryTable[Index].Length         = MT7988_GIC_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Peripherals
  VirtualMemoryTable[++Index].PhysicalBase = MT7988_PERIPH_BASE;
  VirtualMemoryTable[Index].VirtualBase    = MT7988_PERIPH_BASE;
  VirtualMemoryTable[Index].Length         = MT7988_PERIPH_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // End of Table
  VirtualMemoryTable[++Index].PhysicalBase = 0;
  VirtualMemoryTable[Index].VirtualBase    = 0;
  VirtualMemoryTable[Index].Length         = 0;
  VirtualMemoryTable[Index].Attributes     = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT ((Index + 1) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);

  *VirtualMemoryMap = VirtualMemoryTable;
}
