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
// Copyright (c) 2021-2025 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later


// 24/7/29
// python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 summary
//  python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 set_flash_voltage 3.3V
// to disable selecting flash voltage from pin stage SPI interface

// memo cq sp frequency gets equal if quickly alt-q's pressed
// in band changing operation cq/s and p, and phone/cw should be remembered for each band
// radio->cq_modetype_bank  does that but not implemented anything 24/07/30
// save/recall routine would take care of that
// implemented above but cq/sp switch doesnot recall old frequency (may be saved overwriting)

#include "Arduino.h"
#include "user_contest_md.h"
#include "decl.h"
#include "hardware.h"
#include "variables.h"
#include "multi.h"
#include <Wire.h>
#include "i2c_guard.h"
#include "SPIFFS.h"
#include <WiFi.h>
#include "sd_files.h"
#include "usb_host.h"
#include "cw_keying.h"
#include "iambic_keyer.h"
#include "display.h"
#include "cat.h"
#include "log.h"
#include "qso.h"
#include "cluster.h"
#include "bandmap.h"
#include "multi_process.h"
#include "ui.h"
#include "so2r.h"
#include "processes.h"
#include "misc.h"
#include "settings.h"
#include "edit_buf.h"
#include "mcp.h"
#include "console.h"
#include "tcp_server.h"
#include "timekeep.h"
#include "network.h"
#include "zserver.h"
#include "satellite.h"
#include "dac-adc.h"
#include "web_server.h"
#include "mux_transport.h"
#include "adafruit_usbhost.h"
#include "AudioPlayer.h"

#include <stdio.h>
#include <stdarg.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp32_flasher.h"
#include <SoftwareSerial.h>
#include "callhist.h"
#include "callhist_remote.h"
#include "callhist_mem.h"
#include "esp_heap_caps.h"

// Keep the latest SUBCPU profiler report available on MAIN even when routine
// console output is disabled.  Printing is controlled by VERBOSE_PERF.
static char latest_subcpu_profile[256] = {0};
#include "morse_decoder_simple.h"
#include "esp_intr_alloc.h"
#include "cardkey.h"

void usb_loop_task(void *arg)
{
    uint32_t last_acm_ms = 0;
    while(1){
      loop_usb(); // for older USB host library

      // USB Audio capture needs Usb.Task() serviced every 1 ms.  Keep ACM
      // CAT processing near its normal 10-ms cadence during audio capture
      // so it does not consume an unnecessary share of the USB frame budget.
      if (usb_audio_capture_active()) {
        const uint32_t now = millis();
        if ((uint32_t)(now - last_acm_ms) >= 10U) {
          ACMprocess();
          last_acm_ms = now;
        }
      } else {
        ACMprocess(); // test just receiving
        last_acm_ms = millis();
      }

      // While capturing USB audio do not sleep for a FreeRTOS tick here.
      // On builds where configTICK_RATE_HZ is 100, vTaskDelay(1) is about
      // 10 ms and would miss most 1-ms USB audio frames.  The audio Poll()
      // itself synchronizes to SOF, so merely yield after each pass.
      if (usb_audio_capture_active()) {
        taskYIELD();
      } else {
        // Keep the existing fast cadence for CDC RTTY keying.
        vTaskDelay(usb_rtty_fast_service_needed() ? 1 : 10);
      }
    }
}


TaskHandle_t gxHandle_USBloop;

// usb polling process in separate task 
void usb_loop_setup()
{
  xTaskCreate(usb_loop_task, "usb_loop", 4096, NULL, 2, &gxHandle_USBloop);
}

Stream *console; 

extern "C" int dvplogger_console_printf(const char *fmt, ...)
{
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (console) console->print(b);
  return n;
}


static bool memstat_watch_enabled = false;
static bool memstat_request_pending = false;
static bool memstat_update_lcd = true;
static uint32_t memstat_request_ms = 0;
static uint32_t memstat_next_ms = 0;
static Stream *memstat_output = nullptr;

static Stream *memstat_reply_stream()
{
  return memstat_output ? memstat_output : console;
}

void rebind_memstat_output(Stream *old_output, Stream *new_output)
{
  if (memstat_output == old_output) memstat_output = new_output;
}

