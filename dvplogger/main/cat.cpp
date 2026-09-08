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
#include "Ticker.h"
#include "decl.h"
#include "variables.h"
#include "hardware.h"
#include "cat.h"
#include "cw_keying.h"
#include "usb_host.h"
#include "usb_cat_transport.h"
#include "display.h"
#include "log.h"
#include "edit_buf.h"
#include "main.h"
#include "mcp.h"
#include "SD.h"
#include "settings.h"
#include "cat.h"
#include "timekeep.h"
#include "mux_transport.h"
#include "processes.h"
#include "so2r.h"
#include <driver/uart.h>
#include <soc/soc.h>
#include <soc/uart_reg.h>
#include <esp_timer.h>
#include <maidenhead.h>

QueueHandle_t xQueueCATUSBRx,xQueueCATUSBTx;

static void verbose_cat_rx_dump(const struct radio *radio,
                                const char *source,
                                const uint8_t *buf,
                                size_t len)
{
  if (!(verbose & 1) || buf == NULL || len == 0) return;

  const int rig_idx = radio ? radio->rig_idx : -1;
  console->printf("CAT RX rig=%d source=%s len=%u ascii=\"",
                  rig_idx, source ? source : "?",
                  (unsigned int)len);

  for (size_t i = 0; i < len; i++) {
    const uint8_t c = buf[i];
    switch (c) {
    case '\r': console->print("\\r"); break;
    case '\n': console->print("\\n"); break;
    case '\t': console->print("\\t"); break;
    case '\\': console->print("\\\\"); break;
    case '"': console->print("\\\""); break;
    default:
      if (c >= 0x20 && c <= 0x7e) console->write(c);
      else console->print('.');
      break;
    }
  }

  console->print("\" hex=");
  for (size_t i = 0; i < len; i++) {
    console->printf("%02X", buf[i]);
    if (i + 1 < len) console->write(' ');
  }
  console->println();
}

// software serial
#include <SoftwareSerial.h>
//SoftwareSerial Serial4; // to remap console i/o to use Serial to communicate with extension board
extern SoftwareSerial Serial3;

// ci-v related variables
long freq;
char opmode[8];

// ATS Mini absolute-mode emulation -----------------------------------------
// Stock ATS Mini Ad hoc protocol exposes only M/m (next/previous mode).
// The receiver's mode order is FM -> LSB -> USB -> AM.
static int8_t ats_mini_mode_current[N_RADIO] = {-1, -1, -1};
static int8_t ats_mini_mode_target[N_RADIO]  = {-1, -1, -1};
static bool ats_mini_mode_pending[N_RADIO]   = {false, false, false};
static uint32_t ats_mini_mode_last_cmd_ms[N_RADIO] = {0, 0, 0};

// Virtual CW: DVPlogger keeps logical CW carrier frequency/mode while ATS Mini
// actually receives USB 600 Hz above the carrier with 0.5 kHz bandwidth.
static bool ats_mini_virtual_cw[N_RADIO] = {false, false, false};
static uint32_t ats_mini_bw_last_cmd_ms[N_RADIO] = {0, 0, 0};
static const long ATS_MINI_CW_PITCH_HZ = 600;

static int ats_mini_mode_index(const char *mode)
{
  if (!mode) return -1;
  if (!strcmp(mode, "FM"))  return 0;
  if (!strcmp(mode, "LSB")) return 1;
  if (!strcmp(mode, "USB")) return 2;
  if (!strcmp(mode, "AM"))  return 3;
  return -1;
}

static const char *ats_mini_mode_name(int idx)
{
  static const char *const names[] = {"FM", "LSB", "USB", "AM"};
  return (idx >= 0 && idx < 4) ? names[idx] : "?";
}

static void ats_mini_service_mode_target(struct radio *radio)
{
  if (!radio || !radio->rig_spec ||
      radio->rig_spec->cat_type != CAT_TYPE_ATS_MINI)
    return;
  if (radio->rig_idx < 0 || radio->rig_idx >= N_RADIO) return;

  const int idx = radio->rig_idx;
  if (!ats_mini_mode_pending[idx]) return;

  const int current = ats_mini_mode_current[idx];
  const int target = ats_mini_mode_target[idx];
  if (current < 0 || target < 0) return;

  if (current == target) {
    ats_mini_mode_pending[idx] = false;
    if (verbose & VERBOSE_USB)
      console->printf("ATS-MINI mode reached: %s\n",
                      ats_mini_mode_name(target));
    return;
  }

  const uint32_t now = millis();
  // One step at a time; wait for the periodic status stream before advancing.
  if ((uint32_t)(now - ats_mini_mode_last_cmd_ms[idx]) < 300U) return;

  const int forward = (target - current + 4) % 4;
  const int backward = (current - target + 4) % 4;
  const char cmd[2] = {(forward <= backward) ? 'M' : 'm', '\0'};

  send_cat_cmd(radio, cmd);
  ats_mini_mode_last_cmd_ms[idx] = now;

  if (verbose & VERBOSE_USB) {
    console->printf("ATS-MINI mode step: %s -> target %s cmd=%s\n",
                    ats_mini_mode_name(current),
                    ats_mini_mode_name(target), cmd);
  }
}

static void ats_mini_service_cw_bandwidth(struct radio *radio,
                                          const char *bandwidth_desc)
{
  if (!radio || !radio->rig_spec ||
      radio->rig_spec->cat_type != CAT_TYPE_ATS_MINI ||
      radio->rig_idx < 0 || radio->rig_idx >= N_RADIO)
    return;

  const int idx = radio->rig_idx;
  if (!ats_mini_virtual_cw[idx] || ats_mini_mode_current[idx] != 2)
    return; // virtual CW requires ATS USB

  if (bandwidth_desc && !strcmp(bandwidth_desc, "0.5k"))
    return;

  const uint32_t now = millis();
  if ((uint32_t)(now - ats_mini_bw_last_cmd_ms[idx]) < 300U)
    return;

  // Do not depend on the firmware's bandwidth table ordering. Repeatedly
  // cycle one step and use the 500-ms status description as confirmation.
  send_cat_cmd(radio, "w");
  ats_mini_bw_last_cmd_ms[idx] = now;

  if (verbose & VERBOSE_USB)
    console->printf("ATS-MINI CW bandwidth step: %s -> 0.5k\\n",
                    bandwidth_desc ? bandwidth_desc : "?");
}

// DECIMAL -> BCD
byte dec2bcd(byte data) {
  return (((data / 10) << 4) + (data % 10));
}

// BCD -> DECIMAL
byte bcd2dec(byte data) {
  return (((data >> 4) * 10) + (data % 16));
}


// Icom clock synchronization -------------------------------------------------
//
// IC-7300 / IC-9700:
//   1A 05 01 79 YY YY MM DD   date (packed BCD, 4-digit year)
//   1A 05 01 80 HH MM         time (packed BCD)
//
// The time-setting command carries hour/minute but no seconds.  To avoid
// introducing an arbitrary seconds error, automatic synchronization is
// armed when a valid CI-V reply establishes/re-establishes communication,
// then transmitted around second 00 of the next minute.
//
// A gap of more than 5 seconds in valid CI-V replies is treated as a new
// connection.  This also covers a transceiver power cycle without requiring
// DVPlogger to restart.
static uint32_t icom_clock_last_rx_ms[N_RADIO] = {0};
static bool icom_clock_sync_pending[N_RADIO] = {false};

bool icom_clock_sync_supported(const struct radio *radio)
{
  if (radio == nullptr || radio->rig_spec == nullptr) return false;
  if (radio->rig_spec->cat_type != CAT_TYPE_CIV) return false;

  switch (radio->rig_spec->rig_type) {
  case RIG_TYPE_ICOM_IC705:
  case RIG_TYPE_ICOM_IC7300:
  case RIG_TYPE_ICOM_IC9700:
    return true;
  default:
    return false;
  }
}

static void send_icom_clock_civ(struct radio *radio)
{
  if (!icom_clock_sync_supported(radio)) return;

  const uint16_t year = my_rtc.year();
  byte sub_hi = 0x01;
  byte date_sub_lo = 0x65;
  byte time_sub_lo = 0x66;
  const char *rig_name = "IC-705";

  // The Time Set subcommands are model-specific.  They are BCD-like
  // four-digit subcommands carried as two bytes after 1A 05.
  switch (radio->rig_spec->rig_type) {
  case RIG_TYPE_ICOM_IC7300:
    sub_hi = 0x00; date_sub_lo = 0x94; time_sub_lo = 0x95; rig_name = "IC-7300";
    break;
  case RIG_TYPE_ICOM_IC9700:
    sub_hi = 0x01; date_sub_lo = 0x79; time_sub_lo = 0x80; rig_name = "IC-9700";
    break;
  case RIG_TYPE_ICOM_IC705:
  default:
    sub_hi = 0x01; date_sub_lo = 0x65; time_sub_lo = 0x66; rig_name = "IC-705";
    break;
  }

  // Set date: 1A 05 <model date subcommand> YYYY MM DD
  send_head_civ(radio);
  add_civ_buf((byte)0x1a);
  add_civ_buf((byte)0x05);
  add_civ_buf(sub_hi);
  add_civ_buf(date_sub_lo);
  add_civ_buf(dec2bcd((byte)(year / 100)));
  add_civ_buf(dec2bcd((byte)(year % 100)));
  add_civ_buf(dec2bcd((byte)my_rtc.month()));
  add_civ_buf(dec2bcd((byte)my_rtc.day()));
  send_tail_civ(radio);

  // Set time: 1A 05 <model time subcommand> HH MM
  send_head_civ(radio);
  add_civ_buf((byte)0x1a);
  add_civ_buf((byte)0x05);
  add_civ_buf(sub_hi);
  add_civ_buf(time_sub_lo);
  add_civ_buf(dec2bcd((byte)my_rtc.hour()));
  add_civ_buf(dec2bcd((byte)my_rtc.minute()));
  send_tail_civ(radio);

  plogw->ostream->printf(
      "CLOCKSYNC radio=%d rig=%s %04u/%02u/%02u %02u:%02u civ=%02X "
      "date-sub=%02X%02X time-sub=%02X%02X\n",
      radio->rig_idx, rig_name, (unsigned)year,
      (unsigned)my_rtc.month(), (unsigned)my_rtc.day(),
      (unsigned)my_rtc.hour(), (unsigned)my_rtc.minute(),
      radio->rig_spec->civaddr, sub_hi, date_sub_lo, sub_hi, time_sub_lo);
}

void request_icom_clock_sync(struct radio *radio)
{
  if (!icom_clock_sync_supported(radio)) return;
  if (radio->rig_idx < 0 || radio->rig_idx >= N_RADIO) return;
  icom_clock_sync_pending[radio->rig_idx] = true;
}

int request_icom_clock_sync_all()
{
  int n = 0;
  for (int i = 0; i < N_RADIO; ++i) {
    struct radio *radio = &radio_list[i];
    if (!radio->enabled || !icom_clock_sync_supported(radio)) continue;
    request_icom_clock_sync(radio);
    ++n;
  }
  return n;
}

static void service_icom_clock_sync_on_rx(struct radio *radio)
{
  if (!icom_clock_sync_supported(radio)) return;
  if (radio->rig_idx < 0 || radio->rig_idx >= N_RADIO) return;

  const int idx = radio->rig_idx;
  const uint32_t now_ms = millis();
  const uint32_t last_ms = icom_clock_last_rx_ms[idx];

  // Keep track of valid CI-V receive timing only.  Clock synchronization is
  // intentionally NOT armed here: it must be requested explicitly with the
  // CLOCKSYNC command.
  (void)last_ms;
  icom_clock_last_rx_ms[idx] = now_ms;

}

void service_icom_clock_sync()
{
  // Check the clock independently of CI-V receive timing.  Accept second 01
  // as well so a busy loop crossing the exact boundary still synchronizes.
  if (my_rtc.second() > 1) return;

  for (int i = 0; i < N_RADIO; ++i) {
    if (!icom_clock_sync_pending[i]) continue;

    struct radio *radio = &radio_list[i];
    if (!radio->enabled || !icom_clock_sync_supported(radio)) {
      icom_clock_sync_pending[i] = false;
      continue;
    }

    send_icom_clock_civ(radio);
    icom_clock_sync_pending[i] = false;
  }
}


// mode name string to rig mode number
int rig_modenum(const char *opmode) {
  int mode;
  if (verbose & 1) {
    plogw->ostream->print("rig_modenum():");
    plogw->ostream->print(opmode);
    plogw->ostream->println(":");
  }
  if (strcmp(opmode, "LSB") == 0) mode = 0;
  else if (strcmp(opmode, "USB") == 0) mode = 1;
  else if (strcmp(opmode, "AM") == 0) mode = 2;
  else if (strcmp(opmode, "CW") == 0) mode = 3;
  else if (strcmp(opmode, "RTTY") == 0) mode = 4;
  else if (strcmp(opmode, "FM") == 0) mode = 5;
  else if (strcmp(opmode, "WFM") == 0) mode = 6;
  else if (strcmp(opmode, "CW-R") == 0) mode = 7;
  else if (strcmp(opmode, "RTTY-R") == 0) mode = 8;
  else if (strcmp(opmode, "DV") == 0) mode = 0x17;
  else mode = -1;
  return mode;
}

char *opmode_string(int modenum) {
  switch (modenum) {
    case 0: return "LSB"; break;
    case 1: return "USB"; break;
    case 2: return "AM"; break;
    case 3: return "CW"; break;
    case 4: return "RTTY"; break;
    case 5: return "FM"; break;
    case 6: return "WFM"; break;
    case 7: return "CW-R"; break;
    case 8: return "RTTY-R"; break;
    case 0x17: return "DV"; break;
    default: return ""; break;
  }
}

int is_manual_rig(struct radio *radio) {
  if (radio->rig_spec !=NULL) {
    if (radio->rig_spec->cat_type == 3) {
      return 1;
    }
  }
  return 0;
}

// set frequency to radio
void set_frequency_rig_radio(unsigned int freq, struct radio *radio) {
  if (!radio->enabled) return;

  radio->f_freqchange_program = 1;
  radio->freqchange_program_guard = 500;
  // check manual radio
  if (radio->rig_spec->cat_type == 3) {
    // manual rig
    set_frequency(freq, radio);
    radio->f_freqchange_pending=0;
    radio->f_freqchange_program=0;
    return;
  }

  send_freq_set_civ(radio, freq);  // send to civ port
  radio->freqchange_timer =
      (radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI) ? 800 : 200;
  radio->f_freqchange_pending = 1;
  radio->freq_target = freq;
}



// set frequency to radio_selected
void set_frequency_rig(unsigned int freq) {
  struct radio *radio;
  radio = so2r.radio_selected();
  set_frequency_rig_radio(freq, radio);
}

// internal memory buffer for civ 
byte civ_buf[65];
int civ_buf_idx = 0;

void add_civ_buf(byte c) {
  if (civ_buf_idx < 64) {
    civ_buf[civ_buf_idx++] = c;
  } else {
    return;
  }
}

void send_head_civ(struct radio *radio) {
  // header part sending
  clear_civ_buf();
  add_civ_buf((byte)0xfe);  // $FE
  add_civ_buf((byte)0xfe);
  add_civ_buf((byte)radio->rig_spec->civaddr);
  add_civ_buf((byte)0xe0);
}

void send_tail_civ(struct radio *radio) {
  // tail part sending
  add_civ_buf((byte)0xfd);
  //  send_civ_buf(radio->rig_spec->civport); // previously 
  send_civ_buf_radio(radio); // new routine to direct any type ports 
}


// Per-rig RTTY settings.  Negative values preserve the old behaviour.
int rig_fsk_port(const struct rig *r) {
  if (!r) return 0;
  return (r->fskport >= 0 && r->fskport <= 4) ? r->fskport : r->cwport;
}

bool rig_rtty_invert(const struct rig *r) {
  if (r && (r->rtty_polarity == 0 || r->rtty_polarity == 1))
    return r->rtty_polarity != 0;
  return USBRTTYgetInvert();
}

// ptt control
void set_ptt_rig(struct radio *radio, int on) {
  if (radio && radio->rig_spec &&
      radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI) {
    radio->ptt_stat = 0;
    return; // receiver only
  }

  // Hardware MIC/PTT always follows the RADIO slot:
  // Radio0 -> PTT1, Radio1 -> PTT2, Radio2 -> PTT3.
  // This is independent of the rig PTT: setting.
  if (radio->rig_idx >= 0 && radio->rig_idx <= 2)
    ptt_switch(radio->rig_idx, on);

  // PTT: selects only an additional transmit-control method.
  // 0/1: don't care (no additional control)
  // 2: CAT/CI-V PTT
  // 3: USB DTR
  // 4: USB RTS
  if (radio->rig_spec->pttmethod == 3 || radio->rig_spec->pttmethod == 4) {
    usb_keying_request((uint8_t)radio->rig_spec->pttmethod, on != 0);
  }

  if (radio->rig_spec->pttmethod == 2) {
  switch (radio->rig_spec->cat_type) {

  case CAT_TYPE_CIV:  // icom ci-v
    send_head_civ(radio);
    add_civ_buf((byte)0x1c);  //
    add_civ_buf((byte)0x00);  //

    if (on) {
      add_civ_buf((byte)0x01);  //

    } else {
      add_civ_buf((byte)0x00);  //
    }
    send_tail_civ(radio);
    break;
    //    case 1:  // cat
  case CAT_TYPE_YAESU_NEW:  // Yaesu
  case CAT_TYPE_YAESU_OLD:
    if (on) {
      send_cat_cmd(radio, "TX1;");
    } else {
      send_cat_cmd(radio, "TX0;");
    }
    break;
  case CAT_TYPE_YAESU_FT817:
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy      
    if (on) {
      add_civ_buf((byte)0x08); // dummy      
    } else {
      add_civ_buf((byte)0x88); // dummy      	
    }
    send_civ_buf_radio(radio);
    break;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:  // kenwood cat ( QCX special command and not of Kenwood general)
    if (on) {
      send_cat_cmd(radio, "TQ1;");
    } else {
      send_cat_cmd(radio, "TQ0;");
    }
    break;
  case CAT_TYPE_ELECRAFT_KX: {
    // Elecraft KX-line CAT PTT control.  IF; reports the resulting TX/RX state.    
    char tx[] = "TX;";
    char rx[] = "RX;";
    send_cat_cmd(radio, on ? tx : rx);
    break;
  }

  }
  } // PTT:2 CAT/CI-V

  // if CW mode  also key on / off
  if (radio->modetype == LOG_MODETYPE_CW) {
    if (on) {
      // key on
      //  digitalWrite(LED, 1);
      keying(1);
      if (verbose & 1) {
        plogw->ostream->print("Key On");
      }
    } else {
      // key on
      // digitalWrite(LED, 0);
      keying(0);
      if (verbose & 1) {
        plogw->ostream->print("Key Off");
      }
    }
  }
}

void send_voice_memory(struct radio *radio, int num)
// num 0 cancel send 1  F1 CQ  2 F2 number 3 F3 TU  4 F4 CALLSIGN 5 F5 CALL+NUM
{
  char buf[10];
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_CIV:  // icom ci-v
      if (!plogw->f_console_emu) {
        plogw->ostream->print("voice ");
        plogw->ostream->println(num);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x28);
      add_civ_buf((byte)0x00);
      add_civ_buf((byte)num);
      send_tail_civ(radio);
      break;
      //    case 1:  // cat
  case CAT_TYPE_YAESU_NEW:  // Yaesu
  case CAT_TYPE_YAESU_OLD:
      sprintf(buf, "PB0%d;", num);
      send_cat_cmd(radio, buf);
      break;
  }
}

void send_rtty_memory(struct radio *radio, int num)
// num 0 cancel send 1  F1 CQ  2 F2 number 3 F3 TU  4 F4 CALLSIGN 5 F5 CALL+NUM
{
  char buf[100];
  switch (radio->rig_spec->cat_type) {
    case 0:
      if (!plogw->f_console_emu) {
        plogw->ostream->print("voice ");
        plogw->ostream->println(num);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x28);
      add_civ_buf((byte)0x00);
      add_civ_buf((byte)num);
      send_tail_civ(radio);
      break;
      //    case 1:  // cat
  case CAT_TYPE_YAESU_NEW:  // Yaesu
  case CAT_TYPE_YAESU_OLD:
      
      if (!plogw->f_console_emu) plogw->ostream->println("rtty encoder");
      sprintf(buf, "EN0%d;", num);
      send_cat_cmd(radio, buf);
      break;
  }
}


void clear_civ_buf() {
  civ_buf_idx = 0;
}

void send_civ_buf_radio(struct radio *radio) {
  struct catmsg_t catmsg;
  int ret;
  //  console->print("send_civ_buf_radio()");
  if (verbose & 1) {
    char ostr[32];
    sprintf(ostr, "send_civ_buf_radio():");
    plogw->ostream->print(ostr);
    for (int i = 0; i < civ_buf_idx; i++) {
      sprintf(ostr, "%02X ", civ_buf[i]);
      plogw->ostream->print(ostr);
    }
    plogw->ostream->println("");
  }
  switch(radio->rig_spec->civport_num) {
  case -2: // MANUAL
    break;
  case -1: // USB
    // send by queue
    // send catmsg
    catmsg.size=civ_buf_idx;
    memcpy(catmsg.buf,civ_buf,civ_buf_idx);
    ret = xQueueSend(xQueueCATUSBTx, &catmsg, 0);
    if (verbose &1) plogw->ostream->printf("CATUSBTx queuesend ret=%d size=%d\r\n",ret,catmsg.size);
    //
    break;
  case 0: // port0 console (should not set)
    // do not send to these port (may be different later) 24/11/24 EAraki
    break;
  case 1: // port1 BT
    // use mux transport
    if (f_mux_transport==1) {
      mux_transport.send_pkt(MUX_PORT_BT_SERIAL_MAIN,MUX_PORT_BT_SERIAL_EXT,civ_buf,civ_buf_idx);
    } else {
      // direct send
      //      send_civ_buf(radio->rig_spec->civport);
      send_civ_buf(&Serial2);
    }
    break;
  case 2: // port2 CIV
  case 3: // port3 TTL-SER
    send_civ_buf(radio->rig_spec->civport);
    break;
  case 4: // extension board CAT2 through port 1 (not read directory from port1 but data pushed from demux service)
    if (f_mux_transport==1) {
      // use mux transport
      mux_transport.send_pkt(MUX_PORT_CAT2_MAIN,MUX_PORT_CAT2_EXT,civ_buf,civ_buf_idx);
      //civport->write(civ_buf, civ_buf_idx);			     
    } else {
      // may not send directly
    }
    break;
  }
}



// old routine
void send_civ_buf(Stream *civport) {
  if (civport == NULL) {
    // send to Ftdi USB serial port
    // send to USB host serial adapter
    usb_send_civ_buf() ;

  } else {
    // normal serial port write
    //		for (int i=0;i<civ_buf_idx;i++) {
    //			civport->write(civ_buf[i]);
    //		}
    civport->write(civ_buf, civ_buf_idx);
  }
  clear_civ_buf();
}



#define YAESU_QUERY_TIMEOUT_MS 300U

#define YAESU_QUERY_INTERVAL_MS 20U
//#define YAESU_QUERY_INTERVAL_MS 200U

enum {
  YAESU_QUERY_IF = 0,
  YAESU_QUERY_SM,
  YAESU_QUERY_TX,
  YAESU_QUERY_RA,
  YAESU_QUERY_PA,
  YAESU_QUERY_PC,
  YAESU_QUERY_ID,
  YAESU_QUERY_COUNT
};

static const char *const yaesu_query_commands[YAESU_QUERY_COUNT] = {
  "IF;", "SM0;", "TX;", "RA0;", "PA0;", "PC;", "ID;"
};

static const char *const yaesu_query_prefixes[YAESU_QUERY_COUNT] = {
  "IF", "SM", "TX", "RA", "PA", "PC", "ID"
};

static bool is_yaesu_ascii_radio(const struct radio *radio)
{
  return radio != NULL && radio->rig_spec != NULL &&
         (radio->rig_spec->cat_type == CAT_TYPE_YAESU_NEW ||
          radio->rig_spec->cat_type == CAT_TYPE_YAESU_OLD);
}

static int yaesu_query_slot(const char *cmd)
{
  for (int i = 0; i < YAESU_QUERY_COUNT; ++i) {
    if (strcmp(cmd, yaesu_query_commands[i]) == 0) return i;
  }
  return -1;
}

static bool cat_cmd_send_raw(struct radio *radio, const char *cmd)
{
  bool sent = false;
  switch (radio->rig_spec->civport_num) {
  case -2: // MANUAL
    break;
  case -1: { // USB
    const size_t len = strlen(cmd);
    if (len == 0 || len > sizeof(((struct catmsg_t *)nullptr)->buf)) {
      console->printf("!USB CAT TX invalid length rig=%d len=%u\n",
                      radio->rig_idx, (unsigned int)len);
      break;
    }

    const bool is_qmx =
      radio->rig_spec->cat_type == CAT_TYPE_QMX;
    const bool is_ats_mini =
      radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI;

    if (is_qmx && !usb_cat_ready_for_rig_type(CAT_TYPE_QMX)) {
      static uint32_t last_qmx_not_ready_log = 0;
      const uint32_t now = millis();
      if (now - last_qmx_not_ready_log >= 1000) {
        last_qmx_not_ready_log = now;
        if (verbose & VERBOSE_USB) console->printf(
          "QMX CAT TX suppressed: USB QMX not ready len=%u cmd=%s\n",
          (unsigned int)len, cmd);
      }
      break;
    }

    if (is_ats_mini && !usb_cat_ready_for_rig_type(CAT_TYPE_ATS_MINI)) {
      static uint32_t last_ats_not_ready_log = 0;
      const uint32_t now = millis();
      if (now - last_ats_not_ready_log >= 1000) {
        last_ats_not_ready_log = now;
        if (verbose & VERBOSE_USB)
          console->println("ATS-MINI TX suppressed: USB CDC ACM not ready");
      }
      break;
    }

    /*
     * Transport policy is selected by the rig protocol.  QMX polling
     * commands are snapshots, so an old queued poll may be dropped.  The
     * same common queue API is also used by the future Yaesu/CP2105 path.
     */
    bool dropped_oldest = false;
    const usb_cat_profile_t &profile = usb_cat_profile(
      is_qmx ? USB_CAT_BACKEND_ACM_QMX : USB_CAT_BACKEND_ACM_GENERIC);
    sent = usb_cat_enqueue(reinterpret_cast<const uint8_t *>(cmd), len,
                           profile.drop_oldest_poll, &dropped_oldest);

    if (is_qmx && (verbose & VERBOSE_USB)) {
      console->printf(
        "QMX CAT TX enqueue rig=%d sent=%d drop=%d "
        "waiting=%u free=%u len=%u cmd=%s\n",
        radio->rig_idx, sent ? 1 : 0, dropped_oldest ? 1 : 0,
        (unsigned int)usb_cat_tx_waiting(),
        (unsigned int)usb_cat_tx_free(),
        (unsigned int)len, cmd);
    }
    break;
  }
  case 0:  // port0 console (should not set)
    break;
  case 1:  // port1 BT
    if (f_mux_transport) {
      mux_transport.send_pkt(MUX_PORT_BT_SERIAL_MAIN, MUX_PORT_BT_SERIAL_EXT,
                             (unsigned char *)cmd, strlen(cmd));
      sent = true;
    } else {
      Serial2.print(cmd);
      sent = true;
    }
    break;
  case 2: // port2 CIV
  case 3: // port3 TTL-SER
    if (radio->rig_spec->civport != NULL) {
      radio->rig_spec->civport->print(cmd);
      sent = true;
    } else {
      console->println("!port is NULL");
    }
    break;
  case 4: // extension board CAT2 through MUX transport
    if (f_mux_transport) {
      mux_transport.send_pkt(MUX_PORT_CAT2_MAIN, MUX_PORT_CAT2_EXT,
                             (unsigned char *)cmd, strlen(cmd));
      sent = true;
    }
    break;
  }

  if (sent && (verbose & 1)) {
    plogw->ostream->print("send cat cmd port:");
    plogw->ostream->print(radio->rig_spec->civport_num);
    plogw->ostream->print(" cmd:");
    plogw->ostream->println(cmd);
  }
  return sent;
}

