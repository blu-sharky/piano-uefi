#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

//
// piano UFS host controller, stage U3a: report the state the bootloader
// left the controller in. Reads UFSHCI registers only; registers nothing.
//
// The host controller sits at 0x1D84000 (Linux sm8750.dtsi ufshc reg; the
// stock UEFI ufs0@1D80000 node covers PHY + HC in 0x7000). A later stage
// hands it to the generic UfsPassThruDxe without re-enabling the controller
// or restarting the link, so it has to be found enabled with the device
// present.
//

#define UFS_HC_BASE       0x01D84000

#define UFS_HC_CAP        0x00
#define UFS_HC_VER        0x08
#define UFS_HC_IS         0x20
#define UFS_HC_STATUS     0x30
#define UFS_HC_ENABLE     0x34
#define UFS_HC_UTRLBA     0x50
#define UFS_HC_UTRLBAU    0x54
#define UFS_HC_UTRLRSR    0x60
#define UFS_HC_UTMRLRSR   0x80

#define UFS_HC_STATUS_DP        BIT0
#define UFS_HC_STATUS_UTRLRDY   BIT1
#define UFS_HC_STATUS_UTMRLRDY  BIT2
#define UFS_HC_STATUS_UCRDY     BIT3
#define UFS_HC_ENABLE_HCE       BIT0

EFI_STATUS
EFIAPI
UfsPlatformDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Status;
  UINT32  Enable;

  Status = MmioRead32 (UFS_HC_BASE + UFS_HC_STATUS);
  Enable = MmioRead32 (UFS_HC_BASE + UFS_HC_ENABLE);

  DEBUG ((DEBUG_WARN, "UfsPlatform: CAP 0x%08x VER 0x%08x IS 0x%08x\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_CAP),
          MmioRead32 (UFS_HC_BASE + UFS_HC_VER),
          MmioRead32 (UFS_HC_BASE + UFS_HC_IS)));
  DEBUG ((DEBUG_WARN, "UfsPlatform: HCS 0x%08x HCE 0x%08x UTRLBA 0x%08x%08x RSR %u/%u\n",
          Status,
          Enable,
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBAU),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBA),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLRSR),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTMRLRSR)));
  DEBUG ((DEBUG_WARN, "UfsPlatform: controller %a, device %a, UIC %a\n",
          ((Enable & UFS_HC_ENABLE_HCE) != 0) ? "enabled" : "DISABLED",
          ((Status & UFS_HC_STATUS_DP) != 0) ? "present" : "ABSENT",
          ((Status & UFS_HC_STATUS_UCRDY) != 0) ? "ready" : "NOT READY"));

  // The boot manager menu covers the framebuffer log; keep these lines up
  // long enough to be read off the panel.
  DEBUG ((DEBUG_WARN, "UfsPlatform: holding 10 s\n"));
  MicroSecondDelay (10 * 1000 * 1000);

  return EFI_SUCCESS;
}
