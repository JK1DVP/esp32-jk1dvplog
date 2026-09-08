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
#include "SD.h"
#include "decl.h"
#include "variables.h"
#include "settings.h"
#include "contest.h"
#include "cat.h"
#include "cluster.h"
#include "network.h"
#include "zserver.h"
#include "cw_keying.h"
#include "iambic_keyer.h"
#include "satellite.h"
#include "display.h"
#include "so2r.h"
#include "antenna.h"
#include <vector>

// read one line from file,with line termination 'term'
// maximum line buffer size specified in size
// return with number of chars in the line
// return with negative number ; not complete line read.
int readline(File *f, char *buf, int term, int size) {
  char c;
  int idx;
  idx = 0;
  while (f->available()) {
    if (idx >= size) {
      // no space to store read character
      return -(idx + 1);
    }
    c = f->read();
    switch (c) {
    case 0x0d:
      if (term == 0x0d) {
	buf[idx] = '\0';
	return idx;
      }
      if (term == 0x0d0a) {
	// ignore
	continue;
      }
      break;
    case 0x0a:
      if (term == 0x0d0a || term == 0x0a) {
	buf[idx] = '\0';
	return idx;
      }
      break;
    }
    buf[idx++] = c;
  }
  buf[idx] = '\0';
  return -(idx);
}

int n_settings_dict;
struct dict_item settings_dict[N_SETTINGS_DICT];

// vectorで動的にサイズ拡張
//std::vector<settings_entry_t> settings_dict;