static void yaesu_query_service(struct radio *radio)
{
  if (!is_yaesu_ascii_radio(radio)) return;

  uint32_t now = millis();

  if (radio->yaesu_query_pending_prefix[0] != '\0') {
    if ((int32_t)(now - radio->yaesu_query_pending_until) < 0) return;

    if (verbose & 1) {
      plogw->ostream->print("CAT query pending timeout rig=");
      plogw->ostream->print(radio->rig_idx);
      plogw->ostream->print(" response:");
      plogw->ostream->print(radio->yaesu_query_pending_prefix);
      plogw->ostream->print(" partial_len=");
      plogw->ostream->print(radio->cmd_ptr);
      plogw->ostream->print(" rx_pending=");
      plogw->ostream->println(
			      (radio->w_ptr - radio->r_ptr) & 0xff);
      
    }
    /*
     * The expected response did not finish.  Do not retain its partial
     * contents, because the next response would otherwise be appended to
     * the unfinished frame.
     */
    if (radio->cmd_ptr != 0) {
      clear_civ(radio);
    }
    
    radio->yaesu_query_pending_prefix[0] = '\0';
    radio->yaesu_query_next_send_at = now + YAESU_QUERY_INTERVAL_MS;    
  }

  if ((int32_t)(now - radio->yaesu_query_next_send_at) < 0) return;
  if (radio->yaesu_query_request_mask == 0) return;

  // Round-robin selection prevents frequently requested IF/SM/TX queries
  // from starving slower status queries such as ID, PA, RA and PC.
  for (int n = 0; n < YAESU_QUERY_COUNT; ++n) {
    int slot = (radio->yaesu_query_next_slot + n) % YAESU_QUERY_COUNT;
    uint16_t bit = (uint16_t)1U << slot;
    if ((radio->yaesu_query_request_mask & bit) == 0) continue;

    radio->yaesu_query_request_mask &= ~bit;
    if (cat_cmd_send_raw(radio, yaesu_query_commands[slot])) {
      strncpy(radio->yaesu_query_pending_prefix,
              yaesu_query_prefixes[slot],
              sizeof(radio->yaesu_query_pending_prefix));
      radio->yaesu_query_pending_prefix[
          sizeof(radio->yaesu_query_pending_prefix) - 1] = '\0';
      radio->yaesu_query_pending_until = now + YAESU_QUERY_TIMEOUT_MS;
      radio->yaesu_query_next_slot = (slot + 1) % YAESU_QUERY_COUNT;
    }
    return;
  }
}

static void yaesu_query_response_received(struct radio *radio)
{
  if (!is_yaesu_ascii_radio(radio)) return;
  if (radio->yaesu_query_pending_prefix[0] == '\0') return;

  if (strncmp(radio->cmdbuf, radio->yaesu_query_pending_prefix, 2) != 0)
    return;

  radio->yaesu_query_pending_prefix[0] = '\0';
  radio->yaesu_query_next_send_at = millis() + YAESU_QUERY_INTERVAL_MS;
}

void send_cat_cmd(struct radio *radio, const char *cmd)
{
  int slot = is_yaesu_ascii_radio(radio) ? yaesu_query_slot(cmd) : -1;
  if (slot >= 0) {
    //    if (slot != YAESU_QUERY_IF) {
    //      return;
    //    }
    // Coalesce duplicate requests.  The scheduler sends one query at a time.
    radio->yaesu_query_request_mask |= (uint16_t)1U << slot;
    yaesu_query_service(radio);
    return;
  }

  // Set commands and protocols other than Yaesu ASCII retain their previous
  // immediate-send behavior; many set commands do not return a response.
  cat_cmd_send_raw(radio, cmd);
}

// Return the ESP32 hardware UART used by a radio, or NULL for USB,
// MUX transport, SoftwareSerial and unconnected/manual ports.
static HardwareSerial *hardware_serial_port(struct radio *radio)
{
  if (radio == NULL || radio->rig_spec == NULL) return NULL;

  Stream *port = radio->rig_spec->civport;
  switch (radio->rig_spec->civport_num) {
  case 1: // direct Bluetooth serial; Serial2 is reserved for MUX when enabled
    if (f_mux_transport != 0) return NULL;
    port = &Serial2;
    break;
  case -1: // USB
  case 4:  // MUX transport
  case -2: // manual
    return NULL;
  default:
    break;
  }

  if (port == &Serial)  return &Serial;
  if (port == &Serial1) return &Serial1;
  if (port == &Serial2) return &Serial2;
  return NULL; // includes Serial3 (SoftwareSerial)
}

// Return the ESP-IDF UART number corresponding to an Arduino HardwareSerial.
static int cat_hardware_uart_index(HardwareSerial *port)
{
  if (port == &Serial)  return UART_NUM_0;
  if (port == &Serial1) return UART_NUM_1;
  if (port == &Serial2) return UART_NUM_2;
  return -1;
}

// Lightweight UART diagnostics.  Sample on every receive pass, but print only
// once per second so that the diagnostic itself does not disturb CAT reception.
static void cat_uart_rx_diagnostics(struct radio *radio,
                                    Stream *port,
                                    HardwareSerial *hw)
{
  if (!(verbose &0x100)) return;
  
  static uint32_t reported_radio_mask = 0;
  static uint32_t next_report_ms[3] = { 0, 0, 0 };
  static size_t max_buffered[3] = { 0, 0, 0 };
  static uint32_t frame_error_edges[3] = { 0, 0, 0 };
  static uint32_t parity_error_edges[3] = { 0, 0, 0 };
  static uint32_t overflow_edges[3] = { 0, 0, 0 };
  static uint32_t previous_error_bits[3] = { 0, 0, 0 };

  if (radio == NULL || radio->rig_spec == NULL || hw == NULL) return;

  const int uart_num = cat_hardware_uart_index(hw);
  if (uart_num < UART_NUM_0 || uart_num > UART_NUM_2) return;

  // Print the resolved Stream/HardwareSerial mapping once for each radio.
  const int rig_idx = radio->rig_idx;
  if (rig_idx >= 0 && rig_idx < 32) {
    const uint32_t bit = (uint32_t)1U << rig_idx;
    if ((reported_radio_mask & bit) == 0) {
      reported_radio_mask |= bit;
      console->printf(
        "CAT port=%p hw=%p civport_num=%d uart=%d rig=%d\n",
        (void *)port, (void *)hw, radio->rig_spec->civport_num,
        uart_num, rig_idx);
    }
  }

  size_t buffered = 0;
  if (uart_get_buffered_data_len((uart_port_t)uart_num, &buffered) == ESP_OK &&
      buffered > max_buffered[uart_num]) {
    max_buffered[uart_num] = buffered;
  }

  const uint32_t raw = REG_READ(UART_INT_RAW_REG(uart_num));
  const uint32_t error_bits = raw &
    (UART_FRM_ERR_INT_RAW | UART_PARITY_ERR_INT_RAW | UART_RXFIFO_OVF_INT_RAW);
  const uint32_t rising = error_bits & ~previous_error_bits[uart_num];

  if (rising & UART_FRM_ERR_INT_RAW)    frame_error_edges[uart_num]++;
  if (rising & UART_PARITY_ERR_INT_RAW) parity_error_edges[uart_num]++;
  if (rising & UART_RXFIFO_OVF_INT_RAW) overflow_edges[uart_num]++;
  previous_error_bits[uart_num] = error_bits;

  const uint32_t now = millis();
  if ((int32_t)(now - next_report_ms[uart_num]) < 0) return;
  next_report_ms[uart_num] = now + 1000;

  const uint32_t status = REG_READ(UART_STATUS_REG(uart_num));
  const uint32_t fifo_count =
    (status & UART_RXFIFO_CNT_M) >> UART_RXFIFO_CNT_S;


  console->printf(
    "CAT UART%d buffered=%u max=%u fifo=%lu "
    "frm=%lu parity=%lu ovf=%lu raw=%08lX\n",
    uart_num,
    (unsigned int)buffered,
    (unsigned int)max_buffered[uart_num],
    (unsigned long)fifo_count,
    (unsigned long)frame_error_edges[uart_num],
    (unsigned long)parity_error_edges[uart_num],
    (unsigned long)overflow_edges[uart_num],
    (unsigned long)raw);


  max_buffered[uart_num] = buffered;
}


void receive_cat_data(struct radio *radio) {
  if (!radio->enabled) return;
  // check port
  Stream *port;
  port=radio->rig_spec->civport;
  switch(radio->rig_spec->civport_num) {
  case 1: // serial BT
    if (f_mux_transport!=0) return; // only read here for direct transport
    port=&Serial2;
    break;
  case 4: // can only read in mux transport
    return;
    break;
  default:
    break;
  }

  if (port != NULL) {
    HardwareSerial *hw = hardware_serial_port(radio);
    cat_uart_rx_diagnostics(radio, port, hw);

    char c;
    int count;
    count=0;
    uint8_t rx_dump[64];
    size_t rx_dump_len = 0;
    while (port->available()) {
      c = port->read();
      //      if (rx_dump_len < sizeof(rx_dump)) rx_dump[rx_dump_len++] = (uint8_t)c;
      //      if (!isprint((unsigned char)c)) continue;
      radio->bt_buf[radio->w_ptr] = c;
      radio->w_ptr++;
      radio->w_ptr %= 256;
      count++;
    }
    //    verbose_cat_rx_dump(radio, "serial", rx_dump, rx_dump_len);
    if (count>0) {
      //      console->print("receive_cat_data():");
      //      plogw->ostream->println(count);
    }
    //    // record max number of received characters during the civport interrupt for debugging
    //    if (count>receive_civport_count) receive_civport_count=count;
    
    return;
  } else {
    usb_receive_cat_data(radio);
  }
}


// return with mode specific frequency width 
int freq_width_mode(char *opmode)
{
  int mode;
  int span;
  mode = rig_modenum(opmode);
  //  console->println(opmode);
  //  console->println(mode);
  span =1000/FREQ_UNIT; //default
      switch (mode) {
      case 3:  // CW
          span = 500/FREQ_UNIT;  // 500Hz
	  break;
      case 7:
          span = 500/FREQ_UNIT;  // 500Hz 
          break;
      case 0: // SSB
          span = 1000/FREQ_UNIT;  // 3 k
	  break;
      case 1: 
          span = 1000/FREQ_UNIT;  // 3 k
          break;
      case 5:              // FM
	span = 20000/FREQ_UNIT;  // 20k
	break;
      case 6:              // WFM
	span = 20000/FREQ_UNIT;  // 20k
	break;
      }
      return span;
}

void set_power(struct radio *radio, int power)  // power value is in W
{
  char buf[40];
  int max_power=50;

  switch(radio->rig_spec->cat_type) {
  
  case CAT_TYPE_CIV:
    // icom	    
    switch (radio->rig_spec->rig_type) {
    case RIG_TYPE_ICOM_IC7300:  // IC-7300  ... almost the same as IC705 except for preamp gain in 50MHz
      max_power=50;
      break;
    case RIG_TYPE_ICOM_IC705:  // IC-705
      max_power=10;
      break;
    case RIG_TYPE_ICOM_IC9700:  // IC-9700
      max_power=50;
      break;
    }
    power=power*255/max_power; // normalize power
    if (power>255) power=255;
    if (power<0) power=0;
    send_head_civ(radio);
    add_civ_buf((byte)0x14);
    add_civ_buf((byte)0x0a);
    add_civ_buf((byte)dec2bcd(power/100));
    add_civ_buf((byte)dec2bcd(power%100));
    send_tail_civ(radio);
    break;
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
    sprintf(buf, "PC%03d;", power);
    send_cat_cmd(radio, buf);
    break;
  }
}

void set_scope() {
  int mode;
  struct radio *radio;
  radio = so2r.radio_selected();
  mode = rig_modenum(radio->opmode);
  set_scope_mode(radio,mode);
}

void set_scope_mode(struct radio *radio,int mode) {
  int span;
  char buf[40];

  if (radio == nullptr || !radio->enabled || radio->rig_spec == nullptr) return;

  span=0;
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_CIV:  // Icom CI-V
      switch (mode) {
        case 3:       // CW
        case 7:       // CW-R
        case 4:       // RTTY
        case 8:       // RTTY-R
          span = 0x10000;    // 10 kHz
          break;
        case 0:       // LSB
        case 1:       // USB
          span = 0x25000;    // 25 kHz
          break;
        case 2:       // AM
        case 0x17:    // DV
          span = 0x50000;    // 50 kHz
          break;
        case 5:       // FM
        case 6:       // WFM
          span = 0x0250000;  // 250 kHz
          break;
        default:
          return;             // do not send an invalid/zero span
      }

      send_head_civ(radio);
      add_civ_buf((byte)0x27);
      add_civ_buf((byte)0x15);
      add_civ_buf((byte)0x00);
      for (int i = 0; i < 5; i++) {
        add_civ_buf((byte)(span & 0xff));
        span = span >> 8;
      }
      send_tail_civ(radio);
      break;

  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
      switch (mode) {
        case 3:       // CW
        case 7:       // CW-R
        case 4:       // RTTY
        case 8:       // RTTY-R
          span = 3;   // 10 kHz
          break;
        case 0:       // LSB
        case 1:       // USB
        case 2:       // AM
        case 0x17:    // DV (future Yaesu support)
          span = 5;   // 50 kHz
          break;
        case 5:       // FM
        case 6:       // WFM
          span = 6;   // 100 kHz
          break;
        default:
          return;
      }
      sprintf(buf, "SS0640000;SS05%d0000;SS0670000;", span);
      send_cat_cmd(radio, buf);
      break;

  default:
      break;
  }
}

void send_rit_setting(struct radio *radio, int rit, int xit) {
  int type;
  int sign;
  type = radio->rig_spec->cat_type;
  char buf[30];
  switch (type) {
  case CAT_TYPE_YAESU_NEW:  // Yaesu
  case CAT_TYPE_YAESU_OLD:
      if (freq < 0) return;
      sprintf(buf, "CF000%c%c000;", (rit == 1) ? '1' : '0', (xit == 1) ? '1' : '0');
      send_cat_cmd(radio, buf);
      return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
    // kenwood TS480
    return;
  case CAT_TYPE_CIV:  // icom ci-v
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // RIT
      add_civ_buf((byte)0x01);  //  on/off
      add_civ_buf(rit ? 0x01 : 0x00);
      send_tail_civ(radio);
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // XIT
      add_civ_buf((byte)0x02);  //  on/off
      add_civ_buf(xit ? 0x01 : 0x00);
      send_tail_civ(radio);
      return;
  }
}


// RIT/dTX frequency set to the rig (freq offset in freq)
void send_rit_freq_civ(struct radio *radio, int freq) {
  int type;
  int sign;
  type = radio->rig_spec->cat_type;
  char buf[30];
  
  freq=freq*FREQ_UNIT; 
  if (freq < 0) {
    sign = 1;
    freq = -freq;
  } else {
    sign = 0;
  }

  switch (type) {
    //    case 1:  // Yaesu
  case CAT_TYPE_YAESU_NEW:  // Yaesu
  case CAT_TYPE_YAESU_OLD:
      
      if (freq < 0) return;
      sprintf(buf, "CF001%c%04d;", (sign == 0) ? '+' : '-', freq);
      send_cat_cmd(radio, buf);
      return;
    case 2:
      // kenwood TS480
      return;
    case 0:  // icom ci-v
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // RIT frequency
      add_civ_buf((byte)0x00);  //

      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10 Hz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1kHz
      add_civ_buf((byte)sign);
      send_tail_civ(radio);
      return;
  }
}



// CW S&P XIT support.  Keep this deliberately limited to rig families for
// which DVPlogger has a CAT/CI-V path capable of setting TX clarifier/XIT.
bool xit_control_supported(struct radio *radio) {
  if (radio == NULL || radio->rig_spec == NULL) return false;

  const int cat_type = radio->rig_spec->cat_type;
  const int rig_type = radio->rig_spec->rig_type;

  // Select XIT capability by rig family/type, while keeping a CAT protocol
  // guard so a mismatched profile cannot send unsupported commands.
  switch (rig_type) {
  case RIG_TYPE_ICOM_IC705:
  case RIG_TYPE_ICOM_IC7300:
    return cat_type == CAT_TYPE_CIV;

  case RIG_TYPE_YAESU:
    return cat_type == CAT_TYPE_YAESU_NEW ||
           cat_type == CAT_TYPE_YAESU_OLD;

  case RIG_TYPE_ELECRAFT_KX:
    return cat_type == CAT_TYPE_ELECRAFT_KX;

  default:
    return false;
  }
}

void set_xit_control(struct radio *radio, bool on) {
  if (!xit_control_supported(radio)) return;
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_CIV:
    send_rit_setting(radio, 0, on ? 1 : 0);
    break;
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD: {
    char buf[24];
    // CF MAIN, CLAR setting: RX CLAR=0, TX CLAR=on/off.
    sprintf(buf, "CF0000%c000;", on ? '1' : '0');
    send_cat_cmd(radio, buf);
    break;
  }
  case CAT_TYPE_ELECRAFT_KX:
    send_cat_cmd(radio, on ? "XT1;" : "XT0;");
    break;
  default:
    break;
  }
}

void set_xit_offset_hz(struct radio *radio, int offset_hz) {
  if (!xit_control_supported(radio)) return;
  if (offset_hz > 9999) offset_hz = 9999;
  if (offset_hz < -9999) offset_hz = -9999;
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_CIV:
    // Existing helper uses DVPlogger's 10-Hz frequency unit.
    send_rit_freq_civ(radio, offset_hz / FREQ_UNIT);
    break;
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD: {
    char buf[24];
    sprintf(buf, "CF001%c%04d;", offset_hz >= 0 ? '+' : '-', abs(offset_hz));
    send_cat_cmd(radio, buf);
    break;
  }
  case CAT_TYPE_ELECRAFT_KX: {
    // KX3/K3-family RO command sets the shared RIT/XIT offset in 10-Hz units.
    char buf[20];
    int v = offset_hz / 10;
    sprintf(buf, "RO%+04d;", v);
    send_cat_cmd(radio, buf);
    break;
  }
  default:
    break;
  }
}

// Yaesu TX CLAR has an important UI side effect on FTX-1 (and can on other
// recent Yaesu rigs): while TX CLAR is enabled the main tuning dial is used
// for CLAR adjustment.  Therefore XITON only *arms* Yaesu XIT while receiving.
// The TX CLAR bit is asserted just before an automatic CW message and cleared
// again when that message finishes.
static bool xit_yaesu_transient(const struct radio *radio) {
  if (radio == NULL || radio->rig_spec == NULL) return false;
  return radio->rig_spec->cat_type == CAT_TYPE_YAESU_NEW ||
         radio->rig_spec->cat_type == CAT_TYPE_YAESU_OLD;
}

static volatile int xit_cw_finish_pending_radio = -1;

void apply_xit_for_operating_mode(struct radio *radio) {
  if (!xit_control_supported(radio)) return;
  if (radio->xit_enabled && radio->cq[radio->modetype] == LOG_SandP) {
    set_xit_offset_hz(radio, radio->xit_offset_hz);
    if (xit_yaesu_transient(radio)) {
      // Keep the Yaesu tuning dial as a VFO dial while receiving.
      set_xit_control(radio, false);
    } else {
      set_xit_control(radio, true);
    }
  } else {
    // CQ must always be transmitted at zero offset, irrespective of XITON.
    set_xit_control(radio, false);
    set_xit_offset_hz(radio, 0);
  }
}

bool prepare_xit_for_cw_message(struct radio *radio) {
  if (!xit_control_supported(radio) || !radio->xit_enabled ||
      radio->cq[radio->modetype] != LOG_SandP) return false;

  set_xit_offset_hz(radio, radio->xit_offset_hz);
  if (xit_yaesu_transient(radio)) {
    set_xit_control(radio, true);
    return true; // caller should give CAT a short head start before keying
  }
  return false;
}

void request_finish_xit_cw_message(struct radio *radio) {
  if (radio == NULL || !xit_yaesu_transient(radio)) return;
  xit_cw_finish_pending_radio = radio->rig_idx;
}

void service_xit_cw_message() {
  int idx = xit_cw_finish_pending_radio;
  if (idx < 0 || idx >= N_RADIO) return;
  xit_cw_finish_pending_radio = -1;

  struct radio *radio = &radio_list[idx];
  if (!xit_control_supported(radio) || !xit_yaesu_transient(radio)) return;
  // Leave the stored CLAR frequency alone; only release TX CLAR so the main
  // dial immediately goes back to normal VFO tuning.
  set_xit_control(radio, false);
}

void send_freq_set_civ(struct radio *radio, unsigned int freq) {


  // check transverter frequency
  for (int i = 0; i < NMAX_TRANSVERTER; i++) {
    if (radio->rig_spec->transverter_freq[i][0] == 0) continue;  // not defined
    if (!radio->rig_spec->transverter_enable[i]) continue;
    if ((freq >= radio->rig_spec->transverter_freq[i][0]) && (freq <= radio->rig_spec->transverter_freq[i][1])) {
      // transverter frequency
      freq = freq - (radio->rig_spec->transverter_freq[i][0] - radio->rig_spec->transverter_freq[i][2]);
      break;
    }
  }
  if (!plogw->f_console_emu) {  
    console->print("FREQ=");
    console->println(freq);
  }
  int type;
  type = radio->rig_spec->cat_type;
  char buf[30];
  byte bytebuf[4];
  switch (type) {
  case CAT_TYPE_YAESU_NEW: // FTDX3000  etc 
    sprintf(buf, "FA%09lld;", ((long long)freq*FREQ_UNIT));
    send_cat_cmd(radio, buf);
    return;
  case CAT_TYPE_YAESU_OLD:
    sprintf(buf, "FA%08lld;", ((long long)freq*FREQ_UNIT));
    send_cat_cmd(radio, buf);
    return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
      // kenwood TS480 uses 11 digit
      sprintf(buf, "FA%011lld;", ((long long)freq*FREQ_UNIT));
      send_cat_cmd(radio, buf);
      return;
  case CAT_TYPE_ATS_MINI: {
      unsigned long long ats_hz = (unsigned long long)freq * FREQ_UNIT;
      if (radio->rig_idx >= 0 && radio->rig_idx < N_RADIO &&
          ats_mini_virtual_cw[radio->rig_idx]) {
        ats_hz += 600ULL;
      }
      snprintf(buf, sizeof(buf), "F%llu\r", ats_hz);
      send_cat_cmd(radio, buf);
      return;
  }
  case CAT_TYPE_ELECRAFT_KX:
      // Elecraft KX-line FA command also uses an 11-digit frequency in Hz.
      sprintf(buf, "FA%011lld;", ((long long)freq*FREQ_UNIT));
      send_cat_cmd(radio, buf);
      return;
  case CAT_TYPE_NOCAT:
      return;
  case CAT_TYPE_YAESU_FT817:
    // FT-817/818 frequency data is four packed-BCD bytes in 10 Hz units.
    // radio->freq is already stored in FREQ_UNIT (10 Hz) units, so do not
    // divide it again here.  Dividing by (100/FREQ_UNIT) made 28.600 MHz
    // become 2.860 MHz on the rig.
    bytebuf[3]=dec2bcd(freq % 100);
    freq = freq / 100;  // 1kHz
    bytebuf[2]=dec2bcd(freq % 100);
    freq = freq / 100;  // 100kHz
    bytebuf[1]=dec2bcd(freq % 100);
    freq = freq / 100;  // 10MHz
    bytebuf[0]=dec2bcd(freq % 100);
    clear_civ_buf();
    add_civ_buf(bytebuf[0]); // freq 10M
    add_civ_buf(bytebuf[1]); // freq 100k
    add_civ_buf(bytebuf[2]); // freq 1k
    add_civ_buf(bytebuf[3]); // freq 10Hz
    add_civ_buf((byte)0x01);     // Set Frequency command
    send_civ_buf_radio(radio);
    return;
  }


  if (freq <= 0) {
    // ignore
    if (!plogw->f_console_emu) plogw->ostream->println("<0, ignore sending freq");
    return;
  }


  send_head_civ(radio);
  add_civ_buf((byte)0x05);  // Frequency !
  add_civ_buf((byte)dec2bcd(freq % (100/FREQ_UNIT)));
  freq = freq / (100/FREQ_UNIT);  // 10 Hz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 1kHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 100kHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 10MHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 1GHz
  send_tail_civ(radio);
}


void send_mode_set_civ_radio(const char *opmode, int filnr, struct radio *radio) {
  if (!radio || !radio->enabled || !radio->rig_spec) return;

  int mode;
  mode = rig_modenum(opmode);
  if (mode < 0) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("No matching mode");
      plogw->ostream->println(opmode);
    }
    return;
  }

  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_ATS_MINI: {
    if (radio->rig_idx < 0 || radio->rig_idx >= N_RADIO) return;
    const int idx = radio->rig_idx;

    if (!strcmp(opmode, "CW")) {
      ats_mini_virtual_cw[idx] = true;
      ats_mini_mode_target[idx] = ats_mini_mode_index("USB");
      ats_mini_mode_pending[idx] = true;
      ats_mini_bw_last_cmd_ms[idx] = 0;

      // Logical/DVPlogger mode remains CW; physical ATS Mini mode is USB.
      set_mode_nonfil("CW", radio);

      if (verbose & VERBOSE_USB)
        console->println(
            "ATS-MINI virtual CW request: USB +600Hz BW=0.5k");

      ats_mini_service_mode_target(radio);

      // Re-send current logical carrier frequency; send_freq_set_civ()
      // applies the +600 Hz physical offset while virtual CW is active.
      if (radio->freq > 0)
        send_freq_set_civ(radio, radio->freq);
      return;
    }

    // Explicit non-CW mode exits virtual-CW translation.
    ats_mini_virtual_cw[idx] = false;

    const int ats_target = ats_mini_mode_index(opmode);
    if (ats_target < 0) {
      if (!plogw->f_console_emu) {
        plogw->ostream->printf(
            "ATS-MINI mode unsupported: %s "
            "(supported: CW LSB USB AM FM)\n",
            opmode);
      }
      return;
    }

    ats_mini_mode_target[idx] = ats_target;
    ats_mini_mode_pending[idx] = true;

    if (verbose & VERBOSE_USB)
      console->printf("ATS-MINI mode request: %s\n",
                      ats_mini_mode_name(ats_target));

    ats_mini_service_mode_target(radio);
    return;
  }
  case CAT_TYPE_YAESU_NEW: // FTDX3000  etc
  case CAT_TYPE_YAESU_OLD:    
      // cat
      switch (mode) {
        case 0:  // LSB
          send_cat_cmd(radio, "MD01;");
          break;
        case 1:  // USB
          send_cat_cmd(radio, "MD02;");
          break;
        case 2:  // AM
          send_cat_cmd(radio, "MD05;");
          break;
        case 3:  // CW
          send_cat_cmd(radio, "MD03;");
          break;
        case 4:  // RTTY-LSB
          send_cat_cmd(radio, "MD06;");
          break;
        case 5:  // FM
          send_cat_cmd(radio, "MD04;");
          break;
        case 6:  // WFM
          send_cat_cmd(radio, "MD0B;");
          break;
        case 7:  // CW-R
          send_cat_cmd(radio, "MD07;");
          break;
        case 8:  // RTTY-R
          send_cat_cmd(radio, "MD09;");
          break;
        case 0x17:  // DV
          break;
      }
      break;
  case CAT_TYPE_YAESU_FT817:
    // use civ_buf to construct binary command data and send
    clear_civ_buf();
      // cat
      switch (mode) {
        case 0:  // LSB
	  add_civ_buf((byte)0x00);
          break;
        case 1:  // USB
	  add_civ_buf((byte)0x01);
          break;
        case 2:  // AM
	  add_civ_buf((byte)0x04);
          break;
        case 3:  // CW
	  add_civ_buf((byte)0x02);
          break;
        case 4:  // RTTY-LSB
	  add_civ_buf((byte)0x0A);
          break;
        case 5:  // FM
	  add_civ_buf((byte)0x08);
          break;
        case 7:  // CW-R
	  add_civ_buf((byte)0x03);
          break;
      default:
	  add_civ_buf((byte)0x0A);
	break;
      }
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x07);     // Set Operating Mode
    send_civ_buf_radio(radio);
    break;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
      // Kenwood and Elecraft KX-line use the one-digit MD command form.
      switch (mode) {
        case 3:                         // CW
          send_cat_cmd(radio, "MD3;");  // OK?
          break;
        case 0:  // LSB
          send_cat_cmd(radio, "MD1;");  // OK?	  
          break;
        case 1:  // USB
          send_cat_cmd(radio, "MD2;");  // OK?	  
          break;
        case 2:  // AM
          send_cat_cmd(radio, "MD5;");  // OK?	  
          break;
        case 4:  // RTTY-LSB
          send_cat_cmd(radio, "MD6;");  // OK?	  
          break;
        case 5:  // FM
          send_cat_cmd(radio, "MD4;");  // OK?	  
          break;
        case 6:  // WFM
          break;
        case 7:  // CW-R
          send_cat_cmd(radio, "MD7;");  // OK?	  
          break;
        case 8:  // RTTY-R
          send_cat_cmd(radio, "MD9;");  // OK?	  
          break;
        case 0x17:  // DV
          break;
      }
      break;
  case CAT_TYPE_CIV:  // icom ci-v
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // Frequency !
      add_civ_buf((byte)mode);  // mode data
      if (filnr <= 0) filnr = 1;
      add_civ_buf((byte)filnr);  // filter data;
      send_tail_civ(radio);
      break;
  }
}


