/** @file
  MediaTek MT7988 (Filogic 880) memory map.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MT7988_H_
#define MT7988_H_

//
// GICv3: GICD at 0x0C000000, GICR at 0x0C080000, GICC/GICH/GICV up to 0x0C422000
//
#define MT7988_GIC_BASE  0x0C000000
#define MT7988_GIC_SIZE  0x00500000

//
// On-chip peripherals: UART, MSDC, pinctrl, clocks, PCIe/USB controllers, ...
//
#define MT7988_PERIPH_BASE  0x10000000
#define MT7988_PERIPH_SIZE  0x10000000

//
// MediaTek UART extension, register index 9 (byte offset 0x24 at stride 4).
// Non-zero selects the high-speed baud generator that ATF leaves enabled.
//
#define MTK_UART_HIGHSPEED  0x24

//
// Clock gates, Linux drivers/clk/mediatek/clk-mt7988-{infracfg,topckgen}.c.
// Writing a bit to a CLR register ungates that clock.
//
#define MT7988_INFRACFG_BASE     0x10001000
#define MT7988_INFRA1_CG_CLR     0x44
#define MT7988_INFRA2_CG_CLR     0x54
#define MT7988_INFRA_AP_DMA_BCK  BIT21              // infra1
#define MT7988_INFRA_I2C_BCK     BIT1               // infra2

#define MT7988_TOPCKGEN_BASE        0x1001B000
#define MT7988_TOP_CLK_CFG4         0x40            // i2c_sel: mux [9:8], pdn bit 15
#define MT7988_TOP_CLK_CFG4_CLR     0x48
#define MT7988_TOP_CLK_CFG_UPDATE   0x1C0
#define MT7988_TOP_I2C_SEL_MASK     (0x3 << 8)
#define MT7988_TOP_I2C_SEL_NET1PLL  (0x1 << 8)      // net1pll_d5_d4, 125 MHz
#define MT7988_TOP_I2C_PDN          BIT15
#define MT7988_TOP_I2C_UPDATE       BIT17
#define MT7988_I2C_SEL_HZ           125000000

//
// GPIO / pinmux, ATF plat/mediatek/mt7988/drivers/gpio/mt7988_gpio.c:
// 32 pins per DIR/DOUT register, 8 pins (4 bits each) per MODE register,
// registers 0x10 apart, SET at +0x4 and CLR at +0x8.
//
#define MT7988_GPIO_BASE       0x1001F000
#define MT7988_GPIO_DIR(Pin)   (MT7988_GPIO_BASE + 0x000 + 0x10 * ((Pin) / 32))
#define MT7988_GPIO_DOUT(Pin)  (MT7988_GPIO_BASE + 0x100 + 0x10 * ((Pin) / 32))
#define MT7988_GPIO_MODE(Pin)  (MT7988_GPIO_BASE + 0x300 + 0x10 * ((Pin) / 8))
#define MT7988_GPIO_SET        0x4
#define MT7988_GPIO_CLR        0x8

//
// I2C2 (mediatek,mt7981-i2c) and its AP DMA channel, from the Linux DT
//
#define MT7988_I2C2_BASE      0x11005000
#define MT7988_I2C2_DMA_BASE  0x10217180

#endif // MT7988_H_
