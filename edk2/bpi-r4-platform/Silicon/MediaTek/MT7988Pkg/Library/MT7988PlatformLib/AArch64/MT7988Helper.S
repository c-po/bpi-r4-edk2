//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

//
// Called by PeilessSec before a stack exists. ATF BL2/BL31 have already done
// all early platform setup, so there is nothing left to do here.
//
ASM_FUNC(ArmPlatformPeiBootAction)
  ret
