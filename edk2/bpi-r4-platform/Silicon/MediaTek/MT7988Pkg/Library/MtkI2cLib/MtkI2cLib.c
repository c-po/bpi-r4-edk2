/** @file
  Polled DMA-mode driver for the MT7988 I2C2 controller (mediatek,mt7981-i2c).

  Register layout (mt_i2c_regs_v3) and the transfer sequence are ported from
  U-Boot drivers/i2c/mtk_i2c.c (GPL-2.0+ there; only register offsets and the
  programming order are reused). Clock and pin numbers come from the Linux DT
  of the BPI-R4: clocks infra_i2c_bck and infra_66m_ap_dma_bck, pins
  i2c2_1 (GPIO71/72, function 1).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MtkI2cLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <MT7988.h>

#define I2C_CONTROL         0x10
#define I2C_TRANSFER_LEN    0x14
#define I2C_TRANSAC_LEN     0x18
#define I2C_DELAY_LEN       0x1C
#define I2C_TIMING          0x20
#define I2C_START           0x24
#define I2C_EXT_CONF        0x28
#define I2C_LTIMING         0x2C
#define I2C_HS              0x30
#define I2C_IO_CONFIG       0x34
#define I2C_FIFO_ADDR_CLR   0x38
#define I2C_TRANSFER_AUX    0x44
#define I2C_CLOCK_DIV       0x48
#define I2C_SOFTRESET       0x50
#define I2C_SLAVE_ADDR      0x94
#define I2C_INTR_MASK       0x08
#define I2C_INTR_STAT       0x0C
#define I2C_DCM_EN          0xF88

#define I2C_RS_TRANSFER     BIT4
#define I2C_HS_NACKERR      BIT2
#define I2C_ACKERR          BIT1
#define I2C_TRANSAC_COMP    BIT0
#define I2C_INTR_ALL        (I2C_RS_TRANSFER | I2C_HS_NACKERR | I2C_ACKERR | I2C_TRANSAC_COMP)

#define I2C_TRANSAC_START   BIT0
#define I2C_RS_MUL_TRIG     BIT14

#define I2C_CONTROL_RS               BIT1
#define I2C_CONTROL_DMA_EN           BIT2
#define I2C_CONTROL_CLK_EXT_EN       BIT3
#define I2C_CONTROL_DIR_CHANGE       BIT4
#define I2C_CONTROL_ACKERR_DET_EN    BIT5
#define I2C_CONTROL_DMAACK           BIT8
#define I2C_CONTROL_ASYNC            BIT9

#define I2C_ST_START_CON    0x8001
#define I2C_IO_OPEN_DRAIN   0x0003

#define DMA_INT_FLAG        0x00
#define DMA_EN              0x08
#define DMA_RST             0x0C
#define DMA_CON             0x18
#define DMA_TX_MEM_ADDR     0x1C
#define DMA_RX_MEM_ADDR     0x20
#define DMA_TX_LEN          0x24
#define DMA_RX_LEN          0x28
#define DMA_CON_TX          0x0
#define DMA_CON_RX          0x1
#define DMA_HARD_RST        0x2

#define I2C_SCL_PIN         71
#define I2C_SDA_PIN         72
#define I2C_PIN_FUNC        1

#define I2C_SPEED_HZ        100000
#define I2C_CLK_DIV         4          // fixed divider, as in U-Boot
#define I2C_TIMEOUT_US      50000

STATIC UINT8    *mDmaBuffer;           // 32-bit addressable bounce buffer
STATIC BOOLEAN  mInitialized;
STATIC UINT32   mLastStatus;

STATIC
VOID
I2cWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (MT7988_I2C2_BASE + Offset, Value);
}

STATIC
VOID
GpioSetMode (
  IN UINT32  Pin,
  IN UINT32  Mode
  )
{
  UINT32  Shift;

  Shift = 4 * (Pin % 8);
  MmioAndThenOr32 (MT7988_GPIO_MODE (Pin), ~(0xFU << Shift), Mode << Shift);
}

STATIC
VOID
I2cInitHw (
  VOID
  )
{
  MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_RST, DMA_HARD_RST);
  MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_RST, 0);
  I2cWrite (I2C_SOFTRESET, 1);
  I2cWrite (I2C_IO_CONFIG, I2C_IO_OPEN_DRAIN);
  I2cWrite (I2C_DCM_EN, 0);
  I2cWrite (
    I2C_CONTROL,
    I2C_CONTROL_ACKERR_DET_EN | I2C_CONTROL_CLK_EXT_EN | I2C_CONTROL_DMAACK | I2C_CONTROL_ASYNC
    );
  I2cWrite (I2C_DELAY_LEN, 2);
}

/**
  bus = src / (2 * sample * step); pick the smallest product that does not
  exceed the target rate (U-Boot mtk_i2c_calculate_speed, standard mode).
**/
STATIC
VOID
I2cSetSpeed (
  VOID
  )
{
  UINT32  ClkSrc;
  UINT32  OptDiv;
  UINT32  Sample;
  UINT32  Step;
  UINT32  BestSample;
  UINT32  BestStep;
  UINT32  BestMul;

  ClkSrc     = MT7988_I2C_SEL_HZ / I2C_CLK_DIV;
  OptDiv     = ((ClkSrc >> 1) + I2C_SPEED_HZ - 1) / I2C_SPEED_HZ;
  BestSample = 8;
  BestStep   = 64;
  BestMul    = 8 * 64;

  for (Sample = 1; Sample <= 8; Sample++) {
    Step = (OptDiv + Sample - 1) / Sample;
    if ((Step <= 64) && ((Step * Sample) < BestMul)) {
      BestMul    = Step * Sample;
      BestSample = Sample;
      BestStep   = Step;
    }
  }

  I2cWrite (I2C_CLOCK_DIV, I2C_CLK_DIV - 1);
  I2cWrite (I2C_TIMING, ((BestSample - 1) << 8) | (BestStep - 1));
  I2cWrite (I2C_HS, 0);
  I2cWrite (I2C_LTIMING, ((BestSample - 1) << 6) | (BestStep - 1));
}

