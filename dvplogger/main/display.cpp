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
#include "edit_buf.h"
#include "timekeep.h"
#include <U8g2lib.h>
#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#include "i2c_guard.h"
#endif


#ifndef BANDMAP_TRACE
#define BANDMAP_TRACE 0
#endif
// normal 1.3inch OLED display
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_r_1(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_l_1(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// 2.4 inch display 
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2_r_2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2_l_2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

class U8G2 *u8g2_l;
class U8G2 *u8g2_r;


#include "main.h"
#include "display.h"
#include "multi_process.h"
#include "multi.h"
#include "log.h"
#include "qso.h"
#include "bandmap.h"
#include "dupechk.h"
#include "cw_keying.h"
#include "so2r.h"
#include "mux_transport.h"
#include "satellite.h"
#include "freertos/queue.h"
//#include <u8g2_font_t0_8_mf.h>

#define DVP_STRINGIFY_INNER(x) #x
#define DVP_STRINGIFY(x) DVP_STRINGIFY_INNER(x)

uint8_t *dispbuf_r=nullptr, *dispbuf_l=nullptr;

enum DisplayRequestType : uint8_t {
  DISPLAY_REQ_UPDATE = 1,
  DISPLAY_REQ_INFO_FLASH,
  DISPLAY_REQ_CONTEST_SETTINGS,
  DISPLAY_REQ_BANDMAP,
  DISPLAY_REQ_CWBUF,
  DISPLAY_REQ_RTTY_RX
};

struct DisplayRequest {
  uint8_t type;
  int8_t radio_idx;
  char text[192];
};

static QueueHandle_t s_display_queue = nullptr;
static TaskHandle_t s_display_owner = nullptr;
static volatile uint32_t s_display_dropped = 0;

static inline void service_mux_transport()
{
  if (f_mux_transport) {
    mux_transport.recv_pkt();
  }
}

enum DupeAwareDisplayState : uint8_t {
  DUPE_DISPLAY_IDLE = 0,
  DUPE_DISPLAY_WAIT_ACK,
  DUPE_DISPLAY_DRAW_PENDING,
  DUPE_DISPLAY_FLUSH_PENDING
};

static DupeAwareDisplayState s_dupe_display_state = DUPE_DISPLAY_IDLE;
static uint32_t s_dupe_display_wait_started_us = 0;
static bool s_bandmap_update_pending = false;
static const uint32_t DUPE_DISPLAY_ACK_BUDGET_US = 8000U;

static void upd_display_render(bool flush_to_oled);

void init_display_dispatch()
{
  s_display_owner = xTaskGetCurrentTaskHandle();
  if (s_display_queue == nullptr) {
    s_display_queue = xQueueCreate(8, sizeof(DisplayRequest));
  }
}

bool display_is_main_loop()
{
  return s_display_owner == nullptr || xTaskGetCurrentTaskHandle() == s_display_owner;
}

static bool defer_display(uint8_t type, const char *text = nullptr, int radio_idx = -1)
{
  if (display_is_main_loop()) return false;
  if (s_display_queue == nullptr) return true;
  DisplayRequest req{};
  req.type = type;
  req.radio_idx = radio_idx;
  if (text) strlcpy(req.text, text, sizeof(req.text));
  if (xQueueSend(s_display_queue, &req, 0) != pdTRUE) ++s_display_dropped;
  return true;
}

void process_display_requests()
{
  if (!display_is_main_loop() || s_display_queue == nullptr) return;
  DisplayRequest req;
  int budget = 8;
  while (budget-- > 0 && xQueueReceive(s_display_queue, &req, 0) == pdTRUE) {
    switch (req.type) {
      case DISPLAY_REQ_UPDATE: request_display_update_on_demand(); break;
      case DISPLAY_REQ_INFO_FLASH: upd_display_info_flash(req.text); break;
      case DISPLAY_REQ_CONTEST_SETTINGS:
        if (req.radio_idx >= 0 && req.radio_idx < 3)
          upd_display_info_contest_settings(&radio_list[req.radio_idx]);
        break;
      case DISPLAY_REQ_BANDMAP: request_bandmap_update_on_demand(); break;
      case DISPLAY_REQ_CWBUF: display_cw_buf_lcd(req.text); break;
      case DISPLAY_REQ_RTTY_RX: upd_display_rtty_decoder(req.text); break;
    }
  }
  if (s_display_dropped) {
    console->printf("display queue: %lu request(s) dropped\n", (unsigned long)s_display_dropped);
    s_display_dropped = 0;
  }
}

void request_display_update_on_demand()
{
  request_dupe_aware_display_update();
}

void request_bandmap_update_on_demand()
{
  if (!display_is_main_loop()) {
    if (s_display_queue != nullptr) {
      DisplayRequest req{};
      req.type = DISPLAY_REQ_BANDMAP;
      if (xQueueSend(s_display_queue, &req, 0) != pdTRUE) ++s_display_dropped;
    }
    return;
  }
  // This on-demand request supersedes the legacy interval flag.  Clear it
  // here so the same event cannot schedule a second redraw later.
  bandmap_disp.f_update = 0;
  s_bandmap_update_pending = true;
}

void request_dupe_aware_display_update()
{
  if (!display_is_main_loop()) {
    if (s_display_queue != nullptr) {
      DisplayRequest req{};
      req.type = DISPLAY_REQ_UPDATE;
      if (xQueueSend(s_display_queue, &req, 0) != pdTRUE) ++s_display_dropped;
    }
    return;
  }

  s_dupe_display_wait_started_us = micros();
  s_dupe_display_state = dupechk_remote_query_pending()
                           ? DUPE_DISPLAY_WAIT_ACK
                           : DUPE_DISPLAY_DRAW_PENDING;
}

void process_dupe_aware_display_update()
{
  if (!display_is_main_loop()) return;

  switch (s_dupe_display_state) {
    case DUPE_DISPLAY_IDLE:
      break;

    case DUPE_DISPLAY_WAIT_ACK:
      // Keep the MUX moving while waiting, but never block the main loop.
      service_mux_transport();
      if (dupechk_remote_ack_received() ||
          !dupechk_remote_query_pending() ||
          (uint32_t)(micros() - s_dupe_display_wait_started_us) >=
            DUPE_DISPLAY_ACK_BUDGET_US) {
        s_dupe_display_state = DUPE_DISPLAY_DRAW_PENDING;
      }
      break;

    case DUPE_DISPLAY_DRAW_PENDING:
      // Draw into the RAM framebuffer only.  A later loop performs I2C.
      service_mux_transport();
      upd_display_render(false);
      service_mux_transport();
      s_dupe_display_state = DUPE_DISPLAY_FLUSH_PENDING;
      break;

    case DUPE_DISPLAY_FLUSH_PENDING:
      // OLED transfer is still synchronous, so service MUX immediately before
      // and after it.  Consecutive key strokes coalesce by resetting the state.
      service_mux_transport();
      right_display_sendBuffer();
      service_mux_transport();
      s_dupe_display_state = DUPE_DISPLAY_IDLE;
      break;
  }

  // Bandmap redraws use the left OLED and may also take about 50 ms.  Run
  // them only after the right-side update is complete, coalescing repeated
  // band changes into a single redraw.
  if (s_dupe_display_state == DUPE_DISPLAY_IDLE &&
      s_bandmap_update_pending &&
      !plogw->sat &&
      !dupechk_remote_query_pending() &&
      !(info_disp.show_info == INFO_DISP_FLASH && info_disp.timer > 0) &&
      !(info_disp.show_info == INFO_DISP_RTTY_RX && info_disp.timer > 0)) {
    s_bandmap_update_pending = false;
    service_mux_transport();
    upd_display_bandmap();
    service_mux_transport();
  }
}

static void i2c_guarded_send_buffer(U8G2 *display, const char *owner)
{
  if (display == nullptr) return;
  if (!i2c_bus_lock(owner, pdMS_TO_TICKS(10))) return;
  uint32_t started_us = micros();
  display->sendBuffer();
  uint32_t elapsed_us = micros() - started_us;
  i2c_bus_unlock(owner);
  i2c_diag_io(owner, elapsed_us);
}

// Clear exactly one text-row cell.  The input screen is composed from complete
// rows (including rows that contain two logical fields), so clearing the whole
// row and redrawing its complete contents is safer than widening an individual
// field rectangle.  Clamp at the physical display edge to avoid touching a
// neighbouring row or wrapping an unsigned coordinate.
static void clear_display_text_row(U8G2 *display, int row, int row_height)
{
  if (display == nullptr || row < 0 || row_height <= 0) return;

  const int display_height = display->getDisplayHeight();
  const int y = row * row_height;
  if (y < 0 || y >= display_height) return;

  // Some lowercase glyphs (g, j, p, q, y) extend below the nominal
  // font row.  Clear two extra scan lines as a deliberate, inexpensive
  // over-clear.  Full rows are redrawn afterwards, and the final row is
  // clamped at the physical display edge.
  int clear_height = row_height + 2;
  if (y + clear_height > display_height)
    clear_height = display_height - y;

  display->setDrawColor(0);
  display->drawBox(0, y, display->getDisplayWidth(), clear_height);
  display->setDrawColor(1);
}


#define WCOL_STR "                 "
void display_cw_buf_lcd(char *buf) {
  if (!display_is_main_loop()) {
    if (verbose & VERBOSE_SEQUENCE) {
      console->printf("[CWREQ ] tx=%d msg=%d focus=%d seq=%d text=\"%.20s\"\n",
                      so2r.tx(), so2r.msg_tx_radio(), so2r.focused_radio(),
                      so2r.sequence_stat(), buf ? buf : "");
    }
    if (defer_display(DISPLAY_REQ_CWBUF, buf)) return;
  }
  if (verbose & VERBOSE_SEQUENCE) {
    console->printf("[CWDRAW] tx=%d msg=%d focus=%d seq=%d text=\"%.20s\"\n",
                    so2r.tx(), so2r.msg_tx_radio(), so2r.focused_radio(),
                    so2r.sequence_stat(), buf ? buf : "");
  }
  int LCD_CW_POSY ;
  LCD_CW_POSY=dp->hcol[0]*4;
  //#define LCD_CW_POSY (24 + 13 + 13)
  strncpy(dp->lcdbuf, buf, 20);
  //  sprintf(lcdbuf, "%-20s", buf);
  //  plogw->ostream->println(lcdbuf);
  clear_display_text_row(u8g2_r, 4, dp->hcol[0]);
  //  u8g2_r->drawStr(0, LCD_CW_POSY, dp->lcdbuf);
  u8g2_r->drawUTF8(0, LCD_CW_POSY , dp->lcdbuf);  
  // Underline according to the radio that will actually be keyed.
  // During manual keyboard CW this may differ from so2r.tx(), because the
  // automatic SO2R sequence keeps its own TX state while manual CW uses the
  // currently focused radio.
  switch (effective_cw_radio()) {
  case 0: // radio 0
    break;
  case 1: // radio 1
    //    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]*10/10, 128);
    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]-2, 128);    
    break;
  case 2: // radio 2
    //    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]*9/10, 128);    
    //    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]*10/10, 128);
    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]-4, 128);    
    u8g2_r->drawHLine(0, LCD_CW_POSY+dp->hcol[0]-2, 128);
    break;
  }
  i2c_guarded_send_buffer(u8g2_r, "oled_r");  // transfer internal memory to the display
  if (plogw->f_console_emu) {
    char buf[20], buf1[40];
    sprintf(buf, "\033[%d;%dH", int(LCD_CW_POSY / dp->hcol[0]) + 1, 40);
    plogw->ostream->print(buf);
    *buf1='\0';
    strncat(buf1,dp->lcdbuf,18);
    strcat(buf1,"\033[K"); // clear to the end of the line
      //    sprintf(buf1, "%-18s", dp->lcdbuf);
    //		plogw->ostream->print(dp->lcdbuf);
    plogw->ostream->print(buf1);
    
  }
}
// switch buffer memory to update right display
void select_left_display() {
  u8g2_l->setBufferPtr(dispbuf_l);
}
// switch buffer memory to update right display
void select_right_display() {
  u8g2_r->setBufferPtr(dispbuf_r);
}