void send_mode_set_civ(const char *opmode, int filnr) {
  send_mode_set_civ_radio(opmode, filnr, so2r.radio_selected());
}


void send_freq_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_ATS_MINI:
    return; // periodic ATS monitor stream supplies frequency/mode
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:      
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
    // IF returns frequency, mode and TX/RX status for Kenwood/Elecraft.
    send_cat_cmd(radio, "IF;");
    return;
  case CAT_TYPE_YAESU_FT817:
    // use civ_buf to construct binary command data and send
    clear_civ_buf();
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x03);     // Read Frequency and Mode Status
    radio->cat_status=0x30;
    radio->f_civ_response_expected = 1;
    radio->civ_response_timer = 50;
    send_civ_buf_radio(radio);
    return;
  case CAT_TYPE_NOCAT:
    return;
  }
  // icom
  // query frequency
  send_head_civ(radio);
  add_civ_buf((byte)0x03);  // Frequency !
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response

  // query mode
  //  send_head_civ();
  // civport->write((byte)0x04);                  // mode
  // send_tail_civ();
}

void send_mode_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_ELECRAFT_KX:
    send_cat_cmd(radio, "IF;");
    return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_YAESU_FT817:
  case CAT_TYPE_NOCAT:    
    return;
  }
  // icom
  // mode frequency
  send_head_civ(radio);
  add_civ_buf((byte)0x04);  // mode !
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_rotator_head_civ(int from, int to) {
  Serial2.write((byte)0xfe);  // $FE
  Serial2.write((byte)0xfe);
  Serial2.write((byte)to);    // 2
  Serial2.write((byte)from);  // 3 to
}

void send_rotator_tail_civ() {
  Serial2.write((byte)0xfd);  // 7 tail
}

void send_rotator_az_query_civ() {
  // rotator is always connected to civ port Serial2
  // header part sending
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);  // 4 rotator command
  Serial2.write((byte)0x00);  //  5 rotator sub command query
  send_rotator_tail_civ();
  if (verbose & 2) plogw->ostream->println("send_rotator_az_query_civ()");
}

void send_rotator_az_set_civ(int az) {
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);               // 4 rotator command
  Serial2.write((byte)0x01);               //  5 rotator sub command query
  Serial2.write((byte)dec2bcd(az % 100));  //  6 data0
  az = az / 100;
  Serial2.write((byte)dec2bcd(az % 100));  //  7 data1
  send_rotator_tail_civ();
}
// rotator commands
// 00 query az
// 01 set target az
// 02 set north
// 03 set south
// 04 enable tracking
// 05 set type to the second arg. (0 Yaesu 1 DC motor)

void send_rotator_command_civ(byte *cmds, int n) {
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);  // 4 rotator command
  for (int i = 0; i < n; i++) {
    Serial2.write((byte)*cmds);  //  5 rotator sub command query
    cmds++;
  }
  send_rotator_tail_civ();
}


// check tx/r
void send_ptt_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:      
      send_cat_cmd(radio, "TX;");
      return;
  case CAT_TYPE_YAESU_FT817:
    // use civ_buf to construct binary command data and send
    clear_civ_buf();
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0xF7);     // Read TX status
    radio->cat_status=0x20;    // set reading TX status
    radio->f_civ_response_expected = 1;
    radio->civ_response_timer = 50;
    send_civ_buf_radio(radio);
    return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
    // Do nothing; TX/RX status is obtained from the periodic IF response.
    return;
  case CAT_TYPE_NOCAT:
      return;
  }

  if (radio->modetype != LOG_MODETYPE_CW) {
    // set TX status output for phone
    send_head_civ(radio);
    add_civ_buf((byte)0x24);  //
    add_civ_buf((byte)0x00);
    add_civ_buf((byte)0x00);  // set TX status output
    add_civ_buf((byte)0x01);  // on
    send_tail_civ(radio);
    if (verbose & VERBOSE_CAT) plogw->ostream->println("TX query");
    radio->f_civ_response_expected = 1;
    radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
  }
}

void send_gps_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_YAESU_FT817:    
      return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
      return;
  case CAT_TYPE_NOCAT:  // manual radio do nothing
      return;
  }
  send_head_civ(radio);
  add_civ_buf((byte)0x23);  // GPS position query 
  add_civ_buf((byte)0x00);  // 
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_preamp_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:


      send_cat_cmd(radio, "PA0;");
      return;
  case CAT_TYPE_YAESU_FT817:
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
      return;
    case CAT_TYPE_NOCAT:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x16);  //
  add_civ_buf((byte)0x02);  // preamp query
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_identification_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
      send_cat_cmd(radio, "ID;");  //0670 FT991A
      return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
  case CAT_TYPE_YAESU_FT817:
      return;
    case CAT_TYPE_NOCAT:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x19);  // id query  //5416725060A2FD
  add_civ_buf((byte)0x00);  // id query
  //
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_power_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
      send_cat_cmd(radio, "PC;");
      return;
  case CAT_TYPE_ELECRAFT_KX:
      // Power readback is not required for basic KX operation.
      return;
  case CAT_TYPE_YAESU_FT817:        
      return;
    case CAT_TYPE_NOCAT:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x14);  // 
  add_civ_buf((byte)0x0A);  // power query 
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


void send_att_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
      send_cat_cmd(radio, "RA0;");
      return;
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
  case CAT_TYPE_ELECRAFT_KX:
  case CAT_TYPE_YAESU_FT817:
      return;
    case CAT_TYPE_NOCAT:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x11);  // att query
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


void send_smeter_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  
  switch (type) {
  case CAT_TYPE_QMX:
    send_cat_cmd(radio, "SM;");
    return;
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_ELECRAFT_KX:
    send_cat_cmd(radio, "SM0;");
    return;
  case CAT_TYPE_YAESU_FT817:
    // use civ_buf to construct binary command data and send
    clear_civ_buf();
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0x00); // dummy
    add_civ_buf((byte)0xE7);     // Read RX status
    radio->cat_status=0x10;    // set reading RX status
    radio->f_civ_response_expected = 1;
    radio->civ_response_timer = 50;
    send_civ_buf_radio(radio);
    return;
  case CAT_TYPE_NOCAT:  // manual radio do nothing
    return;
  }

  //  plogw->ostream->println("send_smeter_query_civ()");
  send_head_civ(radio);
  add_civ_buf((byte)0x15);  //
  add_civ_buf((byte)0x02);  // smeter level 0000 S0 0120 S9 0241 S9+60
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


// set frequency (received from rig) to the logging system
void set_frequency(int freq, struct radio *radio) {

  //  if (verbose & 16) plogw->ostream->println("set_frequency()");
  if ((radio->f_freqchange_pending) && !((plogw->sat) || (radio->rig_spec->cat_type == 3))) {  // not sat and pending
    if (verbose & 16) plogw->ostream->println("freq_change_pending");
    // wait until received freq is the same as the target plogw->freq (already changed by the program)
    if (radio->freq_target == freq) {
      // frequency set to rig completed
      radio->f_freqchange_pending = 0;
      radio->f_freqchange_program = 0;
      radio->freqchange_timer = 0;
      radio->freqchange_retry = 0;
      radio->freqchange_program_guard = 500;
      radio->freq_change_count = 0;
      radio->freq_change_candidate = 0;
      radio->freq = radio->freq_target;
      radio->freq_prev = radio->freq;  // make the frequency tracking as normal
      if (verbose & 16) plogw->ostream->println("freq_change_completed");
      bandmap_disp.f_update = 1;  // request bandmap change
    } else {
      if (radio->freqchange_timer == 0) {
        // timeout
        radio->freqchange_retry++;
        if (radio->freqchange_retry < 4) {
          set_frequency_rig(radio->freq_target);
        } else {
          radio->f_freqchange_pending = 0;  // abandon
	  radio->f_freqchange_program = 0; 
          radio->freqchange_retry = 0;
	  if (verbose & 16) plogw->ostream->println("freq_change akirameru");	  
          return;
        }
      }
    }
  } else {
    radio->f_freqchange_pending = 0;  // adhoc

    if (freq != radio->freq) {
      // A program-originated CAT set may be followed by delayed/stale frequency
      // reports.  Do not interpret those as a human dial movement.
      if (!is_manual_rig(radio) &&
          radio->rig_spec->cat_type != CAT_TYPE_ATS_MINI &&
          radio->freqchange_program_guard > 0) {
        radio->freq_change_count = 0;
        radio->freq_change_candidate = 0;
        if (verbose & 16) {
          console->print("set_frequency():ignore guarded freq=");
          console->println(freq);
        }
        return;
      }

      // Confirm a manual dial movement only when the *same* new frequency is
      // received twice.  The old code merely counted any two different
      // values, so two CAT glitches could force CQ -> S&P.
      if (!is_manual_rig(radio) &&
          radio->rig_spec->cat_type != CAT_TYPE_ATS_MINI) {
        if (radio->freq_change_count == 0 ||
            radio->freq_change_candidate != (unsigned int)freq) {
          radio->freq_change_candidate = freq;
          radio->freq_change_count = 1;
          if (verbose & 16) {
            console->print("set_frequency():candidate freq=");
            console->println(freq);
          }
          return;
        }
      }

      if (verbose & 16) {
        if (radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI)
          console->print("set_frequency():ATS immediate freq=");
        else
          console->print("set_frequency():confirm change freq=");
        console->println(freq);
      }
      radio->freq_change_count = 0;
      radio->freq_change_candidate = 0;
    } else {
      radio->freq_change_count = 0;
      radio->freq_change_candidate = 0;
    }
    // set received frequency
    radio->freq_prev = radio->freq;
    radio->freq = freq;
    radio->bandid = freq2bandid(radio->freq);
    if (radio->bandid != radio->bandid_prev) {
      // band change
      radio->bandid_prev = radio->bandid;
      // also move the bandmap display band
      radio->bandid_bandmap = radio->bandid;
      if (!plogw->f_console_emu) plogw->ostream->println("band change");
      //upd_display_bandmap();
      bandmap_disp.f_update = 1;  // just request update flag (updated in interval jobs
    }
    if (radio->freq != radio->freq_prev) {
      // operating frequency (dial) moved or band change
      // check cq/sat status to set mode appropriately
      //      if (verbose & 16) {
	plogw->ostream->print("freq change detected ");
	console->print(radio->freq_prev); console->print(" -> ");console->println(radio->freq);
	//      }

	
      if (plogw->sat == 0) {
	//        if (radio->cq[radio->modetype]) {
	//if (radio->cq[radio->modetype] && !radio->f_recall_freq_mode_filt) {
	if (radio->cq[radio->modetype] && !radio->f_freqchange_program) {
          // frequency change in CQ and non-sat detected
          // save previous frequency to memorize as cq frequency
          radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->freq_prev;
          radio->cq[radio->modetype] = LOG_SandP;  // change to s&p
        } else {
          // s&p  and frequency(dial) moved situation
          // if dupe  wipe the qso
          if (radio->dupe) {
            wipe_log_entry();
            radio->dupe = 0;
	    if (verbose&16) console->println("dupe and display");
            upd_display();
          }
	}
        // update bandmap display
        //upd_display_bandmap();
        bandmap_disp.f_update = 1;  // just request update flag (updated in interval jobs

      } else {
        // satellite mode
        // set frequency to uplink down link frequency
        switch (plogw->sat_vfo_mode) {
          case SAT_VFO_SINGLE_A_RX:  //
            if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
              plogw->dn_f = radio->freq*FREQ_UNIT; // satellite related frequency is in Hz unit (not FREQ_UNIT Hz)
            }
            break;
          case SAT_VFO_SINGLE_A_TX:  // received frequency is TX
            if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
              plogw->up_f = radio->freq*FREQ_UNIT;
            }
            break;
          case SAT_VFO_MULTI_TX_0:
            if (radio->rig_idx == 0) {
              // received frequency is TX
              if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
                plogw->up_f = radio->freq*FREQ_UNIT;
              }
            }
            if (radio->rig_idx == 1) {
              // received frequency is RX
              if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
                plogw->dn_f = radio->freq*FREQ_UNIT;
              }
            }
            break;
          case SAT_VFO_MULTI_TX_1:  //
            if (radio->rig_idx == 1) {
              // received frequency is TX
              if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
                plogw->up_f = radio->freq*FREQ_UNIT;
              }
            }
            if (radio->rig_idx == 0) {
              // received frequency is RX
              if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
                plogw->dn_f = radio->freq*FREQ_UNIT;
              }
            }
            break;
        }
      }
    }
    radio->f_recall_freq_mode_filt = 0;    
  }
}

void set_mode_nonfil(const char *opmode, struct radio *radio) {
  strcpy(radio->opmode, opmode);
  radio->mode_initialized = (opmode != nullptr && opmode[0] != '\0');

  radio->modetype = modetype_string(radio->opmode);
  if (radio->f_tone_keying && radio->modetype == LOG_MODETYPE_PH ) {
    // tone keying flag set  in PH modes is regarded as CW operattion 24/7/28
    radio->modetype = LOG_MODETYPE_CW;
  } 
  if (radio->modetype != radio->modetype_prev) {
    set_log_rst(radio);  // adjust RST according to the mode
  }
  radio->modetype_prev = radio->modetype;
}

void set_mode(const char *opmode, byte filt, struct radio *radio) {
  if (radio == nullptr || radio->rig_spec == nullptr ||
      opmode == nullptr || opmode[0] == '\0') return;

  // Change the scope width only when the operating mode really changes.
  // Repeated CAT polling reports of the same mode must not keep sending
  // spectrum-scope commands.  The first valid mode after startup counts as
  // a change from "unknown" and initializes the scope once.
  const int old_mode =
      radio->mode_initialized ? rig_modenum(radio->opmode) : -1;

  set_mode_nonfil(opmode, radio);

  const int new_mode = rig_modenum(radio->opmode);
  if (new_mode >= 0 && new_mode != old_mode) {
    set_scope_mode(radio, new_mode);
  }
}


// analyze cmdbuf as Elecraft KX line 
void get_cat_elecraft(struct radio *radio) {

  if (!radio->enabled) return;
  int len;
  len=strlen(radio->cmdbuf);

  int tmp;
  int filt;

  //plogw->ostream->println(radio->cmdbuf);
  if (verbose&VERBOSE_CAT) {
    plogw->ostream->print("get_cat_elecraft() l=");
    plogw->ostream->print(len);
    plogw->ostream->print(":");            
    print_cat_cmdbuf(radio);
  }
  /*
    kenwoodと同じ？
  IF (Transceiver Information; GET only)
  012345678901234567890123456789
  IF12345678901     +yyyyrx 00tmvspbd1 ;
RSP format: IF[f]*****+yyyyrx*00tmvspbd1*; where the fields are defined as follows:
[f] Operating frequency, excluding any RIT/XIT offset (11 digits; see FA command format)
* represents a space (BLANK, or ASCII 0x20)
+ either "+" or "-" (sign of RIT/XIT offset)
yyyy RIT/XIT offset in Hz (range is -9999 to +9999 Hz when computer-controlled)
r 1 if RIT is on, 0 if off
x 1 if XIT is on, 0 if off
t 1 if the K3 is in transmit mode, 0 if receive
m operating mode (see MD command)
v receive-mode VFO selection, 0 for VFO A, 1 for VFO B
s 1 if scan is in progress, 0 otherwise
p 1 if the transceiver is in split mode, 0 otherwise
b Basic RSP format: always 0; K2 Extended RSP format (K22): 1 if present IF response
is due to a band change; 0 otherwise
d Basic RSP format: always 0; K3 Extended RSP format (K31): DATA sub-mode,
if applicable (0=DATA A, 1=AFSK A, 2= FSK D, 3=PSK D)
The fixed-value fields (space, 0, and 1) are provided for syntactic compatibility with existing software.
								      */
  if (strncmp(radio->cmdbuf, "IF", 2) == 0) {
    // information
    // Basic KX/K3 IF response is 38 characters including ';'.  Accept
    // longer extended responses as long as all fields used below exist.
    if (len < 38 || radio->cmdbuf[len - 1] != ';') {
      if (!plogw->f_console_emu) {
        plogw->ostream->print("!IF elecraft len ignored: ");
        plogw->ostream->println(len);
      }
      return;
    }
    for (int i = 2; i < 13; i++) {
      if (radio->cmdbuf[i] < '0' || radio->cmdbuf[i] > '9') {
        if (!plogw->f_console_emu)
          plogw->ostream->println("!IF elecraft frequency ignored");
        return;
      }
    }

    if (verbose & 1) plogw->ostream->print("IF received");
    freq = 0;
    //
    for (int i = 0; i < 11-(FREQ_UNIT==10 ? 1 : 0 ); i++) {
	freq *= 10;
	freq += radio->cmdbuf[i + 2] - '0';
    }
    // check frequency range of the TRX
    //    if (freq <= 1800000UL/FREQ_UNIT || freq >= 1100000000UL) {    
    if (freq2bandid(freq)==0) {    
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored b");
      }
      return;
    }
    set_frequency(freq, radio);
    // set mode
    switch (radio->cmdbuf[29]) {
      case '1':  // LSB
        sprintf(opmode, "LSB");
        break;
      case '2':  // USB
        sprintf(opmode, "USB");
        break;
      case '3':  // CW
        sprintf(opmode, "CW");
        break;
      case '4':  // FM
        sprintf(opmode, "FM");
        break;
      case '5':  // AM
        sprintf(opmode, "AM");
        break;
      case '6':  // RTTY-LSB
        sprintf(opmode, "RTTY");
        break;
      case '7':  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case '9':  // RTTY-USB
        sprintf(opmode, "RTTY-R");
        break;
      case '8':  // DATA-R
      case 'A':  // DATA-FM
      case 'B':  // FM-NB
        sprintf(opmode, "Other");
        break;
        break;
    }
    filt = 1;
    //  if (radio->rig_idx == 0) {
    radio->filt = filt;
    //  }

    //   if (radio->rig_idx == 0) {

    set_mode(opmode, filt, radio);
    //    }
    // check tx/rx status
    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[28] == '1') {
      // TX
      radio->ptt_stat = 1;
    } else {
      radio->ptt_stat = 0;
    }
    //    check_repeat_function();
    //    sequence_manager_tx_status_updated();
    so2r.onTx_stat_update();

  } else if (strncmp(radio->cmdbuf, "SM", 2) == 0) {
    // Elecraft firmware variants may return different numbers of decimal
    // digits.  Parse all digits up to the terminating semicolon.
    tmp = 0;
    int ndigit = 0;
    for (int i = 2; radio->cmdbuf[i] != '\0' && radio->cmdbuf[i] != ';'; i++) {
      if (radio->cmdbuf[i] < '0' || radio->cmdbuf[i] > '9') return;
      tmp = tmp * 10 + radio->cmdbuf[i] - '0';
      ndigit++;
    }
    if (ndigit == 0) return;

    radio->smeter = tmp;
    conv_smeter(radio);
    smeter_postprocess(radio);
    //   }

  } else {
    return;
  }
  if (radio->rig_idx == so2r.focused_radio()) {
    //    if (radio->rig_idx == plogw->focused_radio) {
    upd_display();
  }
}


// analyze cmdbuf as Kenwood Cat response
void get_cat_kenwood(struct radio *radio) {

  if (!radio->enabled) return;
  int len;
  len=strlen(radio->cmdbuf);

  int tmp;
  int filt;

  //plogw->ostream->println(radio->cmdbuf);
  if (verbose&VERBOSE_CAT) {
    plogw->ostream->print("get_cat_kenwood() l=");
    plogw->ostream->print(len);
    plogw->ostream->print(":");            
    print_cat_cmdbuf(radio);
  }
  
  if (strncmp(radio->cmdbuf, "IF", 2) == 0) {
    // information
    if (len!=38) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("!IF kenwood len ignored: ");
	plogw->ostream->println(len);
      }
      return;
    }

    if (verbose & 1) plogw->ostream->print("IF received");
    freq = 0;
    //
    for (int i = 0; i < 11-(FREQ_UNIT==10 ? 1 : 0 ); i++) {
	freq *= 10;
	freq += radio->cmdbuf[i + 2] - '0';
    }

    check_and_set_frequency(radio,freq);
    /*
    // check frequency range of the TRX
    //    if (freq <= 1800000UL/FREQ_UNIT || freq >= 1100000000UL) {    
    if (freq2bandid(freq)==0) {    
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored b");
      }
      return;
    }
    set_frequency(freq, radio);
    */
    // set mode
    switch (radio->cmdbuf[29]) {
      case '1':  // LSB
        sprintf(opmode, "LSB");
        break;
      case '2':  // USB
        sprintf(opmode, "USB");
        break;
      case '3':  // CW
        sprintf(opmode, "CW");
        break;
      case '4':  // FM
        sprintf(opmode, "FM");
        break;
      case '5':  // AM
        sprintf(opmode, "AM");
        break;
      case '6':  // RTTY-LSB
        sprintf(opmode, "RTTY");
        break;
      case '7':  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case '9':  // RTTY-USB
        sprintf(opmode, "RTTY-R");
        break;
      case '8':  // DATA-R
      case 'A':  // DATA-FM
      case 'B':  // FM-NB
        sprintf(opmode, "Other");
        break;
        break;
    }
    filt = 1;
    //  if (radio->rig_idx == 0) {
    radio->filt = filt;
    //  }

    //   if (radio->rig_idx == 0) {

    set_mode(opmode, filt, radio);
    //    }
    // check tx/rx status
    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[28] == '1') {
      // TX
      radio->ptt_stat = 1;
    } else {
      radio->ptt_stat = 0;
    }
    //    check_repeat_function();
    //    sequence_manager_tx_status_updated();
    so2r.onTx_stat_update();
  } else if (strncmp(radio->cmdbuf,"PC",2)==0) {
    // power
    tmp=radio->cmdbuf[2] -'0';
    tmp=tmp*10+(radio->cmdbuf[3]-'0');
    tmp=tmp*10+(radio->cmdbuf[4]-'0');
    radio->power=tmp;
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->println(radio->cmdbuf);
      plogw->ostream->print("power:");
      plogw->ostream->println(radio->power);
    }
  } else if (strncmp(radio->cmdbuf, "SM", 2) == 0) {
    // read meter
    // could be HEX ?
    // QCX case 5 digit hex number
    // kenwood TS-480 case, could be HEX 4 digit number
    tmp = 0;
    bool have_digit = false;
    for (int i = 2; radio->cmdbuf[i] != '\0' && radio->cmdbuf[i] != ';'; i++) {
      if (radio->cmdbuf[i] < '0' || radio->cmdbuf[i] > '9') {
        if (verbose & VERBOSE_CAT) {
          plogw->ostream->print("!SM kenwood invalid response: ");
          plogw->ostream->println(radio->cmdbuf);
        }
        return;
      }
      have_digit = true;
      tmp = tmp * 10 + (radio->cmdbuf[i] - '0');
    }
    if (!have_digit) return;

    radio->smeter = tmp;
    conv_smeter(radio);
    smeter_postprocess(radio);
    //   }

  } else {
    return;
  }
  if (radio->rig_idx == so2r.focused_radio()) {
    //    if (radio->rig_idx == plogw->focused_radio) {
    upd_display();
  }
}


void print_cat_cmdbuf(struct radio *radio)
{
  if (!plogw->f_console_emu) {  
    plogw->ostream->print("cmdbuf len:");
    plogw->ostream->print(strlen(radio->cmdbuf));
    plogw->ostream->print(":");      
    plogw->ostream->println(radio->cmdbuf);
  }

}

