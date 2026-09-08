# DVPlogger RIG Settings

RIG definitions are comma-separated `KEY:VALUE` fields.

```text
NAME:FTDX10,TP:1_2,P:-1,B:38400,CW:4,FSK:3,PTT:2,RP:0
```

## CW, RTTY FSK, and PTT

### `CW:0..4` — CW key output

| Value | Output |
|---:|---|
| 0 | LED/GPIO CW output |
| 1 | KEY1 |
| 2 | KEY2 |
| 3 | USB DTR |
| 4 | USB RTS |

For USB rigs, select DTR or RTS to match the rig's PC KEYING setting. DVPlogger does not force a fixed DTR/RTS assignment for Yaesu rigs.

### `FSK:0..4` — RTTY FSK key output

| Value | Output |
|---:|---|
| 0 | LED/GPIO key output |
| 1 | KEY1 |
| 2 | KEY2 |
| 3 | USB DTR |
| 4 | USB RTS |

`FSK` is independent of `CW`. If `FSK` is omitted, the `CW` port is used for backward compatibility.

This permits, for example, `CW:4,FSK:3` when CW uses RTS but RTTY FSK uses DTR.

### `RP:0|1` — RTTY polarity

| Value | Meaning |
|---:|---|
| 0 | Normal |
| 1 | Reverse |

`RP` is stored per rig. If omitted, the legacy/global `rttyinvert` setting is used.

If RTTY is transmitted but cannot be decoded while mode, baud rate, and shift are correct, try the opposite `RP` value.

### `PTT:0..4` — additional PTT method

The three hardware MIC/PTT outputs are controlled by the selected RADIO slot regardless of `PTT:`:

| RADIO | Hardware PTT |
|---:|---|
| Radio0 | PTT1 |
| Radio1 | PTT2 |
| Radio2 | PTT3 |

`PTT:` selects an **additional** PTT method:

| Value | Additional PTT |
|---:|---|
| 0 | Don't care / none |
| 1 | Don't care / none |
| 2 | CAT / CI-V PTT |
| 3 | USB DTR |
| 4 | USB RTS |

Thus `PTT:2` means that the RADIO-associated hardware PTT is switched and CAT/CI-V PTT is also sent.

## Yaesu USB example

Example where CW uses RTS, RTTY FSK uses DTR, and PTT uses CAT in addition to the hardware PTT:

```text
CW:4,FSK:3,PTT:2,RP:0
```

Meaning:

```text
CW key         = USB RTS
RTTY FSK       = USB DTR
Hardware PTT   = PTT1/PTT2/PTT3 according to Radio0/1/2
Additional PTT = CAT
RTTY polarity  = Normal
```

If RTTY polarity is reversed, use `RP:1`.

## Other RIG fields

| Field | Meaning |
|---|---|
| `B:<baud>` | CAT baud rate |
| `P:<port>` | CAT port (`-2` Manual, `-1` USB, `1` Bluetooth, `2` CI-V, `3` CAT, `4` CAT2) |
| `ADR:<addr>` | CI-V address |
| `NAME:<name>` | Rig name |
| `R:0|1` | CAT serial polarity |
| `BM:<hex>` | Band-disable mask |
| `TP:<cat_type>_<rig_type>` | CAT protocol/transport and rig type |
| `XVTR:<...>` | Transverter frequency definitions |

## Backward compatibility

- Existing `CW` behavior is unchanged.
- If `FSK` is absent, RTTY FSK uses the `CW` port.
- If `RP` is absent, the global `rttyinvert` setting is used.
- Existing `PTT:2` continues to select CAT/CI-V PTT in addition to the RADIO-associated hardware PTT.

For new USB RTTY configurations, explicitly specifying `FSK` and `RP` is recommended.
