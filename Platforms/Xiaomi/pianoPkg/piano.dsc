##
#  piano (Xiaomi Pad 8 Pro, SM8750) UEFI platform package.
#  Modeled on Platforms/OnePlus/dodgePkg (same SoC, Project Silicium).
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##
[Defines]
  PLATFORM_NAME                  = piano
  PLATFORM_GUID                  = 44D7CF31-1766-4282-9BDA-235864FCE942
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/pianoPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = pianoPkg/piano.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8750-AB
  # 1 = SM8750-3-AB
  # 2 = SM8750-AC
  # piano 的具体变体 [待验证]（见 AGENTS_Report/UEFI-PORT-ANALYSIS.md §3）
  #
  SOC_TYPE                       = 0

!include PakalaPkg/PakalaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack（与 MemoryMapLib 中 UEFI_Stack 条目一致）
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Pad 8 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"piano"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"piano"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"piano"

  #
  # Simple Frame Buffer：ABL 留下的启动画面帧缓冲（双 DSI LCD，stride 12800）
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|3200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2136
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|pianoPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|pianoPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # CrabApple boot manager (Common/CrabApple)
  #
  CrabAppleLib|CrabApplePkg/Library/CrabAppleLib/CrabAppleLib.inf

[PcdsFixedAtBuild]
  # DEBUG output is drawn on the same framebuffer as the CrabApple menu;
  # keep CrabApple's own log lines below the printed levels.
  gCrabApplePkgTokenSpaceGuid.PcdCrabAppleDebugLevel|0x00400000

[Components]
  #
  # CrabApple: the application plus the registrar that makes it the first
  # boot option. Pure UEFI software, no hardware access of its own.
  #
  CrabApplePkg/Application/CrabApple/CrabApple.inf
  CrabApplePkg/Driver/CrabAppleDxe/CrabAppleDxe.inf