void init_settings_dict() {
  n_settings_dict = 0;
  initialize_cluster2_startup_commands();
  /*
REGISTER_SETTING_OFFSET(settings_dict, plogw, my_callsign, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, sent_exch, 2, DICT_VALUE_TYPE_CHARARRAY);

REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[0], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[1], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[2], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[3], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[4], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[5], 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cw_msg[6], 2, DICT_VALUE_TYPE_CHARARRAY);

REGISTER_SETTING_AUTO(settings_dict, plogw, mask);
REGISTER_SETTING_AUTO(settings_dict, plogw, contest_id);
REGISTER_SETTING_AUTO(settings_dict, , bandmap_mask);

REGISTER_SETTING_AUTO(settings_dict, plogw, multi_type);
REGISTER_SETTING_AUTO(settings_dict, plogw, voice_memory_enable);

REGISTER_SETTING_OFFSET(settings_dict, plogw, cluster_name, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cluster_cmd, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cluster2_name, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, cluster2_cmd, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, email_addr, 2, DICT_VALUE_TYPE_CHARARRAY);

// コメントアウト部分
// REGISTER_SETTING_OFFSET(settings_dict, plogw, wifi_ssid, 2, DICT_VALUE_TYPE_CHARARRAY);
// REGISTER_SETTING_OFFSET(settings_dict, plogw, wifi_passwd, 2, DICT_VALUE_TYPE_CHARARRAY);

REGISTER_SETTING_OFFSET(settings_dict, radio_list[0], rig_name, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, radio_list[1], rig_name, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, radio_list[2], rig_name, 2, DICT_VALUE_TYPE_CHARARRAY);

REGISTER_SETTING_AUTO(settings_dict, plogw, radios_enabled);
REGISTER_SETTING_AUTO(settings_dict, plogw, radio_mode);
REGISTER_SETTING_AUTO(settings_dict, plogw, sequence_mode);

REGISTER_SETTING_AUTO(settings_dict, , callhistfn);
REGISTER_SETTING_AUTO(settings_dict, , callhist_at);
REGISTER_SETTING_OFFSET(settings_dict, plogw, power_code, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, zserver_name, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_OFFSET(settings_dict, plogw, my_name, 2, DICT_VALUE_TYPE_CHARARRAY);

REGISTER_SETTING_AUTO(settings_dict, , paddle_setting);
REGISTER_SETTING_AUTO(settings_dict, , cw_spd);
REGISTER_SETTING_AUTO(settings_dict, plogw, f_toggle_ptt_mode);
REGISTER_SETTING_AUTO(settings_dict, , kbdtype);
REGISTER_SETTING_OFFSET(settings_dict, plogw, grid_locator, 2, DICT_VALUE_TYPE_CHARARRAY);
REGISTER_SETTING_AUTO(settings_dict, , f_mux_transport);
REGISTER_SETTING_AUTO(settings_dict, , display_type);
REGISTER_SETTING_AUTO(settings_dict, , lowmem_trace);
REGISTER_SETTING_AUTO(settings_dict, plogw, f_esm);
REGISTER_SETTING_AUTO(settings_dict, plogw, show_smeter);
REGISTER_SETTING_AUTO(settings_dict, plogw, show_qso_interval);
*/

  settings_dict[n_settings_dict].name = "my_callsign";
  settings_dict[n_settings_dict].value = (void *)plogw->my_callsign + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "sent_exch";
  settings_dict[n_settings_dict].value = (void *)plogw->sent_exch + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_1";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[0] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_2";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[1] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_3";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[2] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_4";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[3] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_5";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[4] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_6";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[5] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_7";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[6] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_1";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[0] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_2";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[1] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_3";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[2] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_4";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[3] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_5";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[4] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_6";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[5] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rtty_msg_7";
  settings_dict[n_settings_dict].value = (void *)plogw->rtty_msg[6] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "dupechk_mask";
  settings_dict[n_settings_dict].value = (void *)&plogw->mask;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "contest_id";
  settings_dict[n_settings_dict].value = (void *)&plogw->contest_id;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "bandmap_mask";
  settings_dict[n_settings_dict].value = (void *)&bandmap_mask;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "bandmap_lifetime_minutes";
  settings_dict[n_settings_dict].value = (void *)&bandmap_lifetime_minutes;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "multi_type";
  settings_dict[n_settings_dict].value = (void *)&plogw->multi_type;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;


  settings_dict[n_settings_dict].name = "voice_memory_enable";
  settings_dict[n_settings_dict].value = (void *)&plogw->voice_memory_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster_name";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster1_auto_enable";
  settings_dict[n_settings_dict].value = (void *)&cluster1_auto_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster_cmd";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster_cmd + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_name";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster2_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_auto_enable";
  settings_dict[n_settings_dict].value = (void *)&cluster2_auto_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_cmd";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster2_cmd + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_startup_cmd_1";
  settings_dict[n_settings_dict].value = (void *)cluster2_startup_cmd[0] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_startup_cmd_2";
  settings_dict[n_settings_dict].value = (void *)cluster2_startup_cmd[1] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_startup_cmd_3";
  settings_dict[n_settings_dict].value = (void *)cluster2_startup_cmd[2] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_startup_cmd_4";
  settings_dict[n_settings_dict].value = (void *)cluster2_startup_cmd[3] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster2_startup_cmd_5";
  settings_dict[n_settings_dict].value = (void *)cluster2_startup_cmd[4] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "email_addr";
  settings_dict[n_settings_dict].value = (void *)plogw->email_addr + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  //  settings_dict[n_settings_dict].name = "wifi_ssid";
  //  settings_dict[n_settings_dict].value = (void *)plogw->wifi_ssid + 2;
  //  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  //  n_settings_dict++;

  //  settings_dict[n_settings_dict].name = "wifi_passwd";
  //  settings_dict[n_settings_dict].value = (void *)plogw->wifi_passwd + 2;
  //  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  //  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "rig_name_1";
  settings_dict[n_settings_dict].value = (void *)radio_list[0].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rig_name_2";
  settings_dict[n_settings_dict].value = (void *)radio_list[1].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rig_name_3";
  settings_dict[n_settings_dict].value = (void *)radio_list[2].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "radios_enabled";
  settings_dict[n_settings_dict].value = (void *)&plogw->radios_enabled;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

 
  settings_dict[n_settings_dict].name = "radio_mode";
  settings_dict[n_settings_dict].value = (void *)&so2r.radio_mode;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "sequence_mode";
  settings_dict[n_settings_dict].value = (void *)&plogw->sequence_mode;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "callhistfn";
  settings_dict[n_settings_dict].value = (void *)callhistfn;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "callhist_at";
  settings_dict[n_settings_dict].value = (void *)&callhist_at;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "dupechk_at";
  settings_dict[n_settings_dict].value = (void *)&dupechk->dupechk_at;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "power_code";
  settings_dict[n_settings_dict].value = (void *)&plogw->power_code + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "hostname";
  settings_dict[n_settings_dict].value = (void *)&plogw->hostname + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "zserver_name";
  settings_dict[n_settings_dict].value = (void *)&plogw->zserver_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "zserver_auto_enable";
  settings_dict[n_settings_dict].value = (void *)&zserver_auto_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "my_name";
  settings_dict[n_settings_dict].value = (void *)&plogw->my_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "paddle_setting";
  settings_dict[n_settings_dict].value = (void *)&paddle_setting;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_spd";
  settings_dict[n_settings_dict].value = (void *)&cw_spd;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "f_toggle_ptt_mode";
  settings_dict[n_settings_dict].value = (void *)&(plogw->f_toggle_ptt_mode);
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "kbdtype";
  settings_dict[n_settings_dict].value = (void *)&(kbdtype);
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "grid_locator";
  settings_dict[n_settings_dict].value = (void *)&plogw->grid_locator+2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "f_mux_transport";
  settings_dict[n_settings_dict].value = (void *)&f_mux_transport;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "display_type";
  settings_dict[n_settings_dict].value = (void *)&display_type;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "clock_display_mode";
  settings_dict[n_settings_dict].value = (void *)&clock_display_mode;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rig_clock_sync";
  settings_dict[n_settings_dict].value = (void *)&rig_clock_sync;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "so2r_pair_a";
  settings_dict[n_settings_dict].value = (void *)so2r.so2r_pair_a_setting_ptr();
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "so2r_pair_b";
  settings_dict[n_settings_dict].value = (void *)so2r.so2r_pair_b_setting_ptr();
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "esm_mode";
  settings_dict[n_settings_dict].value = (void *)&plogw->f_esm;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "wipe_key_swap";
  settings_dict[n_settings_dict].value = (void *)&plogw->wipe_key_swap;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "show_smeter";
  settings_dict[n_settings_dict].value = (void *)&plogw->show_smeter;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "show_qso_interval";
  settings_dict[n_settings_dict].value = (void *)&plogw->show_qso_interval;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  // CW S&P XIT offsets are persistent per radio.  XITON/XITOFF only
  // control whether the saved offset is applied; XITRESET clears it.
  settings_dict[n_settings_dict].name = "xit_offset_radio0";
  settings_dict[n_settings_dict].value = (void *)&radio_list[0].xit_offset_hz;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "xit_offset_radio1";
  settings_dict[n_settings_dict].value = (void *)&radio_list[1].xit_offset_hz;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "xit_offset_radio2";
  settings_dict[n_settings_dict].value = (void *)&radio_list[2].xit_offset_hz;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "autopoweroff";
  settings_dict[n_settings_dict].value = (void *)&plogw->autopoweroff;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "antctrl";
  settings_dict[n_settings_dict].value = (void *)&antenna_control_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "anthost";
  settings_dict[n_settings_dict].value = (void *)antenna_host;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "antport";
  settings_dict[n_settings_dict].value = (void *)&antenna_port;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  for (int i = 0; i < ANTENNA_PREF_ROWS; ++i) {
    static const char *pref_names[ANTENNA_PREF_ROWS] = {"antpref1", "antpref2", "antpref3"};
    settings_dict[n_settings_dict].name = pref_names[i];
    settings_dict[n_settings_dict].value = (void *)antenna_pref[i];
    settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
    n_settings_dict++;
  }

  for (int i = 0; i < ANTENNA_MAX_ID; ++i) {
    static const char *ant_names[ANTENNA_MAX_ID] = {
      "antname1", "antname2", "antname3", "antname4", "antname5",
      "antname6", "antname7", "antname8", "antname9"
    };
    settings_dict[n_settings_dict].name = ant_names[i];
    settings_dict[n_settings_dict].value = (void *)antenna_name[i];
    settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
    n_settings_dict++;
  }

  settings_dict[n_settings_dict].name = "rtty_lead_ms";
  settings_dict[n_settings_dict].value = (void *)&rtty_ptt_lead_ms;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "sat_tle_url";
  settings_dict[n_settings_dict].value = (void *)sat_tle_url;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "";
  settings_dict[n_settings_dict].value = NULL;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  console->print("n_settings_dict=");
  console->print(n_settings_dict);
  console->print(" N_SETTINGS_DICT=");    
  console->println(N_SETTINGS_DICT);    
}


