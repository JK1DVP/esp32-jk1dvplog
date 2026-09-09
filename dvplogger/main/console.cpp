/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
// Copyright (c) 2021-2024 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "cmd_interp.h"
#include "main.h"
#include "ui.h"
#include "console.h"
#include "so2r.h"
#include "SD.h"



namespace {
struct SdPutState {
  bool active;
  File file;
  Stream *out;
  uint32_t expected_size;
  uint32_t received;
  uint32_t expected_crc;
  uint32_t crc;
  uint32_t last_rx_ms;
  uint32_t next_progress;
  char target[96];
  char temp[112];
  char backup[112];
};

static SdPutState sdput = {};

static uint32_t sdput_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
  }
  return crc;
}

static void sdput_reset_state() {
  if (sdput.file) sdput.file.close();
  sdput.active = false;
  sdput.out = nullptr;
}

static void sdput_abort(const char *reason) {
  Stream *out = sdput.out ? sdput.out : console;
  if (sdput.file) sdput.file.close();
  if (sdput.temp[0] && SD.exists(sdput.temp)) SD.remove(sdput.temp);
  if (out) {
    out->print("SDPUT ERROR ");
    out->println(reason ? reason : "aborted");
  }
  sdput_reset_state();
}

static bool sdput_commit() {
  Stream *out = sdput.out ? sdput.out : console;
  if (sdput.file) sdput.file.close();

  const uint32_t actual_crc = ~sdput.crc;
  if (sdput.received != sdput.expected_size) {
    sdput_abort("size mismatch");
    return false;
  }
  if (actual_crc != sdput.expected_crc) {
    if (out) {
      out->printf("SDPUT ERROR crc expected=%08lX actual=%08lX\r\n",
                  (unsigned long)sdput.expected_crc,
                  (unsigned long)actual_crc);
    }
    if (SD.exists(sdput.temp)) SD.remove(sdput.temp);
    sdput_reset_state();
    return false;
  }

  const bool had_target = SD.exists(sdput.target);
  if (SD.exists(sdput.backup)) SD.remove(sdput.backup);
  if (had_target && !SD.rename(sdput.target, sdput.backup)) {
    sdput_abort("cannot backup old file");
    return false;
  }
  if (!SD.rename(sdput.temp, sdput.target)) {
    if (had_target && SD.exists(sdput.backup))
      SD.rename(sdput.backup, sdput.target);
    sdput_abort("cannot rename temp file");
    return false;
  }
  if (had_target && SD.exists(sdput.backup)) SD.remove(sdput.backup);

  if (out) {
    out->printf("SDPUT OK name=%s size=%lu crc=%08lX\r\n",
                sdput.target,
                (unsigned long)sdput.received,
                (unsigned long)actual_crc);
  }
  sdput_reset_state();
  return true;
}

// YMODEM receiver -----------------------------------------------------------
// Minimal receiver for Tera Term and similar terminal programs.  It accepts
// one file per invocation, supports 128-byte (SOH) and 1K (STX) packets,
// CRC16, duplicate packet ACK, the standard two-EOT finish sequence, and CAN.
// The file advertised in block 0 is written to microSD via a temporary file.
// After reception the temporary file is read back and its CRC32 is compared
// with the CRC32 accumulated while writing before the file is committed.
static constexpr uint8_t Y_SOH = 0x01;
static constexpr uint8_t Y_STX = 0x02;
static constexpr uint8_t Y_EOT = 0x04;
static constexpr uint8_t Y_ACK = 0x06;
static constexpr uint8_t Y_NAK = 0x15;
static constexpr uint8_t Y_CAN = 0x18;
static constexpr uint8_t Y_CRC = 'C';

enum YmodemRxState : uint8_t {
  Y_WAIT_START = 0,
  Y_READ_SEQ,
  Y_READ_SEQ_INV,
  Y_READ_DATA,
  Y_READ_CRC_HI,
  Y_READ_CRC_LO
};

struct YmodemState {
  bool active;
  bool have_file;
  bool eot_seen;
  bool wait_final_header;
  bool committed;
  File file;
  Stream *out;
  YmodemRxState rx_state;
  uint16_t packet_size;
  uint16_t packet_pos;
  uint8_t seq;
  uint8_t seq_inv;
  uint8_t expected_seq;
  uint16_t rx_crc;
  uint8_t packet[1024];
  uint32_t expected_size;
  uint32_t received;
  uint32_t crc;
  uint32_t final_crc;
  uint32_t last_rx_ms;
  uint32_t next_c_ms;
  uint32_t crc_errors;
  uint32_t seq_errors;
  uint32_t duplicate_blocks;
  uint32_t packet_resyncs;
  char target[96];
  char temp[112];
  char backup[112];
};

