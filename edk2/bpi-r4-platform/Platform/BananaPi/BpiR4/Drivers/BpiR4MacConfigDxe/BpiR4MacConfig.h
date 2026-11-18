/** @file
  Shared between BpiR4MacConfigDxe and its VFR.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BPI_R4_MAC_CONFIG_H_
#define BPI_R4_MAC_CONFIG_H_

#define BPI_R4_MAC_FORMSET_GUID \
  { 0xdff8bb3a, 0x53a0, 0x485f, { 0xb6, 0x69, 0x7e, 0x3f, 0x7a, 0xcf, 0xa8, 0x54 } }

#define BPI_R4_MAC_FORM_ID   0x1000
#define BPI_R4_MAC_STR_LEN   17         // xx:xx:xx:xx:xx:xx

#define KEY_SET_MAC          0x2001
#define KEY_REGENERATE       0x2002
#define KEY_CLEAR            0x2003

//
// Transport for the string question only; the EEPROM holds the real value.
//
typedef struct {
  CHAR16    NewMac[18];               // BPI_R4_MAC_STR_LEN + NUL, VfrCompile takes no expressions
} BPI_R4_MAC_VARSTORE;

#endif // BPI_R4_MAC_CONFIG_H_
