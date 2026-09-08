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

// common global variables 
#include "Arduino.h"
#include "decl.h"
#include "FS.h"
//#include "DS3231.h"
#include "RTClib.h"

#include <WiFi.h>  // for WiFi shield


// usb task free memory
int usb_task_memory_watermark=0;
int kbdtype=0;
bool f_cardkey_present=1;

int main_loop_revs=0;
File f;
//char buf[512];
char buf[600];
union qso_union_tag qso;  // data is delimited by space in the file

struct score score;
int verbose = 0;  // debug info level
// RTTY PTT-to-first-bit delay.  This is deliberately global rather than
// rig-specific; the FSK output itself is selected per rig with CW:0..4.
int rtty_ptt_lead_ms = 100;
// to test suppress limiting JA temporalily
//int timeout_rtc = 0;
int timeout_interval = 0;
int timeout_interval_minute = 0;  // interval job every minute
int timeout_interval_sat = 0;     // interval job for satellite orbit management
int timeout_cat = 0;
int timeout_second = 0;  // interval job every second
int timeout_rig_disable_temporally = 0; // interval job, temporally disabling rig
int temporarily_disabled_radio = -1; // radio disabled by Alt-I only; -1 means none

int f_printkey=0;
int f_show_clock = 0;
DateTime rtctime, ntptime;

const char *mode_str[NMODEID] = { "CW", "CW-R", "LSB", "USB", "FM", "AM", "RTTY", "RTTY-R" };
// mode code given from rig ci-v
int modetype[NMODEID] = { LOG_MODETYPE_CW, LOG_MODETYPE_CW, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_DG, LOG_MODETYPE_DG };  // RTTY and RTTY-R are both digital/FSK
const char *modetype_str[4] = { "*", "CW", "PH", "DG" };
/// call buffer
struct logwindow logw;
struct logwindow *plogw;
int f_spiram;
bool f_low_memory_mode = false;
int lowmem_trace = 0;  // 0: suppress LOWMEM heap trace, 1: enable
struct disp disp;
struct disp *dp;
struct sat_info sat_info[N_SATELLITES];
int n_satellites = 0;


const char *sat_names[N_SATELLITES] = {
  "AO-07", "AO-27", "FO-29", "ISS", "AO-73",
  "XW-2A", "XW-2C", "XW-2D", "XW-2E", "XW-2F",
  "XW-2B", "CAS-4B", "CAS-4A", "AO-92", "RS-44",
  "EO-88", "JO-97", "AO-109", "FO-99", "HO-113",
  "IO-117", "FO-118", "CAS-10",
  ""
};
bool f_sat_updated = 0;
char sat_tle_url[192] = "http://www.amsat.org/tle/current/nasabare.txt";
volatile bool sat_tle_update_requested = false;
volatile bool sat_tle_update_in_progress = false;
int sat_tle_last_result = 0;


char tcp_ringbuf_buf[NCHR_TCP_RINGBUF];
// SO3R
struct radio radio_list[3]; //, *radio_selected;

struct rig rig_spec[N_RIG];  // specification of the rig_id th rig


bool f_capslock = 0;
struct info_display info_disp;

const char *settingsfn = "/settings.txt";

struct bandmap bandmap[N_BAND + 1];
int bandmap_mask = 0;  // suppress updating bandmap from telnet cluster if the corresponding bit 1<<(bandid-1) is set
int bandmap_lifetime_minutes = 20; // remove spots older than this many minutes
struct bandmap_disp bandmap_disp;

int display_type=0; // 0 1.3" display 1 2.4" display 2 1.3" but flipped 
//#if JK1DVPLOG_HWVER >= 3
//int display_type=1; // 0 1.3" display 1 2.4" display 2 1.3" but flipped 
//#else
//int display_type=2; // 0 1.3" display 1 2.4" display 2 1.3" but flipped 
//#endif
bool display_flip=0; // 2.4" display
bool display_swap=0; // 2.4" display
//bool display_flip=1; // 1.3" display
int wifi_timeout = 0;
int wifi_enable = 1;
int wifi_count = 0;
int wifi_status = 0;
int count = 0;
int rtcadj_count = 0;
int clock_display_mode = 0; // 0: JST (internal UTC+9), 1: UTC display
int rig_clock_sync = 1;     // 1: sync supported Icom rig clock on CI-V connect/reconnect

//int callhistf_stat = 0;  // 0 not open 1 open for reading 2 open for writing
char qsologfn[20];    // qso log filename (append)
char callhistfn[20];  // call history file to read
int callhist_at=1; // 0: MAIN, 1: SUBCPU (default: SUBCPU)
int dupechk_at=1; // 0: MAIN, 1: SUBCPU 2:this is SUBCPU (default: SUBCPU)

