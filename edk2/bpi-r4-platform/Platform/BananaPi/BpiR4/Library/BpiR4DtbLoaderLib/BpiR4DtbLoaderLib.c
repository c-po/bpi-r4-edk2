/** @file
  DtPlatformDtbLoaderLib for the BPI-R4.

  Picks the Linux DTB whose MSDC0 overlay (-sd or -emmc) matches the boot
  medium, and sets "mac-address" on every Ethernet port from the EEPROM.
  An OS loader replacing the FDT (GRUB "devicetree") drops both.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BpiR4MacLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MT7988BootMediumLib.h>

#define DTB_PADDING  SIZE_4KB         // room for the new properties

/**
  Leave a note under /chosen; readable from Linux without a UART.
**/
STATIC
VOID
SetChosenString (
  IN VOID         *Fdt,
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Value
  )
{
  INT32  Node;

  Node = FdtPathOffset (Fdt, "/chosen");
  if (Node < 0) {
    Node = FdtAddSubnode (Fdt, 0, "chosen");
  }

  if (Node >= 0) {
    FdtSetProp (Fdt, Node, Name, Value, (UINT32)AsciiStrSize (Value));
  }
}

STATIC
VOID
SetMacAddresses (
  IN VOID  *Fdt
  )
{
  EFI_STATUS         Status;
  BPI_R4_MAC_SOURCE  Source;
  UINT8              Base[6];
  UINT8              Mac[6];
  CONST CHAR8        *Path;
  UINTN              Index;
  INT32              Node;
  INT32              Ret;

  Status = BpiR4MacGetBase (Base, &Source);
  SetChosenString (Fdt, "bpi-r4,mac-diag", BpiR4MacDiagnostics ());
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no MAC address available: %r\n", __func__, Status));
    SetChosenString (Fdt, "bpi-r4,mac-source", "none");
    return;
  }

  SetChosenString (
    Fdt,
    "bpi-r4,mac-source",
    (Source == BpiR4MacSourceUser) ? "user" :
    (Source == BpiR4MacSourceGenerated) ? "generated" : "volatile"
    );

  DEBUG ((
    DEBUG_INFO,
    "BPI-R4: base MAC %02x:%02x:%02x:%02x:%02x:%02x (%a)\n",
    Base[0],
    Base[1],
    Base[2],
    Base[3],
    Base[4],
    Base[5],
    (Source == BpiR4MacSourceUser) ? "user" :
    (Source == BpiR4MacSourceGenerated) ? "generated" : "volatile"
    ));

  for (Index = 0; Index < BPI_R4_MAC_PORT_COUNT; Index++) {
    BpiR4MacGetPort (Base, Index, Mac, &Path, NULL);

    Node = FdtPathOffset (Fdt, Path);
    if (Node < 0) {
      DEBUG ((DEBUG_WARN, "%a: %a not in DT, skipped\n", __func__, Path));
      continue;
    }

    Ret = FdtSetProp (Fdt, Node, "mac-address", Mac, sizeof (Mac));
    if (Ret != 0) {
      DEBUG ((DEBUG_ERROR, "%a: %a: FdtSetProp failed (%d)\n", __func__, Path, Ret));
    }
  }
}

EFI_STATUS
EFIAPI
DtPlatformLoadDtb (
  OUT   VOID   **Dtb,
  OUT   UINTN  *DtbSize
  )
{
  EFI_STATUS          Status;
  MT7988_BOOT_MEDIUM  Medium;
  EFI_GUID            *File;
  VOID                *Orig;
  UINTN               OrigSize;
  VOID                *Copy;
  UINTN               CopySize;

  // After a SPI-NAND boot SW3 routes the eMMC, not the SD slot, to MSDC0
  Medium = Mt7988GetBootMedium ();
  if ((Medium == Mt7988BootMediumEmmc) || (Medium == Mt7988BootMediumSpiNand)) {
    File = &gBpiR4EmmcDtbFileGuid;
  } else {
    if (Medium == Mt7988BootMediumUnknown) {
      DEBUG ((DEBUG_WARN, "%a: boot medium unknown, using the SD DTB\n", __func__));
    }

    File = &gBpiR4SdDtbFileGuid;
  }

  DEBUG ((
    DEBUG_INFO,
    "BPI-R4: booted from %a\n",
    (Medium == Mt7988BootMediumEmmc) ? "eMMC" :
    (Medium == Mt7988BootMediumSpiNand) ? "SPI-NAND" :
    (Medium == Mt7988BootMediumSd) ? "SD" : "unknown"
    ));

  Status = GetSectionFromAnyFv (File, EFI_SECTION_RAW, 0, &Orig, &OrigSize);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  CopySize = OrigSize + DTB_PADDING;
  Copy     = AllocatePool (CopySize);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Orig, Copy, (INT32)CopySize) != 0) {
    FreePool (Copy);
    return EFI_NOT_FOUND;
  }

  SetMacAddresses (Copy);
  FdtPack (Copy);

  *Dtb     = Copy;
  *DtbSize = FdtTotalSize (Copy);

  return EFI_SUCCESS;
}
