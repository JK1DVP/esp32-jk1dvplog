#!/usr/bin/env python3
"""DVPlogger serial terminal with microSD upload support (v2).

Normal operation is a transparent serial terminal.
Local keys:
  Ctrl-T U   upload a file to microSD using the sdput protocol
  Ctrl-T Y   upload a file using YMODEM
  Ctrl-T H   show local help
  Ctrl-T Ctrl-T  send a literal Ctrl-T
  Ctrl-]     quit

DVPlogger protocol:
  sdput <remote-name> <size> <crc32-hex>\r
  <- SDPUT READY ...
  -> exactly <size> raw bytes
  <- SDPUT OK ... / SDPUT ERROR ...
"""

import argparse
import os
import queue
import sys
import threading
import time
import zlib

try:
    import serial
except ImportError:
    serial = None

CTRL_T = 0x14
CTRL_RBRACKET = 0x1D

# X/YMODEM control bytes.
SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC_REQ = ord('C')
CPMEOF = 0x1A


class YmodemUserCancel(Exception):
    """Raised when the local user cancels an active YMODEM transfer."""


class TerminalOutput:
    """Normalize serial newlines for the local terminal only.

    DVPlogger may emit CR, LF, or CRLF depending on the code path.  On a Unix
    terminal, a bare LF moves down without necessarily returning to column 0,
    while translating every LF blindly can double-space CRLF.  This helper
    renders CR/LF/CRLF consistently as local CRLF without touching the serial
    byte stream or SDPUT binary data.
    """
    def __init__(self):
        self.prev_cr = False

    def write(self, data):
        out = bytearray()
        for b in data:
            if b == 13:  # CR
                out.extend(b"\r\n")
                self.prev_cr = True
            elif b == 10:  # LF
                if self.prev_cr:
                    # CRLF has already been rendered by the CR above.
                    self.prev_cr = False
                else:
                    out.extend(b"\r\n")
            else:
                self.prev_cr = False
                out.append(b)
        if out:
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()


terminal_output = TerminalOutput()


class SerialReader(threading.Thread):
    def __init__(self, ser, line_queue, stop_event, pause_event=None):
        super().__init__(daemon=True)
        self.ser = ser
        self.line_queue = line_queue
        self.stop_event = stop_event
        self.pause_event = pause_event or threading.Event()
        self.linebuf = bytearray()

    def run(self):
        while not self.stop_event.is_set():
            if self.pause_event.is_set():
                time.sleep(0.01)
                continue
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception as exc:
                self.line_queue.put(f"__SERIAL_ERROR__ {exc}")
                return
            if not data:
                continue

            try:
                terminal_output.write(data)
            except Exception:
                pass

            for b in data:
                if b in (10, 13):
                    if self.linebuf:
                        line = self.linebuf.decode("utf-8", errors="replace")
                        self.line_queue.put(line)
                        self.linebuf.clear()
                else:
                    if len(self.linebuf) < 4096:
                        self.linebuf.append(b)
                    else:
                        self.linebuf.clear()


def file_info(path):
    size = 0
    crc = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            crc = zlib.crc32(block, crc)
    return size, crc & 0xFFFFFFFF



