#include <Uefi.h>

#include <IndustryStandard/UfsHci.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/UfsHostControllerPlatform.h>

//
// piano UFS: hand the host controller the bootloader left running to the
// generic UFSHCI stack (NonDiscoverablePciDevice, UfsPciHc, UfsPassThru,
// ScsiBus, ScsiDisk), read-only.
//
// The host controller sits at 0x1D84000 (Linux sm8750.dtsi ufshc reg; the
// stock UEFI ufs0@1D80000 node covers PHY + HC in 0x7000). It is found
// enabled, with the device present and the link up, so UfsPassThruDxe is
// told to skip both the HCE re-enable and the link startup. What the
// bootloader leaves behind, and what is done about it:
//
//   - no SMMU route for the UFS stream: unmatched streams fault and the
//     fault resets the device, so the stream is routed first (MapUfsStream);
//   - its request lists still armed: stopped before UfsPassThru programs
//     its own;
//   - the link in HIBERN8 (last UIC command DME_HIBERNATE_ENTER): exited;
//   - the device in a low-power mode, refusing queries: woken with START
//     STOP UNIT (ACTIVE), as Linux does on resume.
//
// Any check failing leaves the controller alone. Every SCSI command that
// then reaches the UFS EXT_SCSI_PASS_THRU is checked against an allow-list
// of read commands; anything else (writes, unmap, mode select, security
// out, ...) is refused. This also keeps PartitionDxe from restoring a GPT.
//

#define HCI_BASE       0x01D84000
#define HCI_SIZE       0x3000

#define HCI_IS         0x20
#define HCI_STATUS     0x30
#define HCI_ENABLE     0x34
#define HCI_UTRLBA     0x50
#define HCI_UTRLBAU    0x54
#define HCI_UTRLDBR    0x58
#define HCI_UTRLRSR    0x60
#define HCI_UTMRLDBR   0x78
#define HCI_UTMRLRSR   0x80
#define HCI_UICCMD     0x90
#define HCI_UICARG1    0x94
#define HCI_UICARG2    0x98
#define HCI_UICARG3    0x9C

#define HCI_IS_UHXS          BIT5
#define HCI_IS_UCCS          BIT10
#define HCI_STATUS_DP        BIT0
#define HCI_STATUS_UCRDY     BIT3
#define HCI_ENABLE_HCE       BIT0

#define UIC_DME_HIBER_ENTER  0x17
#define UIC_DME_HIBER_EXIT   0x18
#define UPMCRS_PWR_LOCAL     1
#define UIC_TIMEOUT_US       100000

//
// apps SMMU (MMU-500). Linux: iommus = <&apps_smmu 0x60 0>. The hypervisor
// turns S2CR BYPASS into FAULT, so bypass is done the way Linux
// arm-smmu-qcom does it: the last context bank with translation off
// (SCTLR.M = 0, CBAR type S1 translate / S2 bypass), and a stream match
// entry routing 0x60 to it.
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

//
// A one-slot UTP engine, used only between the HIBERN8 exit and the hand-
// over, to read the device power mode and wake the device if needed.
//
#define ENGINE_UCD_OFFSET       0x400
#define ENGINE_RESP_OFFSET      0x200
#define ENGINE_TIMEOUT_US       1000000
#define UPIU_QUERY_READ         0x01
#define UFS_POWER_MODE_ACTIVE   0x11
#define SCSI_START_STOP_UNIT    0x1B