// print string to the specified column in the display
void display_printStr(const char *s, byte ycol) {
  if (!display_is_main_loop()) return;

  if (ycol >= 10) {
    // select left
    select_left_display();
    clear_display_text_row(u8g2_l, ycol - 10, dp->hcol[1]);
    //    u8g2_l->drawStr(0, (ycol - 10) * dp->hcol[1], s);
    u8g2_l->drawUTF8(0, (ycol - 10) * dp->hcol[1], s);    
    if (plogw->f_console_emu) {
      char buf[20], buf1[40];
      sprintf(buf, "\033[%d;%dH", ycol - 10 + 1, 0);
      plogw->ostream->print(buf);
      sprintf(buf1, "%-17s", s);
      plogw->ostream->print(buf1);
      
      //		plogw->ostream->print(s);
    }
  } else {
    select_right_display();
    clear_display_text_row(u8g2_r, ycol, dp->hcol[0]);
    //    u8g2_r->drawStr(0, ycol * dp->hcol[0], s);
    u8g2_r->drawUTF8(0, ycol * dp->hcol[0], s);    
    if (plogw->f_console_emu) {
      char buf[20], buf1[40];
      sprintf(buf, "\033[%d;%dH", ycol + 1, 40);
      plogw->ostream->print(buf);
      sprintf(buf1, "%-17s", s);
      plogw->ostream->print(buf1);
      //		plogw->ostream->print(s);
    }
  }
}

// Display a newline-delimited string on the left display.
// The input may point to a string literal, so it must not be modified.
void upd_display_info_flash(const char *s) {
  if (defer_display(DISPLAY_REQ_INFO_FLASH, s)) return;
  select_left_display();
  u8g2_l->clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  int count = 0;
  const char *p = s;
  while (p != NULL && *p != '\0' && count < 6) {
    const char *eol = strchr(p, '\n');
    size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

    char line[96];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';

    char *right_text = strchr(line, '\x1f');
    if (right_text != NULL) {
      *right_text++ = '\0';
    }

    display_printStr(line, 10 + count);
    if (right_text != NULL && *right_text != '\0') {
      const int width = u8g2_l->getUTF8Width(right_text);
      int x = 128 - width;
      if (x < 0) x = 0;
      u8g2_l->drawUTF8(x, count * dp->hcol[1], right_text);
    }
    count++;

    if (eol == NULL) break;
    p = eol + 1;
  }

  info_disp.show_info = INFO_DISP_FLASH;
  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
  // set timer
  info_disp.timer = 5000;
}

// Show rig-decoded RTTY text on the left OLED.  The USB task may call this
// function; in that case defer the actual I2C/display work to the main loop.
// Row 0 is a small title and rows 1..5 are the latest five decoded lines.
void upd_display_rtty_decoder(const char *s) {
  if (defer_display(DISPLAY_REQ_RTTY_RX, s)) return;

  select_left_display();
  u8g2_l->clearBuffer();
  if (plogw->f_console_emu) clear_display_emu(1);

  display_printStr("RTTY RX", 10);

  int row = 0;
  const char *p = s;
  while (p != nullptr && *p != '\0' && row < 5) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[32];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    display_printStr(line, 11 + row);
    ++row;
    if (!eol) break;
    p = eol + 1;
  }
  while (row < 5) {
    display_printStr("", 11 + row);
    ++row;
  }

  info_disp.show_info = INFO_DISP_RTTY_RX;
  // Keep the decoder visible while characters continue to arrive, but return
  // to the normal bandmap after ten seconds of decoder silence.
  info_disp.timer = 10000;
  i2c_guarded_send_buffer(u8g2_l, "oled_l");
}

void upd_display_tm() {
  select_right_display();
  // yyyy/mm/dd-hh:mm:ss
  // 0123456789012345678
  char buf[40];
  struct radio *radio;
  int x, y, line_flag;
  line_flag = 0;
  radio = so2r.radio_selected();
  char clock_text[24];
  format_display_clock(clock_text, sizeof(clock_text), false);
  const bool utc_clock = (clock_display_mode == 1);
  if (plogw->show_smeter) {
    if (radio->smeter_stat >= 1) {
      // show peak  with underline
      if (radio->smeter_peak != SMETER_MINIMUM_DBM) {
        if (utc_clock) sprintf(buf, "%1d %sS%4d", so2r.focused_radio(), clock_text, radio->smeter_peak/SMETER_UNIT_DBM);
        else sprintf(buf, "%1d %-8s S%4d", so2r.focused_radio(), clock_text, radio->smeter_peak/SMETER_UNIT_DBM);
        y = dp->hcol[0] * 2 - 2;  // 2nd line
        x = 8 * 11;
        line_flag = 1;
      } else {
        if (utc_clock) sprintf(buf, "%1d %sS----", so2r.focused_radio(), clock_text);
        else sprintf(buf, "%1d %-8s S----", so2r.focused_radio(), clock_text);
      }
    } else {
      if (radio->smeter != SMETER_MINIMUM_DBM) {
        if (utc_clock) sprintf(buf, "%1d %sS%4d", so2r.focused_radio(), clock_text, radio->smeter/SMETER_UNIT_DBM);
        else sprintf(buf, "%1d %-8s S%4d", so2r.focused_radio(), clock_text, radio->smeter/SMETER_UNIT_DBM);
      } else {
        if (utc_clock) sprintf(buf, "%1d %sS----", so2r.focused_radio(), clock_text);
        else sprintf(buf, "%1d %-8s S----", so2r.focused_radio(), clock_text);
      }
    }
  } else {
    if (plogw->show_qso_interval) {
      if (utc_clock) sprintf(buf, "%1d %s %4d", so2r.focused_radio(), clock_text, plogw->qso_interval_timer/1000);
      else sprintf(buf, "%1d %-8s  %4d", so2r.focused_radio(), clock_text, plogw->qso_interval_timer/1000);
    } else {
      if (radio->qsodata_loaded) {
	sprintf(buf, "%1c %-8s  %4d", 'E', radio->tm_loaded + 9, radio->seqnr_loaded);
      } else {
	if (utc_clock) sprintf(buf, "%1d %s %4d", so2r.focused_radio(), clock_text, plogw->seqnr);
        else sprintf(buf, "%1d %-8s  %4d", so2r.focused_radio(), clock_text, plogw->seqnr);
      }
    }
  }
  // we still have some space to add some more information
  //display_printStr(plogw->tm + 3, 1);
  display_printStr(buf, 1);
  if (line_flag) u8g2_r->drawHLine(x, y, 8 * 5);
}

