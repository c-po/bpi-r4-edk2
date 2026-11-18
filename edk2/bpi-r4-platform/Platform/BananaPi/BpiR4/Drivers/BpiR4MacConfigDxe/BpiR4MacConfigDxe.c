/** @file
  Setup page to view and change the BPI-R4 MAC addresses.

  The page writes straight to the EEPROM through BpiR4MacLib; the varstore
  only carries the typed string, as variables do not persist on this board.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Guid/MdeModuleHii.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BpiR4MacLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/HiiConfigAccess.h>

#include "BpiR4MacConfig.h"

extern UINT8  BpiR4MacConfigHiiBin[];
extern UINT8  BpiR4MacConfigDxeStrings[];

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;
#pragma pack ()

STATIC HII_VENDOR_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    BPI_R4_MAC_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_HII_HANDLE  mHiiHandle;

STATIC CONST EFI_STRING_ID  mPortStrings[BPI_R4_MAC_PORT_COUNT] = {
  STRING_TOKEN (STR_PORT0),
  STRING_TOKEN (STR_PORT1),
  STRING_TOKEN (STR_PORT2),
  STRING_TOKEN (STR_PORT3),
  STRING_TOKEN (STR_PORT4),
  STRING_TOKEN (STR_PORT5),
  STRING_TOKEN (STR_PORT6),
};

STATIC
VOID
SetString (
  IN EFI_STRING_ID  Id,
  IN CONST CHAR16   *Format,
  ...
  )
{
  CHAR16   Buffer[96];
  VA_LIST  Args;

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);

  HiiSetString (mHiiHandle, Id, Buffer, NULL);
}

/**
  Show the stored base MAC and the addresses each interface gets from it.
**/
STATIC
VOID
RefreshPage (
  IN CONST CHAR16  *Status  OPTIONAL
  )
{
  EFI_STATUS         ReadStatus;
  BPI_R4_MAC_SOURCE  Source;
  UINT8              Base[6];
  UINT8              Mac[6];
  CONST CHAR8        *Label;
  UINTN              Index;

  ReadStatus = BpiR4MacRead (Base, &Source);
  if (EFI_ERROR (ReadStatus)) {
    SetString (
      STRING_TOKEN (STR_BASE),
      L"Base MAC: %s",
      (ReadStatus == EFI_NOT_FOUND) ? L"none stored, generated on the next boot" :
      (ReadStatus == EFI_VOLUME_CORRUPTED) ? L"EEPROM holds an unknown record, random MAC each boot" :
      L"EEPROM not reachable"
      );
    for (Index = 0; Index < BPI_R4_MAC_PORT_COUNT; Index++) {
      SetString (mPortStrings[Index], L"");
    }
  } else {
    SetString (
      STRING_TOKEN (STR_BASE),
      L"Base MAC: %02x:%02x:%02x:%02x:%02x:%02x (%s)",
      Base[0],
      Base[1],
      Base[2],
      Base[3],
      Base[4],
      Base[5],
      (Source == BpiR4MacSourceUser) ? L"set by user" : L"generated"
      );

    for (Index = 0; Index < BPI_R4_MAC_PORT_COUNT; Index++) {
      BpiR4MacGetPort (Base, Index, Mac, NULL, &Label);
      SetString (
        mPortStrings[Index],
        L"  %02x:%02x:%02x:%02x:%02x:%02x  %a",
        Mac[0],
        Mac[1],
        Mac[2],
        Mac[3],
        Mac[4],
        Mac[5],
        Label
        );
    }
  }

  SetString (STRING_TOKEN (STR_STATUS), L"%s", (Status != NULL) ? Status : L"");
}

