/** @file
  BPI-R4 board data for the ArmPkg SMBIOS drivers.

  The board has no serial number or UUID of its own. Both are derived from
  the base MAC in the EEPROM, so they stay stable across reboots and OS
  reinstalls, and change only when the base MAC is changed in the setup UI.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BpiR4MacLib.h>
#include <Library/DebugLib.h>
#include <Library/HiiLib.h>
#include <Library/OemMiscLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>

#define BPI_R4_CPU_MAX_HZ  1800000000ULL    // Cortex-A73 cluster

//
// Version 8 (RFC 9562) UUID namespace for this board. The base MAC replaces
// the last 6 bytes; version and variant bits are already set here.
//
STATIC CONST GUID  mUuidNamespace = {
  0x6e506c8f, 0xa689, 0x84f4, { 0x94, 0xca, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

/**
  Return the EEPROM base MAC, or FALSE if there is no stored one. A random
  per-boot MAC must not end up in an identifier that is meant to be stable.
**/
STATIC
BOOLEAN
GetStableBaseMac (
  OUT UINT8  Mac[6]
  )
{
  STATIC BOOLEAN  Done;
  STATIC BOOLEAN  Valid;
  STATIC UINT8    Base[6];
  BPI_R4_MAC_SOURCE  Source;

  if (!Done) {
    Done  = TRUE;
    Valid = !EFI_ERROR (BpiR4MacGetBase (Base, &Source)) &&
            (Source != BpiR4MacSourceVolatile);
    if (!Valid) {
      DEBUG ((DEBUG_ERROR, "%a: no stored base MAC, SMBIOS UUID and serial left empty\n", __func__));
    }
  }

  if (Valid) {
    CopyMem (Mac, Base, sizeof (Base));
  }

  return Valid;
}

BOOLEAN
EFIAPI
OemIsProcessorPresent (
  UINTN  ProcessorIndex
  )
{
  return ProcessorIndex == 0;
}

UINTN
EFIAPI
OemGetCpuFreq (
  UINT8  ProcessorIndex
  )
{
  return BPI_R4_CPU_MAX_HZ;
}

/**
  One socket (the MT7988A) with PcdCoreCount cores.
**/
BOOLEAN
EFIAPI
OemGetProcessorInformation (
  IN  UINTN                              ProcessorIndex,
  IN OUT PROCESSOR_STATUS_DATA           *ProcessorStatus,
  IN OUT PROCESSOR_CHARACTERISTIC_FLAGS  *ProcessorCharacteristics,
  IN OUT OEM_MISC_PROCESSOR_DATA         *MiscProcessorData
  )
{
  UINT16  CoreCount;

  CoreCount = FixedPcdGet32 (PcdCoreCount);

  ProcessorStatus->Bits.CpuStatus       = 1;  // enabled
  ProcessorStatus->Bits.Reserved1       = 0;
  ProcessorStatus->Bits.SocketPopulated = 1;
  ProcessorStatus->Bits.Reserved2       = 0;

  ProcessorCharacteristics->ProcessorReserved1              = 0;
  ProcessorCharacteristics->ProcessorUnknown                = 0;
  ProcessorCharacteristics->Processor64BitCapable           = 1;
  ProcessorCharacteristics->ProcessorMultiCore              = 1;
  ProcessorCharacteristics->ProcessorHardwareThread         = 0;
  ProcessorCharacteristics->ProcessorExecuteProtection      = 1;
  ProcessorCharacteristics->ProcessorEnhancedVirtualization = 0;
  ProcessorCharacteristics->ProcessorPowerPerformanceCtrl   = 1;
  ProcessorCharacteristics->Processor128BitCapable          = 0;
  ProcessorCharacteristics->ProcessorArm64SocId             = 0;
  ProcessorCharacteristics->ProcessorReserved2              = 0;

  MiscProcessorData->CurrentSpeed = (UINT16)(BPI_R4_CPU_MAX_HZ / 1000000);
  MiscProcessorData->MaxSpeed     = (UINT16)(BPI_R4_CPU_MAX_HZ / 1000000);
  MiscProcessorData->CoreCount    = CoreCount;
  MiscProcessorData->CoresEnabled = CoreCount;
  MiscProcessorData->ThreadCount  = CoreCount;

  return TRUE;
}