EFI_STATUS
EFIAPI
MtkI2cInit (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Buffer;

  if (mInitialized) {
    return EFI_SUCCESS;
  }

  Buffer = MAX_UINT32;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDmaBuffer = (UINT8 *)(UINTN)Buffer;

  MmioAndThenOr32 (
    MT7988_TOPCKGEN_BASE + MT7988_TOP_CLK_CFG4,
    ~(UINT32)MT7988_TOP_I2C_SEL_MASK,
    MT7988_TOP_I2C_SEL_NET1PLL
    );
  MmioWrite32 (MT7988_TOPCKGEN_BASE + MT7988_TOP_CLK_CFG4_CLR, MT7988_TOP_I2C_PDN);
  MmioWrite32 (MT7988_TOPCKGEN_BASE + MT7988_TOP_CLK_CFG_UPDATE, MT7988_TOP_I2C_UPDATE);
  MmioWrite32 (MT7988_INFRACFG_BASE + MT7988_INFRA2_CG_CLR, MT7988_INFRA_I2C_BCK);
  MmioWrite32 (MT7988_INFRACFG_BASE + MT7988_INFRA1_CG_CLR, MT7988_INFRA_AP_DMA_BCK);

  GpioSetMode (I2C_SCL_PIN, I2C_PIN_FUNC);
  GpioSetMode (I2C_SDA_PIN, I2C_PIN_FUNC);

  I2cInitHw ();
  I2cSetSpeed ();

  mInitialized = TRUE;
  return EFI_SUCCESS;
}