// analyze cmdbuf as Yaesu Cat response
void get_cat(struct radio *radio) {
  if (!radio->enabled) return;
  int len;
  len=strlen(radio->cmdbuf);
  int tmp;
  int filt;
  if (verbose&VERBOSE_CAT) {
    print_cat_cmdbuf(radio);
  }
  
  if (strncmp(radio->cmdbuf, "IF", 2) == 0) {
    // information
    // check length
    switch (len) {
    default:
      {
      //    if (len!=28) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("!IF len ignored");
	plogw->ostream->println(len);
      }

      char parser_frame[64];
      int parser_len = radio->cmd_ptr;

      if (parser_len < 0) parser_len = 0;
      if (parser_len >= (int)sizeof(parser_frame)) {
	parser_len = sizeof(parser_frame) - 1;
      }
      
      memcpy(parser_frame, radio->cmdbuf, parser_len);
      parser_frame[parser_len] = '\0';      
      
      console->printf("!IF len ignored%d parser=[%s]\n",
                      radio->cmd_ptr, parser_frame);
 
      return;
      break;
      //    }
      }
    case 30: // FTX-1: IF + 5-byte P1 + 9-byte frequency + ...
      if (radio->rig_spec->cat_type!=CAT_TYPE_YAESU_NEW) return;
      freq = 0;
      for (int i = 0; i < 9-(FREQ_UNIT==10 ? 1 : 0 ); i++) { // FREQ_UNIT=10
	freq *= 10;
	freq += radio->cmdbuf[i + 7] - '0';
      }
      break;
    case 28: // FT-991 FTDX10: IF + 3-byte P1 + 9-byte frequency + ...
      if (radio->rig_spec->cat_type!=CAT_TYPE_YAESU_NEW) return;
      freq = 0;
      for (int i = 0; i < 9-(FREQ_UNIT==10 ? 1 : 0 ); i++) { // FREQ_UNIT=10
	freq *= 10;
	freq += radio->cmdbuf[i + 5] - '0';
      }
      break;
    case 27: // FT-3000
      if (radio->rig_spec->cat_type!=CAT_TYPE_YAESU_OLD) return;      
      freq = 0;
      for (int i = 0; i < 8-(FREQ_UNIT==10 ? 1 : 0 ); i++) { // FREQ_UNIT=10
	freq *= 10;
	freq += radio->cmdbuf[i + 5] - '0';
      }
      break;
    }
    //            012340123456789
    //print_cat():IF001007012640+000000300000;
    //Freq:7012640 Mode:CW
    if (verbose&16) {
      console->print("get_cat():");console->println(radio->cmdbuf);
    }

    // check frequency range of the TRX
    
    //    if (freq <= 1800000UL/FREQ_UNIT || freq >= 11000000000UL) {
    if (freq2bandid(freq)==0) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored c");
      }
      return;
    }

    set_frequency(freq, radio);
    // set mode
    switch (radio->cmdbuf[len-7]) { // was 21
      case '1':  // LSB
        sprintf(opmode, "LSB");
        break;
      case '2':  // USB
        sprintf(opmode, "USB");
        break;
      case '3':  // CW
        sprintf(opmode, "CW");
        break;
      case '4':  // FM
        sprintf(opmode, "FM");
        break;
      case '5':  // AM
        sprintf(opmode, "AM");
        break;
      case '6':  // RTTY-LSB
        sprintf(opmode, "RTTY");
        break;
      case '7':  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case '9':  // RTTY-USB
        sprintf(opmode, "RTTY-R");
        break;
      case '8':  // DATA-R
      case 'A':  // DATA-FM
      case 'B':  // FM-NB
        sprintf(opmode, "Other");
        break;
    }
    filt = 1;
    //  if (radio->rig_idx == 0) {

    radio->filt = filt;
    //  }
    // if (radio->rig_idx == 0) {

    set_mode(opmode, filt, radio);
    // }
  } else if (strncmp(radio->cmdbuf, "SM", 2) == 0) {
    // read meter
    tmp = 0;
    for (int i = 0; i < 3; i++) {
      tmp *= 10;
      tmp += radio->cmdbuf[i + 3] - '0';
    }

    radio->smeter = tmp;
    conv_smeter(radio);
    smeter_postprocess(radio);
    //  }

  } else if (strncmp(radio->cmdbuf, "TX", 2) == 0) {
    // ptt stat
    // sequence
    // 0 R -> 1 PTT send -> 2 PTT release after TX
    //    if (radio->rig_idx == 0) {

    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[2] == '0') {
      //plogw->ostream->print("0");
      if ((radio->ptt_stat == 1) || (radio->ptt_stat == 2)) {
        // previously sending
        radio->ptt_stat = 2;
      } else {
        radio->ptt_stat = 0;  // receiving
      }
    } else {
      radio->ptt_stat = 1;  // transmitting
      //plogw->ostream->print("1");
    }
    //    check_repeat_function();
    //    sequence_manager_tx_status_updated();
    so2r.onTx_stat_update();

  } else if (strncmp(radio->cmdbuf, "RA", 2) == 0) {
    // RF attenuator
    switch (radio->cmdbuf[3]) {
      case '0':  // ATT off
        radio->att = 0;
        break;
      case '1':  // ATT ON 6dB
        radio->att = 6;
        break;
      case '2':  // ATT 12dB
        radio->att = 12;
        break;
      case '3':  // ATT 18dB
        radio->att = 18;
        break;
    }
    if (verbose & VERBOSE_CAT) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("ATT:");
	plogw->ostream->println(radio->att);
      }
    }
  } else if (strncmp(radio->cmdbuf,"PC",2)==0) {
    // power
    tmp=radio->cmdbuf[2] -'0';
    tmp=tmp*10+(radio->cmdbuf[3]-'0');
    tmp=tmp*10+(radio->cmdbuf[4]-'0');
    radio->power=tmp;
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->println(radio->cmdbuf);
      plogw->ostream->print("power:");
      plogw->ostream->println(radio->power);
    }
  } else if (strncmp(radio->cmdbuf, "PA", 2) == 0) {
    // PRE-AMP
    radio->preamp = radio->cmdbuf[3] - '0';
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("preamp:");
      plogw->ostream->println(radio->preamp);
    }
  } else if (strncmp(radio->cmdbuf, "ID", 2) == 0) {
    //
    if (radio != NULL) {
      *radio->rig_spec->rig_identification = '\0';
      strncat(radio->rig_spec->rig_identification, radio->cmdbuf + 2, 4);
      if (verbose & VERBOSE_CAT) {
        plogw->ostream->print("ID:");
        plogw->ostream->println(radio->cmdbuf);
        plogw->ostream->print("ID copied:");
        plogw->ostream->println(radio->rig_spec->rig_identification);
      }
    }
  }

  //  } else {
  //    return ;
  //  }
  if (radio->rig_idx == so2r.focused_radio()) {

    upd_display();
  }
}






void conv_smeter(struct radio *radio) {
  // ATS Mini status RSSI is reported by the SI4732/5 in dBuV.
  // For a 50-ohm system, dBm ~= dBuV - 107.
  // show_smeter==2 is kept as a diagnostic/raw mode and displays the ATS
  // RSSI count itself instead of applying the dBm conversion.
  if (radio->rig_spec->rig_type == RIG_TYPE_ATS_MINI) {
    if (plogw->show_smeter == 2) {
      radio->smeter *= SMETER_UNIT_DBM;
    } else {
      radio->smeter =
          (radio->smeter - 107) * SMETER_UNIT_DBM;
    }
    return;
  }

  // QMX SM responses are already in S-meter units (for example SM007; = 7).
  // Store them in the common internal 0.1-unit representation used by the
  // display, including zero so that SM000; actively clears the old reading.
  if (radio->rig_spec->rig_type == RIG_TYPE_QMX) {
    radio->smeter *= SMETER_UNIT_DBM;
    return;
  }

  if (plogw->show_smeter == 2) {
    if (radio->smeter == 0) {
      radio->smeter = SMETER_MINIMUM_DBM;
    }
    return;  // no conversion
  }
  if (radio->smeter == 0) {
    radio->smeter = SMETER_MINIMUM_DBM;  // no smeter reading -> -200dBm
    return;
  }
  // convert smeter reading into dBm if possible
  switch (radio->rig_spec->rig_type) {
    case RIG_TYPE_ICOM_IC7300:  // IC-7300  ... almost the same as IC705 except for preamp gain in 50MHz
      if (radio->smeter > 123) {
	radio->smeter = (0.5108 * (radio->smeter*1.0) - 133.92) * SMETER_UNIT_DBM;
      } else {
	radio->smeter = (0.2111 * (radio->smeter*1.0) - 96.904) * SMETER_UNIT_DBM;
      }
      // correction ATT and preamp
      switch (radio->preamp) {
      case 0: break;
      case 1:
	if (radio->bandid == 7) { // 1 1.8 2 3.5 3 7 4:14 5:21 6:28 7:50
	  radio->smeter -= 15 * SMETER_UNIT_DBM;
	} else {
	  radio->smeter -= 10 * SMETER_UNIT_DBM; 
	}
	break;
      case 2: radio->smeter -= 14 * SMETER_UNIT_DBM; break;
      }
      radio->smeter += radio->att * SMETER_UNIT_DBM;
      break;
    case RIG_TYPE_ICOM_IC705:  // IC-705
      if (radio->bandid > 7) {
        // 8: 144
        // 144 and up
        if (verbose & VERBOSE_CAT) plogw->ostream->println("144MHz and Up");
        if (radio->smeter >= 125) {
          radio->smeter = (0.5 * (radio->smeter*1.0) - 141.5) * SMETER_UNIT_DBM; // high map
        } else {
          radio->smeter = (0.2313 * (radio->smeter*1.0) - 108.0) * SMETER_UNIT_DBM; // low map
        }
        // correction ATT and preamp
        switch (radio->preamp) {
          case 0: break;
          case 1: radio->smeter -= 14 * SMETER_UNIT_DBM; break;
        }
      } else {
        // 50 MHz and down
        if (verbose & VERBOSE_CAT) plogw->ostream->println("50MHz and down");
        if (radio->smeter >= 123) {
          radio->smeter = (0.5108 * (radio->smeter*1.0) - 133.92) * SMETER_UNIT_DBM;
        } else {
          radio->smeter = (0.2072 * (radio->smeter*1.0) - 96.694) * SMETER_UNIT_DBM;
        }
        // correction ATT and preamp
        switch (radio->preamp) {
          case 0: break;
          case 1: radio->smeter -= 10 * SMETER_UNIT_DBM; break;
          case 2: radio->smeter -= 14 * SMETER_UNIT_DBM; break;
        }
        radio->smeter += radio->att * SMETER_UNIT_DBM;
      }
      break;
    case RIG_TYPE_ICOM_IC9700:  // IC-9700
      // 144 and up
      if (radio->smeter >= 121) {
        radio->smeter = (0.4934 * (radio->smeter*1.0) - 145.16) * SMETER_UNIT_DBM; // high map
      } else {
        radio->smeter = (0.1855 * (radio->smeter*1.0) - 108.1) * SMETER_UNIT_DBM; // low map
      }
      // correction ATT and preamp
      switch (radio->preamp) {
        case 0: break;
        case 1: radio->smeter -= 10.82 * SMETER_UNIT_DBM; break;
      }
      radio->smeter += radio->att * SMETER_UNIT_DBM;
      break;
    case RIG_TYPE_YAESU:  // yaesu (measured for FTDX10 and FT991A)
      if (strncmp(radio->rig_spec->rig_identification, "0670", 4) == 0) {
        // FT991A
        if (verbose & VERBOSE_CAT) {
          plogw->ostream->println("FT991A");
        }
        if (radio->bandid <= 7) {
          // 50MHz and down
          if (verbose & VERBOSE_CAT) plogw->ostream->println("50MHz and down");
          if (radio->smeter >= 130) {  // =0.5026*T6-128.57
            radio->smeter = (0.5026 * (radio->smeter*1.0) - 128.57) * SMETER_UNIT_DBM;
          } else {  // =0.1819*T6-86.916
            radio->smeter = (0.1819 * (radio->smeter*1.0) - 86.916) * SMETER_UNIT_DBM;
          }
          if (radio->bandid == 7) {
            // 50MHz correction needed // -2.7247 dB
            radio->smeter += -2.7247 * SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 10.9 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 26.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
          break;
        } else {
          // 144MHz and up
          if (verbose & VERBOSE_CAT) plogw->ostream->println("144MHz and up ");
          if (radio->smeter >= 130) {  //=0.5006*T25-154.55
            radio->smeter = (0.5006 * (radio->smeter*1.0) - 154.55) * SMETER_UNIT_DBM;
          } else {  // =0.1758*T25-112.28
            radio->smeter = (0.1758 * (radio->smeter*1.0) - 112.28) * SMETER_UNIT_DBM;
          }
          // no preamp and ATT
        }
        break;


      } else if (strncmp(radio->rig_spec->rig_identification, "0761", 4) == 0) {
        // FTDX10 0761
        if (verbose & VERBOSE_CAT) {
          plogw->ostream->println("FTDX10");
        }

        if (radio->bandid > 5) {
          // 28MHz and up
          if (verbose & VERBOSE_CAT) plogw->ostream->println("28MHz and up");
          if (radio->smeter >= 129) {
            radio->smeter = (0.5031 * (radio->smeter*1.0) - 136.01) * SMETER_UNIT_DBM;
          } else {
            radio->smeter = (0.1655 * (radio->smeter*1.0) - 92.5) * SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 9.8 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 18.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
        } else {
          // 21MHz and down
          if (verbose & VERBOSE_CAT) plogw->ostream->println("21MHz and down");
          if (radio->smeter >= 126) {
            radio->smeter = (0.5058 * ((float)radio->smeter*1.0) - 128.08) * (float)SMETER_UNIT_DBM;
          } else {
            radio->smeter = (0.1655 * ((float)radio->smeter*1.0) - 85.188) * (float)SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 9.8 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 18.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
        }
        break;
      }
  }
}

void smeter_postprocess(struct radio *radio)
{
  const bool zero_is_valid =
      (radio->rig_spec->rig_type == RIG_TYPE_QMX ||
       radio->rig_spec->rig_type == RIG_TYPE_ATS_MINI) &&
      radio->smeter == 0;
  if ((radio->smeter != 0 || zero_is_valid) &&
      radio->smeter != SMETER_MINIMUM_DBM) {
    // if smeter value is valid
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("SMETER:");
      plogw->ostream->print(radio->smeter);
      plogw->ostream->print(" Radio:");
      plogw->ostream->println(radio->rig_idx);
    }
    if (!plogw->f_antalt_switched) {
      // sort data on the status of antenna relay
      int relay; relay = plogw->relay[0] + plogw->relay[1] * 2;
      if (relay >= 0 && relay <= 3) {
        radio->smeter_record[relay] = radio->smeter;
        if (plogw->f_rotator_enable) {
          radio->smeter_azimuth[relay] = plogw->rotator_az;
        } else {
          radio->smeter_azimuth[relay] = -1;
        }
        radio->f_smeter_record_ready = 1; // notify new record
      }
      // alternate antenna relay if necessary

    }

  }
  if (radio->smeter_stat == 1) {
    if (radio->smeter > radio->smeter_peak) radio->smeter_peak = radio->smeter;
  }
      if (plogw->f_antalt > 0) {
        plogw->antalt_count++;
        if (plogw->antalt_count >= plogw->f_antalt) {
          // alternate antenna relay1
          alternate_antenna_relay();
          plogw->antalt_count = 0;
        }
      }
  // reset flag
  plogw->f_antalt_switched = 0;
}


void get_cat_ft817(struct radio *radio) {
  if (!radio->enabled) return;
  int tmp;
  int filt;
  byte mode_code;
  
  // A complete fixed-length FT-817 response has arrived.  Release the
  // query gate before applying the decoded status so the next periodic
  // query can be sent.
  radio->f_civ_response_expected = 0;
  radio->civ_response_timer = 0;

  switch (radio->cat_status&0xf0) {
  case 0x10: // RX status
    radio->smeter=radio->cmdbuf[0]&0xf; // smeter
    conv_smeter(radio);
    smeter_postprocess(radio);
    // others ignore 
    break;
  case 0x20: // TX status
    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[0]&0x80) {
      radio->ptt_stat =1;
    } else {
      if ((radio->ptt_stat == 1) || (radio->ptt_stat == 2)) {
        // previously sending
        radio->ptt_stat = 2;
      } else {
        radio->ptt_stat = 0;  // receiving
      }
    }
    so2r.onTx_stat_update();
    // others ignore
    break;
  case 0x30: // awaiting Freq & Mode status
    freq=0;
    freq=freq+bcd2dec(radio->cmdbuf[0]); // 100/10M
    freq=freq*100+bcd2dec(radio->cmdbuf[1]); // 1M/100k
    freq=freq*100+bcd2dec(radio->cmdbuf[2]); // 10/1k
    freq=freq*100+bcd2dec(radio->cmdbuf[3]); // 100/10Hz
    freq=freq*(10/FREQ_UNIT);
    check_and_set_frequency(radio,freq);
    // The FT-817/818 may set bit 7 in the returned mode byte when the
    // optional narrow filter is selected (for example 0x82 for CW-N).
    // The lower nibble is the actual CAT mode code.
    mode_code = ((byte)radio->cmdbuf[4]) & 0x0f;
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("FT817 mode raw=0x");
      plogw->ostream->print((byte)radio->cmdbuf[4], HEX);
      plogw->ostream->print(" code=0x");
      plogw->ostream->println(mode_code, HEX);
    }
    switch (mode_code) {
      case 0x00:  // LSB
        sprintf(opmode, "LSB");
        break;
      case 0x01:  // USB
        sprintf(opmode, "USB");
        break;
      case 0x02:  // CW
        sprintf(opmode, "CW");
        break;
      case 0x08:  // FM
        sprintf(opmode, "FM");
        break;
      case 0x04:  // AM
        sprintf(opmode, "AM");
        break;
      case 0x03:  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case 0x06:  // WFM
        sprintf(opmode, "Other");
        break;
    default:
        sprintf(opmode, "Other");
        break;
    }
    filt = 1;
    radio->filt = filt;
    set_mode(opmode, filt, radio);

    // reset cat status
    radio->cat_status=0;
    break;
    
  default: // ignore
    radio->cat_status=0;
    return ;
    break;
  }

  if (radio->rig_idx == so2r.focused_radio()) {

    upd_display();
  }
  
}


struct radio *search_civ_address(int civaddr)
{
  // search the civ address from enabled raddios and return with radio pointer or NULL
  struct radio *radio;
  struct rig *rig_spec;
  for (int i=0;i<N_RADIO;i++) {
    radio=&radio_list[i];
    if (!radio->enabled) continue;
    rig_spec=radio->rig_spec;
    if (rig_spec==NULL) continue;
    if (radio->rig_spec->cat_type==CAT_TYPE_CIV) {
      if (rig_spec->civaddr==civaddr) {
	return radio;
      }
    }
  }
  return NULL;
}

int civ_check_postamble(struct radio *radio, const char *type) {
  if (radio->cmdbuf[radio->cmd_ptr-1] != 0xfd) {
    if (!plogw->f_console_emu) {
      if (verbose&VERBOSE_CAT) {
      plogw->ostream->print("! CI-V pkt postamble ");
      plogw->ostream->print(type);
      plogw->ostream->print(" :");
      
      for (int i=0;i<int(radio->cmd_ptr);i++) {
	plogw->ostream->print(int(radio->cmdbuf[i]),HEX);
	plogw->ostream->print(" ");
      }
      plogw->ostream->println("");
      }
    }
    return 0;
  } 
  return 1;
}
int civ_check_size(struct radio *radio, int size, const char *type) {
  int tmp;
  tmp=size;

  while (tmp>0) {
    if (tmp %100  ==  radio->cmd_ptr) {
      return 1;
    }
    tmp=tmp/100;
  }
  if (!plogw->f_console_emu) {
      if (verbose&VERBOSE_CAT) {    
    plogw->ostream->print("! CI-V pkt size ");
    plogw->ostream->print(type);
    plogw->ostream->print("=");      
    plogw->ostream->print(int(radio->cmd_ptr));
    plogw->ostream->print(" :");            
    for (int i=0;i<int(radio->cmd_ptr);i++) {
      plogw->ostream->print(int(radio->cmdbuf[i]),HEX);
      plogw->ostream->print(" ");
    }
    plogw->ostream->println("");
      }
  }
  return 0;
}



int check_and_set_frequency(struct radio *radio, unsigned long freq) {
    if (freq2bandid(freq)==0) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored a");
      }
      return 0;
    }
    // check transverter frequency
    radio->transverter_in_use = 0;
    for (int i = 0; i < NMAX_TRANSVERTER; i++) {
      if (radio->rig_spec->transverter_freq[i][0] == 0) break;
      if (!radio->rig_spec->transverter_enable[i]) continue;
      if ((freq >= radio->rig_spec->transverter_freq[i][2]) && (freq <= radio->rig_spec->transverter_freq[i][3])) {
	// transverter frequency
	freq = freq + (radio->rig_spec->transverter_freq[i][0] - radio->rig_spec->transverter_freq[i][2]);
	radio->transverter_in_use = 1;
	break;
      }
    }
    // store to rig information
    set_frequency(freq, radio);
    return 1;
}
// 現在 CI-V バスが単一のリグ占有の前提となっているので、上手くハンドリング
// できないと考えられる。civport_shared[]に共有しているradio のリストを定義する等して、ここで他のradio の受信処理も行うのが適切？

void get_civ(struct radio *radio) {
  int filt;
  int tmp;
  char *p;
  int tmplat,tmplon,tmplatmin,tmplonmin;

  struct radio *radio1;
  //  u8x8.setFont(u8x8_font_chroma48medium8_r);
  // parse civ sentense and set information accordingly
  // check originated from rig
  if (!((radio->cmdbuf[0] == 0xfe) && (radio->cmdbuf[1] == 0xfe))) {
    if (verbose &VERBOSE_CAT) {
    plogw->ostream->printf("! preamble %02X %02X %02X \n",radio->cmdbuf[0],radio->cmdbuf[1],radio->cmdbuf[2]);
    // ci-v port see what this unit send, so sometimes results in corrupted self port ci-v packet output
    }
    return;
  }
  if (radio->cmdbuf[3] != radio->rig_spec->civaddr) {
    if (radio->cmdbuf[3] == 0xe0) {
      // I myself packet
      return;
    }
    // check rotator
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("! from addr=");
      plogw->ostream->println(radio->cmdbuf[3], HEX);
    }
    if (radio->cmdbuf[3] == 0x91) {
      // rotator
      if (radio->cmdbuf[4] == 0x31) {
        // rotator command
        switch (radio->cmdbuf[5]) {
	case 0:  // query az
	  tmp = bcd2dec(radio->cmdbuf[6]) + 100 * bcd2dec(radio->cmdbuf[7]);
	  if (tmp >= 0 && tmp <= 360) {
	    plogw->rotator_az = tmp;
	    if (verbose & 2) {
	      plogw->ostream->print("Rotator AZ=");
	      plogw->ostream->println(plogw->rotator_az);
	    }
	  }
	  break;
        }
      }
      return;
    }
    if ((radio1=search_civ_address(radio->cmdbuf[3]))!=NULL) {
	// found matching radio so swap radio and process accordingly
	radio=radio1;
    } else  {
      // from addr check
      if (verbose&VERBOSE_CAT) {
	plogw->ostream->print("! CI-V addr read ");
	plogw->ostream->print(radio->cmdbuf[3],HEX);
	plogw->ostream->print(" !=  Rig CI-V addr");
	plogw->ostream->println(radio->rig_spec->civaddr,HEX);
      }
	return;	
    }
  }
  // check to addr
  if ((radio->cmdbuf[2]!= 0xe0) && (radio->cmdbuf[2]!=0x00) ) {
      if (verbose&VERBOSE_CAT) {    
	plogw->ostream->print("! CI-V to addr:");
	plogw->ostream->println(radio->cmdbuf[2],HEX);
      }
      return;
  }
  
  //  plogw->ostream->println("get_civ() addr pass");

  // A correctly addressed CI-V frame proves that this radio is reachable.
  // Use it to arm/service one-shot clock synchronization.
  service_icom_clock_sync_on_rx(radio);

  radio->f_civ_response_expected = 0;
  radio->civ_response_timer = 0;
  switch (radio->cmdbuf[4]) {
  case 0:  // frequency
  case CAT_TYPE_NOCAT:
    // check size
    if (!civ_check_size(radio,11,"Freq")) break; // permit size=34, 8
    // check postamble (FD)
    if (!civ_check_postamble(radio,"Freq")) break;	
    //  plogw->ostream->println("get_civ() frequency");
    freq = 0;
    freq = bcd2dec(radio->cmdbuf[9]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[8]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[7]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[6]);
    freq = freq * (100/FREQ_UNIT) + bcd2dec(radio->cmdbuf[5])/FREQ_UNIT;

    check_and_set_frequency(radio,freq);
    /*    
    //    console->print("Freq received=");
    //    console->println(freq);
    // check frequency range of the TRX
    if (freq2bandid(freq)==0) {
      //    if (freq < 1800000UL/FREQ_UNIT || freq >= 1100000000UL) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored a");
      }
      break;
    }
    // check transverter frequency
    radio->transverter_in_use = 0;
    for (int i = 0; i < NMAX_TRANSVERTER; i++) {
      if (radio->rig_spec->transverter_freq[i][0] == 0) break;
      if (!radio->rig_spec->transverter_enable[i]) continue;

      if ((freq >= radio->rig_spec->transverter_freq[i][2]) && (freq <= radio->rig_spec->transverter_freq[i][3])) {
	// transverter frequency
	freq = freq + (radio->rig_spec->transverter_freq[i][0] - radio->rig_spec->transverter_freq[i][2]);
	radio->transverter_in_use = 1;
	break;
      }
    }
    // store to rig information

    //

    set_frequency(freq, radio);
*/

    break;
  case 0x11:  // ATT
    // check size
    if (!civ_check_size(radio,7,"ATT")) break; // permit size=34, 8
    // check postamble (FD)
    if (!civ_check_postamble(radio,"ATT")) break;
    radio->att = bcd2dec(radio->cmdbuf[5]);
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("ATT:");
      plogw->ostream->println(radio->att);
    }
    break;


  case 0x16:  //
    switch (radio->cmdbuf[5]) {
    case 0x02:  // preamp
      // check size
    if (!civ_check_size(radio,8,"PRE")) break; // permit size=34, 8
      // check postamble (FD)
    if (!civ_check_postamble(radio,"PRE")) break;
      radio->preamp = bcd2dec(radio->cmdbuf[6]);
      if (verbose & VERBOSE_CAT) {
	
	plogw->ostream->print("PREAMP:");
	plogw->ostream->println(radio->preamp);
      }
      break;
    }
    break;
  case 0x19:  // ID query --> just stored CI-V ID to cmdbuf[6]
    if (verbose & VERBOSE_CAT ) {
      plogw->ostream->print("CI-V ID:");
      plogw->ostream->print(radio->cmdbuf[5], HEX);
      plogw->ostream->print(radio->cmdbuf[6], HEX);

      plogw->ostream->println(radio->cmdbuf[7], HEX);
    }
    // check size
    if (!civ_check_size(radio,8,"ID")) break; // permit size=34, 8    
    // check postamble (FD)
    if (!civ_check_postamble(radio,"ID")) break;
    
    sprintf(radio->rig_spec->rig_identification,"%02X",radio->cmdbuf[6]); // store ID to rig_identification for ICOM
    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("ID from CI-V:");
      plogw->ostream->println(radio->rig_spec->rig_identification);
    }
    break;
  case 0x14:
    switch (radio->cmdbuf[5]) {
    case 0x0A: // power 
      //		  if (!civ_check_size(radio,) break; // 
      tmp = bcd2dec(radio->cmdbuf[6]);
      tmp = tmp * 100 + bcd2dec(radio->cmdbuf[7]);
      radio->power = tmp; 
      if (verbose&VERBOSE_CAT) {
	console->print("power civ:");
	console->println(radio->power);
      }
      break;
    }
    break;
  case 0x15:  //
    // check subcmd
    switch (radio->cmdbuf[5]) {
    case 0x02:  // s meter
      // check size
      if (!civ_check_size(radio,9,"S")) break; // permit size=34, 8      
      // check postamble (FD)
      if (!civ_check_postamble(radio,"S")) break;	

      // read s-meter
      tmp = bcd2dec(radio->cmdbuf[6]);
      tmp = tmp * 100 + bcd2dec(radio->cmdbuf[7]);


      radio->smeter = tmp;
      conv_smeter(radio);
      smeter_postprocess(radio);
      

      //          radio->smeter = cmdbuf[6] * 256 + cmdbuf[7];  // s-meter reading is BCD not binary  corrected 21/12/30
      break;
    }
    break;
  case 0x23: // GPS data (IC-705)
    //                          1  2 3  4  5 6  7 8   9 10111213 141516 17181920 21 2223 24 25 26 27 28
    //        0   1  2  3  4 5  6  7 8  9 1011 12 13 14 15161718 192021 22232425 26 2728 29 30 21 32 33
    //1 ～ 5 緯度（dd°mm�mmm 形式）
    //6 ～ 11 経度（ddd°mm�mmm 形式）
    //12 ～ 15 高度（0�1m刻み）
    //16、17 進路（1度刻み）
    //18 ～ 20 速度（0�1km/h刻み）
    //21 ～ 27 日時（UTC）

    // check size
    if (!civ_check_size(radio,34,"GPS")) break; // permit size=34, 8
    // check postamble (FD)
    if (!civ_check_postamble(radio,"GPS")) break;	
    p=radio->cmdbuf;
    //
    tmplat=bcd2dec(p[6]);
    //    console->print("tmplat:");console->println(tmplat);
    tmplon=bcd2dec(p[11])*100+bcd2dec(p[12]);
    tmplatmin=0;
    for (int i=7;i<=9;i++) {
      tmplatmin*=100;
      tmplatmin+=bcd2dec(p[i]);
    }
    tmplonmin=0;
    for (int i=13;i<=15;i++) {
      tmplonmin*=100;
      tmplonmin+=bcd2dec(p[i]);
    }
    plogw->lat=tmplat+tmplatmin/600000.;
    plogw->lon=tmplon+tmplonmin/600000.;
    plogw->elev=((bcd2dec(p[17])*100+bcd2dec(p[18]))*100+bcd2dec(p[19]))*0.1;
    
    // grid locator set
    strcpy(plogw->grid_locator+2,get_mh(plogw->lat,plogw->lon,6));
    strcpy(plogw->grid_locator_set,get_mh(plogw->lat,plogw->lon,6));
    //
    if (verbose&VERBOSE_CAT) {
      console->print("GPS CIV:");
      for (int i=0;i<radio->cmd_ptr;i++) {
	console->print(radio->cmdbuf[i],HEX);
	console->print(" ");
      }
      console->println("");
      console->print("cmd_ptr ");console->println(radio->cmd_ptr);
      console->print("gps lat:");console->print(plogw->lat);
      console->print(" lon:");console->print(plogw->lon);
      console->print(" elev:");console->print(plogw->elev);        
      console->print(" calc gl:");console->println(plogw->grid_locator+2);
    }
    break;
  case 0x24:  // TX stat
    // check size
    if (!civ_check_size(radio,9,"TX")) break; // permit size=34, 8
    // check postamble (FD)
    if (!civ_check_postamble(radio,"TX")) break;	

    if (verbose & VERBOSE_CAT) {
      plogw->ostream->print("TX");
      plogw->ostream->print((char)(radio->cmdbuf[5] + '0'));
      plogw->ostream->print((char)(radio->cmdbuf[6] + '0'));
      plogw->ostream->print((char)(radio->cmdbuf[7] + '0'));
    }
    if (radio->cmdbuf[6] == 1) {
      radio->ptt_stat_prev = radio->ptt_stat;
      if (radio->cmdbuf[7] == 1) {
	// TX
	radio->ptt_stat = 1;
      }
      if (radio->cmdbuf[7] == 0) {
	// RX
	radio->ptt_stat = 0;
      }
      //      check_repeat_function();
      //      sequence_manager_tx_status_updated();
      so2r.onTx_stat_update();
    }
    break;
  case 1:  // mode
  case 4:
    // check size
    if (!civ_check_size(radio,8,"MODE")) break; // permit size=34, 8

    // check postamble (FD)
    if (!civ_check_postamble(radio,"MODE")) break;
    
    switch (radio->cmdbuf[5]) {
    case 0:  // LSB
      sprintf(opmode, "LSB");
      break;
    case 1:  // USB
      sprintf(opmode, "USB");
      break;
    case 2:  // AM
      sprintf(opmode, "AM");
      break;
    case 3:  // CW
      sprintf(opmode, "CW");
      break;
    case 7:  // cw-r
      sprintf(opmode, "CW-R");
      break;
    case 4:  // RTTY
      sprintf(opmode, "RTTY");
      break;
    case 6: // WFM
      sprintf(opmode, "Other");
      break;
    case 8:  // RTTY
      sprintf(opmode, "RTTY-R");
      break;
    case 17: // DV
      sprintf(opmode, "DV");
      break;
    case 5:  // FM
      sprintf(opmode, "FM");
      break;
    default:  // other
      //      sprintf(opmode, "Other");
      // not in the list
      plogw->ostream->println("! CI-V MODE not in the list");
      return;
      break;
    }
    filt = radio->cmdbuf[6];
    // check value range 
    if (!(filt>=1 && filt<=3)) {
      plogw->ostream->print("! CI-V filt ");
      plogw->ostream->print(filt);
      plogw->ostream->println(" out of range.");
      return;
    }

    radio->filt = filt;
    if (verbose & 1) {
      plogw->ostream->print("filt:");
      plogw->ostream->println(filt);
    }
    
    // it is rx mode for satellite
    set_mode(opmode, filt, radio);
    break;
  }
  //
  if (radio->rig_idx == so2r.focused_radio()) {
    upd_display();
  }
}