void clear_string(char *s)
{
  char *p;
  p=s;
  while (*p!='\0') {
    *p='\0';
    p++;
  }
}

// assign value from read line according to the dictionary
int assign_settings(char *line, struct dict_item *dict) {
  char *p, *arg, *value;
  p = strtok(line, " ");
  if (p == NULL) {
    return 0;
  }
  arg = p;
  p = strtok(NULL, "");
  if (p == NULL) return 0;
  value = p;

  while ((*dict).value != NULL) {
    if (strcmp((*dict).name, arg) == 0) {
      switch ((*dict).value_type) {
      case DICT_VALUE_TYPE_INT:
	*(int *)((*dict).value) = atoi(value);
        if ((*dict).value == (void *)&cw_spd) clamp_cw_speed();
	break;
      case DICT_VALUE_TYPE_CHARARRAY:
	// clear variable string with '\0'
	clear_string((char *)((*dict).value));
	*(char *)((*dict).value)='\0';
	strcat((char *)((*dict).value), value);
	break;
      }
      return 1;
    }
    dict++;
  }
  return 0;
}


// dump the dictionary values according to dictionary into given file f
int dump_settings(Stream *f, struct dict_item *dict) {
  if (!plogw->f_console_emu) plogw->ostream->println("dump_settings");
  while ((*dict).value != NULL) {

    f->print((*dict).name);
    f->print(" ");
    switch ((*dict).value_type) {
    case DICT_VALUE_TYPE_INT:
      f->println(*(int *)((*dict).value));
      break;
    case DICT_VALUE_TYPE_CHARARRAY:
      f->println((char *)((*dict).value));
      break;
    }
    dict++;  // to the next value
  }
  return 0;
}