UINT8
EFIAPI
OemGetMaxProcessors (
  VOID
  )
{
  return 1;
}

BOOLEAN
EFIAPI
OemGetCacheInformation (
  IN UINT8                   ProcessorIndex,
  IN UINT8                   CacheLevel,
  IN BOOLEAN                 DataCache,
  IN BOOLEAN                 UnifiedCache,
  IN OUT SMBIOS_TABLE_TYPE7  *SmbiosCacheTable
  )
{
  SmbiosCacheTable->CacheConfiguration = CacheLevel - 1;

  if ((CacheLevel == 1) && !DataCache && !UnifiedCache) {
    SmbiosCacheTable->CacheConfiguration |= (3 << 8);   // unknown mode
  } else {
    SmbiosCacheTable->CacheConfiguration |= (1 << 8);   // write back
  }

  return TRUE;
}

MISC_CHASSIS_TYPE
EFIAPI
OemGetChassisType (
  VOID
  )
{
  return MiscChassisEmbeddedPc;
}

VOID
EFIAPI
OemUpdateSmbiosInfo (
  IN EFI_HII_HANDLE                    HiiHandle,
  IN EFI_STRING_ID                     TokenToUpdate,
  IN OEM_MISC_SMBIOS_HII_STRING_FIELD  Field
  )
{
  CHAR16  Serial[13];
  UINT8   Mac[6];
  CHAR16  *String;

  String = NULL;

  switch (Field) {
    case SystemManufacturerType01:
    case ManufacturerType03:
      String = (CHAR16 *)FixedPcdGetPtr (PcdBaseBoardManufacturer);
      break;
    case SerialNumType01:
    case SerialNumberType02:
    case SerialNumberType03:
      if (GetStableBaseMac (Mac)) {
        UnicodeSPrint (
          Serial,
          sizeof (Serial),
          L"%02X%02X%02X%02X%02X%02X",
          Mac[0],
          Mac[1],
          Mac[2],
          Mac[3],
          Mac[4],
          Mac[5]
          );
        String = Serial;
      }

      break;
    default:
      break;
  }

  if (String != NULL) {
    HiiSetString (HiiHandle, TokenToUpdate, String, NULL);
  }
}

MISC_BOOT_INFORMATION_STATUS_DATA_TYPE
EFIAPI
OemGetBootStatus (
  VOID
  )
{
  return BootInformationStatusNoError;
}

MISC_CHASSIS_STATE
EFIAPI
OemGetChassisBootupState (
  VOID
  )
{
  return ChassisStateSafe;
}

MISC_CHASSIS_STATE
EFIAPI
OemGetChassisPowerSupplyState (
  VOID
  )
{
  return ChassisStateSafe;
}

MISC_CHASSIS_STATE
EFIAPI
OemGetChassisThermalState (
  VOID
  )
{
  return ChassisStateSafe;
}

MISC_CHASSIS_SECURITY_STATE
EFIAPI
OemGetChassisSecurityStatus (
  VOID
  )
{
  return ChassisSecurityStatusNone;
}

UINT8
EFIAPI
OemGetChassisHeight (
  VOID
  )
{
  return 0;
}

UINT8
EFIAPI
OemGetChassisNumPowerCords (
  VOID
  )
{
  return 1;
}

/**
  All zeros, which Linux treats as "no UUID", if there is no stored base MAC.
**/
VOID
EFIAPI
OemGetSystemUuid (
  OUT GUID  *SystemUuid
  )
{
  UINT8  Mac[6];

  ZeroMem (SystemUuid, sizeof (*SystemUuid));
  if (GetStableBaseMac (Mac)) {
    CopyGuid (SystemUuid, &mUuidNamespace);
    CopyMem (&SystemUuid->Data4[2], Mac, sizeof (Mac));
  }
}

UINT16
EFIAPI
OemGetBiosRelease (
  VOID
  )
{
  return PcdGet16 (PcdSystemBiosRelease);
}

UINT16
EFIAPI
OemGetEmbeddedControllerFirmwareRelease (
  VOID
  )
{
  return PcdGet16 (PcdEmbeddedControllerFirmwareRelease);
}