static YmodemState ymodem = {};

static uint16_t ymodem_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
  }
  return crc;
}

static void ymodem_send(uint8_t c) {
  if (ymodem.out) ymodem.out->write(c);
}

static void ymodem_close_file() {
  if (ymodem.file) ymodem.file.close();
}

static void ymodem_reset() {
  ymodem_close_file();
  ymodem.active = false;
  ymodem.have_file = false;
  ymodem.eot_seen = false;
  ymodem.wait_final_header = false;
  ymodem.committed = false;
  ymodem.out = nullptr;
  ymodem.rx_state = Y_WAIT_START;
}

static void ymodem_abort(const char *reason, bool send_cancel = true) {
  Stream *out = ymodem.out ? ymodem.out : console;
  if (send_cancel) {
    ymodem_send(Y_CAN);
    ymodem_send(Y_CAN);
  }
  ymodem_close_file();
  if (ymodem.temp[0] && SD.exists(ymodem.temp)) SD.remove(ymodem.temp);
  ymodem.active = false;
  if (out) {
    out->print("\r\nYMODEM ERROR ");
    out->println(reason ? reason : "aborted");
  }
  ymodem_reset();
}

static bool ymodem_make_paths(const char *sent_name) {
  if (!sent_name || !*sent_name) return false;

  // Tera Term normally sends only the basename.  If another sender includes
  // a path, keep only the basename so reception cannot escape the SD root.
  const char *base = sent_name;
  for (const char *p = sent_name; *p; ++p)
    if (*p == '/' || *p == '\\') base = p + 1;
  if (!*base || strstr(base, "..")) return false;

  char normalized[96];
  snprintf(normalized, sizeof(normalized), "/%s", base);
  if (strlen(normalized) >= sizeof(ymodem.target)) return false;

  snprintf(ymodem.target, sizeof(ymodem.target), "%s", normalized);

  // This SD/FAT setup uses 8.3 filenames.  Do not derive temporary names
  // from the target (for example "spiffs.bin.ymodem"), because that exceeds
  // the 8.3 limit.  YMODEM handles only one receive operation at a time, so
  // fixed scratch names are sufficient and easy to clean up.
  snprintf(ymodem.temp, sizeof(ymodem.temp), "/YMODEM.TMP");
  snprintf(ymodem.backup, sizeof(ymodem.backup), "/YMODEM.BAK");
  return true;
}

static bool ymodem_open_file(const char *name, uint32_t size) {
  Stream *out = ymodem.out ? ymodem.out : console;

  if (!ymodem_make_paths(name)) {
    if (out) out->printf("\r\nYMODEM HEADER ERROR invalid filename: %s\r\n",
                         name ? name : "(null)");
    return false;
  }
  if (size == 0) {
    if (out) out->println("\r\nYMODEM HEADER ERROR file size is zero");
    return false;
  }

  if (SD.exists(ymodem.temp) && !SD.remove(ymodem.temp)) {
    if (out) out->printf("\r\nYMODEM SD ERROR cannot remove temp file: %s\r\n",
                         ymodem.temp);
    return false;
  }

  ymodem.file = SD.open(ymodem.temp, FILE_WRITE);
  if (!ymodem.file) {
    if (out) {
      out->printf("\r\nYMODEM SD ERROR cannot open temp file: %s "
                  "(target=%s size=%lu)\r\n",
                  ymodem.temp, ymodem.target, (unsigned long)size);
    }
    return false;
  }

  ymodem.expected_size = size;
  ymodem.received = 0;
  ymodem.crc = 0xFFFFFFFFUL;
  ymodem.final_crc = 0;
  ymodem.have_file = true;

  if (out) {
    out->printf("\r\nYMODEM HEADER OK name=%s size=%lu temp=%s\r\n",
                ymodem.target, (unsigned long)size, ymodem.temp);
  }
  return true;
}