void request_memstat_main_subcpu(bool update_lcd, Stream *output)
{
  if (output) memstat_output = output;
  Stream *out = memstat_reply_stream();
  if (f_mux_transport) {
    const char *request = "memstat";
    memstat_update_lcd = update_lcd;
    memstat_request_pending = true;
    memstat_request_ms = millis();
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                           (unsigned char *)request, strlen(request));
    if (update_lcd) {
      out->println("requested main/sub CPU memory status");
    }
  } else {
    memstat_request_pending = false;
    out->println("MUXTRANS is not active");
    if (update_lcd) {
      sprintf(dp->lcdbuf, "MEMSTAT\nMUXTRANS inactive\n");
      upd_display_info_flash(dp->lcdbuf);
    }
  }
}

void start_memstat_watch(Stream *output)
{
  if (output) memstat_output = output;
  memstat_watch_enabled = true;
  memstat_request_pending = false;
  memstat_next_ms = millis();
  memstat_reply_stream()->println("memstat watch: started (1 second interval)");
}

void stop_memstat_watch(Stream *output)
{
  if (output) memstat_output = output;
  memstat_watch_enabled = false;
  memstat_request_pending = false;
  memstat_reply_stream()->println("memstat watch: stopped");
}

void process_memstat_watch()
{
  if (!memstat_watch_enabled) return;

  uint32_t now = millis();
  if (memstat_request_pending) {
    if ((uint32_t)(now - memstat_request_ms) >= 3000) {
      memstat_request_pending = false;
      memstat_reply_stream()->println("memstat watch: subcpu response timeout");
      memstat_next_ms = now;
    }
    return;
  }

  if ((int32_t)(now - memstat_next_ms) >= 0) {
    request_memstat_main_subcpu(false, memstat_output);
    memstat_next_ms = now + 1000;
  }
}

