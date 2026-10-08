#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/UfsHostControllerPlatform.h>

//
// piano UFS host controller, stage U3b: hand the controller the bootloader
// left running to the generic UFSHCI stack, read-only.
//
// The host controller sits at 0x1D84000 (Linux sm8750.dtsi ufshc reg; the
// stock UEFI ufs0@1D80000 node covers PHY + HC in 0x7000). U3a showed it
// enabled with the device present and the link up when UEFI starts, so
// UfsPassThruDxe is told to skip both the HCE re-enable and the link
// startup. The bootloader's transfer lists are still armed (run-stop set),
// so they are stopped before UfsPassThruDxe programs its own.
//
// Read-only guarantee: every SCSI command that reaches the UFS
// EXT_SCSI_PASS_THRU is checked against an allow-list of read commands;
// anything else (writes, unmap, mode select, security out, ...) is refused
// and counted. This also stops PartitionDxe from "restoring" a GPT.
//

#define UFS_HC_BASE       0x01D84000
#define UFS_HC_SIZE       0x3000

#define UFS_HC_AHIT       0x18
#define UFS_HC_IS         0x20
#define UFS_HC_IE         0x24
#define UFS_HC_STATUS     0x30
#define UFS_HC_ENABLE     0x34
#define UFS_HC_UECPA      0x38
#define UFS_HC_UECDL      0x3C
#define UFS_HC_UECN       0x40
#define UFS_HC_UECT       0x44
#define UFS_HC_UECDME     0x48
#define UFS_HC_UTRLBA     0x50
#define UFS_HC_UTRLBAU    0x54
#define UFS_HC_UTRLDBR    0x58
#define UFS_HC_UTRLRSR    0x60
#define UFS_HC_UTMRLDBR   0x78
#define UFS_HC_UTMRLRSR   0x80
#define UFS_HC_UICCMD     0x90
#define UFS_HC_UICARG1    0x94
#define UFS_HC_UICARG2    0x98
#define UFS_HC_UICARG3    0x9C

#define UFS_HC_IS_UHXS          BIT5
#define UFS_HC_IS_UCCS          BIT10
#define UIC_DME_HIBER_ENTER     0x17
#define UIC_DME_HIBER_EXIT      0x18
#define UPMCRS_PWR_LOCAL        1
#define UIC_TIMEOUT_US          100000

#define UFS_HC_STATUS_DP        BIT0
#define UFS_HC_STATUS_UCRDY     BIT3
#define UFS_HC_ENABLE_HCE       BIT0

#define REPORT_HOLD_US    (15 * 1000 * 1000)

//
// apps SMMU (MMU-500). Unmatched streams fault (sCR0.USFCFG) and a fault
// resets the device, and at this point nothing maps the UFS stream (Linux:
// iommus = <&apps_smmu 0x60 0>). The hypervisor turns S2CR BYPASS into
// FAULT, so bypass is done the way Linux arm-smmu-qcom does it: the last
// context bank with translation off (SCTLR.M = 0, CBAR type S1 translate /
// S2 bypass), and a stream match entry routing 0x60 to it.
//
#define SMMU_BASE                     0x15000000
#define SMMU_IDR0                     0x020
#define SMMU_IDR1                     0x024
#define SMMU_SMR(n)                   (0x800 + 4 * (n))
#define SMMU_S2CR(n)                  (0xC00 + 4 * (n))
#define SMMU_SMR_VALID                BIT31
#define SMMU_S2CR_TYPE_MASK           (BIT17 | BIT16)
#define SMMU_S2CR_CBNDX_MASK          0xFF
#define SMMU_CBAR_TYPE_S1_S2_BYPASS   BIT16
#define SMMU_CB_SCTLR_M               BIT0
#define SMMU_MAX_SMR                  128
#define UFS_STREAM_ID                 0x60
#define TRACED_COMMANDS   24

STATIC EFI_EXT_SCSI_PASS_THRU_PASSTHRU  mUfsPassThru;
STATIC VOID                             *mPassThruRegistration;
STATIC UINTN                            mRefusedCommands;
STATIC UINT8                            mLastRefusedOpcode;
STATIC UINTN                            mTracedCommands;
STATIC UINT32                           mUfsSmr;
STATIC UINT32                           mUfsS2cr;
STATIC BOOLEAN                          mTakeoverTried;
STATIC EFI_HANDLE                       mUfsHandle;