int load_settings(const char *fn) {
  int display_type_bak=display_type;
  char fnbuf[30];
  plogw->ostream->print("load settings from:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/settings");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);
  // f = SPIFFS.open(fnbuf, FILE_READ);
  f = SD.open(fnbuf, FILE_READ);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file settings.txt for reading");
    return 0;
  }
  while (readline(&f, buf, 0x0d0a, 128) != 0) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("line:");
      plogw->ostream->print(buf);
      plogw->ostream->println(":");
    }
    assign_settings(buf, settings_dict);
  }
  f.close();

  // post process
  if (bandmap_lifetime_minutes < 1) bandmap_lifetime_minutes = 1;
  if (bandmap_lifetime_minutes > 1440) bandmap_lifetime_minutes = 1440;

  radio_list[0].enabled = plogw->radios_enabled & 1;
  radio_list[1].enabled = (plogw->radios_enabled >> 1) & 1;
  radio_list[2].enabled = (plogw->radios_enabled >> 2) & 1;
  so2r.validate_so2r_pairs();
  set_rig_from_name(&radio_list[0]);
  set_rig_from_name(&radio_list[1]);
  set_rig_from_name(&radio_list[2]);

  /*
  switch (dupechk->dupechk_at) {
  case 0:
  default:
    init_dupechk_maincpu();
    break;
  case 1:
    init_dupechk_subcpu();    
    break;
  }
  */
  
  if (display_type!=display_type_bak) {
    console->print("initializing display =");
    console->println(display_type);
    init_display();
  }
  // grid_locator
  set_grid_locator_information();

  // set sequence_mode according to the radio_mode
  switch(so2r.radio_mode) {
  case SO2R::RADIO_MODE_SAT:
  case SO2R::RADIO_MODE_SO1R:
    so2r.sequence_mode(SO2R::Manual);
    break;
  case SO2R::RADIO_MODE_SO2R:
    so2r.sequence_mode(SO2R::SO2R_CQSandP);
    break;
  }

  // set so2r state 
  so2r.set_status();

  // keyboard type 
  switch (kbdtype) {
  case 0: // us
    Prs.us();
    Prs1.us();        
    break;
  case 1: // jp
    Prs.jp();
    Prs1.jp();        
    break;
  }

  // mux transport
  //
  // A saved f_mux_transport=0 is a legacy raw/no-MUX setting.  KBD and
  // remote DUPE/CALLHIST now share the SUBCPU MUX, so restoring 0 after a
  // successful startup probe would immediately tear that link down.
  // Migrate legacy settings to MUX mode at load time.
  if (f_mux_transport == 0) {
    f_mux_transport = 1;
  }
  f_mux_transport_cmd=1;
  
  // iambic keyer
  set_paddle();

  //  multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);  // do not try to remember wifi settings here but wifiset.txt
  set_contest_id();
  cluster1_auto_enable = cluster1_auto_enable ? 1 : 0;
  cluster2_auto_enable = cluster2_auto_enable ? 1 : 0;
  zserver_auto_enable = zserver_auto_enable ? 1 : 0;
  set_cluster();
  set_cluster2();
  set_cluster_auto(1, cluster1_auto_enable);
  set_cluster_auto(2, cluster2_auto_enable);
  reconnect_zserver();
  set_zserver_auto(zserver_auto_enable);
  antenna_settings_changed();
  
  return 1;
}


