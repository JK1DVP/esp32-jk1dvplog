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
#include "bandmap.h"
#include "cat.h"
#include "display.h"
#include "ui.h"
#include "so2r.h"
#include "qso.h"
#include "zserver.h"
#include "satellite.h"
#include "misc.h"
#include "main.h"
#include "processes.h"
#include "cw_keying.h"
#include "esp32_flasher.h"
#include "mcp_interface.h"
#include "log.h"
#include "antenna.h"
#include "dupechk.h"
#include "mux_transport.h"
#include "callhist_remote.h"

enum QueryCIVType {Freq,Mode,Smeter,Ptt,Id,Preamp,Gps,Att,Power,RigAnt,ScopeLevel};
void send_query_civ(enum QueryCIVType type,struct radio *radio) {
  switch(type) {
  case Freq:		send_freq_query_civ(radio);break;//0
  case Mode:		send_mode_query_civ(radio);break;//1
  case Smeter:	    send_smeter_query_civ(radio);break;//2
  case Ptt:		send_ptt_query_civ(radio);break;//3
  case Att:		send_att_query_civ(radio);break;//3    
  case Id:	      send_identification_query_civ(radio);break;  // 5
  case Preamp:		send_preamp_query_civ(radio);break;   //7 
  case Gps:		send_gps_query_civ(radio); break; 
  case Power:   send_power_query_civ(radio); break;
  case RigAnt:  send_rig_antenna_query(radio); break;
  case ScopeLevel: send_yaesu_scope_level_query(radio); break;
  }
}



static void process_subcpu_late_recovery()
{
  if (subcpu_online || !f_mux_transport) return;

  static uint32_t next_probe_ms = 0;
  const uint32_t now = millis();
  if (next_probe_ms != 0 && (int32_t)(now - next_probe_ms) < 0) return;
  next_probe_ms = now + 10000U;

  if (!callhist_subcpu_alive(120)) return;

  console->println(
    "SUBCPU late recovery: response detected; DUPE rebuild scheduled");
  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "SUBCPU FOUND\nRebuilding DUPE...");
  upd_display_info_flash(dp->lcdbuf);

  subcpu_online = true;
  callhist_at = 1;
  init_dupechk_maincpu();

  // Use the same deferred rebuild path as startup/contest changes.  This keeps
  // all QSO.TXT -> DUPE reconstruction in one place.
  request_makedupe_rebuild();

  if (plogw->enable_callhist) {
    bool fallback = false;
    int n = load_callhist_subcpu_or_main(callhistfn, &fallback);
    if (n > 0) {
      console->printf("SUBCPU RECOVERED: CALLHIST=%s entries=%d\n",
                      fallback ? "MAIN-PSRAM(fallback)" : "SUBCPU", n);
    } else {
      console->println("SUBCPU RECOVERED: CALLHIST disabled (no usable memory)");
    }
  } else {
    console->println("SUBCPU RECOVERED: CALLHIST=OFF");
  }

  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "SUBCPU RECOVERED\nDUPE: rebuilding\nCALLHIST: %s",
           plogw->enable_callhist ? "ON" : "OFF");
  upd_display_info_flash(dp->lcdbuf);
}

int interval_process_stat = 0;

// Lightweight interval_process() latency diagnostics.
// This deliberately does not use the normal profile banks because the normal
// profile report itself is printed from interval_process().  When one call is
// slower than INTERVAL_DIAG_THRESHOLD_US, print the slowest internal stage.
static const uint32_t INTERVAL_DIAG_THRESHOLD_US = 100000;

struct IntervalDiag {
  uint32_t start_us;
  uint32_t mark_us;
  uint32_t max_us;
  const char *max_stage;
};

static inline void interval_diag_begin(IntervalDiag *diag) {
  diag->start_us = micros();
  diag->mark_us = diag->start_us;
  diag->max_us = 0;
  diag->max_stage = "start";
}

static inline void interval_diag_mark(IntervalDiag *diag, const char *stage) {
  const uint32_t now = micros();
  const uint32_t elapsed = now - diag->mark_us;
  if (elapsed > diag->max_us) {
    diag->max_us = elapsed;
    diag->max_stage = stage;
  }
  diag->mark_us = now;
}