STATIC
BOOLEAN
ParseMac (
  IN  CONST CHAR16  *String,
  OUT UINT8         Mac[6]
  )
{
  UINTN   Index;
  UINTN   Nibble;
  CHAR16  Char;
  UINT8   Value;

  if (StrLen (String) != BPI_R4_MAC_STR_LEN) {
    return FALSE;
  }

  for (Index = 0; Index < 6; Index++) {
    Value = 0;
    for (Nibble = 0; Nibble < 2; Nibble++) {
      Char  = String[3 * Index + Nibble];
      Value = (UINT8)(Value << 4);
      if ((Char >= L'0') && (Char <= L'9')) {
        Value |= (UINT8)(Char - L'0');
      } else if ((Char >= L'a') && (Char <= L'f')) {
        Value |= (UINT8)(Char - L'a' + 10);
      } else if ((Char >= L'A') && (Char <= L'F')) {
        Value |= (UINT8)(Char - L'A' + 10);
      } else {
        return FALSE;
      }
    }

    if ((Index < 5) && (String[3 * Index + 2] != L':') && (String[3 * Index + 2] != L'-')) {
      return FALSE;
    }

    Mac[Index] = Value;
  }

  return TRUE;
}

STATIC
CONST CHAR16 *
SetFromString (
  IN EFI_STRING_ID  Id
  )
{
  EFI_STRING  String;
  UINT8       Mac[6];
  BOOLEAN     Parsed;

  String = HiiGetString (mHiiHandle, Id, NULL);
  if (String == NULL) {
    return L"No input";
  }

  Parsed = ParseMac (String, Mac);
  FreePool (String);

  if (!Parsed) {
    return L"Not a MAC address, use xx:xx:xx:xx:xx:xx";
  }

  if ((Mac[0] & BIT0) != 0) {
    return L"Rejected: multicast address";
  }

  if (EFI_ERROR (BpiR4MacSetBase (Mac, FALSE))) {
    return L"Rejected or EEPROM write failed";
  }

  if ((Mac[5] & 0x7) != 0) {
    return L"Stored. Note: base not 8-aligned, ports carry into upper bytes";
  }

  return L"Stored, used from the next boot";
}

STATIC
EFI_STATUS
EFIAPI
MacExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Request,
  OUT EFI_STRING                            *Progress,
  OUT EFI_STRING                            *Results
  )
{
  // The efivarstore is served by the HII config routing itself
  *Progress = Request;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
MacRouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                            *Progress
  )
{
  *Progress = Configuration;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
MacCallback (
  IN     CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN     EFI_BROWSER_ACTION                    Action,
  IN     EFI_QUESTION_ID                       QuestionId,
  IN     UINT8                                 Type,
  IN OUT EFI_IFR_TYPE_VALUE                    *Value,
  OUT    EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  UINT8  Mac[6];

  if (Action == EFI_BROWSER_ACTION_FORM_OPEN) {
    RefreshPage (NULL);
    return EFI_SUCCESS;
  }

  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  switch (QuestionId) {
    case KEY_SET_MAC:
      RefreshPage (SetFromString (Value->string));
      // Drop the typed text: nothing is left to save on exit
      *ActionRequest = EFI_BROWSER_ACTION_REQUEST_FORM_DISCARD;
      break;

    case KEY_REGENERATE:
      if (EFI_ERROR (BpiR4MacGenerate (Mac)) || EFI_ERROR (BpiR4MacSetBase (Mac, TRUE))) {
        RefreshPage (L"Generating or storing a MAC failed");
      } else {
        RefreshPage (L"Stored, used from the next boot");
      }

      break;

    case KEY_CLEAR:
      RefreshPage (
        EFI_ERROR (BpiR4MacClear ()) ? L"EEPROM write failed"
                                     : L"Erased, a new base MAC is generated on the next boot"
        );
      break;

    default:
      return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mConfigAccess = {
  MacExtractConfig,
  MacRouteConfig,
  MacCallback
};

EFI_STATUS
EFIAPI
BpiR4MacConfigDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           Handle;
  BPI_R4_MAC_VARSTORE  VarStore;
  EFI_GUID             FormSetGuid = BPI_R4_MAC_FORMSET_GUID;

  ZeroMem (&VarStore, sizeof (VarStore));
  Status = gRT->SetVariable (
                  L"BpiR4MacConfig",
                  &FormSetGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS,
                  sizeof (VarStore),
                  &VarStore
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid,
                  &mConfigAccess,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mHiiHandle = HiiAddPackages (
                 &FormSetGuid,
                 Handle,
                 BpiR4MacConfigDxeStrings,
                 BpiR4MacConfigHiiBin,
                 NULL
                 );
  if (mHiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           Handle,
           &gEfiDevicePathProtocolGuid,
           &mDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mConfigAccess,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}