int save_settings(const char *fn) {
  char fnbuf[30];
  plogw->ostream->print("save settings to:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/settings");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);

  //  f = SPIFFS.open(settingsfn, FILE_WRITE);
  //  f = SPIFFS.open(fnbuf, FILE_WRITE);
  f = SD.open(fnbuf, FILE_WRITE);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file settings.txt for writing");
    return 0;
  }


  // before saving set variables
  // plogw->radios_enabled
  plogw->radios_enabled = radio_list[0].enabled | (radio_list[1].enabled << 1) | (radio_list[2].enabled << 2);


  if (!plogw->f_console_emu) plogw->ostream->println("dump_settings");
  int i;
  i = 0;
  while (settings_dict[i].value != NULL) {
    console->print(i);
    console->print(":");
    if (*settings_dict[i].name=='\0') break;
    console->print(settings_dict[i].name);
    console->println(" ");
    //    console->print(settings_dict[i].value);    
    if (!plogw->f_console_emu) {
      plogw->ostream->print(settings_dict[i].name);
      plogw->ostream->print(" ");
    }
    f.print(settings_dict[i].name);
    f.print(" ");
    switch (settings_dict[i].value_type) {
    case DICT_VALUE_TYPE_INT:
      if (!plogw->f_console_emu) plogw->ostream->println(*(int *)(settings_dict[i].value));
      f.println(*(int *)(settings_dict[i].value));
      break;
    case DICT_VALUE_TYPE_CHARARRAY:
      if (!plogw->f_console_emu) plogw->ostream->println((char *)(settings_dict[i].value));
      f.println((char *)(settings_dict[i].value));
      break;
    }
    i++;  // to the next value
  }


  //dump_settings(&f, settings_dict);

  // output options
  /* char cluster_name[LEN_HOST_NAME+3];
     char my_callsign[LEN_CALL_WINDOW + 3];
     char recv_exch[LEN_EXCH_WINDOW + 3];
     char jcc[LEN_EXCH_WINDOW + 3];
     char rig_name[LEN_RIG_NAME + 3];
     char cw_msg[N_CWMSG][LEN_CWMSG_WINDOW + 3];


     // contest type related
     int mask ; // dupe check mask PH/CW 0xff dupe in CW and PH 0xff-3 dupe in operating mode
     int contest_id; // contest id number

     char grid_locator[LEN_GL + 3]; // current location  grid locator  ... editable

     int  sat_freq_mode; // 0... rig freq indipendent of sat  1.. receive frequency fixed transmit freq control  2.. transmission freq fixed receive freq control
     char sat_name[LEN_SATNAME_WINDOW + 3];  //

     // rig type 0 IC-705 TX/TRX (RX kenwood USB connected?) 1 IC-9700 (main tx /sub rx)
     int rig_type;
  */
  //   f.print("my_callsign ");   f.println(plogw->my_callsign+2);
  //   f.print("recv_exch ");   f.println(plogw->recv_exch+2);

  f.close();
  if (!plogw->f_console_emu) plogw->ostream->println("saving finished.");
  return 1;
}


void set_grid_locator_information()
{
    if (strlen(plogw->grid_locator + 2) != 0) {
      // grid_locator entered
      strcpy(plogw->grid_locator_set, plogw->grid_locator + 2);
      set_location_gl_calc(plogw->grid_locator_set);
    }
}

int load_boot_display_type(const char *path)
{
  File file = SD.open(path, FILE_READ);
  if (!file) {
    console->println("boot display setting not found; using default");
    return 0;
  }

  char line[128];

  while (readline(&file, line, 0x0d0a, sizeof(line)) != 0) {
    char *p = line;

    while (*p == ' ' || *p == '\t') {
      ++p;
    }

    if (strncmp(p, "display_type", 12) != 0) {
      continue;
    }

    p += 12;

    if (*p != ' ' && *p != '\t' && *p != '=') {
      continue;
    }

    while (*p == ' ' || *p == '\t' || *p == '=') {
      ++p;
    }

    int value = atoi(p);
    if (value < 0 || value > 2) {
      console->printf("invalid boot display_type=%d; using default\n",
                      value);
      break;
    }

    display_type = value;
    console->printf("boot display_type=%d\n", display_type);
    file.close();
    return 1;
  }

  file.close();
  return 0;
}