static bool ymodem_verify_and_commit() {
  Stream *out = ymodem.out ? ymodem.out : console;
  ymodem_close_file();

  if (!ymodem.have_file || ymodem.received != ymodem.expected_size) return false;
  const uint32_t write_crc = ~ymodem.crc;

  File verify = SD.open(ymodem.temp, FILE_READ);
  if (!verify) return false;
  uint8_t buf[512];
  uint32_t read_count = 0;
  uint32_t read_crc = 0xFFFFFFFFUL;
  while (verify.available()) {
    int n = verify.read(buf, sizeof(buf));
    if (n <= 0) break;
    read_crc = sdput_crc32_update(read_crc, buf, (size_t)n);
    read_count += (uint32_t)n;
  }
  verify.close();
  read_crc = ~read_crc;
  if (read_count != ymodem.expected_size || read_crc != write_crc) return false;

  const bool had_target = SD.exists(ymodem.target);
  if (SD.exists(ymodem.backup)) SD.remove(ymodem.backup);
  if (had_target && !SD.rename(ymodem.target, ymodem.backup)) return false;
  if (!SD.rename(ymodem.temp, ymodem.target)) {
    if (had_target && SD.exists(ymodem.backup))
      SD.rename(ymodem.backup, ymodem.target);
    return false;
  }
  if (had_target && SD.exists(ymodem.backup)) SD.remove(ymodem.backup);
  ymodem.final_crc = read_crc;
  ymodem.committed = true;
  (void)out;
  return true;
}

static bool ymodem_parse_header() {
  // Block 0: filename NUL filesize[ SP ...] NUL ...
  Stream *out = ymodem.out ? ymodem.out : console;
  const char *name = reinterpret_cast<const char *>(ymodem.packet);
  const size_t name_len = strnlen(name, ymodem.packet_size);

  if (name_len >= ymodem.packet_size) {
    if (out) out->println("\r\nYMODEM HEADER ERROR filename is not NUL terminated");
    return false;
  }

  if (name_len == 0) {
    // Empty block 0 terminates the YMODEM batch.
    return true;
  }

  const char *size_str = name + name_len + 1;
  const size_t remain = ymodem.packet_size - name_len - 1;
  if (remain == 0) {
    if (out) out->println("\r\nYMODEM HEADER ERROR missing filesize field");
    return false;
  }
  if (*size_str < '0' || *size_str > '9') {
    if (out) {
      out->printf("\r\nYMODEM HEADER ERROR bad filesize field "
                  "name=%s first=0x%02X\r\n",
                  name, (unsigned int)(uint8_t)*size_str);
    }
    return false;
  }

  char *endp = nullptr;
  unsigned long size = strtoul(size_str, &endp, 10);
  if (endp == size_str || size == 0) {
    if (out) {
      out->printf("\r\nYMODEM HEADER ERROR cannot parse filesize "
                  "name=%s size-field=%s\r\n",
                  name, size_str);
    }
    return false;
  }

  if (out) {
    out->printf("\r\nYMODEM HEADER parsed name=%s size=%lu\r\n",
                name, size);
  }
  return ymodem_open_file(name, (uint32_t)size);
}