//char statstr_sub[8];
char statstr_sub[30];
void upd_display_stat() {
  select_right_display();

  struct radio *radio;
  radio = so2r.radio_selected();

  const char *statstr;
  statstr=statstr_sub;
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    sprintf(statstr_sub, "CW F%-d", radio->ptr_curr - 10 + 1);
    statstr = statstr_sub;
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // rtty msg
      sprintf(statstr_sub, "RT F%-d", radio->ptr_curr - 30 + 1);
      statstr = statstr_sub;
    } else {
      switch (radio->ptr_curr) {
        case 0:
        case 1:
          statstr = "Rcv";
          break;
        case 2:
        case 3:
          statstr = "RST";
          break;
        case 4:
        case 5:
          statstr = "Snt";
          break;
        case 6:
          statstr = "Rmks";
          break;
        case 7:
          statstr = "Sat.";
          break;
        case 8:
          statstr = "G.L.";
          break;
        case 9:
          statstr = "JCCG";
          break;
        case 20:
          statstr = "RIG";
          break;
        case 21:
          statstr = "Clst";
          break;
        case 22:
          statstr = "Mail";
          break;
        case 23:
          statstr = "CCmd";
          break;
        case 24:
          statstr = "Pow";
          break;
        case 25:
          statstr = "SSID";
          break;
        case 26:
          statstr = "Pass";
          break;
        case 27:
          statstr = "Spec";
          break;
        case 28:
          statstr = "zSvr";
          break;
        case 29:
          statstr = "Name";
          break;
        case 40:
          statstr = "Test";
          break;
        case 41:
          statstr = "Clst2";
          break;
        case 42:
          statstr = "C2Cm";
          break;
        case 43:
          statstr = "HN";
          break;
      }
    }
  }

  char c;
  if (radio->dupe == 1) {
    if (radio->multi < 0) {
      c = '!';  // invalid multi and dupe as well
    } else {
      c = 'D';
    }
  } else {
    if (radio->multi < 0) {
      c = 'M';  // invalid multi
    } else {
      c = ' ';
    }
  }

  sprintf(dp->lcdbuf, "%-4s %1s%1s %-3s%-3s%c%c",
          statstr,
          radio->keyer_mode == 1 ? ( f_cw_code_type == 1 ? "J": "K" ): (f_cw_code_type ==1? "j":" "),
	  //	  ((radio->f_romaji) == 1 && (radio->ptr_curr==6)) ? "R":" ",
	  ((radio->f_romaji) == 1 ) ? "R":" ",
          radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
          plogw->sat == 1 ? "Sat" : "",
	  radio->f_qsl==0 ? ' ':(radio->f_qsl==1 ? 'J': (radio->f_qsl==2 ? 'h': ' ')),
          c);
  // CQ/S&P Keyer Dupe info
  display_printStr(dp->lcdbuf, 2);
}
int upd_cursor_calc(int cursor, int wsize)
// cursor position 0 left most ..  wsize : window size (num of chars)
{
  // cursor position *cursor
  //plogw->ostream->print("cursor:");plogw->ostream->println(cursor);
  if (cursor >= wsize-1) {
    //plogw->ostream->print("* ");plogw->ostream->println((wsize-1));
    return (wsize-1) * 8;
    // show string from cursor - wsize +2    01234567  wsize==8
    //                                       JK1DVP/_  c==7
  } else {
    //plogw->ostream->print("- ");plogw->ostream->println((cursor));
    return (cursor * 8);
  }
}

void upd_display_put_lcdbuf(char *s, int cursor, int wsize, int lcdpos) {
  // lcdpos is where the window starts in lcdbuf
  // store string in *s so that cursor is located within the window (of size wsize)
  // sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
  int ofs;
  if (cursor >= wsize-1 ) {
    // cursor need to be at the last position
    ofs = cursor - (wsize - 1);
  } else {
    ofs = 0;
  }
  char *p0, *p1;
  p0 = dp->lcdbuf + lcdpos;
  p1 = s + ofs;
  for (int i = 0; i < wsize; i++) {
    if (*p1 == '\0') break;
    *p0++ = *p1++;
  }

}


void upd_cursor() {
  int x, y;
  char cursor_char=' ';
  select_right_display();

  struct radio *radio;
  radio = so2r.radio_selected();

  y = dp->hcol[0] * 4 - 1;
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    //x = (plogw->cw_msg[radio->ptr_curr - 10][1]) * 8;
    x = upd_cursor_calc(plogw->cw_msg[radio->ptr_curr - 10][1], 16);
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // rtty
      x = upd_cursor_calc(plogw->rtty_msg[radio->ptr_curr - 30][1], 16);
    } else {
      switch (radio->ptr_curr) {
        case 0:  // callsign
          //x = radio->callsign[1] * 8;
          x = upd_cursor_calc(radio->callsign[1], 9);
	  if (radio->callsign[1]< strlen(radio->callsign+2)) {
	    cursor_char=(radio->callsign+2)[(int)radio->callsign[1]];
	  }
          break;
        case 1:  // my exch (received exch)
          //          x = radio->recv_exch[1] * 8 + 70;
          x = upd_cursor_calc(radio->recv_exch[1], 7) + 8 * 9;
	  if (radio->recv_exch[1]< strlen(radio->recv_exch+2)) {	  
	    cursor_char=(radio->recv_exch+2)[(int)radio->recv_exch[1]];
	  }
          break;
        case 2:  // his rst (sent)
          x = (radio->sent_rst[1] + 4) * 8;
          break;
        case 3:  // my rst (recv)
          x = (radio->recv_rst[1] + 12) * 8;
          break;
        case 4:  // my callsign
          x = plogw->my_callsign[1] * 8;
          break;
        case 5:  // his callsign
          x = plogw->sent_exch[1] * 8 + 8 * 9;
          break;
        case 6:  // remarks
	  //          x = upd_cursor_calc(radio->remarks[1], 16);
	  //	  if (radio->remarks[1]< strlen(radio->remarks+2)) {
	  //	    cursor_char=(radio->remarks+2)[(int)radio->remarks[1]];
	  //	  }
	  x=radio->idx_cursor *8;
          break;
        case 7:  // satellite name
          x = plogw->sat_name[1] * 8;
          break;
        case 8:  // grid locator
          x = plogw->grid_locator[1] * 8;
          break;
        case 9:  // jcc
          x = plogw->jcc[1] * 8;
          break;
        case 20:  // rig_name
          x = radio->rig_name[1] * 8;
          break;
        case 21:  // cluster_name
          //x = plogw->cluster_name[1] * 8; break;
          x = upd_cursor_calc(plogw->cluster_name[1], 16);
          break;
        case 22:  // email_addr
          x = upd_cursor_calc(plogw->email_addr[1], 16);
          break;
        case 23:  // cluster_cmd
          x = upd_cursor_calc(plogw->cluster_cmd[1], 16);
          break;
        case 24:  // power_code
          x = upd_cursor_calc(plogw->power_code[1], 16);
          break;
        case 25:  // wifi_ssid
          x = upd_cursor_calc(plogw->wifi_ssid[1], 16);
          break;
        case 26:  // wifi_passwd
          x = upd_cursor_calc(plogw->wifi_passwd[1], 16);
          break;
        case 27:  // rig_spec
          x = upd_cursor_calc(radio->rig_spec_str[1], 16);
          break;
        case 28:  // zserver_name
          x = upd_cursor_calc(plogw->zserver_name[1], 16);
          break;
        case 29:  // my_name
          x = upd_cursor_calc(plogw->my_name[1], 16);
          break;
        case 40:  // contest_name
          x = upd_cursor_calc(plogw->contest_name[1], 16);
          break;
        case 41:
          x = upd_cursor_calc(plogw->cluster2_name[1], 16);
          break;
        case 42:
          x = upd_cursor_calc(plogw->cluster2_cmd[1], 16);
          break;
        case 43:  // hostname / mDNS name
          x = upd_cursor_calc(plogw->hostname[1], 16);
          break;
        default:
          return;
      }
    }
  }
  u8g2_r->drawHLine(x, y, 8);
  if (plogw->f_console_emu) {
    char buf[40];
    sprintf(buf,"\033[%d;%dH\033[4m%c\033[0m",y/dp->hcol[0]+1,x/8+40,cursor_char);
    plogw->ostream->print(buf);
  }
}

void upd_display_freq(unsigned int freq, char *opmode, int col) {
  //
  unsigned int tmp1, tmp2;
  struct radio *radio;
  radio = so2r.radio_selected();
  //  tmp2 = (freq % 1000) / 10;   // 100Hz, 10Hz
  //  tmp1 = freq / 1000;  // KHz 
  //  tmp1 = tmp1 % 1000;  // kHz
  tmp2= (freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT); // below kHz
  tmp1 = freq / (1000/FREQ_UNIT); 
  tmp1 = tmp1 % 1000 ;  // kHz

  sprintf(dp->lcdbuf, "%3d.%03d.%02d%c%-6s", freq / (1000000/FREQ_UNIT), tmp1, tmp2, radio->transverter_in_use ? '*' : ' ', opmode);
  display_printStr(dp->lcdbuf, col);
}

void upd_display() {
  if (defer_display(DISPLAY_REQ_UPDATE)) return;
  // A synchronous display request supersedes any delayed key-entry update.
  s_dupe_display_state = DUPE_DISPLAY_IDLE;
  upd_display_render(true);
}

