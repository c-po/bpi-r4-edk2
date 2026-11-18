/** @file
  Boot medium detection for MT7988.

  No documented register exposes the latched boot strap, so the medium is
  inferred from what ATF BL2 configured for the medium it was built for
  (plat/mediatek/mt7988/bl2/bl2_dev_mmc.c); the BootROM only runs a BL2 from
  the medium SW3 selects. GPIO38 (eMMC) and GPIO24 (SPI0) are already in
  function 1 after reset, so their mux says nothing: an SD BL2 is recognised
  by GPIO32 in function 5, an eMMC BL2 by the pad pull values it writes, and
  anything else is taken to be SPI-NAND.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MT7988BootMediumLib.h>
#include <MT7988.h>

#define EMMC_FUNC      1              // emmc_51 group, GPIO38-49
#define SD_PIN         32             // sdcard group, function 5
#define SD_FUNC        5
#define EMMC_R0_PULLS  0xBFE          // mmc_gpio_setup(), MSDC_INDEX 0
#define EMMC_R1_PULLS  0x401

//
// eMMC pad setup from ATF plat/mediatek/mt7988/bl2/bl2_dev_mmc.c
// (msdc0_pinmux, msdc0_pupd, mmc_gpio_setup() with MSDC_INDEX 0). The pins
// sit in IOCFG_RB; the pull select bit of each pin is from mt7988_pin_infos.
//
#define IOCFG_RB_BASE  0x11D20000
#define RB_DRV_CFG0    0x000
#define RB_DRV_CFG1    0x010
#define RB_IES_CFG0    0x050
#define RB_PUPD_CFG0   0x070
#define RB_R0_CFG0     0x090
#define RB_R1_CFG0     0x0B0
#define RB_RDSEL_CFG0  0x0D0
#define RB_RDSEL_CFG1  0x0E0
#define RB_RDSEL_CFG2  0x0F0
#define RB_SMT_CFG0    0x140
#define RB_TDSEL_CFG0  0x180
#define RB_TDSEL_CFG1  0x190

STATIC CONST struct {
  UINT8      Pin;
  UINT8      PupdBit;
  BOOLEAN    PullDown;
} mEmmcPins[] = {
  { 38, 11, FALSE }, { 39, 10, TRUE  }, { 40, 0, TRUE  }, { 41, 1, FALSE },
  { 42, 9,  FALSE }, { 43, 8,  FALSE }, { 44, 7, FALSE }, { 45, 6, FALSE },
  { 46, 5,  FALSE }, { 47, 4,  FALSE }, { 48, 3, FALSE }, { 49, 2, FALSE },
};

STATIC
UINT32
GpioGetMode (
  IN UINT32  Pin
  )
{
  return (MmioRead32 (MT7988_GPIO_MODE (Pin)) >> (4 * (Pin % 8))) & 0xF;
}

STATIC
VOID
RbClrSet (
  IN UINTN   Offset,
  IN UINT32  Clear,
  IN UINT32  Set
  )
{
  MmioAndThenOr32 (IOCFG_RB_BASE + Offset, ~Clear, Set);
}

MT7988_BOOT_MEDIUM
EFIAPI
Mt7988GetBootMedium (
  VOID
  )
{
  UINT32  SdMode;
  UINT32  R0;
  UINT32  R1;

  SdMode = GpioGetMode (SD_PIN);
  R0     = MmioRead32 (IOCFG_RB_BASE + RB_R0_CFG0) & 0xFFF;
  R1     = MmioRead32 (IOCFG_RB_BASE + RB_R1_CFG0) & 0xFFF;

  DEBUG ((DEBUG_INFO, "%a: GPIO%u=%u R0=0x%03x R1=0x%03x\n", __func__, SD_PIN, SdMode, R0, R1));

  if (SdMode == SD_FUNC) {
    return Mt7988BootMediumSd;
  }

  if ((R0 == EMMC_R0_PULLS) && (R1 == EMMC_R1_PULLS)) {
    return Mt7988BootMediumEmmc;
  }

  return Mt7988BootMediumSpiNand;
}

VOID
EFIAPI
Mt7988MsdcSetupEmmcPins (
  VOID
  )
{
  UINTN   Index;
  UINT32  Shift;

  for (Index = 0; Index < ARRAY_SIZE (mEmmcPins); Index++) {
    Shift = 4 * (mEmmcPins[Index].Pin % 8);
    MmioAndThenOr32 (MT7988_GPIO_MODE (mEmmcPins[Index].Pin), ~(0xFU << Shift), (UINT32)EMMC_FUNC << Shift);
    MmioWrite32 (
      IOCFG_RB_BASE + RB_PUPD_CFG0 + (mEmmcPins[Index].PullDown ? MT7988_GPIO_SET : MT7988_GPIO_CLR),
      1U << mEmmcPins[Index].PupdBit
      );
  }

  RbClrSet (RB_IES_CFG0, 0xFFF, 0xFFF);
  RbClrSet (RB_SMT_CFG0, 0xFFF, 0xFFF);
  RbClrSet (RB_R0_CFG0, 0xFFF, 0xBFE);
  RbClrSet (RB_R1_CFG0, 0xFFF, 0x401);
  RbClrSet (RB_DRV_CFG0, 0x3FFFFFFF, 0x9249249);
  RbClrSet (RB_DRV_CFG1, 0x3F, 0x9);
  RbClrSet (RB_RDSEL_CFG0, 0x3FFFFFFF, 0);
  RbClrSet (RB_RDSEL_CFG1, 0x3FFFFFFF, 0);
  RbClrSet (RB_RDSEL_CFG2, 0xFFF, 0);
  RbClrSet (RB_TDSEL_CFG0, 0xFFFFFFFF, 0);
  RbClrSet (RB_TDSEL_CFG1, 0xFFFF, 0);
}
