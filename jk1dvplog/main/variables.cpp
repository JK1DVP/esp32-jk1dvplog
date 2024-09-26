/* Copyright (c) 2021-2024 Eiichiro Araki
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.
*/

// common global variables 
#include "Arduino.h"
#include "decl.h"
#include "FS.h"
//#include "DS3231.h"
#include "RTClib.h"

#include <WiFi.h>  // for WiFi shield

int main_loop_revs=0;
File f;
//char buf[512];
char buf[600];
union qso_union_tag qso;  // data is delimited by space in the file

struct score score;
int verbose = 0;  // debug info level
int enable_usb_keying = 0;  // if enabled, DTR ON/OFF keying in IC-705
// to test suppress limiting JA temporalily
int timeout_rtc = 0;
int timeout_interval = 0;
int timeout_interval_minute = 0;  // interval job every minute
int timeout_interval_sat = 0;     // interval job for satellite orbit management
int timeout_cat = 0;
int timeout_second = 0;  // interval job every second

int f_printkey=0;
int f_show_clock = 0;
DateTime rtctime, ntptime;

const char *mode_str[NMODEID] = { "CW", "CW-R", "LSB", "USB", "FM", "AM", "RTTY" };
// mode code given from rig ci-v
int modetype[NMODEID] = { LOG_MODETYPE_CW, LOG_MODETYPE_CW, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_PH, LOG_MODETYPE_DG };  // 0 .. CW 1.. phone 2.. DIGI
const char *modetype_str[4] = { "*", "CW", "PH", "DG" };
/// call buffer
struct logwindow logw;
struct logwindow *plogw;

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


char tcp_ringbuf_buf[NCHR_TCP_RINGBUF];
// SO3R
struct radio radio_list[3], *radio_selected;

struct rig rig_spec[N_RIG];  // specification of the rig_id th rig


bool f_capslock = 0;
struct info_display info_disp;

const char *settingsfn = "/settings.txt";

struct bandmap bandmap[N_BAND];
int bandmap_mask = 0;  // suppress updating bandmap from telnet cluster if the corresponding bit 1<<(bandid-1) is set
struct bandmap_disp bandmap_disp;

struct dupechk dupechk;

bool display_flip=1;
int wifi_timeout = 0;
int wifi_enable = 1;
int wifi_count = 0;
int wifi_status = 0;
int count = 0;
int rtcadj_count = 0;

//int callhistf_stat = 0;  // 0 not open 1 open for reading 2 open for writing
char qsologfn[20];    // qso log filename (append)
char callhistfn[20];  // call history file to read