static inline void interval_diag_finish(IntervalDiag *diag) {
  const uint32_t total = micros() - diag->start_us;
  if (total >= INTERVAL_DIAG_THRESHOLD_US) {
    // Use the hardware serial port rather than console/telnet output so a
    // blocked network stream does not hide or amplify the diagnosis.
    Serial.printf("INTERVAL SLOW total=%lu us max=%lu us stage=%s stat=%d wifi=%d core=%d\n",
                  (unsigned long)total,
                  (unsigned long)diag->max_us,
                  diag->max_stage,
                  interval_process_stat,
                  wifi_enable,
                  xPortGetCoreID());
  }
}

void interval_process() {
  IntervalDiag interval_diag;
  interval_diag_begin(&interval_diag);
  struct radio *radio;
  int next_interval;
  static int query_item = 0;
  next_interval = 100;
  service_icom_clock_sync();
  service_yaesu_scope();
  antenna_process();
  interval_diag_mark(&interval_diag, "antenna");
  if (f_mux_transport) mux_transport.recv_pkt();
  if (timeout_interval < millis()) {
    if (verbose & VERBOSE_SEQUENCE) {
      if (so2r.repeat_timer()!=0) {
	plogw->ostream->print("repeat timer=");
	//      plogw->ostream->print(plogw->repeat_func_timer);
	plogw->ostream->print(so2r.repeat_timer());
	plogw->ostream->print("stat ");
	plogw->ostream->println(so2r.sequence_stat());
      }
    }

    if (bandmap_disp.f_update && !dupechk_remote_query_pending()) {
      // Consume the legacy flag before translating it into the single
      // on-demand request path.
      bandmap_disp.f_update = 0;
      request_bandmap_update_on_demand();
      interval_diag_mark(&interval_diag, "bandmap_request");
    }
    if (!dupechk_remote_query_pending()) {
      if (f_mux_transport) mux_transport.recv_pkt();
      upd_display_info();// update info_display (when timer==0)
      if (f_mux_transport) mux_transport.recv_pkt();
    }
    interval_diag_mark(&interval_diag, "display_info");
    
    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      if (!radio->enabled) continue;
      // Common 600-ms query sequence (100 ms per step):
      //   normal:      Freq, Mode, Smeter, Freq, silence, slow query
      //   show_signal: Freq, Smeter, Smeter, Freq, silence, slow query
      // Freq is queried at stat 0 and 3: every 300 ms.
      // GPS and other slow/status items are handled by query_item.
      if (!radio->f_civ_response_expected) {
	if (!plogw->f_show_signal) {
	  // normal 
	  switch (interval_process_stat) {  
	  case 0:
	    send_query_civ(Freq,radio); break;
	  case 1:
	    send_query_civ(Mode,radio); break;
	  case 2:
	    send_query_civ(Smeter,radio); break;	    
	  case 3:
	    send_query_civ(Freq,radio); break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // slow/status query
	    switch(query_item) {
	    case 0:send_query_civ(Id,radio); break;	    	    
	    case 1:send_query_civ(Ptt,radio); break;
	    case 2:send_query_civ(Mode,radio); break;
	    case 3:send_query_civ(Smeter,radio); break;	    
	    case 4:send_query_civ(Preamp,radio); break;
	    case 5:send_query_civ(Ptt,radio); break;
	    case 6:send_query_civ(Mode,radio); break;
	    case 7:send_query_civ(Smeter,radio); break;	    
	    case 8:send_query_civ(Att,radio); break;
	    case 9:send_query_civ(Gps,radio); break;
	    case 10:send_query_civ(Power,radio); break;
	    case 11:send_query_civ(RigAnt,radio); break;
	    case 12:send_query_civ(ScopeLevel,radio); break;
	    }
	    query_item++;
	    if (query_item >= 13) query_item = 0;
	    break;
	  }
	} else {
	  // show signal 
	  switch (interval_process_stat) {  
	  case 0:
	    send_query_civ(Freq,radio); break;
	  case 1:
	  case 2:
	    send_query_civ(Smeter,radio); break;
	  case 3:
	    send_query_civ(Freq,radio); break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // slow/status query 
	    switch(query_item) {
	    case 0:send_query_civ(Id,radio); break;
	    case 1:send_query_civ(Ptt,radio); break;
	    case 2:send_query_civ(Mode,radio); break;
	    case 3:send_query_civ(Smeter,radio); break;
	    case 4:send_query_civ(Preamp,radio); break;
	    case 5:send_query_civ(Ptt,radio); break;
	    case 6:send_query_civ(Mode,radio); break;
	    case 7:send_query_civ(Smeter,radio); break;
	    case 8:send_query_civ(Att,radio); break;
	    case 9:send_query_civ(Gps,radio); break;
	    case 10:send_query_civ(Power,radio); break;
	    case 11:send_query_civ(RigAnt,radio); break;
	    case 12:send_query_civ(ScopeLevel,radio); break;
	    }
	    query_item++;
	    if (query_item >= 13) query_item = 0;
	    break;
	  }
	}
      }
    }		
    interval_diag_mark(&interval_diag, "radio_queries");
    if (f_mux_transport) mux_transport.recv_pkt();


    if (interval_process_stat == 4) {
      //      rotator_process();
    }

    interval_process_stat++;
    if (interval_process_stat > 5) interval_process_stat = 0;
    timeout_interval = millis() + next_interval;
    //	plogw->ostream->print("PTT:");plogw->ostream->print(plogw->ptt_stat);plogw->ostream->print(" S_stat:");plogw->ostream->println(plogw->smeter_stat);
  }
  interval_diag_mark(&interval_diag, "interval_100ms");

  // satellite process 500ms
  if (timeout_interval_sat < millis()) {
    if (f_mux_transport) mux_transport.recv_pkt();
    sat_process();
    if (f_mux_transport) mux_transport.recv_pkt();
    interval_diag_mark(&interval_diag, "sat_process");
    timeout_interval_sat = millis() + 500;
  }
  interval_diag_mark(&interval_diag, "satellite_gate");

  // Re-enable only the radio that Alt-I temporarily disabled.
  // Radios disabled explicitly with Alt-End / Shift-Alt-End / Ctrl-Alt-End
  // must remain disabled.
  if (temporarily_disabled_radio >= 0 &&
      timeout_rig_disable_temporally < millis()) {
    int idx_radio = temporarily_disabled_radio;
    enable_radios(idx_radio,1);
  }
  interval_diag_mark(&interval_diag, "rig_reenable");

  // second process
  if (timeout_second < millis()) {
    // interval job every second
    //    Serial.print("receive_civport nmax in 1ms interrupt.:");
    //    Serial.println(receive_civport_count);

    receive_civport_count=0;
    if (plogw->autopoweroff) {
      plogw->count_autopoweroff++;
      if (verbose&4) {
	console->print("count_autopoweroff=");console->print(plogw->count_autopoweroff);
	console->print(" autopoweroff=");console->println(plogw->autopoweroff);
      }
      if (plogw->autopoweroff< plogw->count_autopoweroff) {
	// power down
	sprintf(dp->lcdbuf,"Auto powerdown\nafter %d sec \ninactive.\n",plogw->count_autopoweroff);
	console->print(dp->lcdbuf);
	upd_display_info_flash(dp->lcdbuf);
	
	// subcpu put to reset state
	//	mcp_write_pin(reset_trigger_mcp_pin, 0);
	mcp_write_pin(15, 0);	
	// main cpu deep sleep
	esp_deep_sleep(3600000000UL); // 1 hr deep sleep 
      }
    }
    
    interval_diag_mark(&interval_diag, "autopower");
    timeout_second = millis() + 1000;

    if (verbose & VERBOSE_MEM) {
      console->printf("DMA free block=%6d\n",heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    // check transport status and send command
    mux_transport.sync_transport_modes_master(); // send transport command
    process_subcpu_late_recovery();
    interval_diag_mark(&interval_diag, "mux_sync");
    

    // print_time_measure results and clear counter
    //    usb_task_memory_watermark=uxTaskGetStackHighWaterMark(gxHandle_USBloop);
    //    plogw->ostream->print("usb task mem=");plogw->ostream->print(usb_task_memory_watermark);
    if (verbose & VERBOSE_PERF) {
      plogw->ostream->print(" profile:");
      for (int i = 0; i < PROF_BANK_COUNT; ++i) {
        const char *name = time_measure_get_name(i);
        if (name[0] == '\0') continue;
        plogw->ostream->print(' ');
        plogw->ostream->print(name);
        plogw->ostream->print('=');
        plogw->ostream->print(time_measure_get(i));
      }
      plogw->ostream->print(" revs=");
      plogw->ostream->println(main_loop_revs);
      interval_diag_mark(&interval_diag, "profile_print");
    }
    for (int i = 0; i < PROF_BANK_COUNT; ++i) time_measure_clear(i);
    main_loop_revs=0;
    interval_diag_mark(&interval_diag, "profile_clear");

  }
  interval_diag_mark(&interval_diag, "second_gate");

  // Receive ASCII CAT data from the configured ports.
  for (int i = 0; i < N_RADIO; i++) {
    if (!unique_num_radio(i)) continue;

    radio = &radio_list[i];

    if (!radio->enabled || radio->rig_spec == NULL) continue;

    if (radio->rig_spec->cat_type != 0) {
      // Yaesu / Kenwood / other ASCII CAT over USB/SoftwareSerial.
      receive_cat_data(radio);
    }
  }
  interval_diag_mark(&interval_diag, "ascii_cat");

  // Receive CI-V data at the existing 50 ms service interval.
  if (timeout_cat < millis()) {
    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;

      radio = &radio_list[i];

      if (!radio->enabled || radio->rig_spec == NULL) continue;

      if (radio->rig_spec->cat_type == 0) {
        receive_civport(radio);
      }
    }
    timeout_cat = millis() + 50;
  }
  interval_diag_mark(&interval_diag, "civ_receive");


  if (timeout_interval_minute < millis()) {
    // minute processes

    // remove old bandmap entry for all bands
    int i;
    for (i = 1; i < N_BAND; i++) {
      delete_old_entry(i, bandmap_lifetime_minutes);
    }
    // Keep the special all-band/new-entry list at its existing short lifetime.
    delete_old_entry(N_BAND, 5);
    
    bandmap_disp.f_update = 1;
    timeout_interval_minute = millis() + 60000;

    // frequency notification to zserver
    zserver_freq_notification();
    interval_diag_mark(&interval_diag, "minute_jobs");
    
    
  }
  interval_diag_mark(&interval_diag, "minute_gate");

  interval_diag_finish(&interval_diag);
  
}


