/** @file
  BPI-R4 MAC addresses in the 24c02 EEPROM.

  Topology from the Linux DT: i2c@11005000 -> pca9545@70 channel 0 ->
  24c02@57 (256 bytes, 8-byte pages). The mux reset is GPIO5, active low.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BpiR4MacLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MtkI2cLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <MT7988.h>

#define MUX_ADDR           0x70
#define MUX_CHANNEL0       BIT0
#define MUX_RESET_PIN      5
#define EEPROM_ADDR        0x57
#define EEPROM_PAGE        8
#define EEPROM_WRITE_US    5000       // 24c02 write cycle time
#define EEPROM_POLL_TRIES  4

#define MTK_SIP_TRNG_GET_RND  0xC2000550   // ATF apsoc_sip_svc_common.h

#define RECORD_VERSION     1
#define RECORD_GENERATED   BIT0
#define RECORD_MAGIC       SIGNATURE_32 ('R', '4', 'M', 'A')

#pragma pack (1)
typedef struct {
  UINT8     Mac[6];
  UINT8     Version;
  UINT8     Flags;
  UINT32    Magic;
  UINT32    Crc;
} MAC_RECORD;
#pragma pack ()

STATIC CONST struct {
  CONST CHAR8    *Path;
  CONST CHAR8    *Label;
} mPorts[BPI_R4_MAC_PORT_COUNT] = {
  { "/soc/ethernet@15100000/mac@0",          "gmac0, switch conduit" },
  { "/soc/switch@15020000/ports/port@0",     "wan"                   },
  { "/soc/switch@15020000/ports/port@1",     "lan1"                  },
  { "/soc/switch@15020000/ports/port@2",     "lan2"                  },
  { "/soc/switch@15020000/ports/port@3",     "lan3"                  },
  { "/soc/ethernet@15100000/mac@1",          "gmac1, SFP"            },
  { "/soc/ethernet@15100000/mac@2",          "gmac2, SFP"            },
};

STATIC CHAR8  mDiag[160];

STATIC
VOID
DiagAdd (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  UINTN    Len;

  Len = AsciiStrLen (mDiag);
  VA_START (Args, Format);
  AsciiVSPrint (mDiag + Len, sizeof (mDiag) - Len, Format, Args);
  VA_END (Args);
}

CONST CHAR8 *
EFIAPI
BpiR4MacDiagnostics (
  VOID
  )
{
  return mDiag;
}

STATIC
EFI_STATUS
EepromOpen (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Channel;

  Status = MtkI2cInit ();
  if (EFI_ERROR (Status)) {
    DiagAdd ("i2c-init:%r ", Status);
    return Status;
  }

  // Release the mux from reset. GPIO5 comes out of reset in function 1
  // (SMI_0_MDC), which holds the mux in reset; Linux claims it as a GPIO too.
  MmioAnd32 (MT7988_GPIO_MODE (MUX_RESET_PIN), ~(0xFU << (4 * (MUX_RESET_PIN % 8))));
  MmioWrite32 (MT7988_GPIO_DOUT (MUX_RESET_PIN) + MT7988_GPIO_SET, 1U << (MUX_RESET_PIN % 32));
  MmioWrite32 (MT7988_GPIO_DIR (MUX_RESET_PIN) + MT7988_GPIO_SET, 1U << (MUX_RESET_PIN % 32));
  MicroSecondDelay (1000);

  Channel = MUX_CHANNEL0;
  Status  = MtkI2cWrite (MUX_ADDR, &Channel, 1);
  if (EFI_ERROR (Status)) {
    DiagAdd (
      "gpio5-mode:%u cfg4:%08x infra1:%08x infra2:%08x ",
      (MmioRead32 (MT7988_GPIO_MODE (MUX_RESET_PIN)) >> (4 * (MUX_RESET_PIN % 8))) & 0xF,
      MmioRead32 (MT7988_TOPCKGEN_BASE + MT7988_TOP_CLK_CFG4),
      MmioRead32 (MT7988_INFRACFG_BASE + 0x48),
      MmioRead32 (MT7988_INFRACFG_BASE + 0x58)
      );
    DEBUG ((DEBUG_ERROR, "%a: PCA9545 not responding: %r\n", __func__, Status));
    DiagAdd ("mux:%r/%x ", Status, MtkI2cLastStatus ());
  }

  return Status;
}

STATIC
EFI_STATUS
EepromRead (
  IN  UINT8  Offset,
  OUT UINT8  *Buffer,
  IN  UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINTN       Try;

  // The EEPROM NACKs while a previous write cycle is still running
  for (Try = 0; Try < EEPROM_POLL_TRIES; Try++) {
    Status = MtkI2cWriteRead (EEPROM_ADDR, &Offset, 1, Buffer, Length);
    if (Status != EFI_NO_RESPONSE) {
      break;
    }

    MicroSecondDelay (EEPROM_WRITE_US);
  }

  if (EFI_ERROR (Status)) {
    DiagAdd ("rd:%r/%x ", Status, MtkI2cLastStatus ());
  }

  return Status;
}

STATIC
EFI_STATUS
EepromWrite (
  IN UINT8        Offset,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  EFI_STATUS  Status;
  UINT8       Page[1 + EEPROM_PAGE];
  UINT8       Verify[sizeof (MAC_RECORD)];
  UINTN       Chunk;
  UINTN       Done;

  ASSERT (Length <= sizeof (Verify));

  for (Done = 0; Done < Length; Done += Chunk) {
    Chunk   = MIN (Length - Done, EEPROM_PAGE - ((Offset + Done) % EEPROM_PAGE));
    Page[0] = (UINT8)(Offset + Done);
    CopyMem (&Page[1], Buffer + Done, Chunk);
    Status = MtkI2cWrite (EEPROM_ADDR, Page, 1 + Chunk);
    if (EFI_ERROR (Status)) {
      DiagAdd ("wr@%u:%r/%x ", (UINT32)(Offset + Done), Status, MtkI2cLastStatus ());
      return Status;
    }

    MicroSecondDelay (EEPROM_WRITE_US);
  }

  Status = EepromRead (Offset, Verify, Length);
  if (!EFI_ERROR (Status) && (CompareMem (Verify, Buffer, Length) != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: read-back mismatch\n", __func__));
    DiagAdd ("verify:mismatch ");
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

STATIC
BOOLEAN
MacIsValid (
  IN CONST UINT8  Mac[6]
  )
{
  STATIC CONST UINT8  Zero[6] = { 0 };

  return ((Mac[0] & BIT0) == 0) && (CompareMem (Mac, Zero, 6) != 0);
}

STATIC
BOOLEAN
RecordIsValid (
  IN CONST MAC_RECORD  *Record
  )
{
  return (Record->Magic == RECORD_MAGIC) &&
         (Record->Version == RECORD_VERSION) &&
         (Record->Crc == CalculateCrc32 ((VOID *)Record, OFFSET_OF (MAC_RECORD, Crc))) &&
         MacIsValid (Record->Mac);
}

STATIC
BOOLEAN
RecordIsBlank (
  IN CONST MAC_RECORD  *Record
  )
{
  CONST UINT8  *Byte;
  UINTN        Index;

  Byte = (CONST UINT8 *)Record;
  for (Index = 0; Index < sizeof (*Record); Index++) {
    if (Byte[Index] != 0xFF) {
      return FALSE;
    }
  }

  return TRUE;
}

EFI_STATUS
EFIAPI
BpiR4MacGenerate (
  OUT UINT8  Mac[6]
  )
{
  UINTN   Index;
  UINTN   Rnd;
  UINTN   Ret;
  UINT32  Value;

  for (Index = 0; Index < 6; Index += 3) {
    Rnd = 0;
    Ret = ArmCallSmc3 (MTK_SIP_TRNG_GET_RND, &Rnd, NULL, NULL);
    if (Ret != 0) {
      DEBUG ((DEBUG_ERROR, "%a: TRNG call failed (%lx)\n", __func__, (UINT64)Ret));
      return EFI_DEVICE_ERROR;
    }

    Value = (UINT32)Rnd;
    CopyMem (&Mac[Index], &Value, 3);
  }

  Mac[0] = (Mac[0] | BIT1) & ~BIT0;  // locally administered, unicast
  Mac[5] &= ~0x7;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BpiR4MacSetBase (
  IN CONST UINT8  Mac[6],
  IN BOOLEAN      Generated
  )
{
  EFI_STATUS  Status;
  MAC_RECORD  Record;

  if (!MacIsValid (Mac)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EepromOpen ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Record.Mac, Mac, 6);
  Record.Version = RECORD_VERSION;
  Record.Flags   = Generated ? RECORD_GENERATED : 0;
  Record.Magic   = RECORD_MAGIC;
  Record.Crc     = CalculateCrc32 (&Record, OFFSET_OF (MAC_RECORD, Crc));

  return EepromWrite (0, (UINT8 *)&Record, sizeof (Record));
}

EFI_STATUS
EFIAPI
BpiR4MacClear (
  VOID
  )
{
  EFI_STATUS  Status;
  MAC_RECORD  Record;

  Status = EepromOpen ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SetMem (&Record, sizeof (Record), 0xFF);
  return EepromWrite (0, (UINT8 *)&Record, sizeof (Record));
}

EFI_STATUS
EFIAPI
BpiR4MacRead (
  OUT UINT8              Mac[6],
  OUT BPI_R4_MAC_SOURCE  *Source
  )
{
  EFI_STATUS  Status;
  MAC_RECORD  Record;

  Status = EepromOpen ();
  if (!EFI_ERROR (Status)) {
    Status = EepromRead (0, (UINT8 *)&Record, sizeof (Record));
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: EEPROM not readable: %r\n", __func__, Status));
    return EFI_DEVICE_ERROR;
  }

  if (RecordIsValid (&Record)) {
    CopyMem (Mac, Record.Mac, 6);
    *Source = ((Record.Flags & RECORD_GENERATED) != 0) ? BpiR4MacSourceGenerated : BpiR4MacSourceUser;
    return EFI_SUCCESS;
  }

  return RecordIsBlank (&Record) ? EFI_NOT_FOUND : EFI_VOLUME_CORRUPTED;
}

EFI_STATUS
EFIAPI
BpiR4MacGetBase (
  OUT UINT8              Mac[6],
  OUT BPI_R4_MAC_SOURCE  *Source
  )
{
  EFI_STATUS  Status;

  Status = BpiR4MacRead (Mac, Source);
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  if (Status == EFI_VOLUME_CORRUPTED) {
    DEBUG ((DEBUG_ERROR, "%a: EEPROM holds an unknown record, leaving it untouched\n", __func__));
  }

  if (EFI_ERROR (BpiR4MacGenerate (Mac))) {
    return EFI_DEVICE_ERROR;
  }

  if ((Status == EFI_NOT_FOUND) && !EFI_ERROR (BpiR4MacSetBase (Mac, TRUE))) {
    DEBUG ((DEBUG_INFO, "%a: blank EEPROM, stored a generated base MAC\n", __func__));
    *Source = BpiR4MacSourceGenerated;
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "%a: using a random MAC for this boot only\n", __func__));
  *Source = BpiR4MacSourceVolatile;
  return EFI_SUCCESS;
}

VOID
EFIAPI
BpiR4MacGetPort (
  IN  CONST UINT8  Base[6],
  IN  UINTN        Index,
  OUT UINT8        Mac[6],
  OUT CONST CHAR8  **Path   OPTIONAL,
  OUT CONST CHAR8  **Label  OPTIONAL
  )
{
  UINTN  Byte;
  UINTN  Sum;

  ASSERT (Index < BPI_R4_MAC_PORT_COUNT);

  // 48-bit add, so a user-set base that is not 8-aligned still carries
  Sum = Index;
  for (Byte = 6; Byte-- > 0;) {
    Sum      += Base[Byte];
    Mac[Byte] = (UINT8)Sum;
    Sum     >>= 8;
  }

  if (Path != NULL) {
    *Path = mPorts[Index].Path;
  }

  if (Label != NULL) {
    *Label = mPorts[Index].Label;
  }
}
