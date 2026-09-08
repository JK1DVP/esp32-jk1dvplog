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

#ifndef FILE_VARIABLES_H
#define FILE_VARIABLES_H
#include "FS.h"
extern int  display_type; // 0 1.3" display 1 2.4" display
extern int usb_task_memory_watermark;
extern int kbdtype;
extern int main_loop_revs;
extern bool f_cardkey_present;
extern union qso_union_tag qso;  // data is delimited by space in the file
extern int f_spiram;
extern bool f_low_memory_mode;
extern int lowmem_trace;
//#include "DS3231.h"
#include "RTClib.h"
extern class DateTime rtctime, ntptime;
extern File f;
extern char buf[512];
extern struct score score;
extern int verbose;  // debug info level
extern int f_printkey;
extern bool f_capslock;  // keyboard 
// to test suppress limiting JA temporalily
extern int timeout_rtc ;
extern int timeout_interval ;
extern int timeout_interval_minute ;  // interval job every minute
extern int timeout_interval_sat ;     // interval job for satellite orbit management
extern int timeout_cat ;
extern int timeout_second ;  // interval job every second
extern int timeout_rig_disable_temporally;
extern int temporarily_disabled_radio;
extern int f_show_clock  ;

extern const char *mode_str[NMODEID] ;
// mode code given from rig ci-v
extern int modetype[NMODEID] ;
extern const char *modetype_str[4] ;
/// call buffer
extern struct logwindow logw;
extern struct logwindow *plogw;

extern struct disp disp;
extern struct disp *dp;


extern struct sat_info sat_info[N_SATELLITES];
extern int n_satellites;
extern const char *sat_names[N_SATELLITES];
extern bool f_sat_updated ;
extern char sat_tle_url[192];
extern volatile bool sat_tle_update_requested;
extern volatile bool sat_tle_update_in_progress;
extern int sat_tle_last_result;


extern char tcp_ringbuf_buf[NCHR_TCP_RINGBUF];
// SO3R
extern struct radio radio_list[3], *radio_selected;
extern struct rig rig_spec[N_RIG];  // specification of the rig_id th rig

extern struct info_display info_disp;

extern const char *settingsfn ;
extern struct bandmap bandmap[N_BAND + 1];
extern int bandmap_mask ;  // suppress updating bandmap from telnet cluster if the corresponding bit 1<<(bandid-1) is set
extern int bandmap_lifetime_minutes;
extern struct bandmap_disp bandmap_disp;

extern struct dupechk *dupechk;
extern bool display_flip;
extern bool display_swap;
extern int wifi_timeout ;
extern int wifi_enable;
extern int wifi_count ;
extern int wifi_status;
extern int count ;
extern uint8_t *dispbuf_r, *dispbuf_l;
extern int rtty_ptt_lead_ms;
extern int rtcadj_count;
extern int clock_display_mode; // 0: JST (internal UTC+9), 1: UTC display
extern int rig_clock_sync;     // auto-sync supported Icom clocks on CI-V connect/reconnect
extern int callhistf_stat ;  // 0 not open 1 open for reading 2 open for writing
extern char qsologfn[20];    // qso log filename (append)
extern char callhistfn[20];  // call history file to read
extern int callhist_at; // 0: MAIN, 1: SUBCPU

#endif
