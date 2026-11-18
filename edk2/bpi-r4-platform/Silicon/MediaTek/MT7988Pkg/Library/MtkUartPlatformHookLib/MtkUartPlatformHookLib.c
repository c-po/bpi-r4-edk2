/** @file
  PlatformHookLib for the MediaTek UART.

  ATF programs the UART in MediaTek "high-speed" mode (HIGHSPEED=3 plus a
  sample count), which BaseSerialPortLib16550 knows nothing about: it would
  rewrite the 16550 divisor underneath it and garble the console. Drop back
  to the plain 16550 baud generator so the generic library is in control.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformHookLib.h>
#include <MT7988.h>

#define R_UART_LSR       5
#define B_UART_LSR_TEMT  BIT6

/**
  Performs platform specific initialization required for the CPU to access
  the hardware associated with a SerialPortLib instance.

  @retval RETURN_SUCCESS       The platform specific initialization succeeded.

**/
RETURN_STATUS
EFIAPI
PlatformHookSerialPortInitialize (
  VOID
  )
{
  UINTN  Base;

  Base = (UINTN)FixedPcdGet64 (PcdSerialRegisterBase);

  if (MmioRead32 (Base + MTK_UART_HIGHSPEED) != 0) {
    // Let whatever ATF still has queued drain at the old rate first.
    while ((MmioRead32 (Base + R_UART_LSR * FixedPcdGet32 (PcdSerialRegisterStride)) &
            B_UART_LSR_TEMT) == 0)
    {
    }

    MmioWrite32 (Base + MTK_UART_HIGHSPEED, 0);
  }

  return RETURN_SUCCESS;
}