void print_civ(struct radio *radio) {
  if (!(verbose & 1)) return;
  char ostr[32];
  sprintf(ostr, "print_civ():");
  plogw->ostream->print(ostr);
  for (int i = 0; i < radio->cmd_ptr; i++) {
    sprintf(ostr, "%02X ", radio->cmdbuf[i]);
    plogw->ostream->print(ostr);
  }
  plogw->ostream->println("");
}

void print_cat(struct radio *radio) {
  if (!(verbose & 1)) return;
  if (!plogw->f_console_emu) {  
    plogw->ostream->print("print_cat():");

    for (int i = 0; i < radio->cmd_ptr; i++) {
      plogw->ostream->print(radio->cmdbuf[i]);
    }
    plogw->ostream->println("");
  }
}



int receive_civport_count=0; // debugging receive_civport

//


void receive_pkt_handler_portnum(struct mux_packet *packet,int port_num)
{
  // assume only btserial is arriving here
  //  console->print("receive_pkt_handler_btserial():");
  struct radio *radio;
  for (int i=0;i<3;i++) {
    radio=&radio_list[i];
    //    if(radio->rig_spec->civport_num == 1 ) { // serial BT
    if (radio->rig_spec->civport_num == port_num ) {
      /*      console->print("found in radio portnum =");
      console->print(port_num);
      console->print(" in radio#");      
      console->print(i);
      console->print(" pkt idx=");
      console->println(packet->idx);
      */
      // deliver to this radio
      char c;
      for (int j=0;j<packet->idx;j++) { // packet->idx number of data in the packet
	c=packet->buf[j];
	radio->bt_buf[radio->w_ptr] = c;
	radio->w_ptr++;
	radio->w_ptr %= 256;
      }
      return;
    }
  }
}


void receive_pkt_handler_btserial(struct mux_packet *packet)
{
  //  receive_pkt_handler_portnum(packet,1);
  receive_pkt_handler_portnum(packet,1);  
}

void receive_pkt_handler_cat2(struct mux_packet *packet)
{
  receive_pkt_handler_portnum(packet,4);
}

void receive_pkt_handler_cat3(struct mux_packet *packet)
{
  receive_pkt_handler_portnum(packet,3);
}


void receive_civport(struct radio *radio) {
  int type;
  Stream *port;
  struct radio *radio1;
  struct catmsg_t catmsg; int ret; // CATUSBRx 
  char c;
  int count;
  int i;


  // check CIV priority in shared radios 
  type = radio->rig_spec->cat_type;
  if ((type == 0) ) {
    // CIV 
    // check if other rig_idx is set to active and CIV then only minimum rig_idx
    for (int i=0;i<radio->rig_idx;i++) {
      radio1=&radio_list[i];
      if (radio1->enabled && radio1->rig_spec!=NULL) {
	if (radio1->rig_spec->cat_type==0 && (radio1->rig_spec->civport_num == radio->rig_spec->civport_num)) {
	  // priotized radio is civ and working
	  return;
	}
      }
    }
  }

  
  port=radio->rig_spec->civport;
  switch(radio->rig_spec->civport_num) {
  case 1: // serial BT
    if (f_mux_transport!=0) return; // only read here for direct transport
    port=&Serial2;
    break;
  case -1: // USB
    // check queue
    while (uxQueueMessagesWaiting(xQueueCATUSBRx)){
      ret = xQueueReceive(xQueueCATUSBRx, &catmsg, 0);
      if (ret==pdTRUE) {
        if (verbose & VERBOSE_USB) {
          static uint32_t bind_civ_rx_seq = 0;
          ++bind_civ_rx_seq;
          console->printf(
            "[USBBIND] RX_CONSUME seq=%lu path=CIV radio=%d spec=%d name=%s "
            "cat_type=%d civport=%d size=%d data=",
            (unsigned long)bind_civ_rx_seq, radio->rig_idx, radio->rig_spec_idx,
            radio->rig_spec->name ? radio->rig_spec->name : "(null)",
            radio->rig_spec->cat_type, radio->rig_spec->civport_num, catmsg.size);
          const int bind_dump_n = catmsg.size < 16 ? catmsg.size : 16;
          for (int bi = 0; bi < bind_dump_n; ++bi) {
            const uint8_t bc = (uint8_t)catmsg.buf[bi];
            if (bc >= 0x20 && bc <= 0x7e) console->print((char)bc);
            else console->printf("\\x%02X", bc);
          }
          if (catmsg.size > bind_dump_n) console->print("...");
          console->println();
        }
	
	if (verbose &1)
	  console->printf("xQueueReceive() : CATUSBRx ret = %d size =%d\n", ret,catmsg.size);
        verbose_cat_rx_dump(radio, "usb", catmsg.buf, catmsg.size);
	count=0;
	for (i=0;i<catmsg.size;i++) {
	  radio->bt_buf[radio->w_ptr] = catmsg.buf[i];
	  radio->w_ptr++;
	  radio->w_ptr %= 256;
	  count++;
	}
	if (count>receive_civport_count) receive_civport_count=count;	
      }
    }
    return;
    break;
  case 4: // can only read in mux transport
    return;
    break;
  default:
    break;
  }
  if (port == NULL) return;

  
  type = radio->rig_spec->cat_type;
  if ((type == 1) || (type == 2) || (type == 3)) {
    // not ICOM CI-V
    receive_cat_data(radio);
    return;
  }

  //  char c;
  //  int count;
  count=0;
  while (port->available() && (count<40)) {
    c = port->read();
    radio->bt_buf[radio->w_ptr] = c;
    radio->w_ptr++;
    radio->w_ptr %= 256;
    count++;
  }
  // record max number of received characters during the civport interrupt for debugging
  if (count>receive_civport_count) receive_civport_count=count;
}


void clear_civ(struct radio *radio) {
  radio->cmd_ptr = 0;
  radio->cmdbuf[radio->cmd_ptr] = '\0';
}

// antenna switch commands
int antenna_switch_commands(char *cmd)
{
  if (strncmp(cmd, "ANT0", 4) == 0) {
    int tmp, tmp1;
    if (sscanf(cmd + 4, "%d", &tmp1) == 1) {
      RELAY_control(0, tmp1);
      plogw->ostream->print("ANT0 set ");
      plogw->ostream->println(tmp1);
    } else {
      plogw->ostream->println("ANT0/1 state(0/1)");
    }
    return 1;
  }
  if (strncmp(cmd, "ANT1", 4) == 0) {
    int tmp, tmp1;
    if (sscanf(cmd + 4, "%d", &tmp1) == 1) {
      RELAY_control(1, tmp1);
      plogw->ostream->print("ANT0 set ");
      plogw->ostream->println(tmp1);
    } else {
      plogw->ostream->println("ANT0/1 state(0/1)");
    }
    return 1;
  }
  return 0;
}

int antenna_alternate_command(char *s)
{
  if (strncmp(s, "ANTALT", 6) == 0) {
    int tmp1, tmp;
    tmp1 = sscanf(s + 6, "%d", &tmp);
    if (tmp1 == 1 && tmp >= 0) {
      plogw->f_antalt = tmp;
    } else {
      plogw->f_antalt = 1; // default number of signal reception before alternating antenna
    }
    plogw->antalt_count = 0; // reset counter
    plogw->ostream->print("antenna alternate (antalt)=");
    plogw->ostream->println((int)plogw->f_antalt);
    return 1;
  }
  return 0;
}

// signal command to print signal strength, ant selection and azimuth
int signal_command(char *s)
{
  if (strncmp(s, "SIGNAL", 6) == 0) {
    // toggle show signal status periodically
    plogw->f_show_signal = 1 - plogw->f_show_signal;
    plogw->ostream->print("show_signal=");
    plogw->ostream->println((int)plogw->f_show_signal);
    return 1;
  }
  return 0;
}
// rotator command
int rotator_commands(char *s)
{

  struct radio *radio;
  radio = so2r.radio_selected();
  if (strncmp(s, "ROT", 3) == 0) {
    int tmp, tmp1;
    // rotator commands
    // check subcmds
    if (strncmp(s + 3, "EN", 2) == 0) {
      // enable/disable rotator
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        // enable/disable rotator query
        plogw->f_rotator_enable = tmp;
        plogw->ostream->print("f_rotator_enable = ");
        plogw->ostream->println(plogw->f_rotator_enable);
      } else {
        plogw->ostream->println("rotator enable en0/1 ?");
      }
    }
    if (strncmp(s + 3, "TYPE", 4) == 0) {
      // rotator type set
      tmp1 = sscanf(s + 7, "%d", &tmp);

      plogw->ostream->print("sent rotator type;");
      plogw->ostream->println(tmp1);
      byte cmds[2];
      cmds[0] = 0x05;  // type
      cmds[1] = (byte)tmp1;
      send_rotator_command_civ(cmds, 2);
    }
    if (strncmp(s + 3, "NORTH", 5) == 0) {
      byte cmds[1];
      cmds[0] = 0x02;  // set north
      send_rotator_command_civ(cmds, 1);
      plogw->ostream->println("rotator set north command");
    }
    if (strncmp(s + 3, "SOUTH", 5) == 0) {
      byte cmds[1];
      cmds[0] = 0x03;  // set south
      send_rotator_command_civ(cmds, 1);
      plogw->ostream->println("rotator set south command");
    }

    if (strncmp(s + 3, "TR", 2) == 0) {
      // enable/disable rotator
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        // enable/disable rotator
        plogw->f_rotator_track = tmp;
        plogw->ostream->print("f_rotator_track = ");
        plogw->ostream->println(plogw->f_rotator_track);
        byte cmds[2];
        cmds[0] = 0x04;  // enable
        cmds[1] = (byte)plogw->f_rotator_track;
        send_rotator_command_civ(cmds, 2);
      } else {
        plogw->ostream->println("rotator tracking tr0/1 ?");
      }
    }
    if (strncmp(s + 3, "AZ", 2) == 0) {
      // set target azimuth
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("send rotator target = ");
        plogw->ostream->println(tmp);
        if (tmp >= 0 && tmp <= 360) {
          plogw->rotator_target_az = tmp;
          send_rotator_az_set_civ(plogw->rotator_target_az);
        }
      } else {
        plogw->ostream->println("rotator en ?");
      }
    }
    if (strncmp(s + 3, "STEP", 4) == 0) {
      // sweep rotator slowly until reaching to the target azimuth
      tmp1 = sscanf(s + 7, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("sweep step = ");
		if (tmp>0) {
			plogw->rotator_sweep_step_default =tmp;
		} else {
			plogw->rotator_sweep_step_default = 2;
		}
		plogw->ostream->println(plogw->rotator_sweep_step_default);
	  }
	}
    if (strncmp(s + 3, "SWEEP", 5) == 0) {
      // sweep rotator slowly until reaching to the target azimuth
      tmp1 = sscanf(s + 8, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("sweep target = ");
        plogw->ostream->println(tmp);
        if (tmp >= 0 && tmp <= 360) {
          int az;
          az = plogw->rotator_az;
          if (az > 180) az -= 360;
          if (tmp > 180) {
            tmp -= 360.0;
          }
          if (tmp > az) {
            plogw->rotator_sweep_step = plogw->rotator_sweep_step_default;
          } else {
            plogw->rotator_sweep_step = -plogw->rotator_sweep_step_default;
          }
          if (tmp < 0) tmp += 360;
          plogw->rotator_target_az_sweep = tmp;
          plogw->ostream->print("start rotator sweep at ");
          plogw->ostream->print(plogw->rotator_sweep_step);
          plogw->ostream->print(" deg step to ");
          plogw->ostream->println(plogw->rotator_target_az_sweep);

          plogw->rotator_sweep_timeout = millis()+100; // start sweeping right now
          plogw->f_rotator_sweep = 1; // start sweeping
        }
      } else {
        plogw->f_rotator_sweep = 0;
        plogw->ostream->println("rotator sweep cancelled ?");
      }
    }

    // show rotator information
    sprintf(dp->lcdbuf, "Rotator\nAZ=%d deg\nTarget=%d\nEN=%d TR=%d\n", plogw->rotator_az, plogw->rotator_target_az, plogw->f_rotator_enable, plogw->f_rotator_track);
    plogw->ostream->print(dp->lcdbuf);
    upd_display_info_flash(dp->lcdbuf);
    clear_buf(radio->callsign);
    return 1;
  }
  return 0;
}

// RELAY1, 2 control
void RELAY_control(int relay, int on)
{
  
  switch (relay) {
    case 0: // RELAY 1
      //      mcp.digitalWrite(RELAY1, on);
      plogw->relay[0] = on;
      break;
    case 1: // RELAY 2
      //      mcp.digitalWrite(RELAY2, on);
      plogw->relay[1] = on;
      break;
  }
}

void print_radio_info(int idx_radio)
{
  if (!plogw->f_console_emu) {
    char buf[30];
    sprintf(buf, "Rig%d:%s %c", radio_list[idx_radio].rig_idx,
            radio_list[idx_radio].rig_spec->name,
            radio_list[idx_radio].enabled ? '*' : ' ');
    plogw->ostream->println(buf);
  }

}

void switch_radios(int idx_radio, int radio_cmd)
// radio_cmd -1 switch to circulate
//           0- set specified number
{
  struct radio *radio;
  int tmp_rig_spec_idx;
  radio = &radio_list[idx_radio];
  tmp_rig_spec_idx=radio->rig_spec_idx;    
  int count=0;
  while (1) {
    if (radio_cmd == -1) {
      // + radio
      tmp_rig_spec_idx++;
      if (tmp_rig_spec_idx >= N_RIG) {
	tmp_rig_spec_idx = 0;
      }
    } else if (radio_cmd == -2) {
      // - radio
      tmp_rig_spec_idx--;
      if (tmp_rig_spec_idx == -1) {
	tmp_rig_spec_idx = N_RIG - 1;
      }
    } else {
      if (radio_cmd >= 0 && radio_cmd < N_RIG) {
	tmp_rig_spec_idx = radio_cmd;
      }
    }
    // check conflict
    int ret;
    ret=check_rig_conflict(radio->rig_idx,&rig_spec[tmp_rig_spec_idx]);
    if (ret== -1) {
      radio->rig_spec_idx=tmp_rig_spec_idx;
      select_rig(radio);
      print_radio_info(idx_radio);
      upd_disp_rig_info();
      upd_display();
      return;
    } else {
      sprintf(dp->lcdbuf,"config %s\nconflict %s",rig_spec[tmp_rig_spec_idx].name,
	      radio_list[ret].rig_spec->name);
      upd_display_info_flash(dp->lcdbuf);
      if (radio_cmd >= 0 && radio_cmd < N_RIG) {
	return ; // op not completed
      }
    }
    count++;
    if (count>N_RIG) { //no more rig available
      sprintf(dp->lcdbuf,"no more rig \nwe can switch.");
      upd_display_info_flash(dp->lcdbuf);
      return;
    }
  }
}

void alternate_antenna_relay()
{
  plogw->relay[0] = 1 - plogw->relay[0];
  RELAY_control(0, plogw->relay[0]);
}


// serial 割り当ての考え方
// mux transport ではSerial2 はMUXに固定的に割り当てる。
// したがってmain rig がmux 関連 であった場合 sub rig にはSerial1 を割り当てる
//                               でない場合かつSub rig もmux 関連でない場合には
// Serial4 (software serial) をsub rig に割り当て。

// さらに推し進めConsole にSerial3などを割当、Serial をcatなどに割り当てられるようにする。ことでパフォーマンスを改善。
int is_local_serial_port(int civport_num)
{
  switch (civport_num) {
  case 2: // CI-V
  case 3: // CAT main
    return 1;
  default:
    return 0;
  }
  return 0;
}

int civport_num(struct radio *radio)
{
   return radio->rig_spec->civport_num;
}


// serial instance の割当の考え方
// 1. port 1 はf_mux_transport 0 のときだけシリアル初期化を行う。いずれにしてもinfo update は行う。
// 2. port 0 はradioに割り当てられていないポートを使用するが、最初はSerial を割り当てる＝他のport割当の状況に合わせパッシブに必要があれば変更
// 3. port 2 は複数のradio で共有可能 可能な限り0-2 に割り当てる
// 4. port 3 は複数のradio で共有できない
// 5. port 4 はmuxを通じる（複数のradio で共有できない）
// 6. port -3 は接続されていないので割当可能
// 7. port -2 はmanualなので、serial instance と関係がなく、port に入れることはない(radioが同じinstance があれば解除-3)。
// 8. port -1 はusbなので、同様にradio が同じものがあれば、-3で解除
// 9. radio が同じinstance が0-2 であればそのまま割り当てる。
// 10. 最初にradio が同じinstance を探して操作、なければ新しいinstance を探す。

struct serial_spec {
  int8_t port[4]; // 各instance が繋がっているcivport_num
  // not allocated port -3 ?
  // mux port 1 
} serial_spec = {{ 0,-3,-4,-3}};

// set radio's civport_num (hardware pins) allocated to the phisical seirial_num (0 Serial 1 Serial1 2 Serial2 3 Serial3(software) 4 Serial4(software) and set serial parameters

void print_serial_instance(Stream *out)
{
  if (!out) out = console;
  out->print("Serial Instance-port map\n");
  for (int j=0;j<4;j++) {
    out->print((int)(serial_spec.port[j]));
    out->print(" ");
  }
  out->println("");
  
}
void config_serial_instance(Stream **civport,int civport_num,int serial_num,int baud,int reverse)
{
  // link  serial port with serial instances

  console->print("config_serial_instance():civport_num=");
  console->print(civport_num);
  console->print(" serial_num=");  
  console->print(serial_num);
  console->print(" baud=");  
  console->print(baud);
  console->print(" reverse=");  
  console->println(reverse);
  
  
  int txPin,rxPin;
  txPin=16;
  rxPin=17; // set default value

  //  civport_num=radio->rig_spec->civport_num;
  switch(civport_num) {
  case 0: // port0 console
    rxPin=3;
    txPin=1;
    break;
  case 1: // port1 BT (shared with mux port)
    if (!f_mux_transport) {
      rxPin=SERIAL1_RX;
      txPin=SERIAL1_TX;
    } else {
      return;
    }
    break;
  case 2: // port2 CIV
    console->println("CI-V port");    
    rxPin=SERIAL2_RX;
    txPin=SERIAL2_TX;
    break;
  case 3: // port3 TTL-SER
#if JK1DVPLOG_HWVER >=3
    console->println("HWVER 3 CAT RX/TX swap");
    rxPin=15;
    txPin=27;
#else
    console->println("HWVER <= CAT RX/TX not swap");    
    rxPin=27;
    txPin=15;
#endif
    break;

  default:
    console->println("ERROR! config_serial_instance()");
    return;
  }

  
  // init serial port 
  switch(serial_num) {
  case 0: // Serial
    delay(10);
    Serial.end();
    if (reverse) {
      //      uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask)
      uart_set_line_inverse(0, UART_SIGNAL_RXD_INV|UART_SIGNAL_TXD_INV);
      Serial.setRxBufferSize(512);
      Serial.begin(baud,SERIAL_8N1, rxPin, txPin, true);
      uart_set_rx_full_threshold(UART_NUM_0, 1);
    } else {
      uart_set_line_inverse(0, UART_SIGNAL_INV_DISABLE);
      Serial.setRxBufferSize(512);
      Serial.begin(baud,SERIAL_8N1, rxPin, txPin, false);
      uart_set_rx_full_threshold(UART_NUM_0, 1);      
    }
    *civport=&Serial;
    serial_spec.port[serial_num]=civport_num; // update connection information
    break;
  case 1: // Serial1
    delay(10);    
    Serial1.end();
    Serial1.setRxBufferSize(512);
    Serial1.begin(baud,SERIAL_8N1,
		  rxPin,
		  txPin,
		  reverse);
    uart_set_rx_full_threshold(UART_NUM_1, 1);
    *civport=&Serial1;
    serial_spec.port[serial_num]=civport_num; // update connection information
    break;
  case 2: // Serial2
    delay(10);        
    Serial2.end();
    Serial2.setRxBufferSize(512);
    Serial2.begin(baud,SERIAL_8N1,
		  rxPin,
		  txPin,
		  reverse);
    uart_set_rx_full_threshold(UART_NUM_2, 1);
    *civport=&Serial2;
    serial_spec.port[serial_num]=civport_num; // update connection information
    break;
  case 3: // Serial3 (software)
    delay(10);
    Serial3.end();        
    Serial3.begin(baud,SWSERIAL_8N1,rxPin,txPin,reverse,64,64*11);
    Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
    *civport=&Serial3;
    serial_spec.port[serial_num]=civport_num; // update connection information
    break;
  }
  if (!plogw->f_console_emu) {    
    console->print("init Serial");
    console->print(serial_num);
    console->print(" for Port ");
    console->print(civport_num);    
    console->print(" rxPin=");
    console->print(rxPin);
    console->print(" txPin=");
    console->print(txPin);
    console->print(" baud=");
    console->print(baud);    
    console->print(" reversed=");
    console->println(reverse);
  }
}


int find_connected_serial(int civport_num) {
  int found;
  for (int i = 0 ; i<4;i++) {
    if (serial_spec.port[i]==civport_num) {
      return i;
    }
  }
  return -1;
}


int count_usage_civport(int civport_num)
{
  struct radio *radio;
  //count number of the radio which uses the civport_num port
  int count;
  count=0;
  for (int i=0;i<3;i++) {
    radio=&radio_list[i];
    if (radio->rig_spec!=NULL) {
      if (radio->rig_spec->civport_num==civport_num) {
	count++;
      }
    }
  }
  return count;
    
}

void release_port_serial(struct radio *radio)
{ int serial_num;
  
  serial_num=find_connected_serial(radio->rig_spec->civport_num);
  if (serial_num != -1) {
    serial_spec.port[serial_num]=-3;
    radio->rig_spec->civport=NULL;
    console->print("released serial port ");console->print(serial_num);
    console->print(" for civport_num ");console->print(radio->rig_spec->civport_num);
  }
  print_serial_instance();  
}


void release_civport_serial(int civport_num)
// release serial port assigned to the civport_num and assign NULL to the civport for all radios with the civport_num
{
  int serial_num;
  serial_num=find_connected_serial(civport_num);
  if (serial_num != -1) {
    serial_spec.port[serial_num]=-3;
    for (int i=0;i<3;i++) {
      if (radio_list[i].rig_spec!=NULL) {
	if (radio_list[i].rig_spec->civport_num == civport_num) {
	  radio_list[i].rig_spec->civport=NULL;
	}
      }
    }
  }
  
}

int find_free_serial() {
  for (int i = 0 ; i<4;i++) {
    if (serial_spec.port[i]==-3) {
      return i;
    }
  }
  return -1;
}



int console_to_softwareserial() {
  // make console moved to software serial and release
  for (int i=3;i<=3;i++) {
  if (serial_spec.port[i]==-3) {
    // find console port in hardware serial
    for (int j=0;j<3;j++) {
      if (serial_spec.port[j]==0) {
	console->print("find hardware to release j=");
	console->println(j);
	console->print(" softwareport i=");
	console->println(i);
	serial_spec.port[j]=-3; // release current console_port
	config_serial_instance(&console, 0, i, 115200 ,0); // init console serial
	plogw->ostream=console;	
	return i; // return with serial_num of the console port 	
      }
    }
    //
    return -2; // strange
  }
  }
  return -1; // not successful
}

int console_to_hardwareserial() {
  // make console move back to hardware serial
  console->println("console_to_hardwareserial()");
  for (int i=3;i<=3;i++) {
    if (serial_spec.port[i]==0) {
      // identify serial port from
      for (int j=0;j<3;j++) {
	if (serial_spec.port[j]==-3) {
	  // find a hardware port not in use
	  console->print("find j=");
	  console->println(j);
	  console->print(" released i=");
	  console->println(i);
	  serial_spec.port[i]=-3; // release
	  config_serial_instance(&console, 0, j, 115200 ,0); // init console serial
	  plogw->ostream=console;
	  print_serial_instance();	  
	  return j; // console is now hardware 	  
	}
      }
      console->println("no free hardware serial found.");      
      // not found free hardware
      return -1;
    }
  }
  for (int j=0;j<2;j++) {
    if (serial_spec.port[j]==0) {
      console->print(" console is j=");
      console->println(j);
      console->println("console is already hardwareserial");
      return j; // console is already hardware
    }
  }
  return -2; // strange !
}

