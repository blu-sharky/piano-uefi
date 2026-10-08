  #
  # Minimal first-pass DXE set: open-source components only.
  # No Binaries/ (piano stock DXE extraction is a later stage),
  # no DisplayDxe (SimpleFb on the splash framebuffer instead),
  # no UFS / USB / Clock — added one hardware class per stage (U2/U3…);
  # U2 (buttons) is the block at the end of this file.
  #
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  # DisplayDxe intentionally absent in the first pass (panel power-rail reset risk).
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  #
  # U2: buttons. The closed-source SM8750 chain from Binaries/9707f, reduced
  # to the closure ButtonsDxe needs. Depex (decoded from the .depex files):
  #   ButtonsDxe      SimpleTextIn AND PlatformInfo AND two PMIC protocols
  #   PmicDxeLa       DALSYS AND SPMI
  #   PlatformInfoDxe DALSYS;  ChipInfoDxe DALSYS AND HWIO;  HWIODxe DALSYS
  #   DALSYSDxe       SMEM;    SmemDxe, SPMIDxe, ULogDxe TRUE
  # All three piano keys sit on PMICs (power: PMK8550 PON pwrkey, volume
  # down: PON resin, volume up: PM8550 GPIO6), so no TLMM is loaded.
  #
  INF Binaries/9707f/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/9707f/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
  INF Binaries/9707f/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