// main board message received
void receive_pkt_handler_main_brd(struct mux_packet *packet)
{
  // packet->buf: data
  // packet->idx: number of data
  // message from ext board
  char buf[256];
  if (verbose &4) console->println("receive_pkt_handler_main_brd()");  
  if (strncmp(packet->buf,"chdone:",7)==0 ||
      strncmp(packet->buf,"chack:",6)==0 ||
      strncmp(packet->buf,"chpong",6)==0) {
    size_t n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, n); buf[n] = '\0';
    process_callhist_control_response_main(buf);
  } else if (strncmp(packet->buf,"dupeack:",8)==0) {
    size_t n = min((size_t)(packet->idx - 8), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 8, n);
    buf[n] = '\0';
    unsigned int query_id = strtoul(buf, NULL, 10);
    dupechk_note_main_ack(query_id);
    if (verbose & 16384) Serial.printf("DUPE MAIN ACK raw=[%.*s]\n", packet->idx, packet->buf);
  } else if (strncmp(packet->buf,"dupepr:",7)==0) {
    dupechk_note_main_rx();
    size_t raw_n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, raw_n);
    buf[raw_n] = '\0';
    if (verbose & 16384) Serial.printf("DUPE MAIN RX raw=[%s]\n", buf);
    size_t n = min((size_t)(packet->idx - 7), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 7, n);
    buf[n] = '\0';
    process_dupechk_partial_response_maincpu(buf);
  } else if (strncmp(packet->buf, "subprof:", 8) == 0) {
    size_t n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, n);
    buf[n] = '\0';

    // Always receive and retain SUBCPU health information on MAIN.  Only the
    // repetitive once-per-second console display is controlled by verbose.
    size_t cached_n = min(n, sizeof(latest_subcpu_profile) - 1);
    memcpy(latest_subcpu_profile, buf, cached_n);
    latest_subcpu_profile[cached_n] = '\0';

    if ((verbose & VERBOSE_PERF) && console) {
      console->println(latest_subcpu_profile);
    }
  } else if (strncmp(packet->buf, "memstat:", 8) == 0) {
    memstat_request_pending = false;
    Stream *out = memstat_reply_stream();
    unsigned int sub_free8, sub_min8, sub_largest8;
    unsigned int sub_internal, sub_spiram;
    unsigned int sub_nmaxqso, sub_ncallsign;

    if (packet->idx < 8) {
      out->println("invalid subcpu memstat packet");
      return;
    }

    size_t len = min((size_t)(packet->idx - 8), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 8, len);
    buf[len] = '\0';

    int n = sscanf(buf,
                   "%u|%u|%u|%u|%u|%u|%u",
                   &sub_free8, &sub_min8, &sub_largest8,
                   &sub_internal, &sub_spiram,
                   &sub_nmaxqso, &sub_ncallsign);

    if (n == 7) {
      unsigned int main_free8 =
          heap_caps_get_free_size(MALLOC_CAP_8BIT);
      unsigned int main_min8 =
          heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      unsigned int main_largest8 =
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      unsigned int main_internal =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      unsigned int main_internal_min =
          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      unsigned int main_internal_largest =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      unsigned int main_spiram =
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

      out->printf(
          "maincpu heap: free8=%u min8=%u largest8=%u "
          "internal=%u internal_min=%u internal_largest=%u spiram=%u\n",
          main_free8, main_min8, main_largest8,
          main_internal, main_internal_min, main_internal_largest, main_spiram);
      out->printf(
          "subcpu heap: free=%u min=%u largest=%u "
          "internal=%u spiram=%u dupe=%u/%u\n",
          sub_free8, sub_min8, sub_largest8,
          sub_internal, sub_spiram,
          sub_ncallsign, sub_nmaxqso);

      if (memstat_update_lcd) {
        sprintf(dp->lcdbuf,
                "DUPE M%u S%u/%u\n"
                "CPU Heap Low Big\n"
                "MAIN %u %u %u\n"
                "SUB  %u %u %u\n"
                "IRAM M%u/%u/%u S%u\n"
                "PSRAM M%u S%u\n",
                (unsigned int)dupechk->nmaxqso,
                sub_ncallsign,
                sub_nmaxqso,
                main_free8 / 1024,
                main_min8 / 1024,
                main_largest8 / 1024,
                sub_free8 / 1024,
                sub_min8 / 1024,
                sub_largest8 / 1024,
                main_internal / 1024,
                main_internal_min / 1024,
                main_internal_largest / 1024,
                sub_internal / 1024,
                main_spiram / 1024,
                sub_spiram / 1024);

        upd_display_info_flash(dp->lcdbuf);
      }
    } else {
      out->print("invalid subcpu memstat response: ");
      out->println(buf);
    }
    return;
  } else if (strncmp(packet->buf,"duper:",6)==0) {
    dupechk_note_main_rx();
    unsigned int query_id;
    int is_dupe, has_exch;
    char exch[LEN_EXCH + 1] = "";

    size_t n = min((size_t)packet->idx, sizeof(buf) - 1);
    memcpy(buf, packet->buf, n);
    buf[n] = '\0';
    if (verbose & 16384) Serial.printf("DUPE MAIN RX raw=[%s]\n", buf);

    unsigned long sub_search_us = 0;
    unsigned int qso_scanned = 0, hist_scanned = 0, cache_hit = 0;
    int parsed = sscanf(buf + 6, "%u %d %d %10s %lu %u %u %u",
                        &query_id, &is_dupe, &has_exch, exch, &sub_search_us,
                        &qso_scanned, &hist_scanned, &cache_hit);
    if (parsed >= 4) {
      if (dupechk->dupechk_status == 1 &&
          query_id == dupechk->dupechk_query_id) {
        dupechk->dupechk_dupe = is_dupe ? 1 : 0;
        dupechk->dupechk_getexch = has_exch ? 1 : 0;
        if (has_exch && strcmp(exch, "-") != 0) {
          strncpy(dupechk->dupechk_exch, exch, LEN_EXCH);
          dupechk->dupechk_exch[LEN_EXCH] = '\0';
        } else {
          dupechk->dupechk_exch[0] = '\0';
        }
        if (parsed >= 8)
          dupechk_log_timing("exact", query_id, (uint32_t)sub_search_us,
                             qso_scanned, hist_scanned, cache_hit != 0);
        dupechk_note_exact_response_success(query_id);
        dupechk->dupechk_status = 0;
      } else if (verbose & 4) {
        console->println("ignored stale duper response");
      }
    } else {
      console->println("invalid duper response");
    }
  } else if (strncmp(packet->buf,"dupebulka:",10)==0) {
    // Accepted-QSO notification from SUBCPU MAKEDUPE bulk processing.
    // Format: dupebulka:<bandmode>|<received exchange>
    size_t n = min((size_t)(packet->idx - 10), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 10, n);
    buf[n] = '\0';
    char *sep = strchr(buf, '|');
    if (sep != NULL) {
      *sep = '\0';
      int bandmode = atoi(buf);
      if (bandmode >= 0 && bandmode <= 255) {
        note_makedupe_accepted_maincpu();
        process_makedupe_multiplier_maincpu(sep + 1,
                                            (unsigned char)bandmode);
      }
    }
  } else if (packet->idx >= 17 &&
             memcmp(packet->buf, "dupebulkbeginack:", 17) == 0) {
    size_t n = min((size_t)(packet->idx - 17), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 17, n);
    buf[n] = '\0';
    unsigned int mask = 0;
    int ncallsign = -1, nmaxqso = -1;
    if (sscanf(buf, "%u,%d,%d", &mask, &ncallsign, &nmaxqso) == 3) {
      console->printf(
        "MAKEDUPE SUBCPU begin mask=0x%02X ncallsign=%d capacity=%d\n",
        mask & 0xffU, ncallsign, nmaxqso);
    } else {
      console->printf("MAKEDUPE SUBCPU begin parse error: <%s>\n", buf);
    }
  } else if (packet->idx >= 12 &&
             memcmp(packet->buf, "dupebulkdup:", 12) == 0) {
    size_t n = min((size_t)(packet->idx - 12), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 12, n);
    buf[n] = '\0';
    unsigned int sample_idx = 0, incoming = 0, existing = 0, match_idx = 0;
    char call[LEN_CALLSIGN + 1];
    call[0] = '\0';
    if (sscanf(buf, "%u|%12[^|]|%u|%u|%u",
               &sample_idx, call, &incoming, &existing, &match_idx) == 5) {
      console->printf(
        "MAKEDUPE DUP sample=%u call=%s incoming=%u existing=%u matchidx=%u\n",
        sample_idx, call, incoming, existing, match_idx);
    } else {
      console->printf("MAKEDUPE DUP sample parse error: <%s>\n", buf);
    }
  } else if (strncmp(packet->buf,"dupebulkdiag:",13)==0) {
    size_t n = min((size_t)(packet->idx - 13), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 13, n);
    buf[n] = '\0';
    process_makedupe_diag_maincpu(buf);
  } else if (strncmp(packet->buf,"dupebulk0:",10)==0 ||
             strncmp(packet->buf,"dupebulk1:",10)==0) {
    int group = packet->buf[8] - '0';
    size_t n = min((size_t)(packet->idx - 10), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 10, n);
    buf[n] = '\0';
    process_makedupe_score_maincpu(buf, group);
  } else if (packet->idx >= 15 &&
             memcmp(packet->buf, "dupereset:done:", 15) == 0) {
    // MUX payloads are length-delimited and are NOT NUL-terminated.
    // Copy only the numeric suffix before parsing; otherwise stale bytes
    // remaining after the payload can turn "0" into "09", "092", etc.
    char reset_count_buf[16];
    size_t reset_count_len = min((size_t)(packet->idx - 15),
                                 sizeof(reset_count_buf) - 1);
    memcpy(reset_count_buf, packet->buf + 15, reset_count_len);
    reset_count_buf[reset_count_len] = '\0';
    notify_dupechk_subcpu_reset(atoi(reset_count_buf));
  } else if (strncmp(packet->buf,"duped:",6)==0) {
    // dupe database status
    size_t n = min((size_t)(packet->idx - 6), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 6, n);
    buf[n] = '\0';
    console->print("received duped=");
    console->println(buf);
  } else if (strncmp(packet->buf,"cwdbg:",6)==0) {
    size_t n = min((size_t)(packet->idx - 6), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 6, n);
    buf[n] = '\0';
    unsigned int phase = 0, ch = 0, fifo_before = 0, fifo_after = 0;
    if (sscanf(buf, "%u|%u|%u|%u",
               &phase, &ch, &fifo_before, &fifo_after) == 4) {
      console->printf("SUBCPU CWDBG phase=%u char=%c(0x%02X) fifo=%u->%u\n",
                      phase,
                      (ch >= 32 && ch <= 126) ? (char)ch : '?',
                      ch, fifo_before, fifo_after);
    } else {
      console->print("invalid SUBCPU CWDBG: ");
      console->println(buf);
    }
  } else if (strncmp(packet->buf,"playq:",6)==0) {
    // response to 'playq' command, playq: with currently playing string
    console->print("Now playing:");
    strncpy(buf,packet->buf+6,packet->idx-6);
    *plogw->playing='\0';
    strncat(plogw->playing,packet->buf+6,min(packet->idx-6,50));
    if (strlen(plogw->playing)==0) {
      console->println("play queue len=0 finished playing?");
      plogw->f_playing=0;
      if (so2r.query_queue_monitor_status()==1) {
	so2r.set_queue_monitor_status(0);
	// clear cw_buf_display
	display_cw_buf_lcd("");
      }
      console->println("play finished.at playq");
      // ptt control
      struct radio *radio;
      radio=so2r.radio_tx();
      if (radio->ptt) {
	set_ptt_rig(radio,0);
	radio->ptt=0;
	console->println("ptt control off");
      } else {
	console->println("ptt already off !? no change");
      }
    }
    console->println(plogw->playing);
    display_cw_buf_lcd(plogw->playing);
  } else if (strncmp(packet->buf,"playc:",6)==0) {
    plogw->f_playing=0;
    if (so2r.query_queue_monitor_status()==1) {
      so2r.set_queue_monitor_status(0);
      // clear cw_buf_display
      display_cw_buf_lcd("");
    }
    console->println("play finished.");
    // ptt control
    struct radio *radio;
    radio=so2r.radio_tx();
    if (radio->ptt) {
      set_ptt_rig(radio,0);
      radio->ptt=0;
      console->println("ptt control off");
    } else {
      console->println("ptt already off !? no change");
    }
  }
}

