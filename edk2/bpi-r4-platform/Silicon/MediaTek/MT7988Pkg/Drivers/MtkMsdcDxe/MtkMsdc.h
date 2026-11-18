/** @file
  MediaTek MSDC SD/MMC host controller registers.

  Derived from the ATF driver plat/mediatek/apsoc_common/drivers/mmc/mtk-sd.c
  (Copyright (c) 2019, MediaTek Inc., BSD-3-Clause).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MTK_MSDC_H_
#define MTK_MSDC_H_

//
// Register offsets from the MSDC base
//
#define MSDC_CFG         0x000
#define MSDC_IOCON       0x004
#define MSDC_PS          0x008
#define MSDC_INT         0x00C
#define MSDC_INTEN       0x010
#define MSDC_FIFOCS      0x014
#define MSDC_TXDATA      0x018
#define MSDC_RXDATA      0x01C
#define SDC_CFG          0x030
#define SDC_CMD          0x034
#define SDC_ARG          0x038
#define SDC_STS          0x03C
#define SDC_RESP(n)      (0x040 + (n) * 4)
#define SDC_BLK_NUM      0x050
#define MSDC_PATCH_BIT0  0x0B0
#define MSDC_PATCH_BIT1  0x0B4
#define MSDC_PATCH_BIT2  0x0B8
#define MSDC_PAD_TUNE0   0x0F0
#define MSDC_MAIN_VER    0x114
#define MSDC_ECO_VER     0x118
#define EMMC50_CFG0      0x208
#define SDC_FIFO_CFG     0x228

//
// Register offsets from the MSDC top base
//
#define EMMC_TOP_CONTROL  0x00
#define EMMC_TOP_CMD      0x04

// MSDC_CFG
#define MSDC_CFG_HS400_CK_MODE_EXT  BIT22
#define MSDC_CFG_CKMOD_EXT_M        0x00300000
#define MSDC_CFG_CKMOD_EXT_S        20
#define MSDC_CFG_CKDIV_EXT_M        0x000FFF00
#define MSDC_CFG_CKDIV_EXT_S        8
#define MSDC_CFG_CKSTB              BIT7
#define MSDC_CFG_PIO                BIT3
#define MSDC_CFG_RST                BIT2
#define MSDC_CFG_CKPDN              BIT1
#define MSDC_CFG_MODE               BIT0

// MSDC_IOCON
#define MSDC_IOCON_RSPL  BIT1

// MSDC_PS
#define MSDC_PS_DAT0  BIT16

// MSDC_INT / MSDC_INTEN
#define MSDC_INT_CMDRDY       BIT8
#define MSDC_INT_CMDTMO       BIT9
#define MSDC_INT_RSPCRCERR    BIT10
#define MSDC_INT_XFER_COMPL   BIT12
#define MSDC_INT_DATTMO       BIT14
#define MSDC_INT_DATCRCERR    BIT15

#define CMD_INTS_MASK   (MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO)
#define DATA_INTS_MASK  (MSDC_INT_XFER_COMPL | MSDC_INT_DATTMO | MSDC_INT_DATCRCERR)

// MSDC_FIFOCS
#define MSDC_FIFOCS_CLR      BIT31
#define MSDC_FIFOCS_TXCNT_M  0x00FF0000
#define MSDC_FIFOCS_TXCNT_S  16
#define MSDC_FIFOCS_RXCNT_M  0x000000FF
#define MSDC_FIFOCS_RXCNT_S  0

#define MSDC_FIFO_SIZE  128

// SDC_CFG
#define SDC_CFG_DTOC_M      0xFF000000
#define SDC_CFG_DTOC_S      24
#define SDC_CFG_SDIOIDE     BIT20
#define SDC_CFG_SDIO        BIT19
#define SDC_CFG_BUSWIDTH_M  0x00030000
#define SDC_CFG_BUSWIDTH_S  16

#define MSDC_BUS_1BITS  0x0
#define MSDC_BUS_4BITS  0x1
#define MSDC_BUS_8BITS  0x2

// SDC_CMD
#define SDC_CMD_BLK_LEN_M  0x0FFF0000
#define SDC_CMD_BLK_LEN_S  16
#define SDC_CMD_STOP       BIT14
#define SDC_CMD_WR         BIT13
#define SDC_CMD_DTYPE_M    0x00001800
#define SDC_CMD_DTYPE_S    11
#define SDC_CMD_RSPTYP_M   0x00000380
#define SDC_CMD_RSPTYP_S   7
#define SDC_CMD_CMD_M      0x0000003F
#define SDC_CMD_CMD_S      0

#define SDC_CMD_DTYPE_NONE    0
#define SDC_CMD_DTYPE_SINGLE  1
#define SDC_CMD_DTYPE_MULTI   2

#define SDC_CMD_RSPTYP_NONE  0
#define SDC_CMD_RSPTYP_R1    1    // also R5, R6, R7
#define SDC_CMD_RSPTYP_R2    2
#define SDC_CMD_RSPTYP_R3    3    // also R4, no CRC
#define SDC_CMD_RSPTYP_R1B   7

// SDC_STS
#define SDC_STS_CMDBUSY  BIT1

// PATCH_BIT0
#define MSDC_INT_DAT_LATCH_CK_SEL_M  0x00000380

// PATCH_BIT1
#define MSDC_PB1_BUSY_CHECK_SEL  BIT7
#define MSDC_PB1_STOP_DLY_M      0x00000F00
#define MSDC_PB1_STOP_DLY_S      8

// PATCH_BIT2
#define MSDC_PB2_CRCSTSENSEL_M   0xE0000000
#define MSDC_PB2_CRCSTSENSEL_S   29
#define MSDC_PB2_CFGCRCSTS       BIT28
#define MSDC_PB2_RESPSTSENSEL_M  0x00070000
#define MSDC_PB2_RESPSTSENSEL_S  16
#define MSDC_PB2_CFGRESP         BIT15
#define MSDC_PB2_RESPWAIT_M      0x0000000C
#define MSDC_PB2_RESPWAIT_S      2

// EMMC50_CFG0
#define EMMC50_CFG_CFCSTS_SEL  BIT4

// SDC_FIFO_CFG
#define SDC_FIFO_CFG_WRVALIDSEL  BIT24
#define SDC_FIFO_CFG_RDVALIDSEL  BIT25

// EMMC_TOP_CONTROL
#define PAD_DAT_RD_RXDLY_SEL  BIT13
#define DATA_K_VALUE_SEL      BIT14

// EMMC_TOP_CMD
#define PAD_CMD_RD_RXDLY_SEL  BIT11

#endif // MTK_MSDC_H_