/**
  One transaction: a write, a read, or a write followed by a repeated-start
  read (WRRD). Data goes through mDmaBuffer: TX at offset 0, RX at 256.
**/
STATIC
EFI_STATUS
I2cTransfer (
  IN  UINT8        Addr,
  IN  CONST UINT8  *WriteBuffer,
  IN  UINTN        WriteLength,
  OUT UINT8        *ReadBuffer,
  IN  UINTN        ReadLength
  )
{
  UINT8   *Tx;
  UINT8   *Rx;
  UINT32  Control;
  UINT32  IntMask;
  UINT32  Stat;
  UINTN   Timeout;

  if (!mInitialized) {
    return EFI_NOT_READY;
  }

  if ((WriteLength > MTK_I2C_MAX_LEN) || (ReadLength > MTK_I2C_MAX_LEN) ||
      ((WriteLength == 0) && (ReadLength == 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Tx = mDmaBuffer;
  Rx = mDmaBuffer + 256;

  Control = MmioRead32 (MT7988_I2C2_BASE + I2C_CONTROL) & ~(UINT32)(I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS);
  if ((WriteLength != 0) && (ReadLength != 0)) {
    Control |= I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS;
    IntMask  = I2C_HS_NACKERR | I2C_ACKERR | I2C_TRANSAC_COMP;
  } else {
    IntMask = I2C_INTR_ALL;
  }

  I2cWrite (I2C_CONTROL, Control | I2C_CONTROL_DMA_EN);
  I2cWrite (I2C_EXT_CONF, I2C_ST_START_CON);
  I2cWrite (I2C_SLAVE_ADDR, (Addr << 1) | ((WriteLength == 0) ? 1 : 0));
  I2cWrite (I2C_INTR_STAT, I2C_INTR_ALL);
  I2cWrite (I2C_FIFO_ADDR_CLR, 1);
  I2cWrite (I2C_INTR_MASK, IntMask);

  if ((WriteLength != 0) && (ReadLength != 0)) {
    I2cWrite (I2C_TRANSFER_LEN, (UINT32)WriteLength);
    I2cWrite (I2C_TRANSFER_AUX, (UINT32)ReadLength);
    I2cWrite (I2C_TRANSAC_LEN, 2);
  } else {
    I2cWrite (I2C_TRANSFER_LEN, (UINT32)((WriteLength != 0) ? WriteLength : ReadLength));
    I2cWrite (I2C_TRANSAC_LEN, 1);
  }

  MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_INT_FLAG, 0);

  if (WriteLength != 0) {
    CopyMem (Tx, WriteBuffer, WriteLength);
    WriteBackDataCacheRange (Tx, WriteLength);
    MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_CON, DMA_CON_TX);
    MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_TX_MEM_ADDR, (UINT32)(UINTN)Tx);
    MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_TX_LEN, (UINT32)WriteLength);
  }

  if (ReadLength != 0) {
    InvalidateDataCacheRange (Rx, ReadLength);
    if (WriteLength == 0) {
      MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_CON, DMA_CON_RX);
    }

    MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_RX_MEM_ADDR, (UINT32)(UINTN)Rx);
    MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_RX_LEN, (UINT32)ReadLength);
  }

  MmioWrite32 (MT7988_I2C2_DMA_BASE + DMA_EN, 1);

  if (IntMask == I2C_INTR_ALL) {
    I2cWrite (I2C_START, I2C_TRANSAC_START | I2C_RS_MUL_TRIG);
  } else {
    I2cWrite (I2C_START, I2C_TRANSAC_START);
  }

  for (Timeout = 0; ; Timeout++) {
    Stat = MmioRead32 (MT7988_I2C2_BASE + I2C_INTR_STAT);
    if ((Stat & (I2C_TRANSAC_COMP | (IntMask & I2C_RS_TRANSFER))) != 0) {
      break;
    }

    if (Timeout >= I2C_TIMEOUT_US) {
      mLastStatus = Stat | BIT31;
      I2cWrite (I2C_INTR_MASK, 0);
      I2cInitHw ();
      DEBUG ((DEBUG_ERROR, "%a: 0x%02x timed out\n", __func__, Addr));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  I2cWrite (I2C_INTR_MASK, 0);
  mLastStatus = Stat;

  if ((Stat & (I2C_HS_NACKERR | I2C_ACKERR)) != 0) {
    I2cInitHw ();
    DEBUG ((DEBUG_INFO, "%a: 0x%02x NACK (0x%x)\n", __func__, Addr, Stat));
    return EFI_NO_RESPONSE;
  }

  if (ReadLength != 0) {
    InvalidateDataCacheRange (Rx, ReadLength);
    CopyMem (ReadBuffer, Rx, ReadLength);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MtkI2cWrite (
  IN UINT8        Addr,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  return I2cTransfer (Addr, Buffer, Length, NULL, 0);
}

EFI_STATUS
EFIAPI
MtkI2cWriteRead (
  IN  UINT8        Addr,
  IN  CONST UINT8  *WriteBuffer,
  IN  UINTN        WriteLength,
  OUT UINT8        *ReadBuffer,
  IN  UINTN        ReadLength
  )
{
  return I2cTransfer (Addr, WriteBuffer, WriteLength, ReadBuffer, ReadLength);
}

UINT32
EFIAPI
MtkI2cLastStatus (
  VOID
  )
{
  return mLastStatus;
}