void check_spiram()
{
  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t psram_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  f_spiram = (psram_total > 0) ? 1 : 0;
  f_low_memory_mode = (f_spiram == 0);

  Serial.printf(
      "memory mode: %s; PSRAM total=%u free=%u largest=%u\r\n",
      f_low_memory_mode ? "LOW (no PSRAM)" : "NORMAL (PSRAM)",
      static_cast<unsigned>(psram_total),
      static_cast<unsigned>(psram_free),
      static_cast<unsigned>(psram_largest));
}

void setup()
{

  init_logwindow();
  check_spiram();
  radio_list[0].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[1].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[2].bt_buf = (char *)malloc(sizeof(char) * 256);
  init_i2c_guard();
  i2c_set_owner_task();
  init_display_dispatch();
  Wire.begin();
  Wire.setTimeOut(50);

  init_mcp_port();

  Serial.println("mcp port init");

  init_mux_serial();
  init_cat_serialport();
  
  pinMode(21, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);

  init_callhist_list();
  
  // Start/reset the SUBCPU.  Blank/corrupt firmware is tolerated: a bounded
  // probe after MUX setup decides whether remote services are usable.
  loader_boot_init_func();
  loader_reset_init_func();
  loader_port_reset_target_func();
  console->println("subcpu reset");

  
  init_sat();

  init_qso();
  init_bandmap();
  init_info_display();

  init_multi(NULL,-1,-1);
  
  init_all_radio();
  init_settings_dict();
  /*
   * SD is needed here only to obtain the display type before initializing
   * the OLED. If the card is unavailable, the compiled default is used.
   */
  init_sd();
  load_boot_display_type("/settings.txt");
  
  init_display();




  /*
   * Create the CAT USB queues before starting the USB task.
   * usb_loop_task() calls ACMprocess(), which accesses these queues.
   */
  init_cat_usb();

  init_usb();
  usb_loop_setup(); // start USB polling task
  // adafruit_usbhost_setup();  
  
  so2r.set_rx(so2r.rx());
  so2r.set_tx(so2r.tx());  
  
  //  #ifdef notdef
  plogw->ostream->print("rig rigspec check");


  init_cw_keying();

  init_iambic_keyer();
  
  //init_sd();

  load_rigs("RIGS");
  
  if (!load_settings("settings")) {
    // No settings.txt: keep the compiled default contest_id=0, but apply it
    // through the normal contest-selection path so contest_name/mask/multi
    // are initialized consistently.  contest_id 0 is NOMULTI.
    console->println("settings.txt not found; applying default contest NOMULTI");
    set_contest_id();
  }

  /*
   * On units without PSRAM, move the large databases off the MAIN CPU
   * before AsyncWebServer registers its handlers.  init_qso() ran before
   * settings were available and may have allocated the normal MAIN-side
   * Call History buffer.  Freeing it only after init_webserver() leaves the
   * internal heap too fragmented for HTTP response buffers.
   */
  if (f_low_memory_mode) {
    callhist_at = 1;
    init_callhist();          // releases the MAIN-side Call History buffer
    init_dupechk_maincpu();   // one-entry remote-query context; DB is on SUBCPU
    console->println("LOWMEM early placement: callhist=SUBCPU dupechk=SUBCPU");
    if (lowmem_trace) console->printf("LOWMEM pre-web heap: free=%u largest=%u min=%u\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }

  memtrace_event("before init_network");
  init_network();
  memtrace_event("after init_network");
  init_timekeep();

  upd_display();


  so2r.set_status();
  open_qsolog();

  strcpy(plogw->grid_locator_set, plogw->grid_locator + 2);
  set_location_gl_calc(plogw->grid_locator_set);
  print_memory();

  //  btserial_init();
  plan13_test();

  adc_setup();
  
  if (f_low_memory_mode && lowmem_trace) {
    console->printf("LOWMEM before init_webserver: free=%u largest=%u min=%u\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  memtrace_event("before init_webserver");
  init_webserver();
  memtrace_event("after init_webserver");
  if (f_low_memory_mode && lowmem_trace) {
    console->printf("LOWMEM after init_webserver: free=%u largest=%u min=%u\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }

  init_cardkey();   

  // multiplexed data transport between boards through a serial port
  f_mux_transport=0;
  subcpu_online=false;
  mux_transport=Mux_transport();
  mux_transport.mux_stream=&Serial2;
  mux_transport.debug_stream=console;
  mux_transport.debug=0;
  mux_transport.set_port_handler(MUX_PORT_CAT2_MAIN,receive_pkt_handler_cat2);
  mux_transport.set_port_handler(MUX_PORT_CAT3_MAIN,receive_pkt_handler_cat3);
  mux_transport.set_port_handler(MUX_PORT_MAIN_BRD_CTRL,receive_pkt_handler_main_brd);
  mux_transport.set_port_handler(MUX_PORT_BT_SERIAL_MAIN,receive_pkt_handler_btserial);
  mux_transport.set_port_handler(MUX_PORT_USB_KEYBOARD1_MAIN,receive_pkt_handler_keyboard1_main);

  // Enter MUX and perform a small number of bounded liveness probes.
  //
  // A failed probe means only that SUBCPU-backed services (DUPE/CALLHIST)
  // are unavailable at this point in boot.  The same MUX also carries the
  // KBD connector, so keep the transport alive even if remote services
  // have to fall back to MAIN.
  //
  // The total wait remains bounded so a blank/broken SUBCPU cannot prevent
  // the MAIN CPU from booting.
  f_mux_transport=1;
  subcpu_online=false;
  for (int attempt = 0; attempt < 3 && !subcpu_online; ++attempt) {
    Serial2.print("\r\ngo_mux\r\n");
    delay(attempt == 0 ? 100 : 150);
    subcpu_online = callhist_subcpu_alive(350);
  }
  if (subcpu_online) {
    console->println("SUBCPU probe: online; DUPE database rebuild scheduled");
    callhist_at=1;
    init_dupechk_maincpu();
  } else {
    console->println(
      "SUBCPU probe: no response; using MAIN DUPE (200 entries), "
      "MUX kept active for KBD/flasher/late recovery");

    // Do not disable Serial2/MUX here.  KBD and SUBCPU flashing must remain
    // usable even when the SUBCPU application is blank or not responding.
    f_mux_transport=1;
    callhist_at=0;
    plogw->enable_callhist=0;
    init_dupechk(200,0);

    console->println("SUBCPU OFFLINE: MAIN DUPE active (200 entries)");
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "SUBCPU OFFLINE\nMAIN DUPE: 200\nWiFi/FLASH OK");
    upd_display_info_flash(dp->lcdbuf);
  }

  // Build the DUPE database exactly once after startup placement is known.
  // When settings.txt exists, set_contest_id() may already have requested a
  // rebuild; request_makedupe_rebuild() is idempotent, so this also covers
  // the no-settings first boot without causing a second rebuild.
  request_makedupe_rebuild();

  /*
   * callhist_at is restored by load_settings().  Load the configured
   * Call History after MUXTRANS is available, because SUBCPU mode needs
   * the inter-CPU control channel.
   */
  if (callhist_at != 0 && callhist_at != 1) {
    console->printf("invalid callhist_at=%d; using MAIN CPU\n", callhist_at);
    callhist_at = 0;
  }
  if (f_low_memory_mode && subcpu_online) {
    if (callhist_at != 1) {
      callhist_at = 1;
      console->println("LOWMEM: Call History forced to SUBCPU");
    }
    if (dupechk != NULL && dupechk->dupechk_at != 1) {
      // MAIN only keeps the one-entry remote-query context.  The actual
      // DUPE database is rebuilt and maintained on the SUBCPU.
      init_dupechk_maincpu();
      console->println("LOWMEM: DUPE check forced to SUBCPU");
    }
  }
  console->printf("database placement: callhist=%s dupechk=%s\n",
                  callhist_at == 1 ? "SUBCPU" : "MAIN",
                  (dupechk != NULL && dupechk->dupechk_at == 1) ? "SUBCPU" : "MAIN");

  if (plogw->enable_callhist) {
    int n = 0;
    if (callhist_at == 1) {
      delay(100);  // allow the SUB CPU MUX command handler to become ready
      if (!callhist_subcpu_alive(350)) {
        console->println("startup callhist: SUBCPU unavailable; trying fallback");
        if (f_spiram) {
          callhist_at = 0;
          n = read_callhist_list(callhistfn);
          if (n <= 0) plogw->enable_callhist = 0;
        } else {
          plogw->enable_callhist = 0;
          console->println("startup callhist: no MAIN PSRAM; CALLHIST disabled");
        }
      } else {
        bool fallback = false;
        n = load_callhist_subcpu_or_main(callhistfn, &fallback);
      }
    } else {
      n = read_callhist_list(callhistfn);
    }
    console->printf("startup callhist: file=%s at=%s entries=%d\n",
                    callhistfn, callhist_at ? "SUBCPU" : "MAIN", n);
  }

}



void check_Serial2()
{
  int count1=0;
  char c;
    while (Serial2.available()) {
      c=Serial2.read();
      console->print("%");
      console->print(c);
      count1++;
    }
  
}



struct paddle_queue paddle_queue_recv;

int prev_n_adc_i2s_read=0;

// Service pending MAIN<->SUBCPU packets at latency-sensitive boundaries.
// recv_pkt() is non-blocking when no data is available.
static inline void service_mux_transport()
{
  if (f_mux_transport) {
    mux_transport.recv_pkt();
  }
}

void loop() {
  time_measure_start_name(PROF_LOOP_TOTAL, "loop");

  // YMODEM fast path -------------------------------------------------------
  //
  // A 1K/STX packet at 115200 bps occupies the UART for about 90 ms.
  // console_process() normally runs near the end of loop(), after display,
  // network, cluster, decoder and other work.  During a file transfer that
  // latency can overflow even an enlarged RX ring and cause repeated packet
  // retries.  While YMODEM is active, dedicate this loop iteration to
  // draining the console as early and as continuously as possible.
  //
  // The Wi-Fi/USB/other application-level tasks are intentionally paused
  // during the transfer.  Driver/RTOS background work still runs because
  // delay(0) yields to the scheduler.
  if (console_ymodem_active()) {
    time_measure_start_name(PROF_CONSOLE, "console_ymodem");
    console_process();
    time_measure_stop(PROF_CONSOLE);

    time_measure_stop(PROF_LOOP_TOTAL);
    main_loop_revs++;

    // While a packet is being assembled, keep draining aggressively.
    // A 1K packet at 115200 bps lasts only about 90 ms, well below the WDT
    // interval.  After ACK/NAK returns the parser to Y_WAIT_START, give IDLE0
    // one full tick.  This avoids WDT starvation without inserting a 1-tick
    // gap repeatedly in the middle of an incoming packet.
    if (console_ymodem_packet_in_progress()) {
      taskYIELD();
    } else {
      vTaskDelay(1);
    }
    return;
  }

  time_measure_start_name(PROF_MUX_RECV, "mux_recv");
  service_mux_transport();
  time_measure_stop(PROF_MUX_RECV);

  time_measure_start_name(PROF_MEMSTAT, "memstat");
  memtrace_poll();
  process_memstat_watch();
  time_measure_stop(PROF_MEMSTAT);

  process_display_requests();
  process_dupe_aware_display_update();
  service_mux_transport();

  time_measure_start_name(PROF_WEB_TERMINAL, "web_term");
  process_web_ui_queue();
  process_web_terminal_log_queue();
  time_measure_stop(PROF_WEB_TERMINAL);


  time_measure_start_name(PROF_WEB_BANDMAP, "web_band");
  // Snapshot construction can take about 10 ms.  Do not let it delay a
  // latency-sensitive remote DUPE reply; the next loop will retry it.
  if (!dupechk_remote_query_pending()) process_web_bandmap();
  time_measure_stop(PROF_WEB_BANDMAP);
  service_mux_transport();

  
  time_measure_start_name(PROF_TCP_SERVER, "tcp");
  process_tcpserver();
  time_measure_stop(PROF_TCP_SERVER);

  time_measure_start_name(PROF_MORSE_DECODE, "decode");
  decoder.morse_decode_task();
  time_measure_stop(PROF_MORSE_DECODE);

  time_measure_start_name(PROF_MORSE_MONITOR, "monitor");
  decoder.monitor_task();
  time_measure_stop(PROF_MORSE_MONITOR);

  time_measure_start_name(PROF_CARDKEY, "cardkey");
  if (f_cardkey_present) cardkey_process();
  time_measure_stop(PROF_CARDKEY);

  time_measure_start_name(PROF_PADDLE_DIAG, "paddle");
  if ((verbose & VERBOSE_PADDLE) && xQueuePaddle != NULL) {
    static uint32_t next_paddle_report_ms = 0;
    uint32_t now_ms = millis();
    if ((int32_t)(now_ms - next_paddle_report_ms) >= 0) {
      next_paddle_report_ms = now_ms + 20;
      if (xQueueReceive(xQueuePaddle, &paddle_queue_recv, 0) == pdTRUE) {
        printf("Paddle %d %d\n", paddle_queue_recv.paddle,
               paddle_queue_recv.voltage);
      }
    }
  }
  time_measure_stop(PROF_PADDLE_DIAG);

  time_measure_start_name(PROF_KEY_MAIN, "key_main");
  Prs.process_keyrpt_queue("main");
  time_measure_stop(PROF_KEY_MAIN);
  process_dupe_aware_display_update();

  time_measure_start_name(PROF_KEY_EXTERNAL, "key_ext");
  Prs1.process_keyrpt_queue("external");
  time_measure_stop(PROF_KEY_EXTERNAL);
  process_dupe_aware_display_update();

  time_measure_start_name(PROF_QSO_FILE, "qso_file");
  process_qso_file_operation();
  time_measure_stop(PROF_QSO_FILE);
  service_mux_transport();

  time_measure_start_name(PROF_USER_MD, "user_md");
  process_user_md_contest();
  time_measure_stop(PROF_USER_MD);

  time_measure_start_name(PROF_MAKEDUPE, "makedupe");
  process_pending_makedupe_rebuild();
  time_measure_stop(PROF_MAKEDUPE);

  time_measure_start_name(PROF_CONTROL_TX, "control_tx");
  Control_TX_process();
  time_measure_stop(PROF_CONTROL_TX);

  service_mux_transport();
  time_measure_start_name(PROF_TIMEKEEP, "timekeep");
  timekeep();
  time_measure_stop(PROF_TIMEKEEP);
  service_mux_transport();

  time_measure_start_name(PROF_SO2R_1, "so2r_1");
  so2r.task();
  time_measure_stop(PROF_SO2R_1);

  time_measure_start_name(PROF_CIV, "civ");
  civ_process();
  time_measure_stop(PROF_CIV);

  time_measure_start_name(PROF_WIFI, "wifi");
  if (wifi_timeout < millis()) {
    wifi_timeout = millis() + 2000;
    check_wifi();
  }
  service_network_background();
  time_measure_stop(PROF_WIFI);

  time_measure_start_name(PROF_INTERVAL, "interval");
  interval_process();
  time_measure_stop(PROF_INTERVAL);
  service_mux_transport();

  time_measure_start_name(PROF_SIGNAL, "signal");
  signal_process();
  time_measure_stop(PROF_SIGNAL);

  time_measure_start_name(PROF_ROTATOR, "rotator");
  rotator_sweep_process();
  time_measure_stop(PROF_ROTATOR);

  time_measure_start_name(PROF_SO2R_2, "so2r_2");
  so2r.task();
  time_measure_stop(PROF_SO2R_2);

  time_measure_start_name(PROF_SATELLITE, "satellite");
  if (plogw->sat) sat_find_nextaos_sequence();
  time_measure_stop(PROF_SATELLITE);

  time_measure_start_name(PROF_CLUSTER, "cluster");
  cluster_process();
  time_measure_stop(PROF_CLUSTER);
  service_mux_transport();

  time_measure_start_name(PROF_ZSERVER, "zserver");
  zserver_process();
  time_measure_stop(PROF_ZSERVER);
  service_mux_transport();

  time_measure_start_name(PROF_CW_DISPLAY, "cw_display");
  display_cwbuf();
  time_measure_stop(PROF_CW_DISPLAY);

  time_measure_start_name(PROF_CONSOLE, "console");
  console_process();
  time_measure_stop(PROF_CONSOLE);

  time_measure_stop(PROF_LOOP_TOTAL);
  main_loop_revs++;
  delay(1);
}

SoftwareSerial Serial3;

extern "C" void app_main(void)
{
  initArduino();
  digitalWrite(LED, 0);
  digitalWrite(CW_KEY1, 0);
  digitalWrite(CW_KEY2, 0);
  pinMode(LED, OUTPUT);
  pinMode(CW_KEY1, OUTPUT);
  pinMode(CW_KEY2, OUTPUT);
  
  Serial.begin(115200);

  while (!Serial) ;  
  console=&Serial;  
  console->println("start console port");
  console->flush();
  
  setup();
  
  while (1) {
    loop();
  }
}
