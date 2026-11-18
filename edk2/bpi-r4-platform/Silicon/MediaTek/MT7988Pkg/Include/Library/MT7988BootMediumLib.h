/** @file
  Report the medium the MT7988 BootROM loaded BL2 from.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MT7988_BOOT_MEDIUM_LIB_H_
#define MT7988_BOOT_MEDIUM_LIB_H_

typedef enum {
  Mt7988BootMediumUnknown,
  Mt7988BootMediumSd,
  Mt7988BootMediumEmmc,
  Mt7988BootMediumSpiNand
} MT7988_BOOT_MEDIUM;

/**
  Return the boot medium selected by the SW3 strap.

  @return  Mt7988BootMediumSd, Mt7988BootMediumEmmc or, for anything else,
           Mt7988BootMediumSpiNand.
**/
MT7988_BOOT_MEDIUM
EFIAPI
Mt7988GetBootMedium (
  VOID
  );

/**
  Mux and pad-configure the eMMC pins of MSDC0 the way an eMMC BL2 does.
  For boots where BL2 did not touch MSDC0 (SPI-NAND).
**/
VOID
EFIAPI
Mt7988MsdcSetupEmmcPins (
  VOID
  );

#endif // MT7988_BOOT_MEDIUM_LIB_H_