int find_free_hardwareserial() {
  int j;
  console->println("find_free_hardwareserial");
  if ((j=find_free_serial())>=3) {
    console_to_softwareserial();
    return find_free_serial();
  } else {
    return j;
  }
}

  
void config_rig_serialport(struct radio *radio) 
{
  int civport_num;
  int rig_idx;
  char cmdbuf[30];
  civport_num=radio->rig_spec->civport_num;
  rig_idx=radio->rig_idx;
  int serial_num;
  int tmp;
  int i;

  switch (civport_num) {
  case -2: // MANUAL
  case -1: // USB
  case 0: // console (not set) as a radio target
    radio->rig_spec->civport=NULL;
    return;
    break;
  case 1: //port1 BT or f_mux_transport
    if (!f_mux_transport) {
      serial_num=2;
      config_serial_instance(&radio->rig_spec->civport,radio->rig_spec->civport_num, serial_num, SERIAL2_BAUD ,radio->rig_spec->civport_reversed); // init serial
      
    } else {
      radio->rig_spec->civport=NULL;
    }
    break;
  case 2: // CIV
    // check if presently allocated
    serial_num=find_connected_serial(civport_num);
    if (serial_num!= -1) {
      console->println("use existing allocated serial for CIV");
      config_serial_instance(&radio->rig_spec->civport,radio->rig_spec->civport_num,serial_num, radio->rig_spec->civport_baud,radio->rig_spec->civport_reversed);
      // need to receive only in one radio for the CIV this will be checked in receive_civ
    } else {
      serial_num= find_free_hardwareserial();
      if (serial_num != -1) {
	console->println("configure new serial for CIV");
	config_serial_instance(&radio->rig_spec->civport,radio->rig_spec->civport_num,serial_num, radio->rig_spec->civport_baud,radio->rig_spec->civport_reversed);      
      } else {
	console->print("No free serial");
	radio->rig_spec->civport=NULL;
      }
    }
    print_serial_instance();          
    break;
  case 3: // TTL-SER
    // check if presently allocated
    serial_num=find_connected_serial(civport_num);
    if (serial_num!= -1) {
      console->println("use existing allocated serial for CAT");
      // release attached 
      release_civport_serial(civport_num);
      // and configure this 
      config_serial_instance(&radio->rig_spec->civport,radio->rig_spec->civport_num,serial_num, radio->rig_spec->civport_baud,radio->rig_spec->civport_reversed);         } else {
      serial_num= find_free_hardwareserial();
      if (serial_num != -1) {
	console->println("configure new serial for CAT");	
	config_serial_instance(&radio->rig_spec->civport,radio->rig_spec->civport_num,serial_num, radio->rig_spec->civport_baud,radio->rig_spec->civport_reversed);
      } else {
	console->print("No free serial");
	radio->rig_spec->civport=NULL;
      }
      print_serial_instance();
    }
    break;
  case 4: // extension board CAT2 through port 1 (not read directory from port1 but data pushed from demux service)
    radio->rig_spec->civport=NULL;  // local civ port is null but need to set remote cat2 port parameter so send command over pkt
    if (f_mux_transport) {
      sprintf(cmdbuf,"cat2_param%d %d", radio->rig_spec->civport_baud,radio->rig_spec->civport_reversed);
      mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char *)cmdbuf,strlen(cmdbuf));
    }
    return;
    break;
  }

  // check to bring console hardware serial if possible
  console_to_hardwareserial();
  print_serial_instance();

}




//
void init_mux_serial() {
  //  Serial2.begin(115200, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port
  //  Serial2.begin(750000, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port   fastest?
  
  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port   fastest?  
}

// deinitialize mux serial port for esp32 flash sub cpu and stop tiker tasks 
void deinit_mux_serial()
{
  //  civ_reader.detach();
  Serial2.end();

}



int check_rig_conflict(int rig_idx,struct rig *rig_spec)
{
  // check rig_idx and rig_spec with current radio 0-2 configurations if conflict of civ ports occurs (cw port conflict is allowed)
  if (rig_spec->civport_num ==-2)  return -1; // manual rig should not conflict
  for (int i =0;i<N_RADIO;i++) {
    if (i==rig_idx) continue;
    if (radio_list[i].rig_spec==NULL) continue;
    if (rig_spec->civport_num != 2) {
      // CI-V port can be shared so should not conflict
      if ((rig_spec->civport_num == radio_list[i].rig_spec->civport_num)) {
	// conflict phys port
	return i;
      }
    }
  }
  return -1; // no conflict
}

// Release the serial resource belonging to the rig currently linked to radio.
// Keep this separate from select_rig() so a rig_spec can be edited without
// overwriting the old port information before it is released.
static void release_rig_serial_resource(struct radio *radio) {
  if (radio == NULL || radio->rig_spec == NULL) return;

  if (radio->rig_spec->civport != NULL) {
    if (radio->rig_spec->civport_num == 3) {
      release_port_serial(radio);
    }
    else if (radio->rig_spec->civport_num == 2) {
      // CI-V port may be shared. Release it only when this is the last user.
      if (count_usage_civport(radio->rig_spec->civport_num) == 1) {
        release_port_serial(radio);
      }
    }
  }
}

// set radio->rig_spec from radio->rig_spec_idx
void select_rig(struct radio *radio) {
  console->printf(
    "[USBBIND] SELECT_BEFORE radio=%d target_spec=%d old_name=%s "
    "old_civport=%d old_cat_type=%d qmx_ready=%d txq=%u rxq=%u r=%d w=%d\n",
    radio ? radio->rig_idx : -1,
    radio ? radio->rig_spec_idx : -1,
    (radio && radio->rig_spec && radio->rig_spec->name) ? radio->rig_spec->name : "(null)",
    (radio && radio->rig_spec) ? radio->rig_spec->civport_num : -99,
    (radio && radio->rig_spec) ? radio->rig_spec->cat_type : -99,
    usb_cat_ready_for_rig_type(CAT_TYPE_QMX) ? 1 : 0,
    xQueueCATUSBTx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBTx) : 0U,
    xQueueCATUSBRx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBRx) : 0U,
    radio ? radio->r_ptr : -1, radio ? radio->w_ptr : -1);

  if (!plogw->f_console_emu) {
    plogw->ostream->print("select_rig() spec:");
    plogw->ostream->println(radio->rig_spec_idx);
  }

  // before changing rig, release Serial resource
  release_rig_serial_resource(radio);
  console_to_hardwareserial();
  
  // link the rig_spec to the radio structure
  radio->rig_spec = &rig_spec[radio->rig_spec_idx];
  strcpy(radio->rig_name + 2, radio->rig_spec->name);
  set_rig_spec_str_from_spec(radio); // reverse set rig_spec_string from rig_spec

  // set serial port characteristics
  config_rig_serialport(radio);

  console->printf(
    "[USBBIND] SELECT_AFTER radio=%d spec=%d name=%s "
    "civport=%d cat_type=%d qmx_ready=%d txq=%u rxq=%u r=%d w=%d\n",
    radio->rig_idx, radio->rig_spec_idx,
    (radio->rig_spec && radio->rig_spec->name) ? radio->rig_spec->name : "(null)",
    radio->rig_spec ? radio->rig_spec->civport_num : -99,
    radio->rig_spec ? radio->rig_spec->cat_type : -99,
    usb_cat_ready_for_rig_type(CAT_TYPE_QMX) ? 1 : 0,
    xQueueCATUSBTx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBTx) : 0U,
    xQueueCATUSBRx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBRx) : 0U,
    radio->r_ptr, radio->w_ptr);

  /*
   * Do not carry CAT requests from the previously selected USB rig into a
   * newly selected QMX session.  This also gives the diagnostics a known
   * empty-queue starting point.
   */
  if (radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI &&
      radio->rig_idx >= 0 && radio->rig_idx < N_RADIO) {
    ats_mini_mode_current[radio->rig_idx] = -1;
    ats_mini_mode_target[radio->rig_idx] = -1;
    ats_mini_mode_pending[radio->rig_idx] = false;
    ats_mini_mode_last_cmd_ms[radio->rig_idx] = 0;
    ats_mini_virtual_cw[radio->rig_idx] = false;
    ats_mini_bw_last_cmd_ms[radio->rig_idx] = 0;
  }

  console->printf(
    "SELECT_RIG_USB_DIAG rig_idx=%d spec=%d name=%s civport=%d cat_type=%d "
    "qmx_const=%d ats_const=%d qmx_ready=%d txq=%u rxq=%u\n",
    radio->rig_idx, radio->rig_spec_idx,
    radio->rig_spec->name ? radio->rig_spec->name : "(null)",
    radio->rig_spec->civport_num,
    radio->rig_spec->cat_type,
    CAT_TYPE_QMX, CAT_TYPE_ATS_MINI,
    usb_cat_ready_for_rig_type(CAT_TYPE_QMX) ? 1 : 0,
    xQueueCATUSBTx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBTx) : 0U,
    xQueueCATUSBRx ? (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBRx) : 0U);

  if (radio->rig_spec->civport_num == -1 &&
      (radio->rig_spec->cat_type == CAT_TYPE_QMX ||
       radio->rig_spec->cat_type == CAT_TYPE_ATS_MINI)) {
    /*
     * USB CAT uses one shared RX/TX transport.  When the physical USB rig is
     * changed before select_rig(), bytes from the new device can already have
     * been queued/parsing under the old protocol (e.g. QMX ASCII while the
     * radio is still IC-705/CI-V).  Do not carry that partial RX session into
     * the newly selected protocol.
     */
    struct catmsg_t stale = {};
    unsigned int tx_cleared = 0;
    unsigned int rx_cleared = 0;

    if (xQueueCATUSBTx != NULL) {
      while (xQueueReceive(xQueueCATUSBTx, &stale, 0) == pdTRUE) {
        ++tx_cleared;
      }
    }
    if (xQueueCATUSBRx != NULL) {
      while (xQueueReceive(xQueueCATUSBRx, &stale, 0) == pdTRUE) {
        ++rx_cleared;
      }
    }

    // Discard bytes already copied into this radio's old-protocol RX state.
    radio->r_ptr = 0;
    radio->w_ptr = 0;
    clear_civ(radio);

    console->printf(
      "USB CAT protocol switch reset rig=%d type=%d tx=%u rx=%u\n",
      radio->rig_idx, radio->rig_spec->cat_type,
      tx_cleared, rx_cleared);

    /*
     * QMX select_rig forced IF restart temporarily disabled.
     * Normal periodic CAT polling should establish the first fresh
     * transaction after the protocol/parser state has been reset.
     *
     * Keep this block for easy A/B testing if the old recovery workaround
     * proves necessary again.
     */
#if 0
    if (radio->rig_spec->cat_type == CAT_TYPE_QMX &&
        usb_cat_ready_for_rig_type(CAT_TYPE_QMX) &&
        xQueueCATUSBTx != NULL) {
      struct catmsg_t startup = {};
      memcpy(startup.buf, "IF;", 3);
      startup.size = 3;
      const BaseType_t qret = xQueueSend(xQueueCATUSBTx, &startup, 0);
      console->printf(
        "QMX CAT select_rig restart qret=%d waiting=%u cmd=IF;\n",
        (int)qret,
        (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBTx));
    }
#endif
  }
  
  radio->f_civ_response_expected = 0;
  radio->civ_response_timer = 0;
  //  if (radio->rig_spec->band_mask != 0 ) {
  console->print("set radio band_mask from rig_spec");
  radio->band_mask = radio->rig_spec->band_mask;

  // initialize ptt hardware status
  set_ptt_rig(radio,0);
  //  }
  if (!plogw->f_console_emu) plogw->ostream->println("select_rig()end");
}

// initialization of KENWOOD cat over software serial 22/05/09 (for QCX mini)
//void init_cat_kenwood() {
//}

void init_cat_usb() {
  // init queue to trx to USB serial
  xQueueCATUSBRx = xQueueCreate(QUEUE_CATUSB_RX_LEN, sizeof(struct catmsg_t));
  xQueueCATUSBTx = xQueueCreate(QUEUE_CATUSB_TX_LEN, sizeof(struct catmsg_t));
  console->println("catusb queue created.");  
}

void init_cat_serialport()
{

  
  // remap to RX 16 TX 4  // BT serial for IC-705
  //  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);

  // remap to RX 32 TX 33 // CI-V port for ICOM rigs
  //  Serial2.begin(19200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
  // to support IC-820 set baudrate of CAT port to 9600BPS
  //  Serial2.begin(9600, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
  //  Serial2.begin(115200, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port
  //  Serial2.begin(750000, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port
  //  Serial2.begin(3000000, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);  // primarily for mux and BT serial port    

  //    Serial3.begin(38400, SWSERIAL_8N1, CATRX, CATTX, false, 95, 0); // original worked with hand wired jk1dvplog
  //Serial3.begin(38400, SWSERIAL_8N1, CATTX, CATRX, false, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, original polarity // worked qcx-mini
  //  Serial3.begin(38400, SWSERIAL_8N1, CATTX, CATRX, true, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, reversed polarity
  //  Serial3.begin(38400, SWSERIAL_8N1, CATRX, CATTX, true, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, reversed polarity    
  //  Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
  //  Serial3.begin(38400, SWSERIAL_8N1, CATRX, CATTX, true, 95, 0); // flip polarity

  //  Serial4.begin(38400, SWSERIAL_8N1, CATTX, CATRX, true, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, reversed polarity    

}


void init_rigspec() {
  // reset rig specification to the default
  
  strcpy(rig_spec[0].name ,"IC-705");
  rig_spec[0].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[0].civaddr = 0xa4;
  //  rig_spec[0].civport = &Serial1;
  rig_spec[0].civport_num=1; // BT
  rig_spec[0].civport_reversed=0; // normal
  rig_spec[0].civport_baud = 115200;    
  rig_spec[0].cwport = 2; // KEY2 reserve KEY1 for manual radio (tone_keying)
  rig_spec[0].rig_type = 0;
  rig_spec[0].pttmethod = 2;
  rig_spec[0].band_mask = ~(BAND_MASK_2400|BAND_MASK_1200|BAND_MASK_HF|BAND_MASK_50|BAND_MASK_144|BAND_MASK_430|BAND_MASK_WARC);
  //                 0b0000001000000000

  // 2400 MHz transverter
  rig_spec[0].transverter_freq[0][0] = 2424000000ULL/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][1] = 2430000000ULL/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][2] = 434000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][3] = 440000000/FREQ_UNIT;
  // 1200MHz transverter
  rig_spec[0].transverter_freq[1][0] = 1294000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][1] = 1296000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][2] = 144000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][3] = 146000000/FREQ_UNIT;

  // 5600 MHz transverter ( use 1200 transverter and 5600 transverter in cascade is not possible
  // 1280 MHz -> 5760 MHz but difficult to produce 1280 IF freq with 1200 MHz transverter (dip sw)

  strcpy(rig_spec[1].name , "IC-9700");
  rig_spec[1].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[1].civaddr = 0xa2;
  //  rig_spec[1].civport = &Serial2;
  rig_spec[1].civport_num=2; // CIV
  rig_spec[1].civport_reversed=0; // normal
  rig_spec[1].civport_baud = 19200;      
  rig_spec[1].cwport = 1; 
  rig_spec[1].rig_type = 1;
  rig_spec[1].pttmethod = 2;
  rig_spec[1].transverter_freq[0][0] = 0;
  rig_spec[1].band_mask = ~0b1110000000; //0x047f;  // ~380


  strcpy(rig_spec[2].name , "FT-991A");
  rig_spec[2].cat_type = CAT_TYPE_YAESU_NEW;  // cat
  rig_spec[2].civaddr = 0;
  rig_spec[2].civport_num=3; // default serial; set P:-1 for USB CAT
  rig_spec[2].civport_reversed=0; // normal
  rig_spec[2].civport_baud = 38400;      
  rig_spec[2].cwport = 2;  // test
  rig_spec[2].rig_type = 2;
  rig_spec[2].pttmethod = 2;
  rig_spec[2].transverter_freq[0][0] = 0;
  rig_spec[2].band_mask = ~(0b111111111|BAND_MASK_WARC);//0x600;

  strcpy(rig_spec[3].name , "FT-991A-SER");
  rig_spec[3].cat_type = 1;  // cat
  rig_spec[3].civaddr = 0;
  rig_spec[3].civport_num=3; // SER-TTL
  rig_spec[3].civport_reversed=1; // normal
  rig_spec[3].civport_baud = 38400;    
  rig_spec[3].cwport = 1;
  rig_spec[3].rig_type = 2;
  rig_spec[3].pttmethod = 2;
  rig_spec[3].transverter_freq[0][0] = 0;
  rig_spec[2].band_mask = ~(0b111111111|BAND_MASK_WARC);//0x600;

  strcpy(rig_spec[4].name , "QCX-MINI");
  rig_spec[4].cat_type = CAT_TYPE_KENWOOD;  // kenwood cat port Serial3
  // y the transceiver for information
  //entered into the log, typically operating frequency, mode etc.
  // connection :
  /// tip QCX rxd ring QCX txd sleeve gnd
  // 38400 bps 8N1

  // FA7030000; set  VFOA
  // FR 0,1,2 ; VFO mode A,B, Split

  rig_spec[4].civaddr = 0;
  rig_spec[4].civport_num=3; // TTL-SER
  rig_spec[4].civport_reversed=0; // normal
  rig_spec[4].civport_baud = 38400;  
  rig_spec[4].cwport = 1;
  rig_spec[4].rig_type = 3;
  rig_spec[4].pttmethod = 2;
  rig_spec[4].transverter_freq[0][0] = 0;

  // manual rig
  strcpy(  rig_spec[5].name , "MANUAL");
  rig_spec[5].cat_type = CAT_TYPE_NOCAT;  // no cat
  rig_spec[5].civaddr = 0;
  rig_spec[5].civport_num=-2; // MANUAL
  rig_spec[5].civport_reversed=0; // normal
  rig_spec[5].civport_baud =0;      
  rig_spec[5].cwport = 1;
  rig_spec[5].rig_type = 4;
  rig_spec[5].pttmethod = 2;
  rig_spec[5].transverter_freq[0][0] = 0;
  rig_spec[5].band_mask = 0x0000;  // 0x2000 = all band nonzero but zero for all band

  // IC-820
  strcpy(rig_spec[6].name , "IC-820");
  rig_spec[6].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[6].civaddr = 0x42;
  //  rig_spec[6].civport = &Serial2;
  rig_spec[6].civport_num=2; // CIV
  rig_spec[6].civport_reversed=0; // normal
  rig_spec[6].civport_baud = 9600;      
  rig_spec[6].cwport = 2;
  rig_spec[6].rig_type = 1;
  rig_spec[6].pttmethod = 2;
  rig_spec[6].transverter_freq[0][0] = 0;
  rig_spec[6].band_mask = ~(BAND_MASK_144|BAND_MASK_430);  // 0x2000 = all band nonzero but zero for all band
  
  strcpy(rig_spec[7].name , "IC-705-SER");
  rig_spec[7].cat_type = CAT_TYPE_CIV;  // civ from port 3
  rig_spec[7].civaddr = 0xa4;
  rig_spec[7].civport_num=3; // TTL-SER
  rig_spec[7].civport_reversed=0; // normal
  rig_spec[7].civport_baud = 38400;      
  rig_spec[7].cwport = 1;
  rig_spec[7].rig_type = 0;
  rig_spec[7].pttmethod = 2;
  rig_spec[7].transverter_freq[0][0] = 0;

  strcpy(rig_spec[8].name , "FTDX10");
  rig_spec[8].cat_type = CAT_TYPE_YAESU_NEW;  // cat
  rig_spec[8].civaddr = 0;
  rig_spec[8].civport_num=3; // TTL-SER
  rig_spec[8].civport_reversed=1; // normal
  rig_spec[8].civport_baud = 38400;      
  rig_spec[8].cwport = 1;  // test
  rig_spec[8].rig_type = 2;
  rig_spec[8].pttmethod = 2;
  rig_spec[8].transverter_freq[0][0] = 0;
  rig_spec[8].band_mask = ~(0b1111111|BAND_MASK_WARC) ;//0x780;

  strcpy(rig_spec[9].name , "QMX");
  rig_spec[9].cat_type = CAT_TYPE_QMX;  // QMX CAT over USB
  rig_spec[9].civaddr = 0;
  rig_spec[9].civport_num=-1; // USB
  rig_spec[9].civport_reversed=0; // normal
  rig_spec[9].civport_baud = 0;      
  rig_spec[9].cwport = 1;
  rig_spec[9].rig_type = RIG_TYPE_QMX;
  rig_spec[9].pttmethod = 2;
  rig_spec[9].transverter_freq[0][0] = 0;

  //
  strcpy(rig_spec[10].name , "IC-905-USB"); // 905 connected to USB
  rig_spec[10].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[10].civaddr = 0xac; // IC-905
  //  rig_spec[10].civport = NULL;
  rig_spec[10].civport_num=-1; // USB
  rig_spec[10].civport_reversed=0; // normal
  rig_spec[10].civport_baud = 115200;
  rig_spec[10].cwport = 0;
  rig_spec[10].rig_type = 0;
  rig_spec[10].pttmethod = 2;
  rig_spec[10].band_mask = ~(BAND_MASK_SHF|BAND_MASK_144|BAND_MASK_430);
  rig_spec[10].transverter_freq[0][0] = 0;

  strcpy(rig_spec[11].name , "DJ-G7"); // DJ-G7
  rig_spec[11].cat_type = CAT_TYPE_NOCAT;  // no cat
  rig_spec[11].civaddr = 0;
  rig_spec[11].civport_num=-2; // manual
  rig_spec[11].civport_reversed=0; // normal
  rig_spec[11].civport_baud = 0;      
  rig_spec[11].cwport = 1;
  rig_spec[11].rig_type = 4;
  rig_spec[11].pttmethod = 2;
  rig_spec[11].band_mask = 0x147f; //1 0100 0111 1111
  //                                 1 5214 1522 1731
  //                                 0 6423 4081 4 58
  //                                 1 1110 0111 1111
  rig_spec[11].transverter_freq[0][0] = 5759000000ULL/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][1] = 5761000000ULL/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][2] = 1279000000/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][3] = 1281000000/FREQ_UNIT;
  
  strcpy(rig_spec[12].name , "KENWOOD-SER");
  rig_spec[12].cat_type = CAT_TYPE_KENWOOD;  // kenwood cat port
  rig_spec[12].civaddr = 0;
  rig_spec[12].civport_num=3; // TTL-SER
  rig_spec[12].civport_reversed=1; // reversed
  rig_spec[12].civport_baud = 38400;  
  rig_spec[12].cwport = 1;
  rig_spec[12].rig_type = 3;
  rig_spec[12].pttmethod = 2;
  rig_spec[12].transverter_freq[0][0] = 0;

  /// IC-7300
  strcpy(rig_spec[13].name , "IC-7300");
  rig_spec[13].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[13].civaddr = 0x94;
  //  rig_spec[13].civport = &Serial1;
  rig_spec[13].civport_num=2; // CIV
  rig_spec[13].civport_reversed=0; // normal
  rig_spec[13].civport_baud = 19200;    
  rig_spec[13].cwport = 2; // KEY2 
  //  rig_spec[13].rig_type = RIG_TYPE_ICOM_IC705;
  rig_spec[13].rig_type = RIG_TYPE_ICOM_IC7300;   
  rig_spec[13].pttmethod = 2;
  rig_spec[13].transverter_freq[0][0] = 0;
  rig_spec[13].band_mask = ~(0b1111111|BAND_MASK_WARC) ;//0x780;


  // FTDX3000
  strcpy(rig_spec[14].name , "FTDX3000");
  rig_spec[14].cat_type = CAT_TYPE_YAESU_OLD;  // cat
  rig_spec[14].civaddr = 0;
  rig_spec[14].civport_num=3; // TTL-SER
  rig_spec[14].civport_reversed=1; // normal
  rig_spec[14].civport_baud = 38400;      
  rig_spec[14].cwport = 1;  // test
  rig_spec[14].rig_type = RIG_TYPE_YAESU ;
  rig_spec[14].pttmethod = 2;
  rig_spec[14].transverter_freq[0][0] = 0;
  rig_spec[14].band_mask = ~(0b1111111|BAND_MASK_WARC) ;//0x780;


  /// IC-706MKIIG
  strcpy(rig_spec[15].name , "IC-706MK2G");
  rig_spec[15].cat_type = CAT_TYPE_CIV;  // civ
  rig_spec[15].civaddr = 0x58;
  //  rig_spec[15].civport = &Serial1;
  rig_spec[15].civport_num=2; // CIV
  rig_spec[15].civport_reversed=0; // normal
  rig_spec[15].civport_baud = 19200;    // default???
  rig_spec[15].cwport = 2; // KEY2 
  rig_spec[15].rig_type = RIG_TYPE_ICOM_IC705; 
  rig_spec[15].pttmethod = 2;
  rig_spec[15].transverter_freq[0][0] = 0;
  rig_spec[15].band_mask = ~(0b111111111|BAND_MASK_WARC);//0x600;
  //  rig_spec[15].band_mask = ~0b1111111 ;//0x780;

  strcpy(rig_spec[16].name , "FTDX10-2"); // FTDX10 at CAT2
  rig_spec[16].cat_type = CAT_TYPE_YAESU_NEW;  // cat
  rig_spec[16].civaddr = 0;
  rig_spec[16].civport_num=4; 
  rig_spec[16].civport_reversed=1; // normal
  rig_spec[16].civport_baud = 38400;      
  rig_spec[16].cwport = 1;  // test
  rig_spec[16].rig_type = 2;
  rig_spec[16].pttmethod = 2;
  rig_spec[16].transverter_freq[0][0] = 0;
  rig_spec[16].band_mask = ~(0b1111111|BAND_MASK_WARC) ;//0x780;

  strcpy(rig_spec[17].name , "FT-991A-2"); // FT-991A at CAT2
  rig_spec[17].cat_type = CAT_TYPE_YAESU_NEW;  // cat
  rig_spec[17].civaddr = 0;
  rig_spec[17].civport_num=4; 
  rig_spec[17].civport_reversed=1; // normal
  rig_spec[17].civport_baud = 38400;      
  rig_spec[17].cwport = 1;  // test
  rig_spec[17].rig_type = 2;
  rig_spec[17].pttmethod = 2;
  rig_spec[17].transverter_freq[0][0] = 0;
  //  rig_spec[17].band_mask = ~0b1111111 ;//0x780;
  rig_spec[17].band_mask = ~(0b111111111|BAND_MASK_WARC);//0x600;  

  strcpy(rig_spec[18].name , "FT-817"); // FT-817
  rig_spec[18].cat_type = CAT_TYPE_YAESU_FT817;  // cat
  rig_spec[18].civaddr = 0;
  rig_spec[18].civport_num=3; 
  // FT-817/FT-818 DATA/ACC CAT is TTL level and works without UART
  // inversion on the DVPlogger TTL serial port.
  rig_spec[18].civport_reversed=0;
  rig_spec[18].civport_baud = 4800;      // default
  rig_spec[18].cwport = 1;  // 
  rig_spec[18].rig_type = RIG_TYPE_YAESU;
  rig_spec[18].pttmethod = 2;
  rig_spec[18].transverter_freq[0][0] = 0;
  rig_spec[18].band_mask = ~(0b111111111|BAND_MASK_WARC);

  strcpy(rig_spec[19].name , "KX3"); // Elecraft KX3
  rig_spec[19].cat_type = CAT_TYPE_ELECRAFT_KX;  // cat
  rig_spec[19].civaddr = 0;
  rig_spec[19].civport_num=3; 
  rig_spec[19].civport_reversed=0;  // assume TTL serial port
  rig_spec[19].civport_baud = 4800;      // default
  rig_spec[19].cwport = 1;  // 
  rig_spec[19].rig_type = RIG_TYPE_ELECRAFT_KX;
  rig_spec[19].pttmethod = 2;
  rig_spec[19].transverter_freq[0][0] = 0;
  rig_spec[19].band_mask = ~(0b001111111|BAND_MASK_WARC); // HF-6m+WARC

  strcpy(rig_spec[20].name , "ATS-MINI");
  rig_spec[20].cat_type = CAT_TYPE_ATS_MINI;
  rig_spec[20].civaddr = 0;
  rig_spec[20].civport_num = -1; // USB CDC ACM
  rig_spec[20].civport_reversed = 0;
  rig_spec[20].civport_baud = 115200;
  rig_spec[20].cwport = 0;
  rig_spec[20].rig_type = RIG_TYPE_ATS_MINI;
  rig_spec[20].pttmethod = 0;
  rig_spec[20].transverter_freq[0][0] = 0;
  rig_spec[20].band_mask = ~(BAND_MASK_HF | BAND_MASK_WARC);

  // Additional editable RIG slots. These are normal rig_spec[] entries;
  // they can be changed from the Web RIG Settings page and saved to rigs.txt.
  strcpy(rig_spec[21].name , "IC-705-USB");
  rig_spec[21].cat_type = CAT_TYPE_CIV;
  rig_spec[21].civaddr = 0xa4;
  rig_spec[21].civport_num = -1; // USB
  rig_spec[21].civport_reversed = 0;
  rig_spec[21].civport_baud = 115200;
  rig_spec[21].cwport = 3; // USB DTR: CW/RTTY keying for IC-705 USB
  rig_spec[21].rig_type = RIG_TYPE_ICOM_IC705;
  rig_spec[21].pttmethod = 2;
  rig_spec[21].transverter_freq[0][0] = 0;
  rig_spec[21].band_mask =
    ~(BAND_MASK_2400 | BAND_MASK_1200 | BAND_MASK_HF | BAND_MASK_50 |
      BAND_MASK_144 | BAND_MASK_430 | BAND_MASK_WARC);

  // FTX-1.  Transport is selected by P: just like the other rigs:
  // P:-1 = USB CAT (CP2105 Enhanced port), P:0..4 = normal serial port.
  strcpy(rig_spec[22].name , "FTX-1");
  rig_spec[22].cat_type = CAT_TYPE_YAESU_NEW;
  rig_spec[22].civaddr = 0;
  rig_spec[22].civport_num = 3; // default serial; set P:-1 for USB CAT
  rig_spec[22].civport_reversed = 0;
  rig_spec[22].civport_baud = 38400;
  rig_spec[22].cwport = 1;
  rig_spec[22].rig_type = RIG_TYPE_YAESU;
  rig_spec[22].pttmethod = 2;
  rig_spec[22].transverter_freq[0][0] = 0;
  rig_spec[22].band_mask = ~(0b1111111 | BAND_MASK_WARC);

  strcpy(rig_spec[23].name , "QMX-SER");
  rig_spec[23].cat_type = CAT_TYPE_KENWOOD;
  rig_spec[23].civaddr = 0;
  rig_spec[23].civport_num = 3; // TTL-SER
  rig_spec[23].civport_reversed = 0;
  rig_spec[23].civport_baud = 38400;
  rig_spec[23].cwport = 1;
  rig_spec[23].rig_type = 3;
  rig_spec[23].pttmethod = 2;
  rig_spec[23].transverter_freq[0][0] = 0;
  rig_spec[23].band_mask = 0x0000;

  strcpy(rig_spec[24].name , "IC-7300-USB");
  rig_spec[24].cat_type = CAT_TYPE_CIV;
  rig_spec[24].civaddr = 0x94;
  rig_spec[24].civport_num = -1; // USB
  rig_spec[24].civport_reversed = 0;
  rig_spec[24].civport_baud = 115200;
  rig_spec[24].cwport = 2;
  rig_spec[24].rig_type = RIG_TYPE_ICOM_IC7300;
  rig_spec[24].pttmethod = 2;
  rig_spec[24].transverter_freq[0][0] = 0;
  rig_spec[24].band_mask = ~(0b1111111 | BAND_MASK_WARC);

  strcpy(rig_spec[25].name , "TS-590G-USB");
  rig_spec[25].cat_type = CAT_TYPE_KENWOOD;
  rig_spec[25].civaddr = 0;
  rig_spec[25].civport_num = -1; // USB
  rig_spec[25].civport_reversed = 0;
  rig_spec[25].civport_baud = 115200;
  rig_spec[25].cwport = 1;
  rig_spec[25].rig_type = 3;
  rig_spec[25].pttmethod = 2;
  rig_spec[25].transverter_freq[0][0] = 0;
  rig_spec[25].band_mask = 0x0000;

}


