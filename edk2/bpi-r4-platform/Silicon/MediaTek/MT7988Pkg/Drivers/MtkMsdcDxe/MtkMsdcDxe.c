/** @file
  MediaTek MSDC SD/eMMC host controller driver, PIO mode.

  Produces EMBEDDED_MMC_HOST_PROTOCOL for EmbeddedPkg/Universal/MmcDxe.
  The controller programming is ported from the ATF driver that loads the
  FIP from the very same device (plat/mediatek/apsoc_common/drivers/mmc/mtk-sd.c,
  Copyright (c) 2019, MediaTek Inc., BSD-3-Clause), with the MT7988 options
  (12-bit divider, pad_tune0, async FIFO, data tune, busy check, stop clock
  fix) folded in.

  Clocks and pinmux are left as ATF BL2 configured them for the medium we
  were loaded from; SD and eMMC share MSDC0 on different pins. After a
  SPI-NAND boot BL2 leaves MSDC0 alone and SW3 routes the eMMC to it, so the
  eMMC pins are set up here.

  The MSDC encodes block length and count into the command register when a
  command is issued, but the MmcHost protocol only reveals the transfer
  length in ReadBlockData/WriteBlockData. Data commands are therefore
  latched by SendCommand and issued once the length is known. MmcDxe never
  asks for the response of a data command before transferring its data.

  CMD6 and CMD8 differ between SD and eMMC. MmcDxe probes with CMD1 first,
  which only an eMMC answers, so a successful CMD1 selects eMMC semantics
  until the next CMD0. DDR timings are refused, MmcDxe then falls back to
  SDR.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MT7988BootMediumLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/MmcHost.h>

#include "MtkMsdc.h"

#define MSDC_INIT_CLK_HZ      400000
#define MSDC_BLOCK_SIZE       512

#define MSDC_TIMEOUT_US       1000000   // reset, FIFO clear, clock stable, command
#define MSDC_CMD_BUSY_US      20000
#define MSDC_DATA_IDLE_US     1000000   // no FIFO progress for this long is fatal

#define MSDC_DATA_TIMEOUT_NS    100000000ULL
#define MSDC_DATA_TIMEOUT_CLKS  (3 * 1048576ULL)

typedef struct {
  UINTN      Base;
  UINTN      TopBase;
  UINT32     SrcClkHz;
  UINT32     SClkHz;            // actual bus clock
  UINT32     Response[4];
  BOOLEAN    AppCmd;            // the previous command was CMD55
  BOOLEAN    IsEmmc;            // CMD1 succeeded since the last CMD0
  BOOLEAN    CmdPending;        // a data command waits for its transfer length
  MMC_CMD    PendingCmd;
  UINT32     PendingArg;
} MSDC_HOST;

STATIC MSDC_HOST  mHost;

STATIC
EFI_STATUS
MsdcPoll (
  IN UINTN   Offset,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINTN   TimeoutUs
  )
{
  while ((MmioRead32 (mHost.Base + Offset) & Mask) != Value) {
    if (TimeoutUs-- == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

/**
  Wait until any bit of Mask is raised in MSDC_INT.
**/
STATIC
EFI_STATUS
MsdcWaitInt (
  IN  UINT32  Mask,
  IN  UINTN   TimeoutUs,
  OUT UINT32  *Status
  )
{
  while (((*Status = MmioRead32 (mHost.Base + MSDC_INT)) & Mask) == 0) {
    if (TimeoutUs-- == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

STATIC
VOID
MsdcResetHw (
  VOID
  )
{
  MmioOr32 (mHost.Base + MSDC_CFG, MSDC_CFG_RST);
  if (EFI_ERROR (MsdcPoll (MSDC_CFG, MSDC_CFG_RST, 0, MSDC_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "MSDC: reset did not complete\n"));
  }
}

STATIC
VOID
MsdcFifoClear (
  VOID
  )
{
  MmioOr32 (mHost.Base + MSDC_FIFOCS, MSDC_FIFOCS_CLR);
  if (EFI_ERROR (MsdcPoll (MSDC_FIFOCS, MSDC_FIFOCS_CLR, 0, MSDC_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "MSDC: FIFO clear did not complete\n"));
  }
}

STATIC
UINT32
MsdcFifoRxBytes (
  VOID
  )
{
  return (MmioRead32 (mHost.Base + MSDC_FIFOCS) & MSDC_FIFOCS_RXCNT_M) >> MSDC_FIFOCS_RXCNT_S;
}

STATIC
UINT32
MsdcFifoTxBytes (
  VOID
  )
{
  return (MmioRead32 (mHost.Base + MSDC_FIFOCS) & MSDC_FIFOCS_TXCNT_M) >> MSDC_FIFOCS_TXCNT_S;
}

/**
  Program the data timeout, in units of 1048576 bus clocks.
**/
STATIC
VOID
MsdcSetTimeout (
  VOID
  )
{
  UINT64  ClkNs;
  UINT64  Timeout;

  Timeout = 0;

  if (mHost.SClkHz != 0) {
    ClkNs   = 1000000000ULL / mHost.SClkHz;
    Timeout = (MSDC_DATA_TIMEOUT_NS + ClkNs - 1) / ClkNs + MSDC_DATA_TIMEOUT_CLKS;
    Timeout = (Timeout + (1 << 20) - 1) >> 20;
    Timeout = Timeout > 1 ? Timeout - 1 : 0;
    Timeout = MIN (Timeout, 255);
  }

  MmioAndThenOr32 (
    mHost.Base + SDC_CFG,
    ~(UINT32)SDC_CFG_DTOC_M,
    (UINT32)Timeout << SDC_CFG_DTOC_S
    );
}

STATIC
VOID
MsdcSetClock (
  IN UINT32  Hz
  )
{
  UINT32  Mode;
  UINT32  Div;
  UINT32  SClk;

  MmioAnd32 (mHost.Base + MSDC_CFG, ~(UINT32)MSDC_CFG_HS400_CK_MODE_EXT);

  if (Hz >= mHost.SrcClkHz) {
    Mode = 1;                           // no divisor
    Div  = 0;
    SClk = mHost.SrcClkHz;
  } else {
    Mode = 0;                           // use divisor
    if (Hz >= (mHost.SrcClkHz >> 1)) {
      Div  = 0;                         // means 1/2
      SClk = mHost.SrcClkHz >> 1;
    } else {
      Div  = (mHost.SrcClkHz + (Hz << 2) - 1) / (Hz << 2);
      Div  = MIN (Div, MSDC_CFG_CKDIV_EXT_M >> MSDC_CFG_CKDIV_EXT_S);
      SClk = (mHost.SrcClkHz >> 2) / Div;
    }
  }

  MmioAnd32 (mHost.Base + MSDC_CFG, ~(UINT32)MSDC_CFG_CKPDN);
  MmioAndThenOr32 (
    mHost.Base + MSDC_CFG,
    ~(UINT32)(MSDC_CFG_CKMOD_EXT_M | MSDC_CFG_CKDIV_EXT_M),
    (Mode << MSDC_CFG_CKMOD_EXT_S) | (Div << MSDC_CFG_CKDIV_EXT_S)
    );

  if (EFI_ERROR (MsdcPoll (MSDC_CFG, MSDC_CFG_CKSTB, MSDC_CFG_CKSTB, MSDC_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "MSDC: bus clock did not stabilise\n"));
  }

  mHost.SClkHz = SClk;
  MsdcSetTimeout ();

  DEBUG ((DEBUG_INFO, "MSDC: bus clock %u Hz (requested %u Hz)\n", SClk, Hz));
}

STATIC
VOID
MsdcSetBusWidth (
  IN UINT32  Width
  )
{
  UINT32  Val;

  switch (Width) {
    case 8:
      Val = MSDC_BUS_8BITS;
      break;
    case 4:
      Val = MSDC_BUS_4BITS;
      break;
    default:
      Val = MSDC_BUS_1BITS;
      break;
  }

  MmioAndThenOr32 (
    mHost.Base + SDC_CFG,
    ~(UINT32)SDC_CFG_BUSWIDTH_M,
    Val << SDC_CFG_BUSWIDTH_S
    );
}

STATIC
EFI_STATUS
MsdcInitHw (
  VOID
  )
{
  DEBUG ((
    DEBUG_INFO,
    "MSDC: controller ver %08x, eco %u\n",
    MmioRead32 (mHost.Base + MSDC_MAIN_VER),
    MmioRead32 (mHost.Base + MSDC_ECO_VER)
    ));

  // MMC/SD mode, PIO
  MmioOr32 (mHost.Base + MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_PIO);

  MmioOr32 (mHost.Base + MSDC_CFG, MSDC_CFG_RST);
  if (EFI_ERROR (MsdcPoll (MSDC_CFG, MSDC_CFG_RST, 0, MSDC_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "MSDC: controller does not leave reset, is its clock gated?\n"));
    return EFI_DEVICE_ERROR;
  }

  // Clear all interrupts, enable command and data status bits
  MmioWrite32 (mHost.Base + MSDC_INT, MmioRead32 (mHost.Base + MSDC_INT));
  MmioWrite32 (mHost.Base + MSDC_INTEN, DATA_INTS_MASK | CMD_INTS_MASK);

  MmioWrite32 (mHost.Base + MSDC_PAD_TUNE0, 0);
  MmioWrite32 (mHost.TopBase + EMMC_TOP_CONTROL, 0);
  MmioWrite32 (mHost.TopBase + EMMC_TOP_CMD, 0);
  MmioWrite32 (mHost.Base + MSDC_IOCON, 0);
  MmioAnd32 (mHost.Base + MSDC_IOCON, ~(UINT32)MSDC_IOCON_RSPL);

  MmioWrite32 (mHost.Base + MSDC_PATCH_BIT0, 0x403C0046);
  MmioWrite32 (mHost.Base + MSDC_PATCH_BIT1, 0xFFFF4089);

  // Stop clock fix
  MmioAnd32 (mHost.Base + SDC_FIFO_CFG, ~(UINT32)(SDC_FIFO_CFG_WRVALIDSEL | SDC_FIFO_CFG_RDVALIDSEL));
  MmioAndThenOr32 (
    mHost.Base + MSDC_PATCH_BIT1,
    ~(UINT32)MSDC_PB1_STOP_DLY_M,
    3 << MSDC_PB1_STOP_DLY_S
    );

  // Busy check
  MmioAnd32 (mHost.Base + MSDC_PATCH_BIT1, ~(UINT32)MSDC_PB1_BUSY_CHECK_SEL);

  MmioOr32 (mHost.Base + EMMC50_CFG0, EMMC50_CFG_CFCSTS_SEL);

  // Async FIFO, which avoids having to tune the internal delay
  MmioAndThenOr32 (
    mHost.Base + MSDC_PATCH_BIT2,
    ~(UINT32)MSDC_PB2_RESPWAIT_M,
    3 << MSDC_PB2_RESPWAIT_S
    );
  MmioAndThenOr32 (
    mHost.Base + MSDC_PATCH_BIT2,
    ~(UINT32)MSDC_PB2_RESPSTSENSEL_M,
    2 << MSDC_PB2_RESPSTSENSEL_S
    );
  MmioAndThenOr32 (
    mHost.Base + MSDC_PATCH_BIT2,
    ~(UINT32)MSDC_PB2_CRCSTSENSEL_M,
    2 << MSDC_PB2_CRCSTSENSEL_S
    );
  MmioAnd32 (mHost.Base + MSDC_PATCH_BIT2, ~(UINT32)(MSDC_PB2_CFGRESP | MSDC_PB2_CFGCRCSTS));

  // Data tune through the top block
  MmioOr32 (mHost.TopBase + EMMC_TOP_CONTROL, PAD_DAT_RD_RXDLY_SEL);
  MmioAnd32 (mHost.TopBase + EMMC_TOP_CONTROL, ~(UINT32)DATA_K_VALUE_SEL);
  MmioOr32 (mHost.TopBase + EMMC_TOP_CMD, PAD_CMD_RD_RXDLY_SEL);
  MmioAnd32 (mHost.Base + MSDC_PATCH_BIT0, ~(UINT32)MSDC_INT_DAT_LATCH_CK_SEL_M);

  // SDIO mode must be on for CMD5 to work; no SDIO interrupt detection
  MmioOr32 (mHost.Base + SDC_CFG, SDC_CFG_SDIO);
  MmioAnd32 (mHost.Base + SDC_CFG, ~(UINT32)SDC_CFG_SDIOIDE);

  MmioAndThenOr32 (mHost.Base + SDC_CFG, ~(UINT32)SDC_CFG_DTOC_M, 3 << SDC_CFG_DTOC_S);

  // Identification runs 1-bit at <= 400 kHz
  MsdcSetBusWidth (1);
  MsdcSetClock (MSDC_INIT_CLK_HZ);

  mHost.AppCmd     = FALSE;
  mHost.IsEmmc     = FALSE;
  mHost.CmdPending = FALSE;

  return EFI_SUCCESS;
}

/**
  Commands that move data on the DAT lines.
  SD:   CMD6 is SWITCH_FUNC (ACMD6 is SET_BUS_WIDTH, no data),
        CMD8 is SEND_IF_COND (no data).
  eMMC: CMD6 is SWITCH (R1b, no data), CMD8 is SEND_EXT_CSD.
**/
STATIC
BOOLEAN
MsdcIsDataCommand (
  IN MMC_CMD  Cmd
  )
{
  switch (MMC_GET_INDX (Cmd)) {
    case 17:
    case 18:
    case 24:
    case 25:
      return TRUE;
    case 6:
      return !mHost.AppCmd && !mHost.IsEmmc;
    case 8:
      return mHost.IsEmmc;
    case 51:
      return mHost.AppCmd;
    default:
      return FALSE;
  }
}

STATIC
UINT32
MsdcResponseType (
  IN MMC_CMD  Cmd
  )
{
  if ((Cmd & MMC_CMD_WAIT_RESPONSE) == 0) {
    return SDC_CMD_RSPTYP_NONE;
  }

  if ((Cmd & MMC_CMD_LONG_RESPONSE) != 0) {
    return SDC_CMD_RSPTYP_R2;
  }

  if ((Cmd & MMC_CMD_NO_CRC_RESPONSE) != 0) {
    return SDC_CMD_RSPTYP_R3;
  }

  switch (MMC_GET_INDX (Cmd)) {
    case 7:
    case 12:
      return SDC_CMD_RSPTYP_R1B;
    case 6:
      return mHost.IsEmmc ? SDC_CMD_RSPTYP_R1B : SDC_CMD_RSPTYP_R1;
    default:
      return SDC_CMD_RSPTYP_R1;
  }
}

/**
  Issue a command, optionally announcing a data phase of Blocks x BlockSize.
**/
STATIC
EFI_STATUS
MsdcIssueCommand (
  IN MMC_CMD  Cmd,
  IN UINT32   Argument,
  IN UINT32   Blocks,
  IN UINT32   BlockSize,
  IN BOOLEAN  Write
  )
{
  UINT32  Index;
  UINT32  RspType;
  UINT32  DType;
  UINT32  RawCmd;
  UINT32  Status;

  Index   = MMC_GET_INDX (Cmd);
  RspType = MsdcResponseType (Cmd);

  if (EFI_ERROR (MsdcPoll (SDC_STS, SDC_STS_CMDBUSY, 0, MSDC_CMD_BUSY_US))) {
    DEBUG ((DEBUG_ERROR, "MSDC: CMD line busy before CMD%u\n", Index));
    MsdcResetHw ();
    return EFI_DEVICE_ERROR;
  }

  MsdcFifoClear ();

  DType = SDC_CMD_DTYPE_NONE;
  if (Blocks != 0) {
    DType = (Blocks > 1 || Index == 18 || Index == 25) ? SDC_CMD_DTYPE_MULTI
                                                       : SDC_CMD_DTYPE_SINGLE;
  }

  RawCmd = ((Index << SDC_CMD_CMD_S) & SDC_CMD_CMD_M) |
           ((RspType << SDC_CMD_RSPTYP_S) & SDC_CMD_RSPTYP_M) |
           ((BlockSize << SDC_CMD_BLK_LEN_S) & SDC_CMD_BLK_LEN_M) |
           ((DType << SDC_CMD_DTYPE_S) & SDC_CMD_DTYPE_M);

  if (Write) {
    RawCmd |= SDC_CMD_WR;
  }

  if (Index == 12) {
    RawCmd |= SDC_CMD_STOP;
  }

  // Clear stale status before starting, so no data error can be missed later
  MmioWrite32 (mHost.Base + MSDC_INT, CMD_INTS_MASK | DATA_INTS_MASK);
  MmioWrite32 (mHost.Base + SDC_BLK_NUM, Blocks);
  MmioWrite32 (mHost.Base + SDC_ARG, Argument);
  MmioWrite32 (mHost.Base + SDC_CMD, RawCmd);

  if (EFI_ERROR (MsdcWaitInt (CMD_INTS_MASK, MSDC_TIMEOUT_US, &Status))) {
    DEBUG ((DEBUG_ERROR, "MSDC: no completion for CMD%u arg 0x%x\n", Index, Argument));
    MsdcResetHw ();
    return EFI_TIMEOUT;
  }

  MmioWrite32 (mHost.Base + MSDC_INT, Status & CMD_INTS_MASK);

  if (RspType == SDC_CMD_RSPTYP_R2) {
    mHost.Response[0] = MmioRead32 (mHost.Base + SDC_RESP (0));
    mHost.Response[1] = MmioRead32 (mHost.Base + SDC_RESP (1));
    mHost.Response[2] = MmioRead32 (mHost.Base + SDC_RESP (2));
    mHost.Response[3] = MmioRead32 (mHost.Base + SDC_RESP (3));
  } else if (RspType != SDC_CMD_RSPTYP_NONE) {
    mHost.Response[0] = MmioRead32 (mHost.Base + SDC_RESP (0));
  }

  if ((Status & MSDC_INT_CMDRDY) == 0) {
    MsdcResetHw ();
    if ((Status & MSDC_INT_CMDTMO) != 0) {
      // Expected while probing: CMD1/CMD5 on an SD card, CMD8 on SD v1
      DEBUG ((DEBUG_BLKIO, "MSDC: CMD%u arg 0x%x timed out\n", Index, Argument));
      return EFI_TIMEOUT;
    }

    DEBUG ((DEBUG_ERROR, "MSDC: CMD%u arg 0x%x response CRC error\n", Index, Argument));
    return EFI_CRC_ERROR;
  }

  if (RspType == SDC_CMD_RSPTYP_R1B) {
    if (EFI_ERROR (MsdcPoll (MSDC_PS, MSDC_PS_DAT0, MSDC_PS_DAT0, MSDC_TIMEOUT_US))) {
      DEBUG ((DEBUG_ERROR, "MSDC: card stuck busy after CMD%u\n", Index));
      MsdcResetHw ();
      return EFI_TIMEOUT;
    }
  }

  return EFI_SUCCESS;
}

/**
  Issue the latched data command for a transfer of Length bytes.
**/
STATIC
EFI_STATUS
MsdcStartTransfer (
  IN UINTN    Length,
  IN BOOLEAN  Write
  )
{
  UINT32  BlockSize;

  if (!mHost.CmdPending) {
    DEBUG ((DEBUG_ERROR, "MSDC: data transfer without a data command\n"));
    return EFI_DEVICE_ERROR;
  }

  mHost.CmdPending = FALSE;

  BlockSize = (Length < MSDC_BLOCK_SIZE) ? (UINT32)Length : MSDC_BLOCK_SIZE;
  if ((Length == 0) || ((Length % BlockSize) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return MsdcIssueCommand (
           mHost.PendingCmd,
           mHost.PendingArg,
           (UINT32)(Length / BlockSize),
           BlockSize,
           Write
           );
}

/**
  Check the data status bits; returns an error for CRC errors and timeouts.
**/
STATIC
EFI_STATUS
MsdcDataStatus (
  OUT BOOLEAN  *Complete
  )
{
  UINT32  Status;

  Status = MmioRead32 (mHost.Base + MSDC_INT) & DATA_INTS_MASK;
  MmioWrite32 (mHost.Base + MSDC_INT, Status);

  if ((Status & MSDC_INT_DATCRCERR) != 0) {
    DEBUG ((DEBUG_ERROR, "MSDC: data CRC error\n"));
    MsdcResetHw ();
    return EFI_CRC_ERROR;
  }

  if ((Status & MSDC_INT_DATTMO) != 0) {
    DEBUG ((DEBUG_ERROR, "MSDC: data timeout\n"));
    MsdcResetHw ();
    return EFI_TIMEOUT;
  }

  if ((Status & MSDC_INT_XFER_COMPL) != 0) {
    *Complete = TRUE;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
MsdcFifoRead (
  OUT UINT8   *Buffer,
  IN  UINT32  Size
  )
{
  if ((((UINTN)Buffer | Size) & 3) == 0) {
    for ( ; Size > 0; Size -= 4, Buffer += 4) {
      *(UINT32 *)Buffer = MmioRead32 (mHost.Base + MSDC_RXDATA);
    }
  } else {
    for ( ; Size > 0; Size--) {
      *Buffer++ = MmioRead8 (mHost.Base + MSDC_RXDATA);
    }
  }
}

STATIC
VOID
MsdcFifoWrite (
  IN CONST UINT8  *Buffer,
  IN UINT32       Size
  )
{
  if ((((UINTN)Buffer | Size) & 3) == 0) {
    for ( ; Size > 0; Size -= 4, Buffer += 4) {
      MmioWrite32 (mHost.Base + MSDC_TXDATA, *(CONST UINT32 *)Buffer);
    }
  } else {
    for ( ; Size > 0; Size--) {
      MmioWrite8 (mHost.Base + MSDC_TXDATA, *Buffer++);
    }
  }
}

/**
  Drain Length bytes from the FIFO, then wait for the transfer to complete.
**/
STATIC
EFI_STATUS
MsdcPioRead (
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Complete;
  UINTN       IdleUs;
  UINT32      Chunk;

  Complete = FALSE;
  IdleUs   = 0;

  while ((Length > 0) || !Complete) {
    Status = MsdcDataStatus (&Complete);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Length > 0) {
      Chunk = (UINT32)MIN (Length, MSDC_FIFO_SIZE);
      if (MsdcFifoRxBytes () >= Chunk) {
        MsdcFifoRead (Buffer, Chunk);
        Buffer += Chunk;
        Length -= Chunk;
        IdleUs  = 0;
        continue;
      }

      // XFER_COMPL is raised only once all data sits in the FIFO
      if (Complete) {
        DEBUG ((DEBUG_ERROR, "MSDC: read ended %u bytes short\n", Length));
        MsdcResetHw ();
        return EFI_DEVICE_ERROR;
      }
    }

    if (IdleUs++ >= MSDC_DATA_IDLE_US) {
      DEBUG ((DEBUG_ERROR, "MSDC: read stalled with %u bytes left\n", Length));
      MsdcResetHw ();
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

/**
  Feed Length bytes into the FIFO, then wait for the card to accept them.
**/
STATIC
EFI_STATUS
MsdcPioWrite (
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Complete;
  UINTN       IdleUs;
  UINT32      Chunk;

  Complete = FALSE;
  IdleUs   = 0;

  while ((Length > 0) || !Complete) {
    Status = MsdcDataStatus (&Complete);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Length > 0) {
      if (Complete) {
        DEBUG ((DEBUG_ERROR, "MSDC: write ended %u bytes short\n", Length));
        MsdcResetHw ();
        return EFI_DEVICE_ERROR;
      }

      Chunk = (UINT32)MIN (Length, MSDC_FIFO_SIZE);
      if ((MSDC_FIFO_SIZE - MsdcFifoTxBytes ()) >= Chunk) {
        MsdcFifoWrite (Buffer, Chunk);
        Buffer += Chunk;
        Length -= Chunk;
        IdleUs  = 0;
        continue;
      }
    }

    if (IdleUs++ >= MSDC_DATA_IDLE_US) {
      DEBUG ((DEBUG_ERROR, "MSDC: write stalled with %u bytes left\n", Length));
      MsdcResetHw ();
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
MsdcIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  // We were loaded from this card; the slot's CD line is not wired to MSDC_PS.
  return TRUE;
}

STATIC
BOOLEAN
EFIAPI
MsdcIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
MsdcBuildDevicePath (
  IN  EFI_MMC_HOST_PROTOCOL     *This,
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  Node = CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyGuid (&((VENDOR_DEVICE_PATH *)Node)->Guid, &gMtkMsdcDevicePathGuid);
  *DevicePath = Node;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MsdcNotifyState (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_STATE              State
  )
{
  if (State == MmcHwInitializationState) {
    return MsdcInitHw ();
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MsdcSendCommand (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_CMD                Cmd,
  IN UINT32                 Argument
  )
{
  EFI_STATUS  Status;

  if (MsdcIsDataCommand (Cmd)) {
    mHost.CmdPending = TRUE;
    mHost.PendingCmd = Cmd;
    mHost.PendingArg = Argument;
    mHost.AppCmd     = FALSE;
    return EFI_SUCCESS;
  }

  if (mHost.CmdPending) {
    DEBUG ((DEBUG_WARN, "MSDC: dropping CMD%u, its data was never requested\n", MMC_GET_INDX (mHost.PendingCmd)));
    mHost.CmdPending = FALSE;
  }

  Status       = MsdcIssueCommand (Cmd, Argument, 0, 0, FALSE);
  mHost.AppCmd = (MMC_GET_INDX (Cmd) == 55) && !EFI_ERROR (Status);

  if (MMC_GET_INDX (Cmd) == 0) {
    mHost.IsEmmc = FALSE;
  } else if ((MMC_GET_INDX (Cmd) == 1) && !EFI_ERROR (Status)) {
    mHost.IsEmmc = TRUE;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MsdcReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_RESPONSE_TYPE      Type,
  IN UINT32                 *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Type == MMC_RESPONSE_TYPE_R2) {
    CopyMem (Buffer, mHost.Response, sizeof (mHost.Response));
  } else {
    Buffer[0] = mHost.Response[0];
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MsdcReadBlockData (
  IN  EFI_MMC_HOST_PROTOCOL  *This,
  IN  EFI_LBA                Lba,
  IN  UINTN                  Length,
  OUT UINT32                 *Buffer
  )
{
  EFI_STATUS  Status;

  Status = MsdcStartTransfer (Length, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MsdcPioRead ((UINT8 *)Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
MsdcWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                Lba,
  IN UINTN                  Length,
  IN UINT32                 *Buffer
  )
{
  EFI_STATUS  Status;

  Status = MsdcStartTransfer (Length, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MsdcPioWrite ((CONST UINT8 *)Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
MsdcSetIos (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN UINT32                 BusClockFreq,
  IN UINT32                 BusWidth,
  IN UINT32                 TimingMode
  )
{
  if ((TimingMode == EMMCHS52DDR1V2) || (TimingMode == EMMCHS52DDR1V8)) {
    return EFI_UNSUPPORTED;
  }

  if (BusWidth != 0) {
    MsdcSetBusWidth (BusWidth);
  }

  if (BusClockFreq != 0) {
    MsdcSetClock (MIN (BusClockFreq, FixedPcdGet32 (PcdMsdcMaxBusClockHz)));
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
MsdcIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return TRUE;
}

STATIC EFI_MMC_HOST_PROTOCOL  mMmcHost = {
  MMC_HOST_PROTOCOL_REVISION,
  MsdcIsCardPresent,
  MsdcIsReadOnly,
  MsdcBuildDevicePath,
  MsdcNotifyState,
  MsdcSendCommand,
  MsdcReceiveResponse,
  MsdcReadBlockData,
  MsdcWriteBlockData,
  MsdcSetIos,
  MsdcIsMultiBlock
};

EFI_STATUS
EFIAPI
MtkMsdcDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  mHost.Base     = (UINTN)FixedPcdGet64 (PcdMsdcBase);
  mHost.TopBase  = (UINTN)FixedPcdGet64 (PcdMsdcTopBase);
  mHost.SrcClkHz = FixedPcdGet32 (PcdMsdcSourceClockHz);

  if (Mt7988GetBootMedium () == Mt7988BootMediumSpiNand) {
    Mt7988MsdcSetupEmmcPins ();
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEmbeddedMmcHostProtocolGuid,
                &mMmcHost,
                NULL
                );
}