static void ymodem_packet_done() {
  const uint16_t calc = ymodem_crc16(ymodem.packet, ymodem.packet_size);
  if ((uint8_t)(ymodem.seq + ymodem.seq_inv) != 0xFF) {
    ymodem.seq_errors++;
    ymodem_send(Y_NAK);
    ymodem.rx_state = Y_WAIT_START;
    return;
  }
  if (calc != ymodem.rx_crc) {
    ymodem.crc_errors++;
    ymodem_send(Y_NAK);
    ymodem.rx_state = Y_WAIT_START;
    return;
  }

  // Sequence number 0 has two different meanings in YMODEM:
  //   * the initial/final metadata block, when no file data is active; and
  //   * a normal data block after the 8-bit sequence counter wraps 255 -> 0.
  // Therefore, never classify seq==0 as a header solely from its number.
  if (ymodem.wait_final_header) {
    if (ymodem.seq != 0 || ymodem.packet[0] != 0) {
      ymodem_abort("unexpected final header");
      return;
    }
    ymodem_send(Y_ACK);
    Stream *out = ymodem.out;
    if (out) {
      out->printf("\r\nYMODEM OK name=%s size=%lu crc32=%08lX verify=OK "
                  "crc_err=%lu seq_err=%lu dup=%lu resync=%lu\r\n",
                  ymodem.target,
                  (unsigned long)ymodem.received,
                  (unsigned long)ymodem.final_crc,
                  (unsigned long)ymodem.crc_errors,
                  (unsigned long)ymodem.seq_errors,
                  (unsigned long)ymodem.duplicate_blocks,
                  (unsigned long)ymodem.packet_resyncs);
    }
    ymodem_reset();
    return;
  }

  if (!ymodem.have_file) {
    if (ymodem.seq != 0 || !ymodem_parse_header()) {
      ymodem_abort("invalid block 0");
      return;
    }
    if (ymodem.packet[0] == 0) {
      ymodem_send(Y_ACK);
      ymodem_reset();
      return;
    }
    ymodem.expected_seq = 1;
    ymodem_send(Y_ACK);
    ymodem_send(Y_CRC);
    ymodem.rx_state = Y_WAIT_START;
    return;
  }

  // File data is active here.  seq==0 is valid whenever expected_seq has
  // wrapped to zero, so process it exactly like any other data block.

  if (ymodem.seq == (uint8_t)(ymodem.expected_seq - 1)) {
    // Sender did not see our ACK.  ACK duplicate without writing it twice.
    ymodem.duplicate_blocks++;
    ymodem_send(Y_ACK);
    ymodem.rx_state = Y_WAIT_START;
    return;
  }
  if (ymodem.seq != ymodem.expected_seq) {
    ymodem_send(Y_NAK);
    ymodem.rx_state = Y_WAIT_START;
    return;
  }

  uint32_t remain = ymodem.expected_size - ymodem.received;
  size_t write_len = ymodem.packet_size;
  if ((uint32_t)write_len > remain) write_len = (size_t)remain;
  if (write_len > 0) {
    size_t total_written = 0;
    int zero_write_retries = 0;

    // File::write() is allowed to return a short count.  Do not abort a
    // YMODEM transfer merely because one SD/VFS write did not consume the
    // whole 1K block.  Advance over partial writes and briefly yield/retry
    // zero-byte writes.  The sender is waiting for our ACK during this time,
    // so no next YMODEM data block is being sent yet.
    while (total_written < write_len) {
      const size_t written = ymodem.file.write(
          ymodem.packet + total_written,
          write_len - total_written);

      if (written > 0) {
        total_written += written;
        zero_write_retries = 0;
        continue;
      }

      zero_write_retries++;
      if (zero_write_retries > 20) {
        Stream *out = ymodem.out ? ymodem.out : console;
        if (out) {
          out->printf("\r\nYMODEM SD ERROR write stalled "
                      "offset=%lu/%lu received=%lu\r\n",
                      (unsigned long)total_written,
                      (unsigned long)write_len,
                      (unsigned long)ymodem.received);
        }
        ymodem_abort("SD write stalled");
        return;
      }

      // Let the idle task and SD/driver background work run before retrying.
      vTaskDelay(1);
    }

    ymodem.crc = sdput_crc32_update(ymodem.crc, ymodem.packet, write_len);
    ymodem.received += (uint32_t)write_len;
  }
  ymodem.expected_seq++;
  ymodem_send(Y_ACK);
  ymodem.rx_state = Y_WAIT_START;
}

static void ymodem_process_byte(uint8_t c) {
  ymodem.last_rx_ms = millis();
  switch (ymodem.rx_state) {
  case Y_WAIT_START:
    if (c == Y_SOH || c == Y_STX) {
      ymodem.packet_size = (c == Y_SOH) ? 128 : 1024;
      ymodem.packet_pos = 0;
      ymodem.rx_crc = 0;
      ymodem.rx_state = Y_READ_SEQ;
      return;
    }
    if (c == Y_EOT && ymodem.have_file && !ymodem.wait_final_header) {
      if (!ymodem.eot_seen) {
        ymodem.eot_seen = true;
        ymodem_send(Y_NAK);
      } else {
        if (!ymodem_verify_and_commit()) {
          ymodem_abort("SD read-back verify/commit failed");
          return;
        }
        ymodem_send(Y_ACK);
        ymodem_send(Y_CRC);
        ymodem.wait_final_header = true;
        ymodem.eot_seen = false;
      }
      return;
    }
    if (c == Y_CAN) {
      ymodem_abort("cancelled by sender", false);
      return;
    }
    return;

  case Y_READ_SEQ:
    ymodem.seq = c;
    ymodem.rx_state = Y_READ_SEQ_INV;
    return;
  case Y_READ_SEQ_INV:
    ymodem.seq_inv = c;
    ymodem.rx_state = Y_READ_DATA;
    return;
  case Y_READ_DATA:
    ymodem.packet[ymodem.packet_pos++] = c;
    if (ymodem.packet_pos >= ymodem.packet_size)
      ymodem.rx_state = Y_READ_CRC_HI;
    return;
  case Y_READ_CRC_HI:
    ymodem.rx_crc = (uint16_t)c << 8;
    ymodem.rx_state = Y_READ_CRC_LO;
    return;
  case Y_READ_CRC_LO:
    ymodem.rx_crc |= c;
    ymodem_packet_done();
    return;
  }
}
}

bool console_ymodem_active() {
  return ymodem.active;
}

bool console_ymodem_packet_in_progress() {
  return ymodem.active && ymodem.rx_state != Y_WAIT_START;
}