void init_rig() {
  /// setting up rigs
  // clear transverter_setting for all rigs.

  // cwport 0: LED, 1: KEY1, 2: KEY2, 3: USB CDC DTR, 4: USB CDC RTS
  for (int j = 0; j < N_RIG; j++) {
    rig_spec[j].band_mask = 0x0;
    rig_spec[j].civport=NULL;
    *rig_spec[j].name='\0';
    rig_spec[j].fskport = -1;
    rig_spec[j].rtty_polarity = -1;
    
    for (int i = 0; i < NMAX_TRANSVERTER; i++) {
      rig_spec[j].transverter_enable[i] = 0;
      rig_spec[j].transverter_freq[i][0] = 0;
      rig_spec[j].transverter_freq[i][1] = 0;
      rig_spec[j].transverter_freq[i][2] = 0;
      rig_spec[j].transverter_freq[i][3] = 0;
    }
  }
  init_rigspec();
}

void init_all_radio() {
  init_rig();
  radio_list[0].rig_idx = 0;  
  init_radio(&radio_list[0], "IC-705");  

  radio_list[0].enabled = 1;

  radio_list[1].rig_idx = 1;  
  init_radio(&radio_list[1], "IC-9700");

  radio_list[1].enabled = 1;
  //  radio_selected = &radio_list[0];
  //  so2r.change_focused_radio(0);

  radio_list[2].rig_idx = 2;  
  //  init_radio(&radio_list[2], "FTDX10");
  init_radio(&radio_list[2], "DJ-G7");  

  radio_list[2].enabled = 0;
  
  new_log_entry();
}

// general rig_spec specification string definition
// rig_idx(0-) name  speficier_string
void set_rig_spec_from_str_rig(struct rig *rig_spec,const char *s)
{
  char s1[300];char *p;
  console->print("RIGSPEC=[");
  console->print(s);
  console->println("]");
  // The rig specification string is a complete replacement, not a partial
  // update.  Clear fields which may be omitted from the serialized string so
  // deleting e.g. XVTR: from the web editor also removes the old RAM value.
  rig_spec->civaddr = 0;
  rig_spec->civport_baud = 0;
  rig_spec->civport_reversed = false;
  rig_spec->fskport = -1;
  rig_spec->rtty_polarity = -1;
  rig_spec->pttmethod = 0;
  memset(rig_spec->transverter_freq, 0, sizeof(rig_spec->transverter_freq));
  char *saveptr1, *saveptr2;  
  char *p1; int idx1,idx2; long long val;// for arg parse
  int n;
  strlcpy(s1, s, sizeof(s1));
  p=strtok_r(s1,", ",&saveptr1);

  console->println("set_rig_spec_from_str_rig()");
  while (p!=NULL) {
    //    console->print("parsed:");console->println(p);
    if (strncmp(p,"CW:",3)==0) {
      console->println("CW: process");      
      // CW port
      n=atoi(p+3);
      if (n>=0 && n<=4) {
	rig_spec->cwport=n;
	console->print("rig_spec cwport =");console->println(	rig_spec->cwport);
      }
    } else if (strncmp(p,"FSK:",4)==0) {
      n=atoi(p+4);
      if (n>=0 && n<=4) rig_spec->fskport=n;
    } else if (strncmp(p,"RP:",3)==0) {
      n=atoi(p+3);
      if (n>=0 && n<=1) rig_spec->rtty_polarity=n;
    } else if (strncmp(p,"B:",2)==0) {
      console->println("B: baudrate");            
      // baudrate
      n=atoi(p+2);
      if (n>=1 && n<=115200) {
	rig_spec->civport_baud = n;
      }
    } else if (strncmp(p,"P:",2)==0) {
      // civport num
      n=atoi(p+2);
      if (n>=-2 && n<=4) {
	rig_spec->civport_num = n;
      }
    } else if (strncmp(p,"ADR:",4)==0) {
      // civaddr
      sscanf(p+4,"%x",&n);
      if (n>=0 && n<256) {
	rig_spec->civaddr=n;
      }
    } else if (strncmp(p,"NAME:",5)==0) {
      if (*(p+5)!='\0') {
	strlcpy(rig_spec->name, p + 5, sizeof(rig_spec->name));
	console->print("name set to rig_spec->name=;");console->println(rig_spec->name);
      }
    } else if (strncmp(p,"XVTR:",5)==0) {
      // transverter frequencies freq jointed by _ from index 0iflo_0ifhi_0rflo_0rfhi_1iflo ...
      //      console->print("parsing token XVTR:");console->println(p);
      p1=strtok_r(p+5,"_",&saveptr2);
      idx1=0;idx2=0;
      while (p1!=NULL) {
	//	console->print("sub token:");console->println(p1);
	if (sscanf(p1,"%lld",&val)==1) {
	  rig_spec->transverter_freq[idx1][idx2]=val/FREQ_UNIT;
	  //	  console->print("transverter freq;");
	  //	  console->print(idx1);console->print(",");
	  //	  console->print(idx2);console->print("=");console->println(rig_spec->transverter_freq[idx1][idx2]);
	  idx2++;
	  if (idx2>=4) {
	    idx2=0;
	    idx1++;
	  }
	  if (idx1>=NMAX_TRANSVERTER) {
	    break; // end of specifying  transverter frequencies
	  }
	  
	} else break;
	// next token
	p1=strtok_r(NULL,"_",&saveptr2);
      }
    } else if (strncmp(p,"R:",2)==0) {
      // port_reverse polarity
      n=atoi(p+2);
      if (n>=0 && n<=1) {
	rig_spec->civport_reversed=n;
      }
    } else if (strncmp(p,"PTT:",4)==0) {
      // Additional PTT method:
      // 0/1 don't care, 2 CAT/CI-V, 3 USB DTR, 4 USB RTS.
      n=atoi(p+4);
      if (n>=0 && n<=4) {
	rig_spec->pttmethod=n;
      }

    } else if (strncmp(p,"BM:",3)==0) {
      console->print("BM token=");
      console->println(p);

      console->print("BM before=");
      console->println(rig_spec->band_mask, HEX);      
      // bandmask hex 
      if (sscanf(p+3,"%x",&n)==1) {
	console->print("parsed=");
	console->println(n, HEX);	
	rig_spec->band_mask = n;
	//	console->print("BM;");console->println(	rig_spec->band_mask ,HEX);
	console->print("after=");
	console->println(rig_spec->band_mask, HEX);
      }
    } else if (strncmp(p,"TP:",3)==0) {
      // cat_type and rig_type
      if (strlen(p)>=3+3) {
	if (isdigit(p[3])) {
	  rig_spec->cat_type=p[3]-'0';
	}
	if (isdigit(p[5])) {
	  rig_spec->rig_type=p[5]-'0';
	}
      }
    }
    p=strtok_r(NULL,", ",&saveptr1);
  }

}

// Apply an edited rig specification atomically.  The new specification is
// parsed into a temporary copy first, so the currently active serial/CAT
// resources can be released using the OLD rig_spec before the global entry is
// replaced.  LCD and Web editors both use this path.
bool update_rig_spec(int rig_index, const char *s, char *errbuf, size_t errbuf_size)
{
  if (errbuf != NULL && errbuf_size > 0) errbuf[0] = '\0';

  if (rig_index < 0 || rig_index >= N_RIG || s == NULL) {
    if (errbuf != NULL && errbuf_size > 0) {
      strlcpy(errbuf, "Invalid RIG index/specification", errbuf_size);
    }
    return false;
  }

  // Preserve the existing parser semantics for omitted fields by starting
  // from the current entry, but do not commit anything until validation has
  // completed.
  struct rig new_spec = rig_spec[rig_index];
  set_rig_spec_from_str_rig(&new_spec, s);

  // A changed NAME must remain unique.  Rig-name lookup intentionally keeps
  // its existing prefix-match behavior; only an exact (case-insensitive)
  // duplicate NAME is rejected here.
  if (strcasecmp(new_spec.name, rig_spec[rig_index].name) != 0) {
    for (int i = 0; i < N_RIG; ++i) {
      if (i == rig_index) continue;
      if (strcasecmp(new_spec.name, rig_spec[i].name) == 0) {
        if (errbuf != NULL && errbuf_size > 0) {
          snprintf(errbuf, errbuf_size, "Duplicate rig name: %s", new_spec.name);
        }
        console->print("RIGSPEC update rejected: duplicate NAME=");
        console->println(new_spec.name);
        return false;
      }
    }
  }

  bool used_by_radio[N_RADIO] = {};
  for (int i = 0; i < N_RADIO; ++i) {
    used_by_radio[i] = (radio_list[i].rig_spec_idx == rig_index);
  }

  // Release using the OLD rig_spec.  Clear each link after releasing it so
  // shared CI-V usage counting remains correct while walking the radios.
  for (int i = 0; i < N_RADIO; ++i) {
    if (!used_by_radio[i]) continue;
    release_rig_serial_resource(&radio_list[i]);
    radio_list[i].rig_spec = NULL;
  }
  console_to_hardwareserial();

  // Never carry the old runtime Stream pointer into the replacement entry.
  // config_rig_serialport() will establish the proper pointer again below.
  new_spec.civport = NULL;
  rig_spec[rig_index] = new_spec;

  // Re-select every radio using this rig_spec.  This refreshes rig_name,
  // rig_spec_str, band_mask, CAT/serial settings, PTT state, and USB rig state
  // identically for LCD and Web edits.
  for (int i = 0; i < N_RADIO; ++i) {
    if (!used_by_radio[i]) continue;
    select_rig(&radio_list[i]);
  }

  return true;
}

// set rig_spec characteristics from string
void set_rig_spec_from_str(struct radio *radio,char *s)
{
  console->println("set_rig_spec_from_str()");    
  struct rig *rig_spec;
  rig_spec=radio->rig_spec;

  set_rig_spec_from_str_rig(rig_spec,s);
}

void print_rig_spec_str(int rig_idx,char *buf) // reverse set rig_spec_string from rig_spec
{
  struct rig *p; char buf1[60];
  if (rig_idx<0|| rig_idx>=N_RIG) {
    //    console->print("rig_idx");
    //    console->print(rig_idx);    
    //    console->print("out of range");
    return;
  }
  p=&rig_spec[rig_idx];
  sprintf(buf1,"CW:%d,", p->cwport);  strcat(buf,buf1);
  if (p->fskport >= 0 && p->fskport <= 4) {
    sprintf(buf1,"FSK:%d,", p->fskport); strcat(buf,buf1);
  }
  if (p->rtty_polarity == 0 || p->rtty_polarity == 1) {
    sprintf(buf1,"RP:%d,", p->rtty_polarity); strcat(buf,buf1);
  }
  sprintf(buf1,"TP:%d_%d,", p->cat_type,p->rig_type);  strcat(buf,buf1);
  sprintf(buf1,"P:%d,", p->civport_num);  strcat(buf,buf1);
  if (p->civaddr!=0) {
    sprintf(buf1,"ADR:%02X,",p->civaddr);  strcat(buf,buf1);
  }
  if (p->civport_baud!=0) {
    sprintf(buf1,"B:%d,",p->civport_baud);strcat(buf,buf1);
  }
  if (p->civport_reversed!=0) {
    sprintf(buf1,"R:%d,",p->civport_reversed);  strcat(buf,buf1);
  }
  sprintf(buf1,"BM:%04X,",p->band_mask&0xffff);  strcat(buf,buf1);  
  if (p->pttmethod!=0) {
    sprintf(buf1,"PTT:%d,",p->pttmethod);  strcat(buf,buf1);
  }
  if (p->transverter_freq[0][0]!=0) {
    sprintf(buf1,"XVTR:"); strcat(buf,buf1);
    for (int i=0;i<NMAX_TRANSVERTER;i++) {
      for (int j=0;j<4;j++) {
	if (p->transverter_freq[i][j]!=0) {
	  // end
	  if (!(i==0 && j==0)) {
	    strcat(buf,"_");
	  }
	  sprintf(buf1,"%lld",((long long)p->transverter_freq[i][j])*FREQ_UNIT);
	  strcat(buf,buf1);
	} else {
	  goto end_loop;
	}
      }
    }
  end_loop:
    strcat(buf,",");
  }

  if (*(p->name)!='\0') {
    // name
    sprintf(buf1,"NAME:%s,",p->name); strcat(buf,buf1);
  }
  // setting print end
  return;
}

void set_rig_spec_str_from_spec(struct radio *radio) // reverse set rig_spec_string from rig_spec
{
  char buf[300];
  int rig_idx;
  int p,len;
  
  *buf='\0';
  rig_idx=radio->rig_spec_idx;
  print_rig_spec_str(rig_idx,buf); // reverse set rig_spec_string from rig_spec
  
  strcpy(radio->rig_spec_str+2,buf);
  // need adjust cursor position rig_spec_str
  adjust_cursor_buf(radio->rig_spec_str);
      
  if (!plogw->f_console_emu) {
    plogw->ostream->print(radio->rig_spec->name);
    plogw->ostream->print(" ");    
    plogw->ostream->println(buf);
  }
}


void save_rigs(const char *fn)
{
  char fnbuf[30];char spec_buf[300];
  plogw->ostream->print("save rig settings to:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/rigs");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);

  // FILE_WRITE appends on ESP32 Arduino.  Remove the old file first so
  // Save RIGs writes exactly the current contents of rig_spec[].
  if (SD.exists(fnbuf) && !SD.remove(fnbuf)) {
    if (!plogw->f_console_emu) {
      plogw->ostream->println("Failed to remove old rigs file");
    }
    return;
  }

  f = SD.open(fnbuf, FILE_WRITE);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file rigs.txt for writing");
    return;
  }
  for (int i=0;i<N_RIG;i++) {
    if (*rig_spec[i].name=='\0') {
      break;
    }
    //    *spec_buf='\0';
    sprintf(spec_buf,"%d ",i);
    //    strcat(spec_buf,rig_spec[i].name);
    //    strcat(spec_buf," ");
    print_rig_spec_str(i,spec_buf); // set string rig_spec_string from rig_spec
    f.println(spec_buf);
    console->println(spec_buf);
  }
  f.close();
  
}


void load_rigs(const char *fn)
{
  char fnbuf[30],spec_buf[300];
  plogw->ostream->print("load rigs from:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/rigs");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);
  // f = SPIFFS.open(fnbuf, FILE_READ);
  f = SD.open(fnbuf, FILE_READ);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file rigs.txt for reading");
    return ;
  }
  while (readline(&f, spec_buf, 0x0d0a, 300) != 0) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("line:");
      plogw->ostream->print(spec_buf);
      plogw->ostream->print(":\r\n");
    }
    //    assign_settings(buf, settings_dict);
    char *p;int idx;
    p=strtok(spec_buf," ");
    if (p!=NULL) {
      if (sscanf(p,"%d",&idx)==1) {
	if (idx>=0&&idx<N_RIG) {
	  // get spec
	  p=strtok(NULL," ");
	  if (p!=NULL) {
	    set_rig_spec_from_str_rig(&rig_spec[idx],p);
	  }
	}
      }
    }
  }
  f.close();
  
  // post process 
  set_rig_from_name(&radio_list[0]);
  set_rig_from_name(&radio_list[1]);
  set_rig_from_name(&radio_list[2]);
}

void init_radio(struct radio *radio, const char *rig_name) {
  radio->xit_enabled = false;
  radio->xit_offset_hz = 0;
  radio->f_freqchange_program=0;
  radio->f_romaji=0;
  radio->antenna=-1; // not connected -1
  radio->f_qsl=0;
  radio->smeter_stat = 0;
  radio->power =0;
  radio->power_bak =0;  
  radio->smeter = 0;
  radio->smeter_peak = SMETER_MINIMUM_DBM;
  radio->bandid = 0;
  radio->bandid_prev = 0;
  radio->enabled = 0;
  radio->band_mask = 0;  // all band may be switched by default
  radio->band_mask_priority = 0;
  radio->smeter_record[0] = 0;
  radio->smeter_record[1] = 0;
  radio->smeter_record[2] = 0;
  radio->smeter_record[3] = 0;
  radio->smeter_azimuth[0] = -1;
  radio->smeter_azimuth[1] = -1;
  radio->smeter_azimuth[2] = -1;
  radio->smeter_azimuth[3] = -1;
  radio->f_smeter_record_ready = 0;
  radio->freq_change_count=0;
  radio->freq_change_candidate=0;
  radio->freqchange_program_guard=0;
  radio->cat_status=0;
  radio->yaesu_query_request_mask = 0;
  radio->yaesu_query_pending_prefix[0] = '\0';
  radio->yaesu_query_pending_until = 0;
  radio->yaesu_query_next_send_at = 0;
  radio->yaesu_query_next_slot = 0;

  radio->bandid_bandmap = 0;

  radio->f_tone_keying = 0;

  radio->qsodata_loaded = 0;

  radio->multi = -1;
  init_buf(radio->callsign, LEN_CALL_WINDOW);
  init_buf(radio->recv_rst, LEN_RST_WINDOW);
  init_buf(radio->sent_rst, LEN_RST_WINDOW);
  init_buf(radio->recv_exch, LEN_DUAL_EXCH_WINDOW);
  init_buf(radio->remarks, LEN_REMARKS_WINDOW);  //37
  radio->callsign_previously_sent[0] = '\0';
  init_buf(radio->rig_name, LEN_RIG_NAME);
  strcpy(radio->rig_name + 2, rig_name);
  set_rig_from_name(radio);
  
  init_buf(radio->rig_spec_str, LEN_RIG_SPEC_STR);
  set_rig_spec_str_from_spec(radio); // reverse set rig_spec_string from rig_spec

  radio->ptt = 0;  // 0 recv 1 send
  radio->ptt_stat = 0;
  radio->ptr_curr = 0;
  radio->keyer_mode = 0;

  radio->modetype = LOG_MODETYPE_UNDEF;       // not defined
  radio->modetype_prev = LOG_MODETYPE_UNDEF;  // not defined
  radio->opmode[0] = '\0';
  radio->mode_initialized = false;

  for (int i = 0; i < 4; i++) {
    radio->cq[i] = LOG_CQ;  // operation mode cq 1 or s&p 0
  }

  // frequency related
  radio->freq = 0;
  radio->freq_prev = 0;
  radio->freq_target = 0;
  radio->freqchange_timer = 0;
  radio->freqchange_retry = 0;
  radio->f_freqchange_pending = 0;
  radio->transverter_in_use = 0;

  set_log_rst(radio);

  // radio->f_civ_response_expected = 0;
  // radio->civ_response_timer = 0;

  for (int k = 0; k < N_BAND; k++) {

    radio->modetype_bank[k]=0; // clear modetype_bank    
    for (int i = 0; i < 4; i++) {
      radio->cq_bank[k][i]=0; // clear cq_bank      
      for (int j = 0; j < 2; j++) {
        radio->freqbank[k][j][i] = 0;  // clear freqency bank
      }
    }
  }
}


void set_rig_from_name(struct radio *radio) {
  int tmp_rig_spec_idx=-1;
  int len;
  len=strlen(radio->rig_name+2);

  for (int i=0;i<N_RIG;i++) {
    if (*rig_spec[i].name=='\0') {
      break;
    }
    if (strncasecmp(rig_spec[i].name,radio->rig_name+2,len)==0) {
      tmp_rig_spec_idx=i;
      break;
    }
  }
  if (tmp_rig_spec_idx<0) {
    // none found
    sprintf(dp->lcdbuf,"NoRig found:\n%s\n",radio->rig_name+2);
    upd_display_info_flash(dp->lcdbuf);
    // reset band_mask
    radio->band_mask=0;
    return ;
  }
  
  int ret;
  ret=check_rig_conflict(radio->rig_idx,&rig_spec[tmp_rig_spec_idx]);
  if (ret== -1) {
    radio->rig_spec_idx=tmp_rig_spec_idx;
    select_rig(radio);
    //    sprintf(dp->lcdbuf,"Rig set:%s",radio->rig_name+2);
    //    upd_display_info_flash(dp->lcdbuf);
    
  } else {
    sprintf(dp->lcdbuf,"config %s\nconflict %s",rig_spec[tmp_rig_spec_idx].name,
	    radio_list[ret].rig_spec->name);
    upd_display_info_flash(dp->lcdbuf);
  }
}

void switch_transverter() {

  // search rig_spec and if current frequency , and
  // if in one of enabled transverter RF frequency, disable it
  // if in one of disabled transverter IF frequency, enable
  sprintf(dp->lcdbuf, "switching XVTR");

  upd_display_info_flash(dp->lcdbuf);

  struct radio *radio;
  radio = so2r.radio_selected();

  for (int i = 0; i < NMAX_TRANSVERTER; i++) {
    if (radio->rig_spec->transverter_freq[i][0] == 0) continue;  // skip if not defined
    switch (radio->rig_spec->transverter_enable[i]) {
      case 1:  // currently enabled
        if ((radio->freq >= radio->rig_spec->transverter_freq[i][0]) && (radio->freq <= radio->rig_spec->transverter_freq[i][1])) {


          radio->f_freqchange_pending = 0;

          radio->rig_spec->transverter_enable[i] = 0;
          // disable it
          sprintf(dp->lcdbuf, "XVTR\nRF %d - %d MHz\nIF %d - %d MHz\nDisabled.",
                  radio->rig_spec->transverter_freq[i][0] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][1] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][2] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][3] / (1000000/FREQ_UNIT));
          upd_display_info_flash(dp->lcdbuf);
        }
        break;
      case 0:  // currently disabled
        if ((radio->freq >= radio->rig_spec->transverter_freq[i][2]) && (radio->freq <= radio->rig_spec->transverter_freq[i][3])) {
          radio->f_freqchange_pending = 0;
          radio->rig_spec->transverter_enable[i] = 1;
          // enable it
          sprintf(dp->lcdbuf, "XVTR\nRF %d - %d MHz\nIF %d - %d MHz\nEnabled.",
                  radio->rig_spec->transverter_freq[i][0] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][1] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][2] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][3] / (1000000/FREQ_UNIT));
          upd_display_info_flash(dp->lcdbuf);
        }
        break;
    }
  }
}

void signal_process()
{
  struct radio *radio;
  radio = so2r.radio_selected();
  if (plogw->f_show_signal) {
    if (radio->f_smeter_record_ready) {
      upd_display_info_signal();
      radio->f_smeter_record_ready = 0;
    }
  }

}

static int receive_civ_frame(struct radio *radio, char c);
static int receive_cat_frame(struct radio *radio, char c);
static int receive_protocol_frame(struct radio *radio);