/**
  Bring the link out of HIBERN8 (the bootloader may park it there).
**/
STATIC
EFI_STATUS
UicHibernateExit (
  VOID
  )
{
  UINT32  Is;
  UINT32  Upmcrs;
  UINTN   Timeout;

  MmioWrite32 (UFS_HC_BASE + UFS_HC_IS, UFS_HC_IS_UCCS | UFS_HC_IS_UHXS);
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UICARG1, 0);
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UICARG2, 0);
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UICARG3, 0);
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UICCMD, UIC_DME_HIBER_EXIT);

  Is = 0;
  for (Timeout = 0; Timeout < UIC_TIMEOUT_US; Timeout++) {
    Is = MmioRead32 (UFS_HC_BASE + UFS_HC_IS);
    if ((Is & UFS_HC_IS_UHXS) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  Upmcrs = (MmioRead32 (UFS_HC_BASE + UFS_HC_STATUS) >> 8) & 0x7;
  DEBUG ((DEBUG_WARN, "UfsPlatform: HIBERN8 exit: IS 0x%08x result 0x%02x UPMCRS %u\n",
          Is, MmioRead32 (UFS_HC_BASE + UFS_HC_UICARG2) & 0xFF, Upmcrs));
  MmioWrite32 (UFS_HC_BASE + UFS_HC_IS, UFS_HC_IS_UCCS | UFS_HC_IS_UHXS);

  return (((Is & UFS_HC_IS_UHXS) != 0) && (Upmcrs == UPMCRS_PWR_LOCAL)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EFIAPI
UfsPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  UINT32  Status;

  if (CallbackPhase == EdkiiUfsHcPreLinkStartup) {
    DEBUG ((DEBUG_WARN, "UfsPlatform: lists stopped (RSR %u/%u), link startup skipped\n",
            MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLRSR), MmioRead32 (UFS_HC_BASE + UFS_HC_UTMRLRSR)));
  }

  if (CallbackPhase != EdkiiUfsHcPostHce) {
    return EFI_SUCCESS;
  }

  // One attempt only: a failed takeover is not retried by later connects.
  if (mTakeoverTried) {
    return EFI_ALREADY_STARTED;
  }

  mTakeoverTried = TRUE;

  Status = MmioRead32 (UFS_HC_BASE + UFS_HC_STATUS);
  if (((MmioRead32 (UFS_HC_BASE + UFS_HC_ENABLE) & UFS_HC_ENABLE_HCE) == 0) ||
      ((Status & (UFS_HC_STATUS_DP | UFS_HC_STATUS_UCRDY)) != (UFS_HC_STATUS_DP | UFS_HC_STATUS_UCRDY)))
  {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: controller not left running (HCS 0x%08x), not taking over\n", Status));
    return EFI_NOT_READY;
  }

  // Requests still in flight from the bootloader: leave the controller alone.
  if ((MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLDBR) != 0) || (MmioRead32 (UFS_HC_BASE + UFS_HC_UTMRLDBR) != 0)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: bootloader requests outstanding, not taking over\n"));
    return EFI_NOT_READY;
  }

  // The list base registers may only change while the lists are stopped.
  DEBUG ((DEBUG_WARN, "UfsPlatform: taking over (old UTRLBA 0x%08x%08x)\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBAU), MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBA)));
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UTRLRSR, 0);
  MmioWrite32 (UFS_HC_BASE + UFS_HC_UTMRLRSR, 0);

  // The last UIC command the bootloader issued tells the link state.
  DEBUG ((DEBUG_WARN, "UfsPlatform: last UIC 0x%02x args 0x%08x 0x%08x 0x%08x IS 0x%08x IE 0x%08x AHIT 0x%08x\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_UICCMD) & 0xFF,
          MmioRead32 (UFS_HC_BASE + UFS_HC_UICARG1), MmioRead32 (UFS_HC_BASE + UFS_HC_UICARG2),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UICARG3), MmioRead32 (UFS_HC_BASE + UFS_HC_IS),
          MmioRead32 (UFS_HC_BASE + UFS_HC_IE), MmioRead32 (UFS_HC_BASE + UFS_HC_AHIT)));

  MmioWrite32 (UFS_HC_BASE + UFS_HC_IS, MmioRead32 (UFS_HC_BASE + UFS_HC_IS));

  if ((MmioRead32 (UFS_HC_BASE + UFS_HC_UICCMD) & 0xFF) == UIC_DME_HIBER_ENTER) {
    return UicHibernateExit ();
  }

  return EFI_SUCCESS;
}

STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  mUfsHcPlatform = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
  NULL,
  UfsPlatformCallback,
  EdkiiUfsCardRefClkFreqObsolete,   // keep bRefClkFreq as the bootloader set it
  TRUE,                             // SkipHceReenable
  TRUE                              // SkipLinkStartup
};