bool console_ymodem_begin(Stream *out) {
  if (ymodem.active || sdput.active) {
    if (out) out->println("YMODEM ERROR another file transfer is active");
    return false;
  }
  Stream *serial_terminal = out ? out : console;
  if (serial_terminal == nullptr) return false;

  ymodem_close_file();
  ymodem.active = true;
  ymodem.have_file = false;
  ymodem.eot_seen = false;
  ymodem.wait_final_header = false;
  ymodem.committed = false;
  ymodem.out = serial_terminal;
  ymodem.rx_state = Y_WAIT_START;
  ymodem.packet_size = 0;
  ymodem.packet_pos = 0;
  ymodem.seq = 0;
  ymodem.seq_inv = 0;
  ymodem.expected_seq = 0;
  ymodem.rx_crc = 0;
  ymodem.expected_size = 0;
  ymodem.crc_errors = 0;
  ymodem.seq_errors = 0;
  ymodem.duplicate_blocks = 0;
  ymodem.packet_resyncs = 0;
  ymodem.received = 0;
  ymodem.crc = 0xFFFFFFFFUL;
  ymodem.final_crc = 0;
  ymodem.target[0] = '\0';
  ymodem.temp[0] = '\0';
  ymodem.backup[0] = '\0';
  ymodem.last_rx_ms = millis();
  ymodem.next_c_ms = ymodem.last_rx_ms;
  serial_terminal->println("YMODEM READY: Tera Term File -> Transfer -> YMODEM -> Send");
  serial_terminal->println("Waiting for sender (CRC mode)...");
  serial_terminal->flush();
  ymodem_send(Y_CRC);
  ymodem.next_c_ms = millis() + 1000UL;
  return true;
}

bool console_sdput_begin(const char *name, uint32_t size, uint32_t crc32, Stream *out) {
  if (sdput.active) {
    if (out) out->println("SDPUT ERROR transfer already active");
    return false;
  }
  if (!name || !*name || size == 0) {
    if (out) out->println("usage: sdput <filename> <size> <crc32-hex>");
    return false;
  }
  if (strstr(name, "..")) {
    if (out) out->println("SDPUT ERROR '..' is not allowed in filename");
    return false;
  }

  char normalized[96];
  if (name[0] == '/') snprintf(normalized, sizeof(normalized), "%s", name);
  else snprintf(normalized, sizeof(normalized), "/%s", name);
  if (strlen(normalized) + 12 >= sizeof(sdput.temp)) {
    if (out) out->println("SDPUT ERROR filename too long");
    return false;
  }

  sdput.active = false;
  sdput.out = nullptr;
  sdput.expected_size = 0;
  sdput.received = 0;
  sdput.expected_crc = 0;
  sdput.crc = 0xFFFFFFFFUL;
  sdput.last_rx_ms = 0;
  sdput.next_progress = 0;
  sdput.target[0] = '\0';
  sdput.temp[0] = '\0';
  sdput.backup[0] = '\0';
  snprintf(sdput.target, sizeof(sdput.target), "%s", normalized);
  snprintf(sdput.temp, sizeof(sdput.temp), "%s.sdput", normalized);
  snprintf(sdput.backup, sizeof(sdput.backup), "%s.sdput.bak", normalized);
  sdput.out = out ? out : console;
  sdput.expected_size = size;
  sdput.expected_crc = crc32;
  sdput.crc = 0xFFFFFFFFUL;
  sdput.last_rx_ms = millis();
  sdput.next_progress = 65536;

  if (SD.exists(sdput.temp)) SD.remove(sdput.temp);
  sdput.file = SD.open(sdput.temp, FILE_WRITE);
  if (!sdput.file) {
    if (sdput.out) sdput.out->println("SDPUT ERROR cannot open temp file");
    sdput_reset_state();
    return false;
  }
  sdput.active = true;
  if (sdput.out) {
    sdput.out->printf("SDPUT READY name=%s size=%lu crc=%08lX\r\n",
                      sdput.target, (unsigned long)size, (unsigned long)crc32);
  }
  return true;
}