static void upd_display_render(bool flush_to_oled) {
  struct radio *radio;
  radio = so2r.radio_selected();
  if (verbose &4) {
    plogw->ostream->println("upd_display()");
  }
  // frequency and mode in the top line
  select_right_display();
  if (plogw->f_console_emu) {
    clear_display_emu(0);
  }
  
  //strcpy(radio->opmode, opmode);
  if (verbose & 1) {
    plogw->ostream->print("Freq:");
    plogw->ostream->print(radio->freq);
    plogw->ostream->print(" Mode:");
    plogw->ostream->println(radio->opmode);
  }

  if (radio->qsodata_loaded) {
    // A loaded QSO already has a recorded mode, independent of the current
    // rig's live CAT/manual-mode initialisation state.
    upd_display_freq(radio->freq_loaded, radio->opmode_loaded, 0);
  } else {
    char mode_display[sizeof(radio->opmode)];
    if (radio->mode_initialized && radio->opmode[0] != '\0') {
      strncpy(mode_display, radio->opmode, sizeof(mode_display) - 1);
      mode_display[sizeof(mode_display) - 1] = '\0';
    } else {
      strcpy(mode_display, "----");
    }
    upd_display_freq(radio->freq, mode_display, 0);
  }
  // second line: time
  upd_display_tm();
  // fill
  strcpy(dp->lcdbuf, "                ");

  // show log window and number  third line
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    //sprintf(dp->lcdbuf, "%-s", radio->cw_msg[radio->ptr_curr - 10] + 2);
    upd_display_put_lcdbuf(plogw->cw_msg[radio->ptr_curr - 10] + 2, plogw->cw_msg[radio->ptr_curr - 10][1], 16, 0);
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // cw msg
      //sprintf(dp->lcdbuf, "%-s", radio->cw_msg[radio->ptr_curr - 10] + 2);
      upd_display_put_lcdbuf(plogw->rtty_msg[radio->ptr_curr - 30] + 2, plogw->rtty_msg[radio->ptr_curr - 30][1], 16, 0);
    } else {
      //      int pre_s = 0, pre_l = 0, caret = 0;
      char composed[256];
      int ps_b, pl_b, caret_b;
      int ps_c, pl_c, caret_c;
      int total_cols;
      int colL,caret_local;
      
      switch (radio->ptr_curr) {
        case 0:  // callsign
        case 1:  // my exch
          //          sprintf(dp->lcdbuf, "%-8s %-7s", radio->callsign + 2, radio->recv_exch + 2);
          // better to allocate space to show 9 characters (??????/?_=9 chrs)
          upd_display_put_lcdbuf(radio->callsign + 2, radio->callsign[1], 9, 0);
          upd_display_put_lcdbuf(radio->recv_exch + 2, radio->recv_exch[1], 7, 8 + 1);
          break;
        case 2:  // sent rst
        case 3:  // rcv  rst
          strcpy(dp->lcdbuf, "Snt     Rcv    ");
          upd_display_put_lcdbuf(radio->sent_rst + 2, radio->sent_rst[1], 4, 4);
          upd_display_put_lcdbuf(radio->recv_rst + 2, radio->recv_rst[1], 4, 12);

          break;
        case 4:  // edit my_callsign
        case 5:  // edit sent_exch
          //          sprintf(dp->lcdbuf, "%-8s %-7s", plogw->my_callsign + 2, plogw->sent_exch + 2);
          upd_display_put_lcdbuf(plogw->my_callsign + 2, plogw->my_callsign[1], 9, 0);
          upd_display_put_lcdbuf(plogw->sent_exch + 2, plogw->sent_exch[1], 7, 8 + 1);
          break;
        case 6:  // editing remarks
	  //	  compose_line_with_preedit((const char*)radio->remarks, &g_pre,
	  //				    dp->lcdbuf, sizeof(dp->lcdbuf),
	  //				    &pre_s, &pre_l, &caret);
	  //          upd_display_put_lcdbuf(radio->remarks + 2, radio->remarks[1], 16, 0);
	  compose_line_with_preedit_cjk((const char*)radio->remarks, &g_pre,
	  			    composed,sizeof(composed),
				    &ps_b,&pl_b,&caret_b,
				    &ps_c,&pl_c,&caret_c,
				    &total_cols);

	  //	  window_line_by_columns_caret_cjk(composed, total_cols, caret_c,
	  //					   15, 13,
	  //					   dp->lcdbuf, sizeof(dp->lcdbuf),
	  //					   &colL, &caret_local);

	  window_from_caret_simple_cjk(
				       composed, total_cols, caret_c,
				       15, 12,
				       dp->lcdbuf,sizeof(dp->lcdbuf),
				       &colL, &caret_local       );
	  radio->idx_cursor=caret_local;	  
          break;
        case 7:  // editing satellite name
          sprintf(dp->lcdbuf, "%-s", plogw->sat_name + 2);
          break;
        case 8:  // editing grid locator
          sprintf(dp->lcdbuf, "%-s", plogw->grid_locator + 2);
          break;
        case 9:  // jcc
          sprintf(dp->lcdbuf, "%-s", plogw->jcc + 2);
          break;
        case 20:  // rig_name
          sprintf(dp->lcdbuf, "%-s", radio->rig_name + 2);
          break;
        case 21:  // cluster_name
          //sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
          upd_display_put_lcdbuf(plogw->cluster_name + 2, plogw->cluster_name[1], 16, 0);
          break;
        case 22:  // email_addr
          upd_display_put_lcdbuf(plogw->email_addr + 2, plogw->email_addr[1], 16, 0);
          break;
        case 23:  // cluster_cmd
          //sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
          upd_display_put_lcdbuf(plogw->cluster_cmd + 2, plogw->cluster_cmd[1], 16, 0);
          break;
        case 24:  // power code
          upd_display_put_lcdbuf(plogw->power_code + 2, plogw->power_code[1], 16, 0);
          break;
        case 25:  // wifi_ssid
          upd_display_put_lcdbuf(plogw->wifi_ssid + 2, plogw->wifi_ssid[1], 16, 0);
          break;
        case 26:  // wifi_passwd
          upd_display_put_lcdbuf(plogw->wifi_passwd + 2, plogw->wifi_passwd[1], 16, 0);
          break;
        case 27:  // rig_spec_str
          upd_display_put_lcdbuf(radio->rig_spec_str + 2, radio->rig_spec_str[1], 16, 0);
          break;
        case 28:  // zserver_name
          upd_display_put_lcdbuf(plogw->zserver_name + 2, plogw->zserver_name[1], 16, 0);
          break;
        case 29:  // my_name
          upd_display_put_lcdbuf(plogw->my_name + 2, plogw->my_name[1], 16, 0);
          break;
        case 40:  // Contest Name
          upd_display_put_lcdbuf(plogw->contest_name + 2, plogw->contest_name[1], 16, 0);
          break;
        case 41:
          upd_display_put_lcdbuf(plogw->cluster2_name + 2, plogw->cluster2_name[1], 16, 0);
          break;
        case 42:
          upd_display_put_lcdbuf(plogw->cluster2_cmd + 2, plogw->cluster2_cmd[1], 16, 0);
          break;
        case 43:  // hostname / mDNS name
          upd_display_put_lcdbuf(plogw->hostname + 2, plogw->hostname[1], 16, 0);
          break;
      }
    }
  }
  display_printStr(dp->lcdbuf, 3);

  // draw underline to the cursor
  upd_cursor();

  upd_display_stat();

  if (flush_to_oled) {
    i2c_guarded_send_buffer(u8g2_r, "oled_r");  // transfer internal memory to the display
  }
  // i2c_guarded_send_buffer(u8g2_l, "oled_l");          // transfer internal memory to the display
}


void right_display_sendBuffer()
{
  if (!display_is_main_loop()) return;
    i2c_guarded_send_buffer(u8g2_r, "oled_r");  // transfer internal memory to the display

}
void left_display_sendBuffer()
{
  if (!display_is_main_loop()) return;
    i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display

}

void left_display_clearBuffer()
{
  u8g2_l->clearBuffer();  // clear the internal memory
}
void right_display_clearBuffer()
{
  u8g2_r->clearBuffer();  // clear the internal memory
}


void init_display() {

  if (dispbuf_r) {
    free(dispbuf_r);
    dispbuf_r = nullptr;
  }
  if (dispbuf_l) {
    free(dispbuf_l);
    dispbuf_l = nullptr;
  }
 
  dp = &disp;

  switch(display_type) {
  case 0: // 1.3" display
    u8g2_r=&u8g2_r_1;
    u8g2_l=&u8g2_l_1;
    display_flip=1; // 1.3" display
    display_swap=1; // 1.3" display    
    break;
  case 1: // 2.4" display
    u8g2_r=&u8g2_r_2;
    u8g2_l=&u8g2_l_2;
    display_flip=0; // 1.3" display
    display_swap=0; // 1.3" display        
    break;
  case 2: // 1.3" but flip
    u8g2_r=&u8g2_r_1;
    u8g2_l=&u8g2_l_1;
    display_flip=0; // 1.3" display
    display_swap=1; // 1.3" display
    break;
    
  }
  
  //  u8g2_r=&u8g2_r_2;
  //  u8g2_l=&u8g2_l_2;
  //  display_flip=0; // 2.4" display

  u8g2_r->begin();

  if (display_swap) {
    u8g2_r->setI2CAddress(0x3c * 2); // flip
  } else {
    u8g2_r->setI2CAddress(0x3d * 2); //non flip
  }

  plogw->ostream->println("getBufferSize=");
  plogw->ostream->print(u8g2_r->getBufferSize());
  dispbuf_r = (uint8_t *)malloc(u8g2_r->getBufferSize());


  select_right_display();

  u8g2_r->initDisplay();
  u8g2_r->enableUTF8Print(); // for japanese printing       
  if (display_flip) {  
    u8g2_r->setFlipMode(1);
  }
  u8g2_r->clearDisplay();
  u8g2_r->setPowerSave(0);
  u8g2_r->clearBuffer();  // clear the internal memory
  //  u8g2_r->setFont(u8g2_font_8x13_mf);
  u8g2_r->setFont(u8g2_font_unifont_t_japanese1);
  // Configure the reference height and top-position mode before measuring the
  // row pitch.  Measuring first can undercount descenders by one scan line for
  // characters such as g, j, p, q and y.
  u8g2_r->setFontRefHeightExtendedText();
  u8g2_r->setFontPosTop();
  dp->hcol[0] = u8g2_r->getFontAscent() - u8g2_r->getFontDescent();
  plogw->ostream->print("u8g2_r hcol =");
  plogw->ostream->println(dp->hcol[0]);

  u8g2_l->begin();

  if (display_swap) {
    u8g2_l->setI2CAddress(0x3d * 2);   // flip    
  } else {
    u8g2_l->setI2CAddress(0x3c * 2); // non flip
  }

  dispbuf_l = (uint8_t *)malloc(u8g2_l->getBufferSize());

  select_left_display();
  u8g2_l->initDisplay();
  u8g2_l->enableUTF8Print(); // for japanese printing     
  if (display_flip) {  
    u8g2_l->setFlipMode(1);
  }
  u8g2_l->clearDisplay();
  u8g2_l->setPowerSave(0);
  u8g2_l->clearBuffer();  // clear the internal memory
  //  u8g2_l->setFont(u8g2_font_8x13_tf);
  //  u8g2_l->setFont(u8g2_font_t0_11_mf); // normally this is used
  u8g2_l->setFont(u8g2_font_b12_t_japanese1); // experimental japanese font  
  //  u8g2_l->setFont(u8x8_font_5x7_f); // narrowr
  u8g2_l->setFontRefHeightExtendedText();
  u8g2_l->setFontPosTop();

  dp->hcol[1] = u8g2_l->getFontAscent() - u8g2_l->getFontDescent();
  plogw->ostream->print("u8g2_l hcol =");
  plogw->ostream->println(dp->hcol[1]);
  //  w = u8g2_r->getStrWidth(lcdbuf);
  dp->wcol = 128;  // the whole line

  char build_line[40];
  snprintf(build_line, sizeof(build_line), "%s HW%s",
           __DATE__, DVP_STRINGIFY(JK1DVPLOG_HWVER));

  u8g2_l->drawStr(0, 0, "DVPlogger");
  u8g2_l->drawStr(0, 13, JK1DVPLOG_VERSION_STRING);
  u8g2_l->drawStr(0, 26, build_line);
  u8g2_l->drawStr(0, 39, __TIME__);
  u8g2_l->drawUTF8(0, 52, "初期化中...");

  console->printf("[BUILD] DVPlogger %s HW%s built %s %s\n",
                  JK1DVPLOG_VERSION_STRING,
                  DVP_STRINGIFY(JK1DVPLOG_HWVER),
                  __DATE__, __TIME__);
  console->println("初期化中...");

  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
  console->println("disp initialized.");
}