STATIC
BOOLEAN
ScsiCommandIsRead (
  IN UINT8  *Cdb
  )
{
  switch (Cdb[0]) {
    case 0x00:    // TEST UNIT READY
    case 0x03:    // REQUEST SENSE
    case 0x08:    // READ (6)
    case 0x12:    // INQUIRY
    case 0x1A:    // MODE SENSE (6)
    case 0x25:    // READ CAPACITY (10)
    case 0x28:    // READ (10)
    case 0x5A:    // MODE SENSE (10)
    case 0x88:    // READ (16)
    case 0xA0:    // REPORT LUNS
    case 0xA8:    // READ (12)
      return TRUE;
    case 0x9E:    // SERVICE ACTION IN (16): only READ CAPACITY (16)
      return (BOOLEAN)((Cdb[1] & 0x1F) == 0x10);
    default:
      return FALSE;
  }
}

STATIC
EFI_STATUS
EFIAPI
ReadOnlyPassThru (
  IN     EFI_EXT_SCSI_PASS_THRU_PROTOCOL                 *This,
  IN     UINT8                                           *Target,
  IN     UINT64                                          Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET      *Packet,
  IN     EFI_EVENT                                       Event OPTIONAL
  )
{
  if ((Packet != NULL) && (Packet->Cdb != NULL) && (Packet->CdbLength > 0) &&
      !ScsiCommandIsRead (Packet->Cdb))
  {
    mRefusedCommands++;
    mLastRefusedOpcode = ((UINT8 *)Packet->Cdb)[0];
    return EFI_UNSUPPORTED;
  }

  // Trace the first commands: the last line on the panel is the one in flight.
  if ((Packet != NULL) && (Packet->Cdb != NULL) && (mTracedCommands < TRACED_COMMANDS)) {
    if (mTracedCommands == 0) {
      DEBUG ((DEBUG_WARN, "UfsPlatform: UTRLBA 0x%08x%08x\n",
              MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBAU), MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLBA)));
    }

    mTracedCommands++;
    DEBUG ((DEBUG_WARN, "UfsPlatform: cmd 0x%02x LUN 0x%lx\n", ((UINT8 *)Packet->Cdb)[0], Lun));
  }

  return mUfsPassThru (This, Target, Lun, Packet, Event);
}

STATIC
VOID
EFIAPI
OnExtScsiPassThru (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL  *PassThru;

  // Runs at TPL_NOTIFY, i.e. before the installing driver returns and
  // before any SCSI bus driver can reach the new instance.
  while (!EFI_ERROR (gBS->LocateProtocol (&gEfiExtScsiPassThruProtocolGuid, mPassThruRegistration, (VOID **)&PassThru))) {
    if (PassThru->PassThru == ReadOnlyPassThru) {
      continue;
    }

    if ((mUfsPassThru != NULL) && (PassThru->PassThru != mUfsPassThru)) {
      // A second, different SCSI host adapter: there is none on this device.
      DEBUG ((DEBUG_ERROR, "UfsPlatform: unknown EXT_SCSI_PASS_THRU left unguarded\n"));
      continue;
    }

    mUfsPassThru       = PassThru->PassThru;
    PassThru->PassThru = ReadOnlyPassThru;
  }
}