// receive command from serial terminal
void console_process() {
  // During YMODEM, every byte belongs to the transfer protocol.
  if (ymodem.active) {
    Stream *serial_terminal = console;
    if (serial_terminal == nullptr) {
      ymodem_abort("console unavailable");
      return;
    }

    while (ymodem.active && serial_terminal->available() > 0) {
      int v = serial_terminal->read();
      if (v < 0) break;
      ymodem_process_byte((uint8_t)v);
    }
    if (!ymodem.active) return;

    const uint32_t now = millis();

    // If a packet was interrupted (UART overrun, long scheduling delay, etc.),
    // do not leave the parser stuck in the middle of that packet.  A sender
    // retry starts again with SOH/STX, so first return to Y_WAIT_START and
    // request retransmission with NAK.
    //
    // This is intentionally much shorter than the whole-transfer timeout:
    // at 115200 bps even a 1K packet arrives in well under 0.2 s.
    if (ymodem.rx_state != Y_WAIT_START &&
        (uint32_t)(now - ymodem.last_rx_ms) > 500UL) {
      ymodem.rx_state = Y_WAIT_START;
      ymodem.packet_pos = 0;
      ymodem.packet_size = 0;
      ymodem.rx_crc = 0;
      ymodem.packet_resyncs++;
      ymodem_send(Y_NAK);
      ymodem.last_rx_ms = now;
      return;
    }

    if ((uint32_t)(now - ymodem.last_rx_ms) > 60000UL) {
      // If the file itself was already committed after EOT, only the final
      // empty batch header was lost.  Report success rather than deleting it.
      if (ymodem.wait_final_header && ymodem.committed) {
        Stream *out = ymodem.out;
        if (out) {
          out->printf("\r\nYMODEM OK name=%s size=%lu crc32=%08lX verify=OK "
                      "(final header timeout)\r\n",
                      ymodem.target,
                      (unsigned long)ymodem.received,
                      (unsigned long)ymodem.final_crc);
        }
        ymodem_reset();
        return;
      }
      ymodem_abort("receive timeout");
      return;
    }

    // Before block 0, periodically advertise CRC mode as required by YMODEM.
    if (!ymodem.have_file && !ymodem.wait_final_header &&
        (int32_t)(now - ymodem.next_c_ms) >= 0) {
      ymodem_send(Y_CRC);
      ymodem.next_c_ms = now + 1000UL;
    }
    return;
  }

  // During SDPUT, consume exactly expected_size raw bytes.  CR/LF and all
  // other byte values are data, not terminal commands.
  if (sdput.active) {
    Stream *serial_terminal = console;
    if (serial_terminal == nullptr) {
      sdput_abort("console unavailable");
      return;
    }

    if ((uint32_t)(millis() - sdput.last_rx_ms) > 30000UL) {
      sdput_abort("receive timeout");
      return;
    }

    uint8_t buf[512];
    while (sdput.active && serial_terminal->available() > 0) {
      uint32_t remain = sdput.expected_size - sdput.received;
      if (remain == 0) {
        sdput_commit();
        return;
      }
      int avail = serial_terminal->available();
      size_t want = sizeof(buf);
      if ((size_t)avail < want) want = (size_t)avail;
      if ((uint32_t)want > remain) want = remain;
      if (want == 0) break;

      size_t got = serial_terminal->readBytes(buf, want);
      if (!got) break;
      size_t written = sdput.file.write(buf, got);
      if (written != got) {
        sdput_abort("SD short write");
        return;
      }
      sdput.crc = sdput_crc32_update(sdput.crc, buf, got);
      sdput.received += got;
      sdput.last_rx_ms = millis();

      if (sdput.received >= sdput.next_progress && sdput.out) {
        sdput.out->printf("SDPUT PROGRESS %lu/%lu\r\n",
                          (unsigned long)sdput.received,
                          (unsigned long)sdput.expected_size);
        sdput.next_progress += 65536;
      }
      if (sdput.received == sdput.expected_size) {
        sdput_commit();
        return;
      }
    }
    return;
  }

  // Serial command input remains available even while the active log console
  // has been redirected to the single Telnet client.
  //Stream *serial_terminal = &Serial;
  Stream *serial_terminal = console;
  if (serial_terminal == nullptr) return;
  
  while (serial_terminal->available() > 0) {
    char c = serial_terminal->read();
    if (verbose & 32) {
      char buf[20];
      sprintf(buf, "[%02X(%c)]", c, isprint(c) ? c : ' ');
      serial_terminal->print(buf);
    }


    if (plogw->f_console_emu) {
      emulate_keyboard(c);
      continue;
    }
    //plogw->ostream->print(c);
    if (c == 0x0d) {  // CR end of the line
      // carriage return end of a line
      plogw->cmdbuf[plogw->cmd_ptr] = '\0';
      // send received line to command interpreter
      cmd_interp(plogw->cmdbuf, serial_terminal);
      // clear buffer
      plogw->cmd_ptr = 0;
      continue;
    }
    if (c == 0x0a) {
      // ignore LF
      continue;
    }
    if (plogw->cmd_ptr < 128) {
      plogw->cmdbuf[plogw->cmd_ptr] = c;
      plogw->cmd_ptr++;
    } else {
      // buffer overflow and clear
      // plogw->cmd_ptr=0;
    }
  }
}