void upd_disp_rig_info() {
  char buf[80];
  *dp->lcdbuf = '\0';
  switch (so2r.radio_mode) {
  case SO2R::RADIO_MODE_SO1R:  // SO1R
      strcat(dp->lcdbuf, "SO1R ");
      break;
  case SO2R::RADIO_MODE_SAT:  // SAT
      strcat(dp->lcdbuf, "SAT ");
      break;
  case SO2R::RADIO_MODE_SO2R:  // SO2R
      strcat(dp->lcdbuf, "SO2R ");
      break;
  }

  //  if ((WiFi.status() == WL_CONNECTED)) {
  if (wifi_status == 1) {
    localip_to_string(buf);
    strcat(dp->lcdbuf, buf);
    strcat(dp->lcdbuf, "\n");
  } else {
    strcat(dp->lcdbuf, "\n");
  }
  for (int i = 0; i < N_RADIO; i++) {
    sprintf(buf, "Rig%d:%s %c", radio_list[i].rig_idx, radio_list[i].rig_spec->name, radio_list[i].enabled ? '*' : ' ');
    strcat(dp->lcdbuf, buf);
    // add SO1R/SAT/SO2R specific infos
    switch (so2r.radio_mode) {
    case SO2R::RADIO_MODE_SO1R:  // SO1R
    case SO2R::RADIO_MODE_SAT:  // SAT
        sprintf(buf, " %c%c %c\n", ' ', ' ', radio_list[i].f_tone_keying ? 't' : ' ');
        break;
    case SO2R::RADIO_MODE_SO2R:  // SO2R
        sprintf(buf, " %c%c %c\n", so2r.tx() == i ? 'T' : ' ', so2r.rx() == i ? 'R' : ' ', radio_list[i].f_tone_keying ? 't' : ' ');
        break;
    }
    strcat(dp->lcdbuf, buf);
  }
  // append callhistfn settingsfn
  strcat(dp->lcdbuf, "callhist:");
  strcat(dp->lcdbuf, callhistfn);
  strcat(dp->lcdbuf, "\n");
  strcat(dp->lcdbuf, "settings:");
  strcat(dp->lcdbuf, settingsfn);
  strcat(dp->lcdbuf, "\n");
  upd_display_info_flash(dp->lcdbuf);
}
/////////////////////////////////////

void init_info_display() {
  info_disp.timer = 0;
  info_disp.show_info = 0;
  info_disp.show_info_prev = 0;
  info_disp.pos = 0;
  info_disp.multi_ofs = 0;
}
void upd_display_info_signal()
{
  // display information related with signal received
  // antenna selected
  // and rotator azimuth ( if enabled )
  struct radio *radio;

  radio = so2r.radio_selected();
  char buf[160],buf1[15];
  *dp->lcdbuf = '\0';
  sprintf(buf, "%6s F:%d kHz\nRelay :%d ", radio->rig_spec->name , radio->freq / 1000, plogw->relay[0] + plogw->relay[1] * 2);
  strcat(dp->lcdbuf, buf);
  if (plogw->f_rotator_enable) {
    sprintf(buf, "Az %03d\n", plogw-> rotator_az);
    strcat(dp->lcdbuf, buf);
  } else {
    strcat(dp->lcdbuf, "\n");
  }
  for (int i = 0; i < 4; i++) {
    if (radio->smeter_record[i] != 0 ) {
	  dtostrf(radio->smeter_record[i]/(SMETER_UNIT_DBM*1.0),4,0,buf1);
	  if (radio->smeter_azimuth[i] != -1) {
        sprintf(buf, "ANT %1d AZ %3d S %s\n", i, radio->smeter_azimuth[i],buf1 );
      } else {
        sprintf(buf, "ANT %1d AZ --- S %s\n", i, buf1);
      }
      strcat(dp->lcdbuf, buf);
    }
  }
  upd_display_info_flash(dp->lcdbuf);

  // print to serial
  plogw->ostream->print("SIG: ");
  *dp->lcdbuf = '\0';
  sprintf(buf, "%-17s %6s F:%d kHz Ant:%d ", plogw->tm, radio->rig_spec->name , radio->freq / 1000, plogw->relay[0] + plogw->relay[1] * 2);
  strcat(dp->lcdbuf, buf);
  if (plogw->f_rotator_enable) {
    sprintf(buf, "Az %03d, ", plogw-> rotator_az);
    strcat(dp->lcdbuf, buf);
  } else {
    strcat(dp->lcdbuf, ", ");
  }
  for (int i = 0; i < 4; i++) {
    if (radio->smeter_record[i] != 0 ) {
	  dtostrf(radio->smeter_record[i]/(SMETER_UNIT_DBM*1.0),6,1,buf1);
      if (radio->smeter_azimuth[i] != -1) {
        sprintf(buf, "ANT %1d AZ %3d S %s , ", i, radio->smeter_azimuth[i], buf1);
      } else {
        sprintf(buf, "ANT %1d AZ --- S %s , ", i, buf1);
      }
      strcat(dp->lcdbuf, buf);
    }
  }
  if (strlen(radio->remarks+2))
    strcat(dp->lcdbuf,radio->remarks+2);
  plogw->ostream->println(dp->lcdbuf);
}


static int count_unworked_bandmap_entries(int band_index) {
  if (band_index < 0 || band_index >= N_BAND) return 0;

  int n_to_work = 0;
  for (int i = 0; i < bandmap[band_index].nentry; i++) {
    struct bandmap_entry *p = bandmap[band_index].entry + i;
    if (*p->station == '\0') continue;
    if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) continue;
    n_to_work++;
  }
  return n_to_work;
}

void upd_display_info_contest_band_nearby(struct radio *radio) {
  select_left_display();
  u8g2_l->clearBuffer();
  if (plogw->f_console_emu) clear_display_emu(1);

  // Keep the two contest-summary lines from the legacy Ctrl-X display.
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%s D:%s",
           plogw->contest_name + 2,
           plogw->mask == 0xff ? "OK C/P" : "NG C/P");
  display_printStr(dp->lcdbuf, 10);

  int total_qso = 0;
  int total_multi = 0;
  for (int band = 0; band < N_BAND; band++) {
    total_qso += score.worked[0][band] + score.worked[1][band];
    total_multi += score.nmulti[band];
  }
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "QSO:%d MUL:%d",
           total_qso, total_multi);
  display_printStr(dp->lcdbuf, 11);

  // Show one band before through two bands after the current operating band.
  // Shift the four-row window at either end of the band table.
  int current_band = radio != NULL ? radio->bandid - 1 : 0;
  if (current_band < 0) current_band = 0;
  if (current_band >= N_BAND) current_band = N_BAND - 1;

  int first_band = max(0, current_band - 1);
  if (first_band + 4 > N_BAND) first_band = max(0, N_BAND - 4);

  for (int row = 0; row < 4; row++) {
    int band = first_band + row;
    if (band >= N_BAND) break;

    int nq = score.worked[0][band] + score.worked[1][band];
    int nm = score.nmulti[band];
    int nw = count_unworked_bandmap_entries(band);

    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%c%s %3dQ>%2d %2dM",
             band == current_band ? '>' : ' ', bandid_str[band], nq, nw, nm);
    display_printStr(dp->lcdbuf, 12 + row);
  }

  i2c_guarded_send_buffer(u8g2_l, "oled_l");
  info_disp.show_info = INFO_DISP_CONTEST_SETTINGS;
  info_disp.timer = 5000;
}