STATIC
VOID
ReportDisks (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  UINTN                     Part;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_DEVICE_PATH_PROTOCOL  *DiskPath;
  EFI_DEVICE_PATH_PROTOCOL  *PartPath;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  EFI_HANDLE                *Parts;
  UINTN                     PartCount;
  UINTN                     DiskPathSize;
  UINTN                     Members;
  UINTN                     Lun;

  // BDS does not connect storage; bring up just the UFS stack (host
  // controller, LUNs, partitions) so there is something to report.
  Status = gBS->ConnectController (mUfsHandle, NULL, NULL, TRUE);
  DEBUG ((DEBUG_WARN, "UfsPlatform: connect: %r\n", Status));

  Handles   = NULL;
  Parts     = NULL;
  PartCount = 0;
  gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &PartCount, &Parts);

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Count = 0;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
        BlockIo->Media->LogicalPartition ||
        EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DiskPath)))
    {
      continue;
    }

    Lun = MAX_UINTN;
    for (Node = DiskPath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Node) == MSG_UFS_DP)) {
        Lun = ((UFS_DEVICE_PATH *)Node)->Lun;
      }
    }

    if (Lun == MAX_UINTN) {
      continue;
    }

    DiskPathSize = GetDevicePathSize (DiskPath) - END_DEVICE_PATH_LENGTH;
    Members      = 0;
    for (Part = 0; Part < PartCount; Part++) {
      if (!EFI_ERROR (gBS->HandleProtocol (Parts[Part], &gEfiDevicePathProtocolGuid, (VOID **)&PartPath)) &&
          (GetDevicePathSize (PartPath) > DiskPathSize) &&
          (CompareMem (PartPath, DiskPath, DiskPathSize) == 0))
      {
        Members++;
      }
    }

    DEBUG ((DEBUG_WARN, "UfsPlatform: LUN 0x%02x %u x %lu B, %u partitions%a\n",
            Lun, BlockIo->Media->BlockSize, BlockIo->Media->LastBlock + 1, Members,
            BlockIo->Media->ReadOnly ? ", read-only" : ""));
  }

  for (Part = 0; Part < PartCount; Part++) {
    if (EFI_ERROR (gBS->HandleProtocol (Parts[Part], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        (Info->Type != PARTITION_TYPE_GPT) ||
        (StrCmp (Info->Info.Gpt.PartitionName, L"userdata") != 0))
    {
      continue;
    }

    DEBUG ((DEBUG_WARN, "UfsPlatform: userdata LBA %lu..%lu\n",
            Info->Info.Gpt.StartingLBA, Info->Info.Gpt.EndingLBA));
  }

  DEBUG ((DEBUG_WARN, "UfsPlatform: UFS stream SMR 0x%08x S2CR 0x%08x\n", mUfsSmr, mUfsS2cr));
  DEBUG ((DEBUG_WARN, "UfsPlatform: HCS 0x%08x IS 0x%08x DBR 0x%08x UIC 0x%02x\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_STATUS), MmioRead32 (UFS_HC_BASE + UFS_HC_IS),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UTRLDBR), MmioRead32 (UFS_HC_BASE + UFS_HC_UICCMD) & 0xFF));
  DEBUG ((DEBUG_WARN, "UfsPlatform: UECPA 0x%08x DL 0x%08x N 0x%08x T 0x%08x DME 0x%08x\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_UECPA), MmioRead32 (UFS_HC_BASE + UFS_HC_UECDL),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UECN), MmioRead32 (UFS_HC_BASE + UFS_HC_UECT),
          MmioRead32 (UFS_HC_BASE + UFS_HC_UECDME)));
  DEBUG ((DEBUG_WARN, "UfsPlatform: %u GPT/MBR partitions, %u non-read commands refused (last 0x%02x)\n",
          PartCount, mRefusedCommands, mLastRefusedOpcode));

  if (Handles != NULL) {
    FreePool (Handles);
  }

  if (Parts != NULL) {
    FreePool (Parts);
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);
  ReportDisks ();

  // The boot manager menu covers the framebuffer log; keep the report up
  // long enough to be read off the panel.
  DEBUG ((DEBUG_WARN, "UfsPlatform: holding 15 s\n"));
  MicroSecondDelay (REPORT_HOLD_US);
}

STATIC
BOOLEAN
SmrMatches (
  IN UINT32  Smr,
  IN UINT32  StreamId
  )
{
  return (BOOLEAN)(((Smr & SMMU_SMR_VALID) != 0) &&
                   ((((Smr ^ StreamId) & ~(Smr >> 16)) & 0x7FFF) == 0));
}

/**
  Route the UFS stream through a translation-off context bank.

  @retval EFI_SUCCESS  The stream is routed (now or already).
  @retval others       Nothing usable; nothing was written.
**/
STATIC
EFI_STATUS
MapUfsStream (
  VOID
  )
{
  UINTN   PageSize;
  UINTN   NumPage;
  UINTN   NumSmr;
  UINTN   Cb;
  UINTN   CbBase;
  UINTN   Index;
  UINTN   Free;
  UINTN   Valid;
  UINT32  Idr1;
  UINT32  Smr;
  UINT32  S2cr;

  Idr1     = MmioRead32 (SMMU_BASE + SMMU_IDR1);
  PageSize = ((Idr1 & BIT31) != 0) ? SIZE_64KB : SIZE_4KB;
  NumPage  = (UINTN)1 << (((Idr1 >> 28) & 0x7) + 1);
  NumSmr   = MIN (MmioRead32 (SMMU_BASE + SMMU_IDR0) & 0xFF, SMMU_MAX_SMR);
  Cb       = (Idr1 & 0xFF) - 1;
  CbBase   = SMMU_BASE + (NumPage + Cb) * PageSize;
  Free     = MAX_UINTN;
  Valid    = 0;

  for (Index = 0; Index < NumSmr; Index++) {
    Smr  = MmioRead32 (SMMU_BASE + SMMU_SMR (Index));
    S2cr = MmioRead32 (SMMU_BASE + SMMU_S2CR (Index));
    if ((Smr & SMMU_SMR_VALID) == 0) {
      if (Free == MAX_UINTN) {
        Free = Index;
      }

      continue;
    }

    Valid++;
    if (SmrMatches (Smr, UFS_STREAM_ID)) {
      DEBUG ((DEBUG_WARN, "UfsPlatform: stream 0x%x already mapped: SMR%u 0x%08x S2CR 0x%08x\n", UFS_STREAM_ID, Index, Smr, S2cr));
      mUfsSmr  = Smr;
      mUfsS2cr = S2cr;
      return EFI_SUCCESS;
    }

    if ((S2cr & SMMU_S2CR_CBNDX_MASK) == Cb) {
      DEBUG ((DEBUG_ERROR, "UfsPlatform: CB%u already used by SMR%u, not mapping\n", Cb, Index));
      return EFI_ACCESS_DENIED;
    }
  }

  DEBUG ((DEBUG_WARN, "UfsPlatform: %u/%u SMRs valid; CB%u SCTLR 0x%08x CBAR 0x%08x\n",
          Valid, NumSmr, Cb, MmioRead32 (CbBase), MmioRead32 (SMMU_BASE + PageSize + 4 * Cb)));

  if ((Free == MAX_UINTN) || ((MmioRead32 (CbBase) & SMMU_CB_SCTLR_M) != 0)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: no free SMR or CB%u in use, not mapping\n", Cb));
    return EFI_OUT_OF_RESOURCES;
  }

  // Same order as Linux: context bank first, then S2CR, then SMR.
  MmioWrite32 (CbBase, 0);
  MmioWrite32 (SMMU_BASE + PageSize + 4 * Cb, SMMU_CBAR_TYPE_S1_S2_BYPASS);
  MmioWrite32 (SMMU_BASE + SMMU_S2CR (Free), (UINT32)Cb);
  MmioWrite32 (SMMU_BASE + SMMU_SMR (Free), SMMU_SMR_VALID | UFS_STREAM_ID);

  Smr      = MmioRead32 (SMMU_BASE + SMMU_SMR (Free));
  S2cr     = MmioRead32 (SMMU_BASE + SMMU_S2CR (Free));
  mUfsSmr  = Smr;
  mUfsS2cr = S2cr;
  DEBUG ((DEBUG_WARN, "UfsPlatform: stream 0x%x -> SMR%u 0x%08x S2CR 0x%08x CB%u\n", UFS_STREAM_ID, Free, Smr, S2cr, Cb));

  if (!SmrMatches (Smr, UFS_STREAM_ID) || ((S2cr & SMMU_S2CR_TYPE_MASK) != 0) ||
      ((S2cr & SMMU_S2CR_CBNDX_MASK) != Cb))
  {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: SMMU did not take the mapping\n"));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UfsPlatformDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;
  EFI_HANDLE  Handle;

  DEBUG ((DEBUG_WARN, "UfsPlatform: HCS 0x%08x HCE 0x%08x\n",
          MmioRead32 (UFS_HC_BASE + UFS_HC_STATUS), MmioRead32 (UFS_HC_BASE + UFS_HC_ENABLE)));

  // No DMA may start before the UFS stream is routed; without it, leave
  // the controller to whoever comes next.
  Status = MapUfsStream ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "UfsPlatform: UFS left alone (%r); holding 15 s\n", Status));
    MicroSecondDelay (REPORT_HOLD_US);
    return Status;
  }

  // The guard goes in first, so no pass-through instance can exist unguarded.
  EfiCreateProtocolNotifyEvent (&gEfiExtScsiPassThruProtocolGuid, TPL_NOTIFY, OnExtScsiPassThru, NULL, &mPassThruRegistration);

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&Handle, &gEdkiiUfsHcPlatformProtocolGuid, &mUfsHcPlatform, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Dma-coherent in Linux; register it non-coherent anyway (uncached
  // buffers are correct for either).
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeUfs,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             &mUfsHandle,
             1,
             UFS_HC_BASE,
             UFS_HC_SIZE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: registering the host controller failed: %r\n", Status));
    return Status;
  }

  EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);

  return EFI_SUCCESS;
}