STATIC EFI_EXT_SCSI_PASS_THRU_PASSTHRU  mUfsPassThru;
STATIC VOID                             *mPassThruRegistration;
STATIC BOOLEAN                          mTakeoverTried;
STATIC UINT8                            *mEngine;

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
  @retval others       Nothing usable, or the SMMU did not take it.
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

  for (Index = 0; Index < NumSmr; Index++) {
    Smr  = MmioRead32 (SMMU_BASE + SMMU_SMR (Index));
    S2cr = MmioRead32 (SMMU_BASE + SMMU_S2CR (Index));
    if ((Smr & SMMU_SMR_VALID) == 0) {
      if (Free == MAX_UINTN) {
        Free = Index;
      }

      continue;
    }

    if (SmrMatches (Smr, UFS_STREAM_ID)) {
      DEBUG ((DEBUG_WARN, "UfsPlatform: stream 0x%x already mapped: SMR%u 0x%08x S2CR 0x%08x\n", UFS_STREAM_ID, Index, Smr, S2cr));
      return EFI_SUCCESS;
    }

    if ((S2cr & SMMU_S2CR_CBNDX_MASK) == Cb) {
      DEBUG ((DEBUG_ERROR, "UfsPlatform: CB%u already used by SMR%u\n", Cb, Index));
      return EFI_ACCESS_DENIED;
    }
  }

  if ((Free == MAX_UINTN) || ((MmioRead32 (CbBase) & SMMU_CB_SCTLR_M) != 0)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: no free SMR, or CB%u translates\n", Cb));
    return EFI_OUT_OF_RESOURCES;
  }

  // Same order as Linux: context bank first, then S2CR, then SMR.
  MmioWrite32 (CbBase, 0);
  MmioWrite32 (SMMU_BASE + PageSize + 4 * Cb, SMMU_CBAR_TYPE_S1_S2_BYPASS);
  MmioWrite32 (SMMU_BASE + SMMU_S2CR (Free), (UINT32)Cb);
  MmioWrite32 (SMMU_BASE + SMMU_SMR (Free), SMMU_SMR_VALID | UFS_STREAM_ID);

  Smr  = MmioRead32 (SMMU_BASE + SMMU_SMR (Free));
  S2cr = MmioRead32 (SMMU_BASE + SMMU_S2CR (Free));
  if (!SmrMatches (Smr, UFS_STREAM_ID) || ((S2cr & SMMU_S2CR_TYPE_MASK) != 0) ||
      ((S2cr & SMMU_S2CR_CBNDX_MASK) != Cb))
  {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: SMMU did not take the mapping (SMR 0x%08x S2CR 0x%08x)\n", Smr, S2cr));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_WARN, "UfsPlatform: stream 0x%x -> SMR%u, CB%u (bypass)\n", UFS_STREAM_ID, Free, Cb));
  return EFI_SUCCESS;
}