void upd_display_info_contest_settings(struct radio *radio) {
  if (defer_display(DISPLAY_REQ_CONTEST_SETTINGS, nullptr, radio ? radio->rig_idx : -1)) return;

  //  struct radio *radio;

  //  radio = so2r.radio_selected();

  // show information about contest settings  dupe check, name of the contest and multi worked
  if (verbose & 1) {
    plogw->ostream->println("display info contest settings()  ");
  }
  select_left_display();
  u8g2_l->clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // show contest name and dupe category
  sprintf(dp->lcdbuf, "%s D:%s", plogw->contest_name+2, plogw->mask == 0xff ? "OK C/P" : "NG C/P");
  display_printStr(dp->lcdbuf, 10 + 0);

  // show worked stations and multi
  int nq, nm, np;  // total number of q and m and p point
  nq = 0;
  nm = 0;
  np = 0;
  for (int i = 0; i < N_BAND; i++) {
    nq += score.worked[0][i];
    nq += score.worked[1][i];
    np += score.worked[0][i] * plogw->cw_pts;
    np += score.worked[1][i];
    if (verbose & 1) {
      plogw->ostream->print("Band:");
      plogw->ostream->print(i);
      plogw->ostream->print(" QC:");
      plogw->ostream->print(score.worked[0][i]);
      plogw->ostream->print(" QP:");
      plogw->ostream->print(score.worked[1][i]);
      plogw->ostream->print(" M:");
      plogw->ostream->println(score.nmulti[i]);
    }
    nm += score.nmulti[i];
  }
  sprintf(dp->lcdbuf, "QSO:%d MUL:%d", nq, nm);
  display_printStr(dp->lcdbuf, 10 + 1);

  const char *s;
  if (multi_list.multi[radio->bandid-1] != NULL) {
    if ((radio->multi >= 0) && (multi_list.n_multi[radio->bandid-1] > radio->multi)) {
      s = multi_list.multi[radio->bandid-1]->name[radio->multi];
    } else {
      s = "Not valid";
    }
  } else {
    s = "No CHECK";
  }


  sprintf(dp->lcdbuf, "mult:%s", s);
  display_printStr(dp->lcdbuf, 10 + 2);


  // show multi list below
  if (radio->bandid >= 1) {
  if ( (multi_list.multi[radio->bandid-1] != NULL) ) {

    sprintf(dp->lcdbuf, "Multi in %s MHz", band_str[radio->bandid - 1]);
    display_printStr(dp->lcdbuf, 13);

    char buf1[10];
    int count;
    int len;
    count = 0;
    int countrow;
    countrow = 0;
    *dp->lcdbuf = '\0';


    for (int i = info_disp.multi_ofs; i < multi_list.n_multi[radio->bandid-1]; i++) {
      if (i >= multi_list.n_multi[radio->bandid-1]) break;
      sprintf(buf1, "%c%s ", multi_worked_get(&multi_list, radio->bandid - 1, i) ? '*' : ' ', multi_list.multi[radio->bandid-1]->mul[i]);
      len = strlen(buf1);
      if (count + len > 16) {  // use next row
        display_printStr(dp->lcdbuf, 14 + countrow);
        *dp->lcdbuf = '\0';
        count = 0;
	// check row
	if (countrow>2) { // no displayable area available
	  break;
	}
        countrow++;
      }
      strcat(dp->lcdbuf, buf1);
      count += len;
    }

    display_printStr(dp->lcdbuf, 14 + countrow);
  }

  }

  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
  // set timer
  info_disp.show_info = INFO_DISP_CONTEST_SETTINGS;   
  info_disp.timer = 5000;
}


static void begin_multi_info_display() {
  select_left_display();
  u8g2_l->clearBuffer();
  if (plogw->f_console_emu) clear_display_emu(1);
}

static void finish_multi_info_display() {
  i2c_guarded_send_buffer(u8g2_l, "oled_l");
  info_disp.show_info = INFO_DISP_CONTEST_SETTINGS;
  info_disp.timer = 5000;
}

static int find_multi_index_on_band(int band_index, const char *mul) {
  if (band_index < 0 || band_index >= N_BAND || mul == NULL || *mul == '\0') return -1;
  if (multi_list.multi[band_index] == NULL) return -1;

  for (int i = 0; i < multi_list.n_multi[band_index]; i++) {
    if (strcmp(multi_list.multi[band_index]->mul[i], mul) == 0) return i;
  }
  return -1;
}

static void utf8_remove_last_codepoint(char *s) {
  size_t len = strlen(s);
  if (len == 0) return;
  size_t p = len - 1;
  while (p > 0 && (((unsigned char)s[p] & 0xC0) == 0x80)) --p;
  s[p] = '\0';
}

static void utf8_truncate_to_pixel_width(char *s, int max_width) {
  if (max_width < 0) max_width = 0;
  while (*s != '\0' && u8g2_l->getUTF8Width(s) > max_width) {
    utf8_remove_last_codepoint(s);
  }
}

static void print_wrapped_multi_item(int *row, int *column, const char *item) {
  const int display_columns = 21;
  int len = strlen(item);

  if (*column > 0 && *column + len > display_columns) {
    display_printStr(dp->lcdbuf, 10 + *row);
    (*row)++;
    *column = 0;
    *dp->lcdbuf = '\0';
  }
  if (*row >= 6) return;

  // A single unusually long multiplier is clipped rather than overflowing lcdbuf.
  int available = display_columns - *column;
  strncat(dp->lcdbuf, item, available);
  *column += min(len, available);
}

void upd_display_info_multi_nearby(struct radio *radio) {
  begin_multi_info_display();

  int band_index = radio->bandid - 1;
  if (band_index < 0 || band_index >= N_BAND || multi_list.multi[band_index] == NULL) {
    display_printStr("mult:No CHECK", 10);
    finish_multi_info_display();
    return;
  }

  // Follow a valid multiplier in EXCH.  If EXCH does not currently contain
  // a valid multiplier, retain the last displayed index so the multiplier
  // pages remain useful while editing an incomplete exchange.
  int selected = radio->multi;
  if (selected >= 0 && selected < multi_list.n_multi[band_index]) {
    info_disp.multi_ofs = selected;
  } else {
    selected = info_disp.multi_ofs;
    if (selected < 0) selected = 0;
    if (selected >= multi_list.n_multi[band_index])
      selected = multi_list.n_multi[band_index] - 1;
    info_disp.multi_ofs = selected;
  }

  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "mult: %s %s",
           multi_list.multi[band_index]->mul[selected],
           multi_list.multi[band_index]->name[selected]);
  utf8_truncate_to_pixel_width(dp->lcdbuf, dp->wcol);
  display_printStr(dp->lcdbuf, 10);

  // Start a few entries before the selected multiplier.  The selected entry
  // is therefore normally visible near the beginning rather than at an edge.
  int start = max(0, selected - 3);
  int row = 1;
  int column = 0;
  *dp->lcdbuf = '\0';

  for (int i = start; i < multi_list.n_multi[band_index] && row < 6; i++) {
    char item[20];
    snprintf(item, sizeof(item), "%c%s ",
             multi_worked_get(&multi_list, band_index, i) ? '*' : ' ',
             multi_list.multi[band_index]->mul[i]);
    print_wrapped_multi_item(&row, &column, item);
  }
  if (row < 6 && column > 0) display_printStr(dp->lcdbuf, 10 + row);

  finish_multi_info_display();
}

void upd_display_info_multi_bands(struct radio *radio) {
  begin_multi_info_display();

  int current_band = radio->bandid - 1;
  if (current_band < 0 || current_band >= N_BAND ||
      multi_list.multi[current_band] == NULL ||
      multi_list.n_multi[current_band] <= 0) {
    display_printStr("mult:No CHECK", 10);
    finish_multi_info_display();
    return;
  }

  // Follow a valid multiplier in EXCH; otherwise keep the previously shown
  // index.  Ctrl-Shift-T/Y can move this retained index manually.
  int selected = radio->multi;
  if (selected >= 0 && selected < multi_list.n_multi[current_band]) {
    info_disp.multi_ofs = selected;
  } else {
    selected = info_disp.multi_ofs;
    if (selected < 0) selected = 0;
    if (selected >= multi_list.n_multi[current_band])
      selected = multi_list.n_multi[current_band] - 1;
    info_disp.multi_ofs = selected;
  }
  const char *selected_mul = multi_list.multi[current_band]->mul[selected];
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "mult: %s %s", selected_mul,
           multi_list.multi[current_band]->name[selected]);
  utf8_truncate_to_pixel_width(dp->lcdbuf, dp->wcol);
  display_printStr(dp->lcdbuf, 10);

  // One-character band labels, from 1.8 through 1200 MHz.
  // Keep the header and the worked-status columns right-aligned.
  const char *band_header = "137ABC524G";
  int header_width = u8g2_l->getUTF8Width(band_header);
  int header_x = max(0, dp->wcol - header_width);

  display_printStr("", 11);  // clear the second line first
  u8g2_l->drawUTF8(header_x, dp->hcol[1], band_header);

  // Underline the current operating band using the same hline convention
  // used elsewhere in DVPlogger.  Measure the actual font width so that the
  // line remains aligned even if the display font changes.
  if (current_band >= 0 && current_band < 10) {
    char prefix[11];
    char current_char[2];
    memcpy(prefix, band_header, current_band);
    prefix[current_band] = '\0';
    current_char[0] = band_header[current_band];
    current_char[1] = '\0';
    int underline_x = header_x + u8g2_l->getUTF8Width(prefix);
    int underline_width = u8g2_l->getUTF8Width(current_char);
    int underline_y = 2 * dp->hcol[1] - 1;
    u8g2_l->drawHLine(underline_x, underline_y, underline_width);
  }

  if (plogw->f_console_emu) {
    char emu_line[32];
    int pad = max(0, 21 - 10);
    snprintf(emu_line, sizeof(emu_line), "%*s%s", pad, "", band_header);
    char esc[48];
    snprintf(esc, sizeof(esc), "\033[2;1H%-21s", emu_line);
    plogw->ostream->print(esc);
    if (current_band < 10) {
      snprintf(esc, sizeof(esc), "\033[2;%dH\033[4m%c\033[0m",
               pad + current_band + 1, band_header[current_band]);
      plogw->ostream->print(esc);
    }
  }

  // Show four neighbouring multipliers. Normally this is one before the
  // selected multiplier, the selected one, and two after it. Shift the
  // window at either end so that four entries are shown whenever possible.
  int n_multi = multi_list.n_multi[current_band];
  int first = max(0, selected - 1);
  if (first + 4 > n_multi) first = max(0, n_multi - 4);

  for (int row = 0; row < 4 && first + row < n_multi; row++) {
    int multi_index = first + row;
    const char *mul = multi_list.multi[current_band]->mul[multi_index];
    char status[11];

    for (int band = 0; band < 10; band++) {
      int idx = find_multi_index_on_band(band, mul);
      if (multi_list.multi[band] == NULL || idx < 0) {
        status[band] = '-';
      } else if (multi_worked_get(&multi_list, band, idx)) {
        status[band] = '*';
      } else {
        status[band] = '_';
      }
    }
    status[10] = '\0';

    // Put the multiplier and as much of its name as will fit at the left.
    // The right-aligned ten-band status always starts at header_x.
    const char *multi_name = multi_list.multi[current_band]->name[multi_index];
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%c%s %s",
             multi_index == selected ? '>' : ' ', mul, multi_name);

    // Trim the left field until it fits before the status columns.  This uses
    // the actual font width rather than assuming a fixed number of characters.
    int left_max_width = max(0, header_x - 2);
    while (*dp->lcdbuf != '\0' && u8g2_l->getUTF8Width(dp->lcdbuf) > left_max_width) {
      dp->lcdbuf[strlen(dp->lcdbuf) - 1] = '\0';
    }
    display_printStr(dp->lcdbuf, 12 + row);
    u8g2_l->drawUTF8(header_x, (2 + row) * dp->hcol[1], status);

    if (plogw->f_console_emu) {
      char emu_line[40];
      int left_len = min((int)strlen(dp->lcdbuf), 10);
      int spaces = max(1, 21 - left_len - 10);
      snprintf(emu_line, sizeof(emu_line), "%.10s%*s%.10s",
               dp->lcdbuf, spaces, "", status);
      char esc[64];
      snprintf(esc, sizeof(esc), "\033[%d;1H%-21s", 3 + row, emu_line);
      plogw->ostream->print(esc);
    }
  }

  finish_multi_info_display();
}