// written in decl.h
//  // SO-2R related
//  int so2r_tx; // selected tx 0,1
//  int so2r_rx; // selected rx 0,1
//  int so2r_stereo; // stereo rx 0,1
//  int focused_radio; // radio currently focused
//  int focused_radio_prev; // previously focused radio will be saved to here (used on toggling stereo mode to select both current focus and previous focus)
//  // \ (backspace) will switch actively receiving radio but not transmitting radio
//  //   at the same time, switch focused display by changing radio_selected.


//  int radio_mode ; // 0 so1r  1 sat 2 two radio (main transmit sub receive)
//  int sequence_mode ; // 0 manual 1 repeat function 2 auto cq+s&p  3 dueling CQ (alternate CQ) 4 2BSIQ  (wait sending until the other send finishes)
//  // sequence mode, radio_mode 1に基づき f_repeat_func_stat を制御しつつradio0, radio1 の制御を行う。
//  // 制御は、process.cpp のsequence_manager()  sequence_manager_timer_expired() で行う。sequence_manager_cancel_repeat() で0 manual に戻る。repeat でcancelをした際radioの切り替えは行わない。


// repeat_func_radio is now the which sends message so always set when function_keys is called. (--> change to msg_tx_radio )
// f_repeat_func_stat holds status of the message sending to control SO2R and repeat functions  (--> change to f_sequence_stat )
// radio_mode is SAT, SO1R, SO2R  in SO1R changing radio will stop sending message
//                          in SO2R keep sending message in so2r_tx and finishing sending message rx,focus comeback to the sent message radio, esc suspends sending message in so2r_tx but focus not change
// set_tx_to_focused() in cw_keying bring back tx to the focused radio and recommended to use
// cancel_current_tx_message stops keying and recommended to use
// append_cwbuf() now always based on so2r_tx radio regardless of the focus 

// sequence
// ui_send_cq etc , or function_keys send message and prior to this set repeat_func_stat and repeat_func_radio
//       this also may change status of the rx and focused ( when rx change occurs focus will also changed in SO2R_setrx)
// when message send completion detected ( in cw by $ and in phone check CAT TX status ), depending on the sequence_mode,
//  bring rx, and focus  back to the message sent tx and would start timer (repeat func) or tx another message at once (SO2R sequence mode dependency)
//  after timer expired, message send with the repeat_func_key in the repeat_func_radio(?)

// may better define SO2R class to hold all these status and functions (so2r_tx, rx, stereo, msg_tx_radio f_sequence_stat and focused_radio, ui_send_cq

