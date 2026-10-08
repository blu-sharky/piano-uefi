#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/TimerLib.h>

//
// piano keys, read through the SPMI PMIC arbiter (v7) observer channel.
//
// All three keys sit on PMICs:
//   Power        PMK8550 (SID 0) PON_HLOS RT_STS 0x1310 bit 7, active high
//   Volume Down  PMK8550 (SID 0) PON_HLOS RT_STS 0x1310 bit 6, active high
//   Volume Up    PM8550  (SID 1) GPIO6    RT_STS 0x8D10 bit 0, active low
//
// This library only reads. An observer-channel command makes the arbiter
// issue an SPMI register read; no PMIC register is ever written, and the
// GPIO6 configuration (input, pull-up) is left as the bootloader set it.
// Register layout and arbiter access follow Linux spmi-pmic-arb.c (v7),
// pm8941-pwrkey.c (PON gen3) and pinctrl-spmi-gpio.c (LV/MV GPIO).
//

#define SPMI_CORE_BASE           0x0C400000
#define SPMI_CORE_SIZE           0x3000
#define SPMI_CNFG_BASE           0x0C42D000
#define SPMI_OBSRVR_BASE         0x0C440000

#define SPMI_ARB_VERSION         0x0000
#define SPMI_ARB_FEATURES        0x0004
#define SPMI_ARB_V7_MIN          0x70000000
#define SPMI_ARB_V8_MIN          0x80000000
#define SPMI_APID_COUNT_MASK     0x7FF
#define SPMI_APID_MAP(n)         (0x2000 + 4 * (n))
#define SPMI_APID_OWNER(n)       (0x700 + 4 * (n))
#define SPMI_OWNER_EE(x)         ((x) & 0x7)

#define SPMI_EE                  0
#define SPMI_OBS_CHANNEL(apid)   (SPMI_OBSRVR_BASE + 0x8000 * SPMI_EE + 0x20 * (apid))
#define SPMI_CHAN_CMD            0x00
#define SPMI_CHAN_STATUS         0x08
#define SPMI_CHAN_RDATA0         0x18
#define SPMI_STATUS_DONE         BIT0
#define SPMI_STATUS_ERROR        (BIT1 | BIT2 | BIT3)
#define SPMI_OP_EXT_READL        1
#define SPMI_TIMEOUT_US          1000

#define PMIC_PERIPH_SUBTYPE      0x05
#define PMIC_RT_STS              0x10
#define PMIC_GPIO_MODE_CTL       0x40
#define PMIC_GPIO_EN_CTL         0x46
#define PMIC_GPIO_MASTER_EN      BIT7

#define PON_SID                  0
#define PON_HLOS_BASE            0x1300
#define PON_KPDPWR_N_SET         BIT7
#define PON_RESIN_N_SET          BIT6

#define VOLUP_SID                1
#define VOLUP_GPIO_BASE          0x8D00
#define VOLUP_GPIO_VAL           BIT0

#define MS2NS(ms)                (((UINT64)(ms)) * 1000000ULL)
#define KEY_LONGPRESS_NS         MS2NS (500)
#define KEY_REPEAT_NS            MS2NS (100)

typedef struct {
  EFI_KEY_DATA  KeyData;       // reported on a short press
  EFI_KEY_DATA  LongKeyData;   // reported once on a long press (ScanCode 0 = repeat KeyData instead)
  UINT8         Sid;
  UINT16        Register;
  UINT8         Mask;
  BOOLEAN       ActiveLow;
  BOOLEAN       Present;
  UINT16        Apid;
  BOOLEAN       Pressed;
  BOOLEAN       Reported;      // a long press or repeat already went out for this press
  UINT64        Time;
} PIANO_KEY;

STATIC PIANO_KEY  mKeys[] = {
  // Power: short press = Enter, long press = Escape
  { { { SCAN_NULL, CHAR_CARRIAGE_RETURN } }, { { SCAN_ESC, CHAR_NULL } },
    PON_SID, PON_HLOS_BASE + PMIC_RT_STS, PON_KPDPWR_N_SET, FALSE },
  // Volume Down: Down, repeats while held
  { { { SCAN_DOWN, CHAR_NULL } }, { { SCAN_NULL, CHAR_NULL } },
    PON_SID, PON_HLOS_BASE + PMIC_RT_STS, PON_RESIN_N_SET, FALSE },
  // Volume Up: Up, repeats while held
  { { { SCAN_UP, CHAR_NULL } }, { { SCAN_NULL, CHAR_NULL } },
    VOLUP_SID, VOLUP_GPIO_BASE + PMIC_RT_STS, VOLUP_GPIO_VAL, TRUE },
};

STATIC
BOOLEAN
SpmiFindApid (
  IN  UINT8   Sid,
  IN  UINT16  Register,
  OUT UINT16  *Apid
  )
{
  UINT32   Version;
  UINT32   ApidCount;
  UINT32   Map;
  UINT16   Ppid;
  UINT16   Index;
  BOOLEAN  Found;

  Version = MmioRead32 (SPMI_CORE_BASE + SPMI_ARB_VERSION);
  if ((Version < SPMI_ARB_V7_MIN) || (Version >= SPMI_ARB_V8_MIN)) {
    return FALSE;
  }

  ApidCount = MmioRead32 (SPMI_CORE_BASE + SPMI_ARB_FEATURES) & SPMI_APID_COUNT_MASK;
  Ppid      = (UINT16)((Sid << 8) | (Register >> 8));
  Found     = FALSE;

  // Same rule as Linux: the first mapping of the PPID, or a later one this
  // execution environment owns.
  for (Index = 0; (Index < ApidCount) && (SPMI_APID_MAP (Index) < SPMI_CORE_SIZE); Index++) {
    Map = MmioRead32 (SPMI_CORE_BASE + SPMI_APID_MAP (Index));
    if ((Map == 0) || (((Map >> 8) & 0xFFF) != Ppid)) {
      continue;
    }

    if (!Found || (SPMI_OWNER_EE (MmioRead32 (SPMI_CNFG_BASE + SPMI_APID_OWNER (Index))) == SPMI_EE)) {
      *Apid = Index;
      Found = TRUE;
    }
  }

  return Found;
}

