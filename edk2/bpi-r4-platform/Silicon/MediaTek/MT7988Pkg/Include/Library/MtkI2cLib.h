/** @file
  Polled master access to the MT7988 I2C2 controller.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MTK_I2C_LIB_H_
#define MTK_I2C_LIB_H_

#define MTK_I2C_MAX_LEN  255

/**
  Ungate the I2C2 clocks, mux its pins and reset the controller for 100 kHz.
  Safe to call more than once.
**/
EFI_STATUS
EFIAPI
MtkI2cInit (
  VOID
  );

/**
  Write Length bytes to the device at 7-bit address Addr.
**/
EFI_STATUS
EFIAPI
MtkI2cWrite (
  IN UINT8        Addr,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  );

/**
  Write WriteLength bytes, then read ReadLength bytes after a repeated start.
**/
EFI_STATUS
EFIAPI
MtkI2cWriteRead (
  IN  UINT8        Addr,
  IN  CONST UINT8  *WriteBuffer,
  IN  UINTN        WriteLength,
  OUT UINT8        *ReadBuffer,
  IN  UINTN        ReadLength
  );

/**
  INTR_STAT of the last transfer, BIT31 set if it timed out.
**/
UINT32
EFIAPI
MtkI2cLastStatus (
  VOID
  );

#endif // MTK_I2C_LIB_H_