/**
  Bring the link out of HIBERN8.
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

  MmioWrite32 (HCI_BASE + HCI_IS, HCI_IS_UCCS | HCI_IS_UHXS);
  MmioWrite32 (HCI_BASE + HCI_UICARG1, 0);
  MmioWrite32 (HCI_BASE + HCI_UICARG2, 0);
  MmioWrite32 (HCI_BASE + HCI_UICARG3, 0);
  MmioWrite32 (HCI_BASE + HCI_UICCMD, UIC_DME_HIBER_EXIT);

  Is = 0;
  for (Timeout = 0; Timeout < UIC_TIMEOUT_US; Timeout++) {
    Is = MmioRead32 (HCI_BASE + HCI_IS);
    if ((Is & HCI_IS_UHXS) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  Upmcrs = (MmioRead32 (HCI_BASE + HCI_STATUS) >> 8) & 0x7;
  MmioWrite32 (HCI_BASE + HCI_IS, HCI_IS_UCCS | HCI_IS_UHXS);
  if (((Is & HCI_IS_UHXS) == 0) || (Upmcrs != UPMCRS_PWR_LOCAL)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: HIBERN8 exit failed: IS 0x%08x UPMCRS %u\n", Is, Upmcrs));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EngineExec (
  VOID
  )
{
  UTP_TRD  *Trd;
  UINT64   Ucd;
  UINTN    Timeout;

  Trd = (UTP_TRD *)mEngine;
  Ucd = (UINT64)(UINTN)(mEngine + ENGINE_UCD_OFFSET);
  ZeroMem (Trd, sizeof (*Trd));
  ZeroMem (mEngine + ENGINE_UCD_OFFSET + ENGINE_RESP_OFFSET, sizeof (UTP_RESPONSE_UPIU));
  Trd->Ct     = 1;                  // UFS storage
  Trd->Dd     = 0;                  // no data
  Trd->Ocs    = 0x0F;               // invalid until the controller writes it
  Trd->UcdBa  = (UINT32)(Ucd >> 7);
  Trd->UcdBaU = (UINT32)(Ucd >> 32);
  Trd->RuO    = ENGINE_RESP_OFFSET / 4;
  Trd->RuL    = sizeof (UTP_RESPONSE_UPIU) / 4;
  Trd->PrdtO  = (ENGINE_RESP_OFFSET + sizeof (UTP_RESPONSE_UPIU)) / 4;
  Trd->PrdtL  = 0;

  WriteBackDataCacheRange (mEngine, EFI_PAGE_SIZE);
  MmioWrite32 (HCI_BASE + HCI_UTRLDBR, BIT0);

  for (Timeout = 0; Timeout < ENGINE_TIMEOUT_US; Timeout++) {
    if ((MmioRead32 (HCI_BASE + HCI_UTRLDBR) & BIT0) == 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  InvalidateDataCacheRange (mEngine, EFI_PAGE_SIZE);
  if (Timeout == ENGINE_TIMEOUT_US) {
    return EFI_TIMEOUT;
  }

  return (Trd->Ocs == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EngineReadPowerMode (
  OUT UINT32  *Mode
  )
{
  UTP_QUERY_REQ_UPIU   *Req;
  UTP_QUERY_RESP_UPIU  *Resp;
  EFI_STATUS           Status;

  Req  = (UTP_QUERY_REQ_UPIU *)(mEngine + ENGINE_UCD_OFFSET);
  Resp = (UTP_QUERY_RESP_UPIU *)(mEngine + ENGINE_UCD_OFFSET + ENGINE_RESP_OFFSET);
  ZeroMem (Req, sizeof (*Req));
  Req->TransCode  = 0x16;
  Req->QueryFunc  = UPIU_QUERY_READ;
  Req->Tsf.Opcode = UtpQueryFuncOpcodeRdAttr;
  Req->Tsf.DescId = UfsAttrCurPowerMode;

  Status = EngineExec ();
  *Mode  = SwapBytes32 (Resp->Tsf.Value);
  if (!EFI_ERROR (Status) && (Resp->QueryResp != 0)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

STATIC
EFI_STATUS
EngineStartStopUnitActive (
  VOID
  )
{
  UTP_COMMAND_UPIU   *Cmd;
  UTP_RESPONSE_UPIU  *Resp;
  EFI_STATUS         Status;

  Cmd  = (UTP_COMMAND_UPIU *)(mEngine + ENGINE_UCD_OFFSET);
  Resp = (UTP_RESPONSE_UPIU *)(mEngine + ENGINE_UCD_OFFSET + ENGINE_RESP_OFFSET);
  ZeroMem (Cmd, sizeof (*Cmd));
  Cmd->TransCode = 0x01;
  Cmd->Lun       = UFS_WLUN_UFS_DEV;
  Cmd->Cdb[0]    = SCSI_START_STOP_UNIT;
  Cmd->Cdb[4]    = 0x10;            // POWER CONDITION = ACTIVE

  Status = EngineExec ();
  if (!EFI_ERROR (Status) && (Resp->Status != 0)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

/**
  Wake the device unless it is confirmed active. Runs with the
  bootloader's lists stopped and the link out of HIBERN8.
**/
STATIC
EFI_STATUS
WakeDevice (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Mode;
  UINTN       Try;

  mEngine = AllocateAlignedPages (1, SIZE_4KB);
  if (mEngine == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mEngine, EFI_PAGE_SIZE);
  MmioWrite32 (HCI_BASE + HCI_UTRLBA, (UINT32)(UINTN)mEngine);
  MmioWrite32 (HCI_BASE + HCI_UTRLBAU, (UINT32)RShiftU64 ((UINT64)(UINTN)mEngine, 32));
  MmioWrite32 (HCI_BASE + HCI_UTRLRSR, 1);

  // A sleeping device may refuse the query itself.
  Status = EngineReadPowerMode (&Mode);
  if (EFI_ERROR (Status) || (Mode != UFS_POWER_MODE_ACTIVE)) {
    // A unit attention after the power change is normal; retry like Linux.
    for (Try = 0; Try < 3; Try++) {
      if (!EFI_ERROR (EngineStartStopUnitActive ())) {
        break;
      }
    }

    Status = EngineReadPowerMode (&Mode);
    DEBUG ((DEBUG_WARN, "UfsPlatform: device woken, power mode 0x%02x (%r)\n", Mode, Status));
  }

  MmioWrite32 (HCI_BASE + HCI_UTRLRSR, 0);
  MmioWrite32 (HCI_BASE + HCI_IS, MmioRead32 (HCI_BASE + HCI_IS));
  FreeAlignedPages (mEngine, 1);
  mEngine = NULL;

  // Hand over even if the mode could not be confirmed; UfsPassThru's own
  // NOP and fDeviceInit checks decide whether the device is usable.
  return EFI_SUCCESS;
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
  UINT32      Hcs;
  EFI_STATUS  Status;

  if (CallbackPhase != EdkiiUfsHcPostHce) {
    return EFI_SUCCESS;
  }

  // One attempt only: a failed takeover is not retried by later connects.
  if (mTakeoverTried) {
    return EFI_ALREADY_STARTED;
  }

  mTakeoverTried = TRUE;

  Hcs = MmioRead32 (HCI_BASE + HCI_STATUS);
  if (((MmioRead32 (HCI_BASE + HCI_ENABLE) & HCI_ENABLE_HCE) == 0) ||
      ((Hcs & (HCI_STATUS_DP | HCI_STATUS_UCRDY)) != (HCI_STATUS_DP | HCI_STATUS_UCRDY)))
  {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: controller not left running (HCS 0x%08x)\n", Hcs));
    return EFI_NOT_READY;
  }

  if ((MmioRead32 (HCI_BASE + HCI_UTRLDBR) != 0) || (MmioRead32 (HCI_BASE + HCI_UTMRLDBR) != 0)) {
    DEBUG ((DEBUG_ERROR, "UfsPlatform: bootloader requests outstanding\n"));
    return EFI_NOT_READY;
  }

  // The list base registers may only change while the lists are stopped.
  MmioWrite32 (HCI_BASE + HCI_UTRLRSR, 0);
  MmioWrite32 (HCI_BASE + HCI_UTMRLRSR, 0);
  MmioWrite32 (HCI_BASE + HCI_IS, MmioRead32 (HCI_BASE + HCI_IS));

  if ((MmioRead32 (HCI_BASE + HCI_UICCMD) & 0xFF) == UIC_DME_HIBER_ENTER) {
    Status = UicHibernateExit ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return WakeDevice ();
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
    DEBUG ((DEBUG_WARN, "UfsPlatform: refused SCSI 0x%02x to LUN 0x%lx\n", ((UINT8 *)Packet->Cdb)[0], Lun));
    return EFI_UNSUPPORTED;
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

EFI_STATUS
EFIAPI
UfsPlatformDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  // No DMA may start before the UFS stream is routed; without it, leave
  // the controller to whoever comes next.
  Status = MapUfsStream ();
  if (EFI_ERROR (Status)) {
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
  return RegisterNonDiscoverableMmioDevice (
           NonDiscoverableDeviceTypeUfs,
           NonDiscoverableDeviceDmaTypeNonCoherent,
           NULL,
           NULL,
           1,
           HCI_BASE,
           HCI_SIZE
           );
}