void show_summary(Stream *out) {
  if (!out) out = console;
  struct radio *radio;

  radio = so2r.radio_selected();

  // show information about contest settings  dupe check, name of the contest and multi worked
  if (!plogw->f_console_emu) out->println("display info to work bandmap()  ");

  // show worked stations and multi
  unsigned int bandmask = 0b1111111111111;
  int nidx = 0;
  int nq, nm, np, nw;  // total number of q and m and p point

  struct bandmap_entry *p;
  int idx;

  nidx = 0;
  int cnt;

  cnt = 0;

  for (int j = 0; j < N_BAND; j++) {
    if (((1 << j) & bandmask) == 0) {
      // not show this band
      continue;
    }
    nq = 0;
    nm = 0;
    np = 0;
    nw = 0;
    cnt++;
    //    if (cnt <= plogw->bandmap_summary_idx) continue;


    nq = score.worked[0][j];
    nq += score.worked[1][j];
    nm = score.nmulti[j];

    // procedure to check number of non-worked stations for the band
    for (int i = 0; i < bandmap[j].nentry; i++) {
      p = bandmap[j].entry + i;
      if (*p->station == '\0') continue;
      if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) continue;
      nw++;
    }

    // nw: number of to be worked station for the band

    sprintf(dp->lcdbuf, "%s %3dQ>%2d %2dM", bandid_str[j], nq, nw, nm);
    out->println(dp->lcdbuf);

    // next row
    nidx++;
    //    if (nidx >= 6) break;
  }
}  


// show non worked bandmap stations and worked QSOs for all non-masked bands
// バンドマップのエントリー数の一覧を見ることによってバンドの状況がチェックできる。
void upd_display_info_to_work_bandmap() {
  struct radio *radio;

  radio = so2r.radio_selected();

  // show information about contest settings  dupe check, name of the contest and multi worked
  if (!plogw->f_console_emu) plogw->ostream->println("display info to work bandmap()  ");

  select_left_display();
  u8g2_l->clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // show worked stations and multi
  unsigned int bandmask = 0b1111111111111;
  int nidx = 0;
  int nq, nm, np, nw;  // total number of q and m and p point

  struct bandmap_entry *p;
  int idx;

  nidx = 0;
  int cnt;

  cnt = 0;

  for (int j = 0; j < N_BAND; j++) {
    if (((1 << j) & bandmask) == 0) {
      // not show this band
      continue;
    }
    nq = 0;
    nm = 0;
    np = 0;
    nw = 0;
    cnt++;
    if (cnt <= plogw->bandmap_summary_idx) continue;


    nq = score.worked[0][j];
    nq += score.worked[1][j];
    nm = score.nmulti[j];

    // procedure to check number of non-worked stations for the band
    for (int i = 0; i < bandmap[j].nentry; i++) {
      p = bandmap[j].entry + i;
      if (*p->station == '\0') continue;
      if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) continue;
      nw++;
    }

    // nw: number of to be worked station for the band

    sprintf(dp->lcdbuf, "%s %3dQ>%2d %2dM", bandid_str[j], nq, nw, nm);
    if (verbose & 64 ) plogw->ostream->println(dp->lcdbuf);
    display_printStr(dp->lcdbuf, 10 + nidx);
    // next row
    nidx++;
    if (nidx >= 6) break;
  }
  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
  info_disp.show_info = INFO_DISP_SUMMARY; 
  // set timer
  info_disp.timer = 5000;
}



void upd_display_info_qso(int option) {
  // option 0: load and print on the left display
  // option 1: load and edit mode
  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;

  // pos = pos - len;  // start from the end record
  //pos = 0; // start from the beginning
  pos = info_disp.pos;
  if (verbose & 1) {
    plogw->ostream->print("pos=");
    plogw->ostream->print(pos);
    plogw->ostream->print(" # ");
    plogw->ostream->println(pos/sizeof(qso.all));    
  }
  if (!qsologf.seek(pos)) {

    if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
    goto end;
  }
  ret = qsologf.read(qso.all, len);
  if (ret != len) {
    //
    if (!plogw->f_console_emu) {
      plogw->ostream->print("qso not read bytes=");
      plogw->ostream->println(ret);
    }
    goto end;
  }
  // check type
  if (qso.entry.type[0] != 'Q') {
    // not vaild qso
    if (!plogw->f_console_emu) plogw->ostream->println("not valid qso encountered");
    //     goto end;
  }
  // print content
  //plogw->ostream->print("Pos:");plogw->ostream->print(pos);
  //plogw->ostream->print(" ");

  reformat_qso_entry(&qso);

  struct radio *radio;

  radio = so2r.radio_selected();

  switch (option) {
    case 0:
      print_qso_entry(&qso);

      // show the qso content
      select_left_display();
      u8g2_l->clearBuffer();  // clear the internal memory
      if (plogw->f_console_emu) {
        clear_display_emu(1);
      }

      upd_disp_info_qso_entry();
      i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
      // set timer
      info_disp.timer = 5000;
      break;

    case 1:
      // copy into edit bufferes
      set_qsodata_from_qso_entry();
      radio->qsodata_loaded = 1;
      
      break;
  }
end:
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
  }
  return;
}



/// the following are not yet completed 21/11/15  still not in use 22/01/25


void upd_display_info() {
  if (info_disp.timer != 0) {
    if (verbose &4) {
      console->print("upd_display_info():timer=");
      console->println(info_disp.timer);
    }
    return;
  } else {
    info_disp.timer=-1;
    if (verbose &4) {
      console->print("upd_display_info():display;");
      console->print(info_disp.show_info);      
    }

    // A temporary left-display message has expired.  Return immediately to
    // the bandmap for the currently focused radio instead of leaving the
    // previous message (or an empty screen) visible until the next periodic
    // bandmap refresh.  This path is driven only by the information-display
    // timeout; SO2R's temporary TX/focus changes do not call it.
    if (plogw->sat) {
      // Satellite operation owns the left display.  When a temporary
      // information screen expires, return immediately to live satellite
      // tracking instead of briefly restoring the normal bandmap.
      upd_display_sat();
      return;
    }
    info_disp.show_info = INFO_DISP_BANDMAP;
    upd_display_bandmap();
    return;
  }
    
  select_left_display();
  u8g2_l->clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // the following should be executed after timeout (expiration of info_disp.timer becomes 0 )
  // once after displaying, info_disp.timer is set to -1 and no repeated displaying occurs (one shot) unless the time is not set again.
  //  if (info_disp.show_info != info_disp.show_info_prev) {
  //  info_disp.show_info = info_disp.show_info_prev;
  //	// go back to previous item   after showing info temporarilly
  // }

  switch (info_disp.show_info) {
    case INFO_DISP_QSO:
      // show previous qso for referring and editing (mark deleted and so on ...)
      // upd_display_info_qso();
      // set next update time
      return;
      break;
    case INFO_DISP_BANDMAP:
      //upd_display_bandmap();
      return;
      break;
    case INFO_DISP_CONTEST_SETTINGS:
      //upd_display_info_contest_settings();
      break;
    case INFO_DISP_FLASH:
    case INFO_DISP_RTTY_RX:
    case INFO_DISP_HELP:
      // keep displaying previous
      return;
    case INFO_DISP_SIGNAL:
      // upd_display_info_signal();
      return;
      break;
  }
  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display
}
void clear_display_emu(int side) {
  int ix, iy;
  ix=0;
  switch (side) {
    case 1:  // left
      ix = 1;
      break;
    case 0:  // right  display
      ix = 41;
      break;
  }
  for (int iy = 1; iy <= 7; iy++) {
    char buf[50];
    sprintf(buf, "\033[%d;%dH                               ", iy, ix);
    plogw->ostream->print(buf);
  }
}