STATIC
EFI_STATUS
SpmiReadByte (
  IN  UINT16  Apid,
  IN  UINT16  Register,
  OUT UINT8   *Value
  )
{
  UINTN   Channel;
  UINT32  Status;
  UINTN   Timeout;

  Channel = SPMI_OBS_CHANNEL (Apid);
  MmioWrite32 (Channel + SPMI_CHAN_CMD, (SPMI_OP_EXT_READL << 27) | ((Register & 0xFF) << 4));

  for (Timeout = 0; Timeout < SPMI_TIMEOUT_US; Timeout++) {
    Status = MmioRead32 (Channel + SPMI_CHAN_STATUS);
    if ((Status & SPMI_STATUS_DONE) != 0) {
      if ((Status & SPMI_STATUS_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }

      *Value = (UINT8)MmioRead32 (Channel + SPMI_CHAN_RDATA0);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
BOOLEAN
KeyIsPressed (
  IN PIANO_KEY  *Key
  )
{
  UINT8  Value;

  // A failed read counts as released; never block the console.
  if (!Key->Present || EFI_ERROR (SpmiReadByte (Key->Apid, Key->Register, &Value))) {
    return FALSE;
  }

  return (BOOLEAN)(((Value & Key->Mask) != 0) != Key->ActiveLow);
}

RETURN_STATUS
EFIAPI
KeypadDeviceConstructor (
  VOID
  )
{
  UINTN   Index;
  UINT16  Apid;
  UINT8   Subtype;
  UINT8   Mode;
  UINT8   Enable;

  DEBUG ((DEBUG_WARN, "Keypad: SPMI arbiter version 0x%08x\n", MmioRead32 (SPMI_CORE_BASE + SPMI_ARB_VERSION)));

  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    mKeys[Index].Present = SpmiFindApid (mKeys[Index].Sid, mKeys[Index].Register, &Apid);
    if (!mKeys[Index].Present) {
      DEBUG ((DEBUG_WARN, "Keypad: key %u: no APID for SID %u 0x%04x\n", Index, mKeys[Index].Sid, mKeys[Index].Register));
      continue;
    }

    mKeys[Index].Apid = Apid;
    Subtype           = 0;
    SpmiReadByte (Apid, (mKeys[Index].Register & 0xFF00) | PMIC_PERIPH_SUBTYPE, &Subtype);
    DEBUG ((DEBUG_WARN, "Keypad: key %u: SID %u 0x%04x APID %u subtype 0x%02x\n", Index, mKeys[Index].Sid, mKeys[Index].Register, Apid, Subtype));
  }

  // Volume Up only works if the bootloader left GPIO6 enabled; never fix it up here.
  if (mKeys[2].Present) {
    Mode   = 0;
    Enable = 0;
    SpmiReadByte (mKeys[2].Apid, VOLUP_GPIO_BASE | PMIC_GPIO_MODE_CTL, &Mode);
    SpmiReadByte (mKeys[2].Apid, VOLUP_GPIO_BASE | PMIC_GPIO_EN_CTL, &Enable);
    DEBUG ((DEBUG_WARN, "Keypad: GPIO6 mode 0x%02x enable 0x%02x\n", Mode, Enable));
    if ((Enable & PMIC_GPIO_MASTER_EN) == 0) {
      mKeys[2].Present = FALSE;
    }
  }

  return RETURN_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceReset (
  KEYPAD_DEVICE_PROTOCOL  *This
  )
{
  UINTN  Index;

  // A key that already reads pressed (still held, or a line that idles
  // low) is ignored until it has been released once.
  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    mKeys[Index].Pressed  = KeyIsPressed (&mKeys[Index]);
    mKeys[Index].Reported = TRUE;
    mKeys[Index].Time     = 0;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
KeyUpdate (
  IN PIANO_KEY          *Key,
  IN KEYPAD_RETURN_API  *KeypadReturnApi,
  IN BOOLEAN            IsPressed,
  IN UINT64             Delta
  )
{
  if (!Key->Pressed) {
    if (IsPressed) {
      Key->Pressed  = TRUE;
      Key->Reported = FALSE;
      Key->Time     = 0;
    }

    return;
  }

  Key->Time += Delta;

  if (!IsPressed) {
    // Short press: report on release so a long press is not also a short one.
    if (!Key->Reported) {
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Key->KeyData);
    }

    Key->Pressed = FALSE;
    return;
  }

  if (Key->LongKeyData.Key.ScanCode != SCAN_NULL) {
    if (!Key->Reported && (Key->Time >= KEY_LONGPRESS_NS)) {
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Key->LongKeyData);
      Key->Reported = TRUE;
    }
  } else if (Key->Time >= (Key->Reported ? KEY_REPEAT_NS : KEY_LONGPRESS_NS)) {
    KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Key->KeyData);
    Key->Reported = TRUE;
    Key->Time     = 0;
  }
}

EFI_STATUS
KeypadDeviceGetKeys (
  KEYPAD_DEVICE_PROTOCOL  *This,
  KEYPAD_RETURN_API       *KeypadReturnApi,
  UINT64                  Delta
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    if (mKeys[Index].Present) {
      KeyUpdate (&mKeys[Index], KeypadReturnApi, KeyIsPressed (&mKeys[Index]), Delta);
    }
  }

  return EFI_SUCCESS;
}