void print_status_console()
{
  // print current logger status to console to link with PC
  // print logwindow infos
  plogw->ostream->print("# 0 ");
  plogw->ostream->print("focused: ");
  plogw->ostream->print(so2r.focused_radio());
  plogw->ostream->print(" rx: ");
  plogw->ostream->print(so2r.rx());
  plogw->ostream->print(" tx: ");
  plogw->ostream->print(so2r.tx());
  plogw->ostream->print(" ");
  print_wifiinfo();

  struct radio *radio;

  char bu[80];
  for (int i = 0; i < N_RADIO; i++) {
    radio = &radio_list[i];
    if (!radio->enabled) continue;
    sprintf(bu, "f: %10d m: %4s s: %4d cq: %1d rig %c %s",
            radio->freq, radio->opmode, radio->smeter/SMETER_UNIT_DBM, radio->cq[radio->modetype],
            radio_list[i].enabled ? '*' : ' ',
            radio_list[i].rig_spec->name
           );
    plogw->ostream->print("# "); plogw->ostream->print(i + 1);
    plogw->ostream->print(" ");
    if (so2r.focused_radio() == i) {
      plogw->ostream->print("*");
    } else {
      plogw->ostream->print(" ");
    }
    plogw->ostream->println(bu);
  }

}



int in_keys(char c, const uint8_t *keys, int nkeys) {
  for (int i = 0; i < nkeys; i++) {
    if (keys[i] == c) {
      return i;
    }
  }
  return -1;
}