void upd_disp_info_qso_entry() {
  sprintf(dp->lcdbuf, "%-14s %4s%c", qso.entry.tm + 3, qso.entry.seqnr, qso.entry.type[0]);  // line 1 time
  display_printStr(dp->lcdbuf, 10);                                                          //
  upd_display_freq((unsigned long)(atoll(qso.entry.freq)/FREQ_UNIT), qso.entry.opmode, 11);                              // line 2 freq
  sprintf(dp->lcdbuf, "%-8s %3s %-s", qso.entry.hiscall, qso.entry.rcvrst, qso.entry.rcvexch);
  display_printStr(dp->lcdbuf, 12);  // line3 his
  sprintf(dp->lcdbuf, "%-8s %3s %-s", qso.entry.mycall, qso.entry.sentrst, qso.entry.sentexch);
  display_printStr(dp->lcdbuf, 13);            // line 4 my
  //  strncpy(dp->lcdbuf, qso.entry.remarks, 16);  // line 5 remarks ( single line can show 21 characters )
  utf8_slice_by_columns_cjk(qso.entry.remarks, 0, 21,dp->lcdbuf, sizeof(dp->lcdbuf));  // line 5 remarks  
  display_printStr(dp->lcdbuf, 14);            // line 5 remarks
  utf8_slice_by_columns_cjk(qso.entry.remarks, 21, 21+21,dp->lcdbuf, sizeof(dp->lcdbuf));// line 6 Remarks (cont.)
  display_printStr(dp->lcdbuf, 15);            
}


static void bandmap_display_heap_trace(const char *tag) {
#if BANDMAP_TRACE
  if (defer_display(DISPLAY_REQ_BANDMAP)) return;
  console->printf("[BANDMAPTRACE] display %-18s free=%u largest=%u min=%u\n",
                  tag,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
  (void)tag;
#endif
}

void upd_display_bandmap() {
  // The left OLED is the live satellite tracking display while SAT is ON.
  // Many normal UI paths request a bandmap redraw asynchronously; ignore
  // those redraws here so they cannot overwrite the satellite screen.
  if (plogw->sat) return;

  static uint32_t trace_sequence = 0;
  const uint32_t this_trace = ++trace_sequence;
  const size_t trace_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t trace_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (this_trace <= 5) bandmap_display_heap_trace("enter");
  // plogw->ostream->println("upd_display_bandmap()");
  // plogw->ostream->print("top_column:");
  // plogw->ostream->println(bandmap_disp.top_column);
  //  plogw->ostream->print("cursor:");
  //  plogw->ostream->println(bandmap_disp.cursor );

  // show current bandmap structure for the current band
  int idx;
  struct bandmap_entry *p;

  struct radio *radio;
  radio = so2r.radio_selected();
  if (verbose & 4)  console->println("upd_display_bandmap()");
  int bandid;
  bandid = radio->bandid_bandmap;
  if (verbose & 64) {
    plogw->ostream->print("bandid_bandmap=");
    plogw->ostream->print(bandid);
    plogw->ostream->print("radio->bandid=");
    plogw->ostream->println(radio->bandid);
  }
  if (bandid == 0) {
    console->println("upd_display_bandmap bandid==0");
    return;
  }


  // maintenances display bandmap
  // delete old entry
  // delete_old_entry(bandid, 20); // now performed in minute interval jobs
  // sort the entry

  sort_bandmap(bandid);
  if (this_trace <= 5) bandmap_display_heap_trace("after sort");

  select_left_display();

  left_display_clearBuffer();
  if (this_trace <= 5) bandmap_display_heap_trace("after clear buffer");

  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }
  // place the on the frequency bandmap entry on the top if exists

  int f_worked;

  f_worked = 0;
  // print on frequency station on top every time if we have
  int onfreq_bandid = radio->bandid;
  int i_onfreq = find_on_freq_bandmap(radio->bandid, radio->freq, 100/FREQ_UNIT);
  idx = radio->bandid - 1;
  if ((i_onfreq != -1) && (idx >= 0)) {
    bandmap_disp.f_onfreq = 1;
    p = bandmap[idx].entry + i_onfreq;
    strncpy(bandmap_disp.on_freq_station, p->station, 10);
    bandmap_disp.on_freq_modeid = p->mode;

    // check if already worked this station
    //    if (dupe_check_nocallhist(p->station, bandid * 4 + modetype[p->mode], plogw->mask)) {
    if (dupe_check_nocallhist(p->station, bandmode_param(radio->bandid,modetype[p->mode]), plogw->mask)) {
      // already worked
      f_worked = 1;
      if (!plogw->f_console_emu) {
        plogw->ostream->print(p->station);
        plogw->ostream->println(" is worked before");
      }
      if (!(p->flag &BANDMAP_ENTRY_FLAG_WORKED)) {
	// worked but not flag set
	p->flag|=BANDMAP_ENTRY_FLAG_WORKED;
	console->println("worked flag not set -> set");
      }
    } else {
      if (!plogw->f_console_emu) {
        plogw->ostream->print(p->station);
        plogw->ostream->println(" is NOT worked before");
      }

    }
    if (plogw->f_console_emu) {
      // underlined
      plogw->ostream->print("\033[4m");
      if (f_worked) {
        // yellow color
        plogw->ostream->print("\033[33m");
      }
    }
    upd_display_bandmap_show_entry(p, -1,radio->bandid_bandmap);
    if (plogw->f_console_emu) {
      // restore
      plogw->ostream->print("\033[0m\033[39m");
    }
    // put underline
    u8g2_l->drawHLine(0, dp->hcol[1] - 1, 128);
    if (f_worked) {
      // middle line
      u8g2_l->drawHLine(0, dp->hcol[1] - 5, 128);
    }

  } else {
    bandmap_disp.f_onfreq = 0;
  }



  int count;
  count = 0;

  // show entry normally
  idx = bandid - 1;
  if (verbose & 64) {
    plogw->ostream->print("show bandmap entry for idx=");
    plogw->ostream->println(idx);
  }

  if (idx >= 0) {
    // clear on_cursor information
    bandmap_disp.on_cursor_station[0] = '\0';

    // check entries

    // first , check number of displayable entries

    int i;
    //    int count1;
    bandmap_disp.ncount1 = 0;
    for (i = 0; i < bandmap[idx].nentry; i++) {
      p = bandmap[idx].entry + i;
      if (*p->station == '\0') {
        if (verbose & 64) {
          plogw->ostream->print(".");
        }

        continue;
      }
      //      if (p->flag & BANDMAP_ENTRY_FLAG_ONFREQ) {
      if (bandid == onfreq_bandid && i == i_onfreq) {
        if (verbose & 64) {
          plogw->ostream->print("O");
        }
        continue;
      }
      if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) {
        if (verbose & 64) {
          plogw->ostream->print("W");
        }

        continue;
      }
      if (verbose & 64) {
        plogw->ostream->print("+");
      }

      bandmap_disp.ncount1++;
    }
    if (verbose & 64) {
      plogw->ostream->print("\nbandmap count1=");
      plogw->ostream->println(bandmap_disp.ncount1);
    }
    if (bandmap_disp.ncount1 > 0) {

      // print
      if (verbose & 64) {
        plogw->ostream->print("bandmap ");
        plogw->ostream->print(idx);
        plogw->ostream->print(" nentry ");
        plogw->ostream->println(bandmap[idx].nentry);
      }

      for (i = 0; i < bandmap[idx].nentry; i++) {

        if (verbose & 128) {
          plogw->ostream->print("count ");
          plogw->ostream->print(count);
          plogw->ostream->print(" i ");
          plogw->ostream->print(i);
          plogw->ostream->print(" top_column ");
          plogw->ostream->println(bandmap_disp.top_column[idx]);
        }


        if (count - bandmap_disp.top_column[idx] >= 5 + bandmap_disp.f_onfreq) {
          if (verbose & 64) plogw->ostream->println("break");
          break;  // may not show more than this
        }
        p = bandmap[idx].entry + i;
        if (*p->station == '\0') continue;
        // check if on the freq flag
	//        if (p->flag & BANDMAP_ENTRY_FLAG_ONFREQ) {
	if (bandid == onfreq_bandid && i == i_onfreq) {
          if (verbose & 64) {
            plogw->ostream->print((String)p->station);
            plogw->ostream->print(" f=");
            plogw->ostream->print(p->freq);
            plogw->ostream->println(" onfreq not show");
          }
          continue;
        }
        if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) {
          if (verbose & 64) {
            plogw->ostream->print((String)p->station);
            plogw->ostream->print(" f=");
            plogw->ostream->print(p->freq);
            plogw->ostream->println(" worked not show");
          }
          continue;
        }
        if (count - bandmap_disp.top_column[idx] >= 0) {
	  //          if (verbose & 64) {
	  //	    plogw->ostream->print("show entry  \n");
	  //          }
          upd_display_bandmap_show_entry(p, count,radio->bandid_bandmap);
        }
        count++;
      }
    }
  }

  if ((count + bandmap_disp.f_onfreq )== 0) {
    sprintf(dp->lcdbuf, "Bandmap %s", bandid_str[idx]);  
    display_printStr(dp->lcdbuf, 10);
  }

  if (this_trace <= 5) bandmap_display_heap_trace("before sendBuffer");
  i2c_guarded_send_buffer(u8g2_l, "oled_l");  // transfer internal memory to the display

  const size_t trace_free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t trace_largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (this_trace <= 5 || trace_free_after + 512 < trace_free_before ||
      trace_largest_after + 512 < trace_largest_before) {
    bandmap_display_heap_trace("leave");
#if BANDMAP_TRACE
    console->printf("[BANDMAPTRACE] display delta seq=%lu free=%d largest=%d
",
                    (unsigned long)this_trace,
                    (int)trace_free_after - (int)trace_free_before,
                    (int)trace_largest_after - (int)trace_largest_before);
#endif
  }
}