void get_cat_ats_mini(struct radio *radio)
{
  if (!radio || !radio->rig_spec) return;

  char line[sizeof(radio->cmdbuf)];
  strlcpy(line, radio->cmdbuf, sizeof(line));

  char *field[15] = {};
  char *save = NULL;
  int n = 0;
  for (char *tok = strtok_r(line, ",", &save);
       tok && n < 15;
       tok = strtok_r(NULL, ",", &save))
    field[n++] = tok;

  if (n != 15) {
    if ((verbose & VERBOSE_USB) && radio->cmdbuf[0])
      console->printf("ATS-MINI RX ignored: %s\n", radio->cmdbuf);
    return;
  }

  char *endp = NULL;
  const long base = strtol(field[1], &endp, 10);
  if (!endp || *endp != '\0' || base <= 0) return;

  endp = NULL;
  const long bfo = strtol(field[2], &endp, 10);
  if (!endp || *endp != '\0') return;

  const char *mode = field[5];
  const long long physical_hz = !strcmp(mode, "FM")
      ? (long long)base * 10000LL
      : (long long)base * 1000LL + (long long)bfo;
  if (physical_hz <= 0) return;

  const int ridx = radio->rig_idx;
  const int reported_ats_mode = ats_mini_mode_index(mode);

  // Once virtual CW has reached USB, a later physical-mode change made on
  // ATS itself means the user wants to leave CW; resume normal mode following.
  if (ridx >= 0 && ridx < N_RADIO &&
      ats_mini_virtual_cw[ridx] &&
      !ats_mini_mode_pending[ridx] &&
      reported_ats_mode >= 0 && reported_ats_mode != ats_mini_mode_index("USB")) {
    ats_mini_virtual_cw[ridx] = false;
    if (verbose & VERBOSE_USB)
      console->printf("ATS-MINI virtual CW exited by receiver mode=%s\n", mode);
  }

  long long logical_hz = physical_hz;
  if (ridx >= 0 && ridx < N_RADIO && ats_mini_virtual_cw[ridx]) {
    logical_hz -= ATS_MINI_CW_PITCH_HZ;
    if (logical_hz <= 0) return;
  }

  const unsigned int freq = (unsigned int)(logical_hz / FREQ_UNIT);
  if (freq2bandid(freq) == 0) {
    if (verbose & VERBOSE_USB)
      console->printf(
          "ATS-MINI status outside DVP bands: physical=%lld logical=%lld Hz %s\n",
          physical_hz, logical_hz, mode);
    return;
  }

  const unsigned int old_freq = radio->freq;
  const int old_mode = radio->modetype;
  const int old_smeter = radio->smeter;

  set_frequency((int)freq, radio);

  // field[10] is ATS Mini RSSI (0..127 dBuV). Feed it into the same common
  // S-meter path used by the other rigs, including peak/QSO/antenna handling.
  char *rssi_endp = NULL;
  const long ats_rssi = strtol(field[10], &rssi_endp, 10);
  if (rssi_endp && *rssi_endp == '\0' &&
      ats_rssi >= 0 && ats_rssi <= 127) {
    radio->smeter = (int)ats_rssi;
    conv_smeter(radio);
    smeter_postprocess(radio);
  }

  const int ats_mode = reported_ats_mode;
  if (ats_mode >= 0) {
    if (ridx >= 0 && ridx < N_RADIO)
      ats_mini_mode_current[ridx] = ats_mode;

    if (ridx >= 0 && ridx < N_RADIO && ats_mini_virtual_cw[ridx]) {
      // During virtual CW, ATS reports USB but DVPlogger remains CW.
      set_mode_nonfil("CW", radio);
    } else {
      // Normal physical mode changes on ATS follow into DVPlogger.
      set_mode_nonfil(mode, radio);
    }

    ats_mini_service_mode_target(radio);

    // field[7] is the current bandwidth description (e.g. "0.5k").
    ats_mini_service_cw_bandwidth(radio, field[7]);
  }

  // ATS Mini pushes status every 500 ms. Refresh the main LCD only when the
  // displayed radio state really changed, not on every periodic status frame.
  if (radio->freq != old_freq || radio->modetype != old_mode ||
      (plogw->show_smeter && radio->smeter != old_smeter)) {
    request_display_update_on_demand();
    upd_display_stat();
  }

  if (verbose & VERBOSE_USB) {
    console->printf(
      "ATS-MINI status ver=%s band=%s physical=%lld logical=%lld "
      "mode=%s bw=%s rssi=%s snr=%s smeter=%d seq=%s\n",
      field[0], field[4], physical_hz, logical_hz,
      mode, field[7], field[10], field[11],
      radio->smeter / SMETER_UNIT_DBM, field[14]);
  }
}

static void process_cat_frame(struct radio *radio)
{
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
    yaesu_query_response_received(radio);
    get_cat(radio);
    print_cat(radio);
    break;

  case CAT_TYPE_ATS_MINI:
    get_cat_ats_mini(radio);
    break;

  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX:
    get_cat_kenwood(radio);
    print_cat(radio);
    break;

  case CAT_TYPE_ELECRAFT_KX:
    get_cat_elecraft(radio);
    print_cat(radio);
    break;

  default:
    break;
  }
}

static void process_civ_frame(struct radio *radio)
{
    print_civ(radio);
    get_civ(radio);
}

static void process_protocol_frame(struct radio *radio)
{
    switch (radio->rig_spec->cat_type) {

    case CAT_TYPE_CIV:
        process_civ_frame(radio);
        break;

    case CAT_TYPE_YAESU_FT817:
        get_cat_ft817(radio);
        break;

    case CAT_TYPE_YAESU_NEW:
    case CAT_TYPE_YAESU_OLD:
    case CAT_TYPE_KENWOOD:
    case CAT_TYPE_QMX:
    case CAT_TYPE_ATS_MINI:
    case CAT_TYPE_ELECRAFT_KX:
        process_cat_frame(radio);
        break;

    default:
        break;
    }
}

void civ_process() {
  struct radio *radio;

  /*  
  static uint32_t last_cat_rx_overflow[N_RADIO] = {};
  
  if (verbose & 1) {
    for (int i = 0; i < N_RADIO; i++) {
      uint32_t count = radio_list[i].cat_rx_overflow;
      if (count != last_cat_rx_overflow[i]) {
        plogw->ostream->print("!CAT RX ring overflow rig=");
        plogw->ostream->print(i);
        plogw->ostream->print(" total=");
        plogw->ostream->print(count);
        plogw->ostream->print(" delta=");
        plogw->ostream->println(count - last_cat_rx_overflow[i]);
        last_cat_rx_overflow[i] = count;
      }
    }
  }
  */
  static uint32_t report_at = 0;
  static uint32_t previous_overflow[N_RADIO] = {};

  uint32_t now = millis();
  if ((int32_t)(now - report_at) >= 0) {
    report_at = now + 1000;

    for (int i = 0; i < N_RADIO; i++) {
      uint32_t count = radio_list[i].cat_rx_overflow;

      if (count != previous_overflow[i]) {
        plogw->ostream->printf(
          "!CAT RX overflow rig=%d total=%lu delta=%lu r=%d w=%d\n",
          i,
          (unsigned long)count,
          (unsigned long)(count - previous_overflow[i]),
          radio_list[i].r_ptr,
          radio_list[i].w_ptr);

        previous_overflow[i] = count;
      }
    }
  }
  
  // receive_protocol_frame() consumes one byte per call.  Drain several bytes on each
  // main-loop pass so the 1 ms HardwareSerial producer cannot outrun the
  // command parser when the loop is temporarily busy.  Keep a finite budget
  // so a continuously active CAT port cannot monopolize the main loop.
  const int byte_budget_per_radio = 128;

  for (int i = 0; i < N_RADIO; i++) {
    if (!unique_num_radio(i)) continue;
    radio = &radio_list[i];  // main rig reception
    
    /*
     * Always process already-received CAT data before checking query
     * timeout or sending another query.  Otherwise a complete response
     * waiting in the RX ring may be timed out before its remaining bytes
     * are parsed.
     */
    int byte_budget = byte_budget_per_radio;
    while ((byte_budget-- > 0) && (radio->r_ptr != radio->w_ptr)) {
      if (!receive_protocol_frame(radio)) continue;
      
      process_protocol_frame(radio);
      clear_civ(radio);
    }

  /*
   * Check timeout and send the next query only after draining the
   * received data.
   */
  yaesu_query_service(radio);
  }

  radio = so2r.radio_selected();
  // check smeter reading trigger
  // S&P and Phone
  if ((!radio->cq[radio->modetype]) && (radio->modetype == LOG_MODETYPE_PH)) {
    if (radio->ptr_curr != 0) {  // if not in callsign window, assume QSO starts
      // number input window
      if (radio->smeter_stat == 0) {
        // start checking smeter_peak
        radio->smeter_stat = 1;
        radio->smeter_peak = SMETER_MINIMUM_DBM;
        if (!plogw->f_console_emu) plogw->ostream->println("start reading smeter_peak");
      }

    } else {
      // callsign window now
      if (radio->smeter_stat != 0) {
        // cancel smeter reading
        radio->smeter_stat = 0;
        radio->smeter_peak = SMETER_MINIMUM_DBM;
        if (!plogw->f_console_emu) plogw->ostream->println("cancel reading smeter_peak");
      }
    }
  }
  if (radio->ptt_stat == 2) {
    radio->ptt_stat = 0;  // force receive ptt why??? 23/1/11
  }
}

static int receive_civ_frame(struct radio *radio, char c) {
  (void)radio;
  if (c == 0xfd) return 1;
  return 0;
}

static int receive_cat_frame(struct radio *radio, char c) {
  switch (radio->rig_spec->cat_type) {
  case CAT_TYPE_YAESU_FT817:
    radio->cat_status++;
    switch (radio->cat_status & 0xf0) {
    case 0x10: // awaiting RX status
      return 1;
    case 0x20: // awaiting TX status
      return 1;
    case 0x30: // awaiting Freq & Mode status
      if ((radio->cat_status & 0x0f) == 5) {
        return 1;
      }
      return 0;
    default: // ignore
      return 0;
    }

  case CAT_TYPE_YAESU_NEW: // Yaesu
  case CAT_TYPE_YAESU_OLD:
    // Lost ';' ?  If another IF appears, restart from there.
    if (radio->cmd_ptr > 2 &&
        radio->cmdbuf[0] == 'I' &&
        radio->cmdbuf[1] == 'F' &&
        radio->cmdbuf[radio->cmd_ptr - 2] == 'I' &&
        radio->cmdbuf[radio->cmd_ptr - 1] == 'F') {

      if (verbose & 1) {
        plogw->ostream->print("!Yaesu IF resync rig=");
        plogw->ostream->print(radio->rig_idx);
        plogw->ostream->print(" discarded=");
        plogw->ostream->println(radio->cmd_ptr - 2);
      }

      radio->cmdbuf[0] = 'I';
      radio->cmdbuf[1] = 'F';
      radio->cmd_ptr = 2;
      return 0;
    }    
    // A normal Yaesu ASCII CAT response is well below 40 bytes.
    // If the terminator was lost or the stream became corrupted, discard the
    // partial frame early so it cannot be joined to the next response.
    if (radio->cmd_ptr > 40) {
      if (verbose & 1) {
        plogw->ostream->print("!Yaesu CAT frame too long; discard rig=");
        plogw->ostream->print(radio->rig_idx);
        plogw->ostream->print(" len=");
        plogw->ostream->println(radio->cmd_ptr);
      }
      clear_civ(radio);
      return 0;
    }
    if (c == ';') {
      radio->cmdbuf[radio->cmd_ptr] = '\0';
      return 1;
    }
    return 0;

  case CAT_TYPE_ATS_MINI:
    if (c == '\n') {
      if (radio->cmd_ptr >= 2 && radio->cmdbuf[radio->cmd_ptr - 2] == '\r')
        radio->cmdbuf[radio->cmd_ptr - 2] = '\0';
      else
        radio->cmdbuf[radio->cmd_ptr - 1] = '\0';
      return 1;
    }
    return 0;

  case CAT_TYPE_KENWOOD:
  case CAT_TYPE_QMX: // Kenwood
    if (c == ';') {
      radio->cmdbuf[radio->cmd_ptr] = '\0';
      return 1;
    }
    return 0;

  default:
    return 0;
  }
}

static int receive_protocol_frame(struct radio *radio) {
  if (!radio->enabled) return 0;

  char c;
  while (radio->r_ptr != radio->w_ptr) {
    // Leave one byte for the NUL terminator used by ASCII CAT protocols.
    // If a terminator was lost, discard the partial frame rather than writing
    // beyond cmdbuf or joining it indefinitely to following responses.
    if (radio->cmd_ptr >= (int)sizeof(radio->cmdbuf) - 1) {
      if (verbose & 1) {
        plogw->ostream->print("!CAT command buffer overflow rig=");
        plogw->ostream->println(radio->rig_idx);
      }
      clear_civ(radio);
    }

    c = radio->bt_buf[radio->r_ptr];
    radio->r_ptr = (radio->r_ptr + 1) % 256;
    radio->cmdbuf[radio->cmd_ptr++] = c;

    /*
    if (verbose & 1) {
      console->printf("RX '%c' %02X cmd_ptr=%d\n",
		      isprint((unsigned char)c) ? c : '.',
		      (unsigned char)c,
		      radio->cmd_ptr);
    }    
    */
    switch (radio->rig_spec->cat_type) {
    case CAT_TYPE_CIV:
      return receive_civ_frame(radio, c);
    default:
      return receive_cat_frame(radio, c);
    }
  }
  return 0;
}

// check if the radio index i is the radio which did not appear in previos indexes.
int unique_num_radio(int i) {
  if (i == 0) return 1;
  if (i == 1) {
    if (radio_list[0].rig_spec_idx == radio_list[1].rig_spec_idx) {
      return 0;
    } else {
      return 1;
    }
  }
  if (i == 2) {
    if (radio_list[0].rig_spec_idx == radio_list[2].rig_spec_idx) {
      return 0;
    }
    if (radio_list[1].rig_spec_idx == radio_list[2].rig_spec_idx) {
      return 0;
    }
    return 1;
  }
  return 0;
}

// RTTYメッセージ送信前後　などのPTT制御を行う。
void Control_TX_process() {
  // Also service deferred Yaesu TX-CLAR release requests from the CW ticker.
  service_xit_cw_message();
  struct radio *radio;
  radio = &radio_list[so2r.tx()];
  switch (f_transmission) {
    case 0:  // do nothing
      break;
    case 1:  // force transmission
      set_ptt_rig(radio, 1);
      // For USB FSK RTTY, measure the MARK lead from here, after the PTT ON
      // command has actually been issued.  Previously the lead started in
      // the 1-ms CW ticker before this main-loop PTT operation, so a busy
      // main loop could clip the beginning of the RTTY message.
      if (f_rtty_usb_lead_pending) {
        const uint8_t fskport = (uint8_t)rig_fsk_port(radio->rig_spec);
        if (!radio->f_tone_keying && (fskport == 3 || fskport == 4)) {
          int lead_ms = rtty_ptt_lead_ms;
          if (lead_ms < 0) lead_ms = 0;
          if (lead_ms > 2000) lead_ms = 2000;
          usb_rtty_begin(fskport, (uint32_t)lead_ms, rig_rtty_invert(radio->rig_spec));
          cw_count_ms = lead_ms;
        }
        f_rtty_usb_lead_pending = false;
      }
      if (!plogw->f_console_emu) {
        plogw->ostream->println("PTT ON TX=");
        plogw->ostream->println(so2r.tx());
      }
      if (radio->f_tone_keying) {
	keying(1); // PTT in tone keying by keying contact
      }
      break;
    case 2:  // force stop transmission
      set_ptt_rig(radio, 0);
      if (radio->f_tone_keying) {
	//        ledcWrite(LEDC_CHANNEL_0, 0);  // stop sending tone
	keying(0);
      } else {
        // digitalWrite(LED, 0);
        keying(0);
      }
      if (!plogw->f_console_emu) {
        plogw->ostream->print("PTT OFF TX=");
        plogw->ostream->println(so2r.tx());
      }

      break;
  }
  f_transmission = 0;
}

void rotator_sweep_process()
{
  if ((plogw->f_rotator_sweep==1) && (plogw->f_rotator_enable==1)) {
    if (plogw->rotator_sweep_timeout < millis()) {
      plogw->rotator_sweep_timeout = millis() + 2000; // interval of the next sweep control set
      // compare current with the target
      // boundary is south (180deg)
      int az, target_az ;
      az = plogw->rotator_az;
      if (az > 180.0) az -= 360.0;
      target_az = plogw->rotator_target_az_sweep;
      if (target_az > 180.0) target_az -= 360.0;
      plogw->ostream->println("rotator_sweep_process");
	  if (plogw->f_rotator_sweep_suspend>0) {
		  plogw->ostream->println("suspending");
		  if (plogw->f_rotator_sweep_suspend>1) {
			  return;
		  } else {
		     plogw->rotator_target_az = az;
			 plogw->f_rotator_sweep_suspend=2;
		  }
	  } else {
      if (plogw->rotator_sweep_step > 0) {
        if (target_az > az) {
          plogw->rotator_target_az = az + plogw->rotator_sweep_step;
        } else {
          // reached target
          plogw->f_rotator_sweep = 0;
          plogw->ostream->println("rotator sweep finished.");
        }
      } else {
        if (target_az < az) {
          plogw->rotator_target_az = az + plogw->rotator_sweep_step;
        } else {
          // reached target
          plogw->f_rotator_sweep = 0;
          plogw->ostream->println("rotator sweep finished.");
        }
      }
	  }
      if (plogw->f_rotator_sweep) {
        if (plogw->rotator_target_az < 0) plogw->rotator_target_az += 360;
        plogw->ostream->print("send rotator target=");
        plogw->ostream->println(plogw->rotator_target_az);
        send_rotator_az_set_civ(plogw->rotator_target_az);
      }
    }
  }
}


/// satellite related ?
void set_sat_opmode(struct radio *radio, char *opmode) {
  if (!radio->enabled) return;
  switch (radio->rig_spec->rig_type) {
    case 0:  // IC-705 VFO sel/unselected to set RX/TX frequency
      break;
    case 1:  // IC-9700 main (TX) and sub (RX) frequencies  normally select sub
      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo
      add_civ_buf((byte)0xd0);  // main (TX)
      send_tail_civ(radio);

      // set mode for uplink
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // set mode
      add_civ_buf((byte)rig_modenum(opmode));
      add_civ_buf((byte)0x01);  // FIL1
      send_tail_civ(radio);

      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo
      add_civ_buf((byte)0xd1);  // sub (RX)
      send_tail_civ(radio);

      // set mode for downlink
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // mode
      if ((strcmp(opmode, "CW-R") == 0) || (strcmp(opmode, "CW") == 0)) {
        add_civ_buf((byte)rig_modenum("USB"));

      } else {
        add_civ_buf((byte)rig_modenum(opmode));
      }
      add_civ_buf((byte)0x01);  // FIL1
      send_tail_civ(radio);

      // set opmode, and filt for operation purpose
      set_mode(opmode, 1, radio);

      break;
  }
}

void adjust_frequency(int dfreq) {
  // dfreq is in FREQ_UNIT
  
  // frequency control up and down
  if (!plogw->f_console_emu) plogw->ostream->println("adjust_frequency()");
  if (plogw->sat) {
    // satellite mode manipulate up_f, .. etc
    // satellite frequencies are in Hz (not FREQ_UNIT)
    switch (plogw->sat_freq_tracking_mode) {
      case SAT_RX_FIX:  //RX fixed (tx controll)
        if (plogw->dn_f != 0) {
          plogw->dn_f += dfreq*FREQ_UNIT;
        }
        break;
      case SAT_TX_FIX:  // TX fixed
        if (plogw->up_f != 0) {
          plogw->up_f += dfreq*FREQ_UNIT;
        }

        break;
      case SAT_SAT_FIX:  // satellite freq fixed
        if (plogw->satdn_f != 0) plogw->satdn_f += dfreq*FREQ_UNIT;
        break;
    }
    //    set_sat_freq_calc();   // finally set frequency
  } else {
    set_frequency_rig((int)(so2r.radio_selected()->freq + dfreq));  //
  }
}

void save_freq_mode_filt(struct radio *radio) {
  radio->bandid = freq2bandid(radio->freq);  // make sure that these are always matching
  if (verbose &4) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("save f=");
      plogw->ostream->print(radio->freq);
      plogw->ostream->print(" mode ");
      plogw->ostream->print(radio->opmode);
      plogw->ostream->print(" filt ");
      plogw->ostream->print(radio->filt);
      plogw->ostream->print(" in b ");
      plogw->ostream->print(radio->bandid);
      plogw->ostream->print(" cq ");
      plogw->ostream->print(radio->cq[radio->modetype]);
      plogw->ostream->print(" modetype ");
      plogw->ostream->print(radio->modetype);
      plogw->ostream->println("");
    }
  }
  //  radio->cq_modetype_bank[radio->bandid]=radio->cq[radio->modetype]*4+(radio->modetype&0x3);
  // save cq and modetype
  radio->cq_bank[radio->bandid][radio->modetype]=radio->cq[radio->modetype];
  radio->modetype_bank[radio->bandid]=radio->modetype;
  // then save freq
  radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->freq;
  radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = rig_modenum(radio->opmode);
  radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->filt;
}

static void recall_freq_mode_filt_impl(struct radio *radio, int target_modetype, bool force_modetype) {
  int freq;

  // Normal recalls follow the band's remembered mode type.  Explicit CW/PH
  // bank changes (Alt-Z) must not be overwritten by a stale modetype_bank.
  if (force_modetype) {
    if (target_modetype != LOG_MODETYPE_CW &&
        target_modetype != LOG_MODETYPE_PH &&
        target_modetype != LOG_MODETYPE_DG) {
      return;
    }
    radio->modetype = target_modetype;
    radio->modetype_bank[radio->bandid] = target_modetype;
  } else if (radio->modetype_bank[radio->bandid] != LOG_MODETYPE_UNDEF) {
    radio->modetype = radio->modetype_bank[radio->bandid];
  } else if (radio->rig_spec &&
             radio->rig_spec->cat_type == CAT_TYPE_NOCAT) {
    // A manual rig has no CAT response that can resolve an undefined mode.
    // Start an unused band in the CW bank so the displayed default mode and
    // the ESM/CW-keying decision use the same, valid mode type.
    radio->modetype = LOG_MODETYPE_CW;
    radio->modetype_bank[radio->bandid] = LOG_MODETYPE_CW;
  }

  radio->cq[radio->modetype] = radio->cq_bank[radio->bandid][radio->modetype];

  // then recover freq
  freq = radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];

  if (!plogw->f_console_emu) {
    plogw->ostream->print("recall f=");
    plogw->ostream->print(freq);

    plogw->ostream->print(" in b ");
    plogw->ostream->print(radio->bandid);
    plogw->ostream->print(" cq ");
    plogw->ostream->print(radio->cq[radio->modetype]);
    plogw->ostream->print(" modetype ");
    plogw->ostream->print(radio->modetype);
  }
  int modenum, filt;

  if (freq == 0) {
    // default not set
    freq = bandid2freq(radio->bandid, radio);
    char *opmode;
    // set default mode and filt in modebank and filtbank
    opmode = default_opmode(radio->bandid, radio->modetype);
    filt = default_filt(opmode);
    strcpy(radio->opmode, opmode);
    modenum = rig_modenum(radio->opmode);
    radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = modenum;
    radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = filt;
  }
  //set_frequency(freq, radio);  // this should be performed from civ receive process not to be forced? 23/4/23
  //  set_frequency_rig(freq);
  
  radio->f_recall_freq_mode_filt = 1;
  set_frequency_rig(freq);
  //    int modenum, filt;
  modenum = radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];
  filt = radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];

  // A manual rig has no CAT response that can update the local mode/filter
  // state after a bank recall.  Keep DVPlogger's internal state in sync with
  // the recalled bank before updating the display and bandmap.
  if (radio->rig_spec && radio->rig_spec->cat_type == CAT_TYPE_NOCAT) {
    const char *recalled_opmode = opmode_string(modenum);
    if (recalled_opmode && recalled_opmode[0] != '\0') {
      strncpy(radio->opmode, recalled_opmode, sizeof(radio->opmode) - 1);
      radio->opmode[sizeof(radio->opmode) - 1] = '\0';
    }
    radio->filt = filt;

    // Keep the operating class used by ESM/CW keying consistent with the
    // mode text shown on the display.  Without CAT, no later response exists
    // to perform this conversion for us.
    int recalled_modetype = modetype_string(radio->opmode);
    if (recalled_modetype != LOG_MODETYPE_UNDEF) {
      radio->modetype = recalled_modetype;
      radio->modetype_bank[radio->bandid] = recalled_modetype;
    }
    radio->mode_initialized =
        (radio->opmode[0] != '\0' &&
         radio->modetype != LOG_MODETYPE_UNDEF);
  }

  if (!plogw->f_console_emu) {
    plogw->ostream->print(" modenum ");
    plogw->ostream->print(modenum);
    plogw->ostream->print(" filt ");
    plogw->ostream->print(filt);
    plogw->ostream->println("");
  }

  send_mode_set_civ(opmode_string(modenum), filt);
  // also send bandscope width here? 25/5/5
  set_scope_mode(radio,modenum);
}

void recall_freq_mode_filt(struct radio *radio) {
  recall_freq_mode_filt_impl(radio, LOG_MODETYPE_UNDEF, false);
}

void recall_freq_mode_filt_for_modetype(struct radio *radio, int target_modetype) {
  recall_freq_mode_filt_impl(radio, target_modetype, true);
}

int bandid2freq(int bandid, struct radio *radio) {
  // later, obtain previous frequency from the freqbank (with bandid)
  switch (radio->modetype) {
    case 0:  // default
    case 1:  // CW
    case 3:  // DG
      switch (bandid) {
        case 0:  // default
        default:
          return 0;
        case 1:  // 1.9
          return 1801000/FREQ_UNIT;
        case 2:  // 3.5
          return 3510000/FREQ_UNIT;
        case 3:  // 7.0
          return 7010000/FREQ_UNIT;
        case 4:  // 14.0
          return 14050000/FREQ_UNIT;
        case 5:  // 21.
          return 21050000/FREQ_UNIT;
        case 6:  // 28.
          return 28050000/FREQ_UNIT;
        case 7:  // 50.
          return 50050000/FREQ_UNIT;
        case 8:  // 144
          return 144050000/FREQ_UNIT;
        case 9:  // 430
          return 430050000/FREQ_UNIT;
        case 10:  // 1200
          return 1294050000/FREQ_UNIT;
        case 11:  // 2400
          return 2424050000/FREQ_UNIT;
        case 12:  // 5600
          return 5760000000ULL/FREQ_UNIT;
      case 13:  // 10G
          return 10240000000ULL/FREQ_UNIT;
      case 14:  // 10M
          return 10110000ULL/FREQ_UNIT;
      case 15:  // 18M
          return 18078000ULL/FREQ_UNIT;
      case 16:  // 24M
          return 24900000ULL/FREQ_UNIT;
      }
      return 0;
      break;

    case 2:  // PHONE
      switch (bandid) {
        case 0:  // default
        default:
          return 0;
        case 1:  // 1.9
          return 1850000/FREQ_UNIT;
        case 2:  // 3.5
          return 3535000/FREQ_UNIT;
        case 3:  // 7.0
          return 7060000/FREQ_UNIT;
        case 4:  // 14.0
          return 14250000/FREQ_UNIT;
        case 5:  // 21.
          return 21350000/FREQ_UNIT;
        case 6:  // 28.
          return 28600000/FREQ_UNIT;
        case 7:  // 50.
          return 50350000/FREQ_UNIT;
        case 8:  // 144
          return 144250000/FREQ_UNIT;
        case 9:  // 430
          return   433020000/FREQ_UNIT;
        case 10:  // 1200
          return  1295000000/FREQ_UNIT;
        case 11:  // 2400
          return  2427000000/FREQ_UNIT;
        case 12:  // 5600
          return  5760000000ULL/FREQ_UNIT;
        case 13:  // 10000
          return 10240000000ULL/FREQ_UNIT;
      case 14:  // 10M
          return 10110000ULL/FREQ_UNIT;
      case 15:  // 18M
          return 18110000ULL/FREQ_UNIT;
      case 16:  // 24M
          return 24930000ULL/FREQ_UNIT;
      }
      return 0;
      break;
  }
  return 0;
}


char *default_opmode(int bandid, int modetype) {
  // 1    2    3 4  5  6  7  8   9   10   11   12     13 14 15
  // 1.8  3.5  7 14 21 28 50 144 430 1200 2400 5600
  switch (modetype) {
    case LOG_MODETYPE_CW: return "CW"; break;
    case LOG_MODETYPE_PH:
      if (bandid <= 3) {
        return "LSB";
      }
      // HF high bands, 50 MHz and 144 MHz default to USB.
      // 430 MHz and above retain the existing FM defaults.
      if (bandid <= 8) {
        return "USB";
      }
      return "FM";
    case LOG_MODETYPE_DG:
      return "RTTY";
      break;
    case LOG_MODETYPE_UNDEF:
      return "CW";
      break;
  }
  return "CW";
}

int default_filt(const char *opmode) {
  int modenum;
  modenum = rig_modenum(opmode);
  // LSB 0 USB 1 AM 2 CW 3 RTTY 4 FM 5 WFM 6 CW-R 7 RTTY-R 8 DV 0x17
  switch (modenum) {
    case 3:      // CW
    case 7:      // CW-R
      return 2;  // FIL2
      break;
    case 0:     // LSB
    case 1:     // USB
    case 2:     // AM
    case 4:     // RTTY
    case 5:     // FM
    case 6:     // WFM
    case 8:     // RTTY-R
    case 0x17:  // DV
    default:
      return 1;  // FIL1 (wide)
      break;
  }
}