void emulate_keyboard(char c) {
  static int f_esc = 0;
  static int f_funcnum = 0;
  const uint8_t numKeys[10] PROGMEM = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' };
  const uint8_t symKeysUp[12] PROGMEM = { '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?' };
  const uint8_t symKeysLo[12] PROGMEM = { '-', '=', '[', ']', '\\', ' ', ';', '\'', '`', ',', '.', '/' };
  //    const uint8_t padKeys[5] PROGMEM = {'/', '*', '-', '+', '\r'};

  //void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key)
  uint8_t key;
  int ret;


  static MODIFIERKEYS modkey;


  // UP 1B 5B 41
  // DN 1B 5B 42
  // RIGHT 1B 5B 43
  // LEFT 1B 5B 44

  // HOME 1B 5B 31 7E
  // F1 1B 5B 31 31 7E
  // F2 1B 5B 31 32 7E
  // F5          35
  // F6          37
  // F8          39

  // F9       32 30
  // F10      32 31
  // F11         33
  // F12         34

  // END 1B 5B 34 7E
  // PGUP 1B 5B 35 7E
  // PGDN 1B 5B 36 7E

  // by sending meta Alt -> 1B (ESC) +key
  // Alt-A 1B 61
  // Alt-B 1B 7A
  // ...
  modkey.bmLeftShift = 0;
  modkey.bmRightShift = 0;
  modkey.bmLeftCtrl = 0;
  modkey.bmRightCtrl = 0;
  modkey.bmLeftAlt = 0;
  modkey.bmRightAlt = 0;
  key = 0;
  // check esc sequence
  switch (f_esc) {
    case 0:  // no esc
      break;
    case 1:  // esc pressed 1b
      if (c == 0x5b) {
        // sequence
        f_esc = 2;
        return;
      }
      if (c == 0x1b) {
        // double ESC --> ESC
        key = 0x29;
        c = 0;
        break;
      }
      modkey.bmLeftAlt = 1;
      break;
    case 3:  // 1b 5b 31
      if (c == 0x7e) {
        // HOME
        key = 0x4a;
        c = 0;
        break;
      }
      if (c >= 0x30 && c <= 0x34) {
        f_funcnum = c;
        f_esc = 8;  // 1b5b313x
        return;
      }
      break;
    case 8:  // 1b5b313x
      if (c == 0x7e) {
        // end of sequence
        if (f_funcnum >= 0x31 && f_funcnum <= 0x35) {
          key = f_funcnum + 0x3a - 0x31;
          c = 0;
        }
        if (f_funcnum >= 0x37 && f_funcnum <= 0x39) {
          key = f_funcnum + 0x3f - 0x31;
          c = 0;
        }
      }
      break;

    case 4:  // 1b 5b 32
      if (c >= 0x31 && c <= 0x39) {
        f_funcnum = c;  //save
        f_esc = 9;      // 1b5b323x
        return;
      }
      break;
    case 9:  // 1b5b323x
      if (c == 0x7e) {
        // end of sequence
        key = f_funcnum + 0x42 - 0x30;
        c = 0;
      }
      break;
    case 5:  // 1b 5b 34
      if (c == 0x7e) {
        // END
        key = 0x4d;
        c = 0;
      }
      break;
    case 6:  // 1b 5b 35
      if (c == 0x7e) {
        // PGUP
        key = 0x4b;
        c = 0;
      }
      break;
    case 7:  // 1b 5b 36
      if (c == 0x7e) {
        // PGDN
        key = 0x4e;
        c = 0;
      }
      break;

    case 2:  // 0x1b 0x5b+ ?
      if (c == 0x31) {
        f_esc = 3;  // 1b 5b 31
        return;
      }
      if (c == 0x32) {
        f_esc = 4;  // 1b 5b 32
        return;
      }
      if (c == 0x34) {  // 1b 5b 34
        f_esc = 5;
        return;
      }
      if (c == 0x35) {  // 1b 5b 35
        f_esc = 6;
        return;
      }
      if (c == 0x36) {  // 1b 5b 36
        f_esc = 7;
        return;
      }
      if (c == 0x41) {  // UP
        key = 0x52;
        c = 0;
        break;
      }
      if (c == 0x42) {  // DN
        key = 0x51;
        c = 0;
        break;
      }
      if (c == 0x44) {  // LEFT
        key = 0x50;
        c = 0;
        break;
      }
      if (c == 0x43) {  // RIGHT
        key = 0x4F;
        c = 0;
        break;
      }
      if (c == 0x5a) {  // Shift + TAB
        key = 0x2b;
        c = 0;
        modkey.bmLeftShift = 1;
        break;
      }
  }
  if (key == 0) {
    switch (c) {
      case ' ': key = 0x2c; break;
      case 0x0d:
        key = 0x28;
        c = 0;
        break;
      case 0x08:
        key = 0x2a;
        c = 0;
        break;  // BS
      case 0x09:
        key = 0x2b;
        c = 0;
        break;  // TAB
      case 0x7f:
        key = 0x4c;
        c = 0;
        break;    // DEL
      case 0x1b:  // ESC
        if (f_esc == 0) {
          f_esc = 1;
          return;
        }
        break;
      default:
        // ctrl keys
        if (c >= 0x01 && c <= 0x1f) {
          c = c - 0x01 + 'A';
          modkey.bmLeftCtrl = 1;
          if (c >= 'A' && c <= 'Z') key = c - 'A' + 0x04;
        } else {
          if (c >= 'a' && c <= 'z') {
            key = c - 'a' + 0x04;
          } else {
            if (c >= 'A' && c <= 'Z') {
              // cap characters
              key = c - 'A' + 0x04;
              // shift up
              modkey.bmLeftShift = 1;
            } else {
              if (c >= '0' && c <= '9') {
                // numbers
                key = c - '0' + 0x1e;
              } else {
                if ((ret = in_keys(c, numKeys, 10)) != -1) {
                  key = 0x1e + ret;
                  // shift up
                  modkey.bmLeftShift = 1;

                } else {
                  if ((ret = in_keys(c, symKeysUp, 12)) != -1) {
                    key = 0x2d + ret;
                    // shift up
                    modkey.bmLeftShift = 1;
                  } else {
                    if ((ret = in_keys(c, symKeysLo, 12)) != -1) {
                      key = 0x2d + ret;
                      // shift down
                    }
                  }
                }
              }
            }
          }
        }
    }
  }
  f_esc = 0;

  // print key information
  if (verbose & 4) {
    char buf[10];
    sprintf(buf, " $%02x", key);
    plogw->ostream->print("key=");
    plogw->ostream->print(buf);
    plogw->ostream->print(" ctrl=");
    plogw->ostream->print(modkey.bmLeftCtrl);
    plogw->ostream->print(" shift=");
    plogw->ostream->print(modkey.bmLeftShift);
    plogw->ostream->print(" alt=");
    plogw->ostream->println(modkey.bmLeftAlt);
  }
  if ((verbose & 16) && ((uint8_t)key == 0x36 || (uint8_t)key == 0x37)) {
    Serial.printf("KBDLOW t=%lu src=CONSOLE hid=0x%02X mod=0x%02X on=1\n",
                  (unsigned long)millis(), (unsigned int)(uint8_t)key,
                  (unsigned int)(*((uint8_t *)&modkey)));
  }
  on_key_down(modkey, (uint8_t)key, (uint8_t)c);
  plogw->ostream->flush();
}

