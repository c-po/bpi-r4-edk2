/** @file
  Bring up the MT7988 xHCI controller at 0x11200000 (the BPI-R4 USB-A port)
  and register it as a non-discoverable xHCI device, so the generic
  MdeModulePkg XhciDxe/UsbBusDxe stack can drive it.

  The controller has port power control (HCCPARAMS1.PPC), so its root ports
  come out of the xHCI reset unpowered, and MdeModulePkg XhciDxe ignores
  EfiUsbPortPower. Port power is therefore switched on here, once XhciDxe has
  reset and started the controller (it then installs EFI_USB2_HC_PROTOCOL).

  The IPPC and T-PHY programming is ported from U-Boot
  drivers/usb/host/xhci-mtk.c and drivers/phy/phy-mtk-tphy.c (generic
  tphy v2); only register offsets and values are reused. Clocks and PHY
  addresses are those of the Linux DT node usb@11200000 and its phys
  t-phy@11c50000/usb-phy@11c50000 (USB2) and usb-phy@11c50700 (USB3).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Usb2HostController.h>
#include <MT7988.h>

#define XHCI_BASE        0x11200000
#define XHCI_SIZE        0x2E00
#define IPPC_BASE        0x11203E00
#define U2PHY_COM        (0x11C50000 + 0x300)
#define U3PHY_BASE       0x11C50700
#define U3PHY_SPLLC      (U3PHY_BASE + 0x000)
#define U3PHY_PHYD       (U3PHY_BASE + 0x200)
#define U3PHY_PHYA       (U3PHY_BASE + 0x400)

//
// Clocks, Linux drivers/clk/mediatek/clk-mt7988-{topckgen,infracfg}.c.
// The parents are those Linux reports on a running board.
//
#define TOP_CLK_CFG6       0x060      // usb_sys_p1_sel [0], gate 7; usb_xhci_p1_sel [16], gate 23
#define TOP_CLK_CFG7       0x070      // usb_frmcnt_p1_sel [0], gate 7
#define TOP_CLK_CFG8       0x080      // usb_phy_sel [16], gate 23
#define TOP_CLK_UPDATE     0x1C0
#define TOP_CLK_UPDATE1    0x1C4
#define INFRA3_CG_CLR      0x64
#define INFRA3_USB_GATES   (BIT1 | BIT3 | BIT5 | BIT7 | BIT11 | BIT13 | BIT15)

// IPPC
#define IPPC_IP_PW_CTRL0   0x00
#define CTRL0_IP_SW_RST    BIT0
#define IPPC_IP_PW_CTRL1   0x04
#define CTRL1_IP_HOST_PDN  BIT0
#define IPPC_IP_PW_STS1    0x10
#define STS1_U3_MAC_RST    BIT16
#define STS1_XHCI_RST      BIT11
#define STS1_SYS125_RST    BIT10
#define STS1_REF_RST       BIT8
#define STS1_SYSPLL_STABLE BIT0
#define IPPC_IP_XHCI_CAP   0x24
#define IPPC_U3_CTRL(p)    (0x30 + (p) * 0x08)
#define IPPC_U2_CTRL(p)    (0x50 + (p) * 0x08)
#define CTRL_PORT_HOST_SEL BIT2
#define CTRL_PORT_PDN      BIT1
#define CTRL_PORT_DIS      BIT0

// T-PHY USB2 (com bank)
#define U3P_USBPHYACR0           0x000
#define PA0_RG_USB20_INTR_EN     BIT5
#define U3P_USBPHYACR5           0x014
#define PA5_RG_U2_HSTX_SRCTRL    (0x7 << 12)
#define PA5_RG_U2_HS_100U_U3_EN  BIT11
#define U3P_USBPHYACR6           0x018
#define PA6_RG_U2_BC11_SW_EN     BIT23
#define PA6_RG_U2_OTG_VBUSCMP_EN BIT20
#define PA6_RG_U2_SQTH           0xF
#define U3P_U2PHYACR4            0x020
#define P2C_U2_GPIO_CTR_MSK      (BIT9 | BIT8)
#define U3P_U2PHYDTM0            0x068
#define P2C_FORCE_UART_EN        BIT26
#define P2C_FORCE_DATAIN         BIT23
#define P2C_FORCE_DM_PULLDOWN    BIT21
#define P2C_FORCE_DP_PULLDOWN    BIT20
#define P2C_FORCE_XCVRSEL        BIT19
#define P2C_FORCE_SUSPENDM       BIT18
#define P2C_FORCE_TERMSEL        BIT17
#define P2C_RG_DATAIN            (0xF << 10)
#define P2C_RG_DMPULLDOWN        BIT7
#define P2C_RG_DPPULLDOWN        BIT6
#define P2C_RG_XCVRSEL           (0x3 << 4)
#define P2C_RG_TERMSEL           BIT2
#define P2C_DTM0_PART_MASK       (P2C_FORCE_DATAIN | P2C_FORCE_DM_PULLDOWN | P2C_FORCE_DP_PULLDOWN | \
                                  P2C_FORCE_XCVRSEL | P2C_FORCE_TERMSEL | P2C_RG_DMPULLDOWN | \
                                  P2C_RG_DPPULLDOWN | P2C_RG_TERMSEL)
#define U3P_U2PHYDTM1            0x06C
#define P2C_RG_UART_EN           BIT16
#define P2C_RG_VBUSVALID         BIT5
#define P2C_RG_SESSEND           BIT4
#define P2C_RG_AVALID            BIT2

// T-PHY USB3
#define U3P_SPLLC_XTALCTL3       0x018
#define XC3_RG_U3_XTAL_RX_PWD    BIT9
#define XC3_RG_U3_FRC_XTAL_RX_PWD  BIT8
#define U3P_U3_PHYA_REG6         0x018
#define U3P_U3_PHYA_REG9         0x024
#define U3P_U3_PHYA_DA_REG0      0x100
#define U3P_U3_PHYD_LFPS1        0x00C
#define U3P_U3_PHYD_CDR1         0x05C
#define U3P_U3_PHYD_RXDET1       0x128
#define U3P_U3_PHYD_RXDET2       0x12C

#define IPPC_TIMEOUT_US          20000

// xHCI capability / port registers
#define XHC_CAPLENGTH            0x00
#define XHC_HCSPARAMS1           0x04
#define XHC_PORTSC(n)            (0x400 + 0x10 * (n))
#define XHC_PORTSC_PP            BIT9
// Read-only and read/write-sticky bits; writing them back changes nothing,
// unlike the write-1-to-clear change bits and PED (Linux
// xhci_port_state_to_neutral)
#define XHC_PORTSC_NEUTRAL       0x4E00FFE9

STATIC VOID  *mUsb2HcRegistration;

STATIC
VOID
ClrSet (
  IN UINTN   Address,
  IN UINT32  Clear,
  IN UINT32  Set
  )
{
  MmioAndThenOr32 (Address, ~Clear, Set);
}

STATIC
VOID
UsbClocksEnable (
  VOID
  )
{
  UINTN  Top;

  Top = MT7988_TOPCKGEN_BASE;

  // usb_sys_p1_sel and usb_xhci_p1_sel: net1pll_d5_d4 (125 MHz), ungated
  ClrSet (Top + TOP_CLK_CFG6, BIT0 | BIT7 | BIT16 | BIT23, BIT0 | BIT16);
  // usb_frmcnt_p1_sel: mmpll_d3_d5 (48 MHz), ungated
  ClrSet (Top + TOP_CLK_CFG7, BIT0 | BIT7, BIT0);
  // usb_phy_sel: mpll_d8_d2 (26 MHz), ungated
  ClrSet (Top + TOP_CLK_CFG8, BIT16 | BIT23, BIT16);
  MmioWrite32 (Top + TOP_CLK_UPDATE, BIT24 | BIT26 | BIT28);
  MmioWrite32 (Top + TOP_CLK_UPDATE1, BIT3);

  MmioWrite32 (MT7988_INFRACFG_BASE + INFRA3_CG_CLR, INFRA3_USB_GATES);
  MicroSecondDelay (50);
}

STATIC
VOID
U2PhyInit (
  VOID
  )
{
  // u2_phy_instance_init: USB function, USB PLL on
  ClrSet (U2PHY_COM + U3P_U2PHYDTM0, P2C_FORCE_UART_EN | P2C_FORCE_SUSPENDM | P2C_RG_XCVRSEL | P2C_RG_DATAIN, 1 << 4);
  ClrSet (U2PHY_COM + U3P_U2PHYDTM1, P2C_RG_UART_EN, 0);
  ClrSet (U2PHY_COM + U3P_USBPHYACR0, 0, PA0_RG_USB20_INTR_EN);
  ClrSet (U2PHY_COM + U3P_USBPHYACR5, PA5_RG_U2_HS_100U_U3_EN, 0);
  ClrSet (U2PHY_COM + U3P_U2PHYACR4, P2C_U2_GPIO_CTR_MSK, 0);
  ClrSet (U2PHY_COM + U3P_USBPHYACR6, PA6_RG_U2_BC11_SW_EN | PA6_RG_U2_SQTH, 2);
  ClrSet (U2PHY_COM + U3P_USBPHYACR5, PA5_RG_U2_HSTX_SRCTRL, 4 << 12);

  // u2_phy_instance_power_on
  ClrSet (U2PHY_COM + U3P_U2PHYDTM0, P2C_RG_XCVRSEL | P2C_RG_DATAIN | P2C_DTM0_PART_MASK, 0);
  ClrSet (U2PHY_COM + U3P_USBPHYACR6, 0, PA6_RG_U2_OTG_VBUSCMP_EN);
  ClrSet (U2PHY_COM + U3P_U2PHYDTM1, P2C_RG_SESSEND, P2C_RG_VBUSVALID | P2C_RG_AVALID);
}

STATIC
VOID
U3PhyInit (
  VOID
  )
{
  // u3_phy_instance_init
  ClrSet (U3PHY_SPLLC + U3P_SPLLC_XTALCTL3, 0, XC3_RG_U3_XTAL_RX_PWD | XC3_RG_U3_FRC_XTAL_RX_PWD);
  ClrSet (U3PHY_PHYA + U3P_U3_PHYA_DA_REG0, 0x3 << 10, 2 << 10);
  ClrSet (U3PHY_PHYA + U3P_U3_PHYA_REG9, 0x1F << 1, 4 << 1);
  ClrSet (U3PHY_PHYA + U3P_U3_PHYA_REG6, 0xFU << 28, 0xEU << 28);
  ClrSet (U3PHY_PHYD + U3P_U3_PHYD_CDR1, (0x1F << 8) | (0x1F << 24), (0xC << 8) | (0x3 << 24));
  ClrSet (U3PHY_PHYD + U3P_U3_PHYD_LFPS1, 0x3F << 16, 0x34 << 16);
  ClrSet (U3PHY_PHYD + U3P_U3_PHYD_RXDET1, 0x1FF << 9, 0x10 << 9);
  ClrSet (U3PHY_PHYD + U3P_U3_PHYD_RXDET2, 0x1FF, 0x10);
}

STATIC
EFI_STATUS
IppcHostEnable (
  VOID
  )
{
  UINT32  Cap;
  UINT32  U3Ports;
  UINT32  U2Ports;
  UINT32  Port;
  UINT32  Check;
  UINT32  Value;
  UINTN   Timeout;

  MmioOr32 (IPPC_BASE + IPPC_IP_PW_CTRL0, CTRL0_IP_SW_RST);
  MicroSecondDelay (1);
  MmioAnd32 (IPPC_BASE + IPPC_IP_PW_CTRL0, ~(UINT32)CTRL0_IP_SW_RST);

  Cap     = MmioRead32 (IPPC_BASE + IPPC_IP_XHCI_CAP);
  U3Ports = Cap & 0xFF;
  U2Ports = (Cap >> 8) & 0xFF;
  DEBUG ((DEBUG_INFO, "%a: u2p:%u u3p:%u\n", __func__, U2Ports, U3Ports));

  MmioAnd32 (IPPC_BASE + IPPC_IP_PW_CTRL1, ~(UINT32)CTRL1_IP_HOST_PDN);

  for (Port = 0; Port < U3Ports; Port++) {
    ClrSet (IPPC_BASE + IPPC_U3_CTRL (Port), CTRL_PORT_PDN | CTRL_PORT_DIS, CTRL_PORT_HOST_SEL);
  }

  for (Port = 0; Port < U2Ports; Port++) {
    ClrSet (IPPC_BASE + IPPC_U2_CTRL (Port), CTRL_PORT_PDN | CTRL_PORT_DIS, CTRL_PORT_HOST_SEL);
  }

  Check = STS1_SYSPLL_STABLE | STS1_REF_RST | STS1_SYS125_RST | STS1_XHCI_RST;
  if (U3Ports != 0) {
    Check |= STS1_U3_MAC_RST;
  }

  for (Timeout = 0; Timeout < IPPC_TIMEOUT_US; Timeout++) {
    Value = MmioRead32 (IPPC_BASE + IPPC_IP_PW_STS1);
    if ((Value & Check) == Check) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  DEBUG ((DEBUG_ERROR, "%a: clocks not stable, STS1 0x%x\n", __func__, Value));
  return EFI_TIMEOUT;
}

STATIC
VOID
EFIAPI
XhciPowerPorts (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN   Op;
  UINT32  Ports;
  UINT32  Port;
  UINT32  Value;

  Op    = XHCI_BASE + (MmioRead32 (XHCI_BASE + XHC_CAPLENGTH) & 0xFF);
  Ports = MmioRead32 (XHCI_BASE + XHC_HCSPARAMS1) >> 24;

  for (Port = 0; Port < Ports; Port++) {
    Value = MmioRead32 (Op + XHC_PORTSC (Port));
    if ((Value & XHC_PORTSC_PP) == 0) {
      MmioWrite32 (Op + XHC_PORTSC (Port), (Value & XHC_PORTSC_NEUTRAL) | XHC_PORTSC_PP);
    }

    DEBUG ((DEBUG_INFO, "%a: port %u PORTSC 0x%x -> 0x%x\n", __func__, Port + 1, Value, MmioRead32 (Op + XHC_PORTSC (Port))));
  }
}

EFI_STATUS
EFIAPI
MtkXhciDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  UsbClocksEnable ();
  U2PhyInit ();
  U3PhyInit ();

  Status = IppcHostEnable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Only this controller produces EFI_USB2_HC_PROTOCOL on this platform
  EfiCreateProtocolNotifyEvent (
    &gEfiUsb2HcProtocolGuid,
    TPL_CALLBACK,
    XhciPowerPorts,
    NULL,
    &mUsb2HcRegistration
    );

  return RegisterNonDiscoverableMmioDevice (
           NonDiscoverableDeviceTypeXhci,
           NonDiscoverableDeviceDmaTypeNonCoherent,
           NULL,
           NULL,
           1,
           (UINTN)XHCI_BASE,
           (UINTN)XHCI_SIZE
           );
}