def crc16_xmodem(data):
    """CRC-16/XMODEM used by XMODEM/YMODEM packet framing."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def ymodem_packet(block_no, payload, use_1k=False, pad_byte=CPMEOF):
    """Build one CRC-mode X/YMODEM packet."""
    block_size = 1024 if use_1k else 128
    if len(payload) > block_size:
        raise ValueError("payload too large for YMODEM packet")
    fill = bytes([pad_byte]) * (block_size - len(payload))
    body = payload + fill
    crc = crc16_xmodem(body)
    lead = STX if use_1k else SOH
    return bytes([lead, block_no & 0xFF, 0xFF - (block_no & 0xFF)]) + body + bytes([crc >> 8, crc & 0xFF])


def ymodem_read_control(ser, wanted, timeout, echo_text=False,
                         control_at_line_start=False, cancel_check=None):
    """Read until one of wanted control bytes is received.

    When ``control_at_line_start`` is true, printable control bytes such as
    YMODEM's CRC request ``'C'`` are accepted only at the start of a line.
    This avoids mistaking the 'C' in text like ``(CRC mode)`` for the actual
    protocol byte emitted after the receiver's prompt.
    """
    deadline = time.monotonic() + timeout
    text = bytearray()
    at_line_start = True

    def flush_text():
        nonlocal text
        if echo_text and text:
            try:
                terminal_output.write(bytes(text))
            except Exception:
                pass
            text.clear()

    while time.monotonic() < deadline:
        if cancel_check is not None and cancel_check():
            raise YmodemUserCancel()
        b = ser.read(1)
        if not b:
            continue
        v = b[0]

        accept_control = v in wanted
        if accept_control and control_at_line_start and v >= 0x20:
            accept_control = at_line_start

        if accept_control:
            flush_text()
            return v

        if echo_text and v not in (0x00,):
            text.append(v)
            if v in (10, 13) or len(text) >= 256:
                flush_text()

        if v in (10, 13):
            at_line_start = True
        elif v >= 0x20:
            at_line_start = False

    flush_text()
    raise TimeoutError("timeout waiting for YMODEM receiver")


def ymodem_read_error_tail(ser, timeout=0.5):
    """Collect printable receiver diagnostics after CAN for local display."""
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        b = ser.read(ser.in_waiting or 1)
        if not b:
            continue
        data.extend(b)
        if len(data) >= 1024:
            break
    if data:
        try:
            terminal_output.write(bytes(data))
        except Exception:
            pass


def ymodem_send_packet(ser, packet, retries=10, timeout=10.0,
                       cancel_check=None, stats=None):
    """Send one packet and wait for ACK, retrying on NAK/timeout."""
    for attempt in range(1, retries + 1):
        ser.write(packet)
        ser.flush()
        try:
            # Receiver diagnostics may be printed before CAN/NAK.  Echo them
            # while waiting so detailed YMODEM HEADER/SD errors are not lost.
            ctl = ymodem_read_control(
                ser, {ACK, NAK, CAN}, timeout,
                echo_text=True,
                cancel_check=cancel_check,
            )
        except TimeoutError:
            ctl = None
        if ctl == ACK:
            return True
        if ctl == CAN:
            # DVPlogger sends CAN CAN and then a printable diagnostic line.
            # Drain that tail before raising so the real receiver-side reason
            # is visible in the local terminal.
            ymodem_read_error_tail(ser, timeout=1.0)
            raise RuntimeError("YMODEM cancelled by receiver")
        if attempt < retries:
            if stats is not None:
                stats["retries"] = stats.get("retries", 0) + 1
                stats["last_attempt"] = attempt
            # Keep the transfer display readable.  Cumulative retries are
            # shown in the periodic progress line and final summary.
    return False


def ymodem_upload_file(ser, local_path, remote_name=None, pause_event=None,
                       retries=10, use_1k=False, cancel_check=None):
    """Send one file with YMODEM after asking DVPlogger to enter ymodem mode.

    The default data block size is 128 bytes (SOH), which is more tolerant of
    a busy ESP32 console/UART path.  Set use_1k=True to use 1024-byte STX
    blocks after the basic path is confirmed stable.
    """
    local_path = os.path.expanduser(local_path)
    if not os.path.isfile(local_path):
        print(f"\r\n[local] file not found: {local_path}")
        return False

    if not remote_name:
        remote_name = os.path.basename(local_path)
    remote_name = remote_name.replace(" ", "_")
    if "/" in remote_name or "\\" in remote_name:
        remote_name = os.path.basename(remote_name)

    size, crc32 = file_info(local_path)
    if size == 0:
        print("\r\n[local] refusing to send an empty file")
        return False

    block_size = 1024 if use_1k else 128
    mode_name = "1K/STX" if use_1k else "128/SOH"
    stats = {"retries": 0}
    print(f"\r\n[local] YMODEM {local_path} -> {remote_name} "
          f"({size} bytes, CRC32={crc32:08X}, blocks={mode_name})")

    if pause_event is not None:
        pause_event.set()
        # Give SerialReader time to leave any blocking read before we take over.
        time.sleep(0.15)

    try:
        # Discard stale terminal input before entering the binary protocol.
        try:
            ser.reset_input_buffer()
        except Exception:
            pass

        ser.write(b"ymodem\r")
        ser.flush()

        # Receiver requests CRC mode with 'C'.
        ctl = ymodem_read_control(
            ser, {CRC_REQ, CAN}, 12.0,
            echo_text=True,
            control_at_line_start=True,
            cancel_check=cancel_check,
        )
        if ctl == CAN:
            ymodem_read_error_tail(ser)
            raise RuntimeError("YMODEM cancelled by receiver")

        # Block 0: filename NUL filesize NUL.  YMODEM permits further metadata,
        # but DVPlogger needs only these two fields.
        meta = remote_name.encode("utf-8", errors="replace") + b"\0" + str(size).encode("ascii") + b"\0"
        if len(meta) > 128:
            raise RuntimeError("remote filename/metadata is too long for YMODEM block 0")
        if not ymodem_send_packet(
            ser, ymodem_packet(0, meta, use_1k=False, pad_byte=0x00),
            retries=retries,
            cancel_check=cancel_check,
            stats=stats,
        ):
            raise RuntimeError("YMODEM block 0 was not acknowledged")

        ctl = ymodem_read_control(
            ser, {CRC_REQ, CAN}, 10.0,
            cancel_check=cancel_check,
        )
        if ctl == CAN:
            ymodem_read_error_tail(ser)
            raise RuntimeError("YMODEM cancelled by receiver after block 0")

        sent = 0
        block_no = 1
        # Report more often so progress remains visible even at 115200 bps.
        next_report = 16 * 1024
        t0 = time.monotonic()

        with open(local_path, "rb") as f:
            while True:
                payload = f.read(block_size)
                if not payload:
                    break
                packet = ymodem_packet(block_no, payload, use_1k=use_1k)
                if not ymodem_send_packet(
                    ser, packet, retries=retries,
                    cancel_check=cancel_check,
                    stats=stats,
                ):
                    raise RuntimeError(f"YMODEM data block {block_no} was not acknowledged")
                sent += len(payload)
                block_no = (block_no + 1) & 0xFF

                if sent >= next_report or sent == size:
                    elapsed = max(time.monotonic() - t0, 0.001)
                    print(f"\r[local] sent {sent}/{size} "
                          f"({100.0*sent/size:5.1f}%) "
                          f"{sent/elapsed/1024:6.1f} KiB/s "
                          f"retries={stats['retries']}",
                          end="", flush=True)
                    next_report += 16 * 1024
        print()

        # Standard YMODEM end-of-file handshake: EOT -> NAK -> EOT -> ACK -> 'C'.
        ser.write(bytes([EOT]))
        ser.flush()
        ctl = ymodem_read_control(
            ser, {NAK, ACK, CAN}, 10.0,
            cancel_check=cancel_check,
        )
        if ctl == CAN:
            ymodem_read_error_tail(ser)
            raise RuntimeError("YMODEM cancelled at EOT")
        if ctl == NAK:
            ser.write(bytes([EOT]))
            ser.flush()
            ctl = ymodem_read_control(
                ser, {ACK, CAN}, 10.0,
                cancel_check=cancel_check,
            )
            if ctl == CAN:
                ymodem_read_error_tail(ser)
                raise RuntimeError("YMODEM cancelled at final EOT")
        if ctl != ACK:
            raise RuntimeError("YMODEM EOT was not acknowledged")

        ctl = ymodem_read_control(
            ser, {CRC_REQ, CAN}, 10.0,
            cancel_check=cancel_check,
        )
        if ctl == CAN:
            ymodem_read_error_tail(ser)
            raise RuntimeError("YMODEM cancelled before final block")

        # Empty block 0 terminates a YMODEM batch.
        if not ymodem_send_packet(
            ser, ymodem_packet(0, b"", use_1k=False, pad_byte=0x00),
            retries=retries,
            cancel_check=cancel_check,
            stats=stats,
        ):
            raise RuntimeError("YMODEM final empty block 0 was not acknowledged")

        # The receiver prints its final read-back CRC32 verification after
        # ACKing the empty batch header.  Preserve and show that tail instead
        # of discarding it when terminal mode resumes.
        ymodem_read_error_tail(ser, timeout=0.75)
        print(f"[local] YMODEM complete: {remote_name} size={size} "
              f"CRC32={crc32:08X} retries={stats['retries']}")
        return True

    except YmodemUserCancel:
        print("\r\n[local] YMODEM cancelled by user")
        try:
            ser.write(bytes([CAN, CAN, CAN, CAN, CAN]))
            ser.flush()
        except Exception:
            pass
        return False

    except KeyboardInterrupt:
        print("\r\n[local] YMODEM cancelled by keyboard interrupt")
        try:
            ser.write(bytes([CAN, CAN, CAN, CAN, CAN]))
            ser.flush()
        except Exception:
            pass
        return False

    except Exception as exc:
        print(f"\r\n[local] YMODEM failed: {exc}")
        try:
            ser.write(bytes([CAN, CAN, CAN, CAN, CAN]))
            ser.flush()
        except Exception:
            pass
        return False
    finally:
        if pause_event is not None:
            # Do not reset the input buffer here: the receiver may have just
            # printed the final "YMODEM OK ... verify=OK" diagnostic.
            time.sleep(0.05)
            pause_event.clear()


def drain_queue(q):
    try:
        while True:
            q.get_nowait()
    except queue.Empty:
        pass


def wait_for(q, wanted, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        left = deadline - time.monotonic()
        try:
            line = q.get(timeout=max(0.05, min(left, 0.5)))
        except queue.Empty:
            continue
        if line.startswith("__SERIAL_ERROR__"):
            raise RuntimeError(line)
        if any(token in line for token in wanted):
            return line
    raise TimeoutError("timeout waiting for " + "/".join(wanted))


def upload_file(ser, q, local_path, remote_name=None, chunk=1024):
    local_path = os.path.expanduser(local_path)
    if not os.path.isfile(local_path):
        print(f"\r\n[local] file not found: {local_path}")
        return False

    if not remote_name:
        remote_name = os.path.basename(local_path)
    remote_name = remote_name.replace(" ", "_")

    size, crc = file_info(local_path)
    if size == 0:
        print("\r\n[local] refusing to send an empty file")
        return False

    drain_queue(q)
    command = f"sdput {remote_name} {size} {crc:08X}\r"
    print(f"\r\n[local] upload {local_path} -> {remote_name} ({size} bytes, CRC32={crc:08X})")
    ser.write(command.encode("ascii"))
    ser.flush()

    try:
        line = wait_for(q, ("SDPUT READY", "SDPUT ERROR"), 10.0)
    except Exception as exc:
        print(f"\r\n[local] {exc}")
        return False
    if "SDPUT ERROR" in line:
        print(f"\r\n[local] device rejected upload: {line}")
        return False

    sent = 0
    next_report = 64 * 1024
    t0 = time.monotonic()
    try:
        with open(local_path, "rb") as f:
            while True:
                data = f.read(chunk)
                if not data:
                    break
                ser.write(data)
                sent += len(data)
                # Let the ESP32 drain its UART ring and write to SD.  At
                # 115200 this is a small pacing delay, not a severe slowdown.
                if ser.baudrate <= 115200:
                    time.sleep(0.002)
                if sent >= next_report or sent == size:
                    elapsed = max(time.monotonic() - t0, 0.001)
                    print(f"\r[local] sent {sent}/{size} ({100.0*sent/size:5.1f}%) {sent/elapsed/1024:6.1f} KiB/s", end="", flush=True)
                    next_report += 64 * 1024
        ser.flush()
        print()
    except Exception as exc:
        print(f"\r\n[local] send failed: {exc}")
        return False

    try:
        line = wait_for(q, ("SDPUT OK", "SDPUT ERROR"), 30.0)
    except Exception as exc:
        print(f"[local] {exc}")
        return False
    if "SDPUT OK" not in line:
        print(f"[local] upload failed: {line}")
        return False

    print("[local] upload complete; terminal mode resumed")
    return True


def local_help(ymodem_use_1k=False):
    print("\r\n--- local terminal commands ---")
    print("Ctrl-T U       upload a file using legacy sdput")
    print("Ctrl-T Y       upload a file using YMODEM")
    print(f"               YMODEM data blocks: {'1K/STX' if ymodem_use_1k else '128/SOH'}")
    print("Ctrl-T H       show this help")
    print("Ctrl-T Ctrl-T  send literal Ctrl-T")
    print("Ctrl-]         quit")
    print("-------------------------------")


def prompt_upload(ser, q):
    restore = None
    if os.name != "nt":
        import termios
        fd = sys.stdin.fileno()
        saved = termios.tcgetattr(fd)
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        restore = (fd, saved)
    try:
        print("\r")
        local_path = input("Local file: ").strip()
        if not local_path:
            return
        default_remote = os.path.basename(os.path.expanduser(local_path)).replace(" ", "_")
        remote = input(f"Remote microSD name [{default_remote}]: ").strip() or default_remote
        upload_file(ser, q, local_path, remote)
    finally:
        if restore is not None:
            import tty
            tty.setraw(restore[0])


def terminal_unix(ser, q, stop_event, pause_event, ymodem_use_1k=False):
    import select
    import termios
    import tty

    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    prefix = False

    def ymodem_cancel_check():
        readable, _, _ = select.select([fd], [], [], 0)
        if not readable:
            return False
        data = os.read(fd, 1)
        if not data:
            return False
        # Ctrl-] is the normal terminal quit key; Ctrl-C is also accepted
        # during a binary transfer when the tty is in raw mode.
        return data[0] in (CTRL_RBRACKET, 0x03)

    tty.setraw(fd)
    try:
        local_help(ymodem_use_1k)
        while not stop_event.is_set():
            readable, _, _ = select.select([fd], [], [], 0.1)
            if not readable:
                continue
            data = os.read(fd, 1)
            if not data:
                break
            b = data[0]
            if prefix:
                prefix = False
                if b in (ord('u'), ord('U')):
                    termios.tcsetattr(fd, termios.TCSADRAIN, saved)
                    try:
                        local_path = input("\r\nLocal file: ").strip()
                        if local_path:
                            default_remote = os.path.basename(os.path.expanduser(local_path)).replace(" ", "_")
                            remote = input(f"Remote microSD name [{default_remote}]: ").strip() or default_remote
                            upload_file(ser, q, local_path, remote)
                    finally:
                        tty.setraw(fd)
                elif b in (ord('y'), ord('Y')):
                    termios.tcsetattr(fd, termios.TCSADRAIN, saved)
                    try:
                        local_path = input("\r\nLocal file for YMODEM: ").strip()
                        if local_path:
                            default_remote = os.path.basename(os.path.expanduser(local_path)).replace(" ", "_")
                            remote = input(f"Remote microSD name [{default_remote}]: ").strip() or default_remote
                            # Switch back to raw mode for the transfer so
                            # Ctrl-] / Ctrl-C can be detected immediately.
                            tty.setraw(fd)
                            ymodem_upload_file(
                                ser, local_path, remote,
                                pause_event=pause_event,
                                use_1k=ymodem_use_1k,
                                cancel_check=ymodem_cancel_check,
                            )
                    finally:
                        tty.setraw(fd)
                elif b in (ord('h'), ord('H')):
                    local_help(ymodem_use_1k)
                elif b == CTRL_T:
                    ser.write(bytes([CTRL_T]))
                else:
                    print("\r\n[local] unknown Ctrl-T command; Ctrl-T H for help")
                continue

            if b == CTRL_RBRACKET:
                break
            if b == CTRL_T:
                prefix = True
                continue

            # Unix raw mode normally returns LF (0x0A) for Enter.  DVPlogger's
            # command console expects CR (0x0D), matching the Windows path and
            # common serial terminals such as Tera Term/miniterm.
            if b in (10, 13):
                ser.write(b'\r')
            else:
                ser.write(data)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)


def terminal_windows(ser, q, stop_event, pause_event, ymodem_use_1k=False):
    import msvcrt

    def ymodem_cancel_check():
        if not msvcrt.kbhit():
            return False
        ch = msvcrt.getwch()
        return ord(ch) in (CTRL_RBRACKET, 0x03)

    local_help(ymodem_use_1k)
    prefix = False
    while not stop_event.is_set():
        if not msvcrt.kbhit():
            time.sleep(0.02)
            continue
        ch = msvcrt.getwch()
        b = ord(ch)
        if prefix:
            prefix = False
            if ch.lower() == 'u':
                local_path = input("\r\nLocal file: ").strip()
                if local_path:
                    default_remote = os.path.basename(os.path.expanduser(local_path)).replace(" ", "_")
                    remote = input(f"Remote microSD name [{default_remote}]: ").strip() or default_remote
                    upload_file(ser, q, local_path, remote)
            elif ch.lower() == 'y':
                local_path = input("\r\nLocal file for YMODEM: ").strip()
                if local_path:
                    default_remote = os.path.basename(os.path.expanduser(local_path)).replace(" ", "_")
                    remote = input(f"Remote microSD name [{default_remote}]: ").strip() or default_remote
                    ymodem_upload_file(
                        ser, local_path, remote,
                        pause_event=pause_event,
                        use_1k=ymodem_use_1k,
                        cancel_check=ymodem_cancel_check,
                    )
            elif ch.lower() == 'h':
                local_help(ymodem_use_1k)
            elif b == CTRL_T:
                ser.write(bytes([CTRL_T]))
            continue
        if b == CTRL_RBRACKET:
            break
        if b == CTRL_T:
            prefix = True
            continue
        if ch == '\r':
            ser.write(b'\r')
        else:
            ser.write(ch.encode(errors='replace'))


def main():
    ap = argparse.ArgumentParser(description="DVPlogger terminal + microSD uploader v13")
    ap.add_argument("port", nargs="?", help="serial port, e.g. /dev/ttyUSB0 or COM4")
    ap.add_argument("baud", nargs="?", type=int, default=115200)
    ap.add_argument("--upload", metavar="FILE", help="upload FILE with legacy sdput, then enter terminal")
    ap.add_argument("--ymodem", metavar="FILE", help="upload FILE with YMODEM, then enter terminal")
    ap.add_argument("--ymodem-1k", action="store_true",
                    help="use 1K/STX YMODEM data blocks (default: safer 128/SOH)")
    ap.add_argument("--remote", metavar="NAME", help="remote filename for --upload/--ymodem")
    ap.add_argument("--crc32", metavar="FILE", help="print FILE size and CRC32, then exit")
    args = ap.parse_args()

    if args.crc32:
        size, crc = file_info(os.path.expanduser(args.crc32))
        print(f"{args.crc32}: size={size} CRC32={crc:08X}")
        return

    if not args.port:
        ap.error("serial port is required unless --crc32 is used")
    if serial is None:
        print("pyserial is required: python3 -m pip install pyserial", file=sys.stderr)
        raise SystemExit(2)

    # Match pyserial miniterm's port-open behaviour as closely as possible.
    # In particular, do NOT force DTR/RTS here.  Some ESP32 USB/UART paths
    # react to an explicit modem-control transition by toggling reset/boot,
    # while miniterm opens the same port without causing a reset.
    ser = serial.serial_for_url(
        args.port,
        args.baud,
        do_not_open=True,
    )
    ser.timeout = 0.1
    ser.write_timeout = 10
    ser.rtscts = False
    ser.dsrdtr = False
    ser.xonxoff = False
    ser.open()

    q = queue.Queue()
    stop_event = threading.Event()
    reader_pause_event = threading.Event()
    reader = SerialReader(ser, q, stop_event, reader_pause_event)
    reader.start()

    try:
        print(f"--- DVPlogger terminal v13 on {args.port} {args.baud} ---")
        if args.upload:
            upload_file(ser, q, args.upload, args.remote)
        if args.ymodem:
            ymodem_upload_file(
                ser, args.ymodem, args.remote,
                pause_event=reader_pause_event,
                use_1k=args.ymodem_1k,
            )
        if os.name == "nt":
            terminal_windows(
                ser, q, stop_event, reader_pause_event,
                ymodem_use_1k=args.ymodem_1k,
            )
        else:
            terminal_unix(
                ser, q, stop_event, reader_pause_event,
                ymodem_use_1k=args.ymodem_1k,
            )
    finally:
        stop_event.set()
        try:
            ser.close()
        except Exception:
            pass
        print("\r\n--- terminal closed ---")


if __name__ == "__main__":
    main()
