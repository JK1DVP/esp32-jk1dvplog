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

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "SD.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "satellite.h"
#include "cat.h"
#include "settings.h"
#include "display.h"
#include <HTTPClient.h>
#include "Plan13.h"
#include "timekeep.h"
HTTPClient http;
#include <maidenhead.h>
/// sattracking based on https://www.amsat.org/amsat/articles/g3ruh/111.html

uint8_t *buff, *buff1;
char *tle1_line, *tle2_line, *satname_line;
char *tlefilename = "/tle.txt";
File tlefile;

// index of sat_info[] sorted by  satellite aos
int satidx_sort[N_SATELLITES];

void load_satinfo() {
  char fnbuf[30];
  strcpy(fnbuf, "/satinfo.txt");
  // f = SPIFFS.open(fnbuf, FILE_READ);

  plogw->ostream->println("opening satinfo");      
  f = SD.open(fnbuf, FILE_READ);
  if (!f) {
    sprintf(dp->lcdbuf, "Creating default satinfo\n");
    upd_display_info_flash(dp->lcdbuf);
    plogw->ostream->println("Creating default satinfo");
    save_satinfo();
    plogw->ostream->println("opening satinfo again");    
    f = SD.open(fnbuf, FILE_READ);    
  }
  char *satname, *s1;
  int i, n;
  while (readline(&f, buf, 0x0d0a, 128) != 0) {
    // read name and offset_freq
    satname = strtok(buf, " ");
    if (satname == NULL) break;
    i = find_satname(satname);
    plogw->ostream->print("sat ");
    plogw->ostream->println(satname);
    if (i == -1) {
      // not found
      plogw->ostream->print("satellite ");
      plogw->ostream->print(satname);
      plogw->ostream->println(" not found");
      continue;
    }
    s1 = strtok(NULL, " ");
    n = atoi(s1);
    sat_info[i].offset_freq = n;
    plogw->ostream->print("sat ");
    plogw->ostream->print(satname);
    plogw->ostream->print(" ofs=");
    plogw->ostream->println(n);
  }

  f.close();
}




// name up_low up_high  up_mode down_low down_high down_mode beacon(MHz) offset(kHz)
// 複数のトラポンが搭載されている衛星は "sat_name,L" のように記述することでトラポン周波数の設定を区別する。
// 軌道情報(sat_info[])は、複数の同じsat_name のエントリーに記録することが必要となる。
// 決まりを決めただけでまだ実装していない FO-118 はとりあえずリニアトラポンだけ記述 23/1/3
const struct sat_info2 sat_info2[N_SATELLITES] = {
  { "FO-29", 145.900, 146.000, "LSB", 435.800, 435.900, "USB", 435.795, 2.4 },
  {"AO-73", 435.130, 435.150, "LSB", 145.950, 145.970, "USB", 0.0, 1.5},
  { "AO-07", 145.850, 145.950, "USB", 29.400, 29.500, "USB", 29.502, 0.0 },
  { "XW-2A", 435.030, 435.050, "LSB", 145.665, 145.685, "USB", 145.660, -1.3 },
  {"XW-2B", 435.090, 435.110, "LSB", 145.730, 145.750, "USB", 145.725, 0.0},
  {"XW-2C", 435.150, 435.170, "LSB", 145.795, 145.815, "USB", 145.790, 0.0},
  {"XW-2D", 435.210, 435.230, "LSB", 145.860, 145.880, "USB", 145.855, 0.0},
  // {"XW-2F", 435.330, 435.350, "LSB", 145.980, 146.000, "USB", 145.975, 0.0},
  {"MESAT1", 145.910, 145.940, "LSB", 435.810, 435.840, "USB", 435.800, 0.0},  
  { "RS-44", 145.935, 145.995, "LSB", 435.610, 435.670, "USB", 435.605, -0.45 },
  { "EO-88", 435.015, 435.045, "LSB", 145.960, 145.990, "USB", 0.0, 0.1 },
  { "CAS-4A", 435.210, 435.230, "LSB", 145.860, 145.880, "USB", 145.855, -1.1 },
  { "CAS-4B", 435.270, 435.290, "LSB", 145.915, 145.935, "USB", 145.910, -1.5 },
  { "JO-97", 435.100, 435.120, "LSB", 145.855, 145.875, "USB", 145.840, -1.8 },
  { "FO-99", 145.900, 145.930, "LSB", 435.880, 435.910, "USB", 437.075, -1.0 },
  { "HO-113", 145.855, 145.885, "LSB", 435.195, 435.165, "USB", 435.575, 0.8 },
  { "ISS", 144.49, 145.80, "FM", 145.80, 437.80, "FM", 0.0, 0.0 },
  { "IO-117", 435.300, 435.320, "USB", 435.300, 435.320, "USB", 0, 0.0 },
  { "FO-118", 145.805, 145.835, "LSB", 435.525, 435.555, "USB", 435.570, 0.0 },
  { "CAS-10", 145.855, 145.885, "LSB", 435.195, 435.165, "USB", 435.575, 0.0 },
  { "", 0.0, 0.0, "", 0.0, 0.0, "", 0.0, 0.0 }  // end
};

void set_sat_index(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {
    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println("Not found");
    }
    return;
  } else {
    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println(plogw->sat_idx_selected);
    }
    plogw->sat_idx_selected = i;
  }
}


// adjust
void adjust_sat_offset(int dfreq) {
  if (plogw->sat) {
    int i;
    i = plogw->sat_idx_selected;
    if (i < 0) return;
    sat_info[i].offset_freq += dfreq;
    if (!plogw->f_console_emu) {
      plogw->ostream->print("Sat freq offset=");
      plogw->ostream->println(sat_info[i].offset_freq);
    }
    save_satinfo();
    set_sat_freq_calc();
  }
}

void set_sat_info2(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {

    if (verbose & 8) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println("Not found");
    }
    return;
  }

  for (int j = 0; j < N_SATELLITES; j++) {
    if (sat_info2[j].name[0] == '\0') break;
    if (strcmp(sat_info2[j].name, sat_name) == 0) {
      // set j th of sat_info2
      sat_info[i].up_f0 = int(sat_info2[j].up_f0 * 1000000.);
      sat_info[i].up_f1 = int(sat_info2[j].up_f1 * 1000000.);
      sat_info[i].dn_f0 = int(sat_info2[j].dn_f0 * 1000000.);
      sat_info[i].dn_f1 = int(sat_info2[j].dn_f1 * 1000000.);
      sat_info[i].bc_f0 = int(sat_info2[j].bc_f0 * 1000000.);
      sat_info[i].offset_freq = int(sat_info2[j].offset_freq * 1000.);
      strcpy(sat_info[i].up_mode, sat_info2[j].up_mode);
      strcpy(sat_info[i].dn_mode, sat_info2[j].dn_mode);

      return;
    }
  }
}

void set_sat_info_calc() {
  int i;
  i = plogw->sat_idx_selected;
  if (i < 0) return;

  if (verbose & 8) {
    plogw->ostream->print("set_sat_info_calc():");
    plogw->ostream->println(plogw->sat_name_set);
  }
  p13.setElements(
    sat_info[i].YEAR,
    sat_info[i].EPOCH,
    sat_info[i].INCLINATION,
    sat_info[i].RAAN,
    sat_info[i].ECCENTRICITY * ONEPPTM,
    sat_info[i].ARGUMENT_PEDIGREE,
    sat_info[i].MEAN_ANOMALY,
    sat_info[i].MEAN_MOTION,
    sat_info[i].TIME_MOTION_D,
    sat_info[i].EPOCH_ORBIT,
    180);
}



#define TX 1
#define RX 0

void set_vfo_frequency_rig(int freq, int vfo, struct radio *radio)
// vfo 0 for main 1 for sub (unselected) vfo selection
{
  if (!radio->enabled) {
    if (verbose & 8) {
      plogw->ostream->print("Rig #");
      plogw->ostream->print(radio->rig_idx);
      plogw->ostream->println("is not enabled. not set freq.");
    }
    return;
  }
  if (verbose & 8) {
    plogw->ostream->print("set_vfo_frequency_rig(): vfo ");
    plogw->ostream->print(vfo);
    plogw->ostream->print(":");
    plogw->ostream->print("RIG idx:");
    plogw->ostream->print(radio->rig_idx);
    plogw->ostream->print(" freq ");
    plogw->ostream->print(freq);
  }

  switch (radio->rig_spec->rig_type) {
    case 0:  // IC-705 VFO sel/unselected to set RX/TX frequency
      if (verbose & 8) plogw->ostream->print(" IC-705 ( sel RX unsel TX )");
      send_head_civ(radio);
      add_civ_buf((byte)0x25);  // Frequency !
      switch (vfo) {
        case 1:
          if (verbose & 8) plogw->ostream->print(" flip vfo selection");
          add_civ_buf((byte)(1 - vfo));  // flip vfo selection
          break;
        case 0:
          if (verbose & 8) plogw->ostream->print(" normal vfo selection");
          add_civ_buf((byte)vfo);
          break;
      }
      //      civport->write((byte)vfo);
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10 Hz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 100kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10MHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1GHz
      send_tail_civ(radio);
      break;

    case 1:  // IC-9700 main (TX) and sub (RX) frequencies  normally select sub
      if (verbose & 8) plogw->ostream->print(" IC-9700 main TX sub RX");
      // check plogw->freq if correct frequency band assignment is made

      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo
      switch (vfo) {
        case 1:  // select sub
          if (verbose & 8) plogw->ostream->print(" select sub (RX)");
          add_civ_buf((byte)0xd1);
          break;
        case 0:  // select main
          if (verbose & 8) plogw->ostream->print(" select main (TX)");
          add_civ_buf((byte)0xd0);
          break;
      }
      send_tail_civ(radio);

      // set frequency
      send_head_civ(radio);
      add_civ_buf((byte)0x05);  // Frequency !
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10 Hz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 100kHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10MHz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1GHz
      send_tail_civ(radio);

      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo to SUB
      add_civ_buf((byte)0xd1);
      send_tail_civ(radio);
      break;

    case 2:  // FT-991A  Yaesu cat
    case 3:  // QCX-mini Kenwood cat
      // currently just send frequency for these rig types
      // no VFO selection. 22/8/25
      int type;
      type = radio->rig_spec->cat_type;
      char buf[30];
      switch (type) {
        case 1:
          if (verbose & 8) plogw->ostream->print(" Yaesu ");
          if (freq < 0) return;
          sprintf(buf, "FA%09d;", freq);
          send_cat_cmd(radio, buf);
          return;
        case 2:
          // kenwood TS480 uses 11 digit
          if (verbose & 8) plogw->ostream->print(" Kenwood ");
          if (freq < 0) return;
          sprintf(buf, "FA%011d;", freq);
          send_cat_cmd(radio, buf);
          return;
        case 3:
          return;
      }
      break;


    case 4:  // manual  ( ignore )
      if (verbose & 8) plogw->ostream->print(" manual rig not set freq.");
      break;
  }
  if (verbose & 8) plogw->ostream->println("");
}

// set_sat_freq_calc で計算した周波数をリグに適宜セットする
void set_sat_freq_to_rig_vfo(int freq, int tx)
// tx if 1 set freq to TX designated vfo for the rig
{
  struct radio *radio;
  switch (plogw->sat_vfo_mode) {
    case SAT_VFO_MULTI_TX_0:  // rig0 TX rig1 RX
      if (tx) {
        radio = &radio_list[0];
        set_vfo_frequency_rig(freq, 0, radio);
      } else {
        radio = &radio_list[1];
        set_vfo_frequency_rig(freq, 0, radio);
      }
      break;
    case SAT_VFO_MULTI_TX_1:  // rig0 RX rig1 TX
      if (tx) {
        radio = &radio_list[1];
        set_vfo_frequency_rig(freq, 0, radio);
      } else {
        radio = &radio_list[0];
        set_vfo_frequency_rig(freq, 0, radio);
      }
      break;

    case SAT_VFO_SINGLE_A_TX:  // VFO A is TX
      radio = &radio_list[0];
      if (tx) {
        set_vfo_frequency_rig(freq, 0, radio);
      } else {
        set_vfo_frequency_rig(freq, 1, radio);
      }
      break;
    case SAT_VFO_SINGLE_A_RX:  // VFO A RX  VFO B TX (split)
      radio = &radio_list[0];
      if (tx) {
        set_vfo_frequency_rig(freq, 1, radio);
      } else {
        set_vfo_frequency_rig(freq, 0, radio);
      }
      break;
  }
}

void set_sat_freq_up() {
  if (plogw->up_f_prev == 0) {
    set_sat_freq_to_rig_vfo(plogw->up_f, TX);
    plogw->up_f_prev = plogw->up_f;
  } else {
    int df;
    df = plogw->up_f_prev - plogw->up_f;
    if ((df < -(plogw->freq_tolerance)) || (df > (plogw->freq_tolerance))) {
      set_sat_freq_to_rig_vfo(plogw->up_f, TX);
      plogw->up_f_prev = plogw->up_f;
    }
  }
}

void set_sat_freq_dn() {
  if (plogw->dn_f_prev == 0) {
    set_sat_freq_to_rig_vfo(plogw->dn_f, RX);
    plogw->dn_f_prev = plogw->dn_f;
  } else {
    int df;
    df = plogw->dn_f_prev - plogw->dn_f;
    if ((df < -(plogw->freq_tolerance)) || (df > (plogw->freq_tolerance))) {
      set_sat_freq_to_rig_vfo(plogw->dn_f, RX);
      plogw->dn_f_prev = plogw->dn_f;
    }
  }
}


void set_sat_freq_to_rig() {
  switch (plogw->sat_freq_tracking_mode) {
    case SAT_TX_FIX:  // send RX frequency
      set_sat_freq_dn();
      break;
    case SAT_RX_FIX:  // send TX frequency
      set_sat_freq_up();
      break;
    case SAT_SAT_FIX:  // send TX frequency to main  and RX frequcy to SUB RIG
      set_sat_freq_up();
      set_sat_freq_dn();
      break;
  }
}


// up/down frequency calculation
void set_sat_freq_calc() {
  int i;
  i = plogw->sat_idx_selected;
  if (i < 0) return;

  // check plogw->freq to set up_f, satup_f, and dn_f dependnt on sat_vfo_mode and sat_freq_tracking_mode

  // suppress frequency control during nextaos calcuration because p13.RR may be of other satellites
  if (plogw->f_nextaos != 0) return;

  //The program calculates range-rate RR (line 4190) in km/s. If a satellite transmits a frequency FT, then this is received as a frequency FR:
  //FR = FT * (1 - RR/299792). [Note: 299792 is the speed of light in km/s]
  double doppler_factor;
  doppler_factor = (1.0 - p13.RR / 299792.0);
  double ofs;
  double fsat0;  // frequency at satellite


  // calc beacon_frequency for calculation
  if (sat_info[i].bc_f0 != 0) {
    plogw->beacon_f = sat_info[i].bc_f0 * doppler_factor;
  } else {
    plogw->beacon_f = 0;
  }
  if (verbose & 8) {
    plogw->ostream->print("Beacon freq:");
    plogw->ostream->println(plogw->beacon_f);
  }

  //
  if (verbose & 8) plogw->ostream->print("sat_freq_tracking_mode:");
  switch (plogw->sat_freq_tracking_mode) {
    case SAT_RX_FIX:  // RX fixed (TX frequency calc and set )
      if (verbose & 8) plogw->ostream->println("SAT_RX_FIX");

      //
      //  relationship between uplink downlink frequencies
      // ofs0:offset_freq
      //  dn_f0+ofs0 ----ofs ----satdn_f             dn_f1+ofs0
      //	  up_f0        	           (satup_f)----ofs ---up_f1

      fsat0 = plogw->dn_f / doppler_factor;
      // check fsat0 with downlink frequency
      plogw->satdn_f = fsat0;
      if (fsat0 > (sat_info[i].dn_f1 + sat_info[i].offset_freq) || fsat0 < (sat_info[i].dn_f0 + sat_info[i].offset_freq)) {
        if (verbose & 8) {
          plogw->ostream->print("downlink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
      }
      ofs = fsat0 - (sat_info[i].dn_f0 + sat_info[i].offset_freq);
      // usually uplink frequency is inverse so offset from upper edge of downlink band
      plogw->up_f = (sat_info[i].up_f1 - ofs) / doppler_factor;
      break;

    case SAT_TX_FIX:  // TX fixed (RX frequency controlled)
      if (verbose & 8) plogw->ostream->println("SAT_TX_FIX");



      fsat0 = plogw->up_f * doppler_factor;
      if (fsat0 > sat_info[i].up_f1 || fsat0 < sat_info[i].up_f0) {
        if (verbose & 8) {
          plogw->ostream->print("uplink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
        plogw->f_inband_txp = 0;
      } else {
        plogw->f_inband_txp = 1;
      }
      ofs = sat_info[i].up_f1 - fsat0;
      // usually uplink frequency is inverse so offset from upper edge of downlink band
      // offset is down link offset added by satellite transmission
      plogw->satdn_f = sat_info[i].dn_f0 + sat_info[i].offset_freq + ofs;
      plogw->dn_f = plogw->satdn_f * doppler_factor;
      break;

    case SAT_SAT_FIX:  // satellite fixed  (both TX RX frequency controlled )
      if (verbose & 8) plogw->ostream->println("SAT_SAT_FIX");

      // in this mode, sat uplink frequency may not be calculated from current vfo setting reliably. So, satup_f may be set only by Alt-key command to force set frequency, or in different mode


      fsat0 = plogw->satdn_f;
      if (fsat0 > (sat_info[i].dn_f1 + sat_info[i].offset_freq) || fsat0 < (sat_info[i].dn_f0 + sat_info[i].offset_freq)) {
        if (verbose & 8) {
          plogw->ostream->print("downlink frequency ");
          plogw->ostream->print(fsat0);
          plogw->ostream->println(" off the transponder band");
        }
        plogw->f_inband_txp = 0;
      } else {
        plogw->f_inband_txp = 1;
      }
      ofs = fsat0 - (sat_info[i].dn_f0 + sat_info[i].offset_freq);
      plogw->up_f = (sat_info[i].up_f1 - ofs) / doppler_factor;
      plogw->dn_f = plogw->satdn_f * doppler_factor;

      break;
  }

  // set up , down frequencies to rig(s)
  set_sat_freq_to_rig();
  if (verbose & 8) {
    plogw->ostream->print("Up ");
    plogw->ostream->print(plogw->up_f);
    plogw->ostream->print(" Dn ");
    plogw->ostream->println(plogw->dn_f);
  }
}

void set_sat_center_frequency() {
  if (verbose & 8) plogw->ostream->println("set sat center frequency");
  int i;
  i = plogw->sat_idx_selected;
  if (i >= 0) {
    int memo;
    memo = plogw->sat_freq_tracking_mode;  // temprally memorize
    plogw->sat_freq_tracking_mode = 2;     // sat fixed mode
    plogw->satdn_f = (sat_info[i].dn_f0 / 2 + sat_info[i].dn_f1 / 2);
    if (verbose & 8) plogw->ostream->print("-> satdn_f = ");
    plogw->ostream->println(plogw->satdn_f);
    set_sat_freq_calc();
    plogw->sat_freq_tracking_mode = memo;
  }
}

void set_sat_beacon_frequency() {
  if (verbose & 8) plogw->ostream->println("set sat beacon frequency");
  int i;
  i = plogw->sat_idx_selected;
  if (i >= 0) {
    if (sat_info[i].bc_f0 != 0) {
      int memo;
      memo = plogw->sat_freq_tracking_mode;  // temprally memorize
      plogw->sat_freq_tracking_mode = 2;     // sat fixed mode
      plogw->satdn_f = sat_info[i].bc_f0;
      set_sat_freq_calc();
      plogw->sat_freq_tracking_mode = memo;
    }
  }
}


void set_location_gl_calc(char *locator) {
  // set location from grid locator
  double lon, lat;
  lon = mh2lon(locator);
  lat = mh2lat(locator);
  p13.setLocation(lon, lat, 50);
  if (verbose & 8) {
    plogw->ostream->print("Lat:");
    plogw->ostream->print(lat);
    plogw->ostream->print(" Lon:");
    plogw->ostream->println(lon);
  }
}

//    p13.setFrequency(435300000, 145920000);//AO-51  frequency
//    p13.setLocation(-64.375, 45.8958, 20); // Sackville, NB
//    p13.setTime(2009, 10, 1, 19, 5, 0); //Oct 1, 2009 19:05:00 UTC


// satellite display update

void freq2str(char *s, int freq) {
  int tmp1, tmp2;
  tmp2 = (freq % 1000) / 10;
  tmp1 = freq / 1000;
  tmp1 = tmp1 % 1000;
  sprintf(s, "%3d.%03d.%02d", freq / 1000000, tmp1, tmp2);
}

void upd_display_sat() {
  //  Serial.print ("upd_display_sat timer=");
  //  Serial.println(info_disp.timer);  
  if (info_disp.timer > 0) return;
  select_left_display();  
  char sbuf[20], sbuf1[20];
  sprintf(dp->lcdbuf, "%-6s %-6s az%03d%c", plogw->sat_name_set, plogw->grid_locator_set, plogw->rotator_az, plogw->f_rotator_track ? 'T' : ' ');
  display_printStr(dp->lcdbuf, 10);

  dtostrf(p13.AZ, 3, 0, sbuf);
  dtostrf(p13.EL, 3, 0, sbuf1);
  sprintf(dp->lcdbuf, "AZ:%3s EL:%3s %s", sbuf, sbuf1,
          (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_RX ? "RATB" : (plogw->sat_vfo_mode == SAT_VFO_SINGLE_A_TX ? "RBTA" : (plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_0 ? "R1T0" : (plogw->sat_vfo_mode == SAT_VFO_MULTI_TX_1 ? "R0T1" : "?")))));
  display_printStr(dp->lcdbuf, 11);

  freq2str(sbuf1, plogw->up_f);
  sprintf(dp->lcdbuf, "TX%c:%s", plogw->sat_freq_tracking_mode == SAT_TX_FIX ? '*' : ' ', sbuf1);
  display_printStr(dp->lcdbuf, 12);

  freq2str(sbuf, plogw->dn_f);
  sprintf(dp->lcdbuf, "RX%c:%s", plogw->sat_freq_tracking_mode == SAT_RX_FIX ? '*' : ' ', sbuf);
  display_printStr(dp->lcdbuf, 13);

  freq2str(sbuf, plogw->satdn_f);
  sprintf(dp->lcdbuf, "S %c:%s%c", plogw->sat_freq_tracking_mode == SAT_SAT_FIX ? '*' : ' ', sbuf, plogw->f_inband_txp == 1 ? ' ' : '!');
  display_printStr(dp->lcdbuf, 14);
  int tmp;
  char sign;
  tmp = sat_info[plogw->sat_idx_selected].offset_freq;
  if (tmp >= 0) {
    sign = '+';
  } else {
    sign = '-';
    tmp = -tmp;
  }
  freq2str(sbuf1, tmp);
  sprintf(dp->lcdbuf, "Ofs:%c%s", sign, sbuf1);
  display_printStr(dp->lcdbuf, 15);

  //u8g2_l.sendBuffer();  // transfer internal memory to the display
  left_display_sendBuffer();
}

void print_sat_info_by_index(int i) {

  if (verbose & 8) {
    plogw->ostream->print("Name:");
    plogw->ostream->println(sat_info[i].name);
    plogw->ostream->print(" YEAR ");
    plogw->ostream->println(sat_info[i].YEAR);
    plogw->ostream->print(" EPOCH ");
    plogw->ostream->println(sat_info[i].EPOCH);
    plogw->ostream->print(" INCLINATION ");
    plogw->ostream->println(sat_info[i].INCLINATION);
    plogw->ostream->print(" RAAN ");
    plogw->ostream->println(sat_info[i].RAAN);
    plogw->ostream->print(" ECCENTRICITY ");
    plogw->ostream->println(sat_info[i].ECCENTRICITY);
    plogw->ostream->print(" ARGUMENT_PEDIGREE ");
    plogw->ostream->println(sat_info[i].ARGUMENT_PEDIGREE);
    plogw->ostream->print(" MEAN_ANOMALY ");
    plogw->ostream->println(sat_info[i].MEAN_ANOMALY);
    plogw->ostream->print(" MEAN_MOTION ");
    plogw->ostream->println(sat_info[i].MEAN_MOTION);
    plogw->ostream->print(" TIME_MOTION_D ");
    plogw->ostream->println(sat_info[i].TIME_MOTION_D);
    plogw->ostream->print(" EPOCH_ORBIT ");
    plogw->ostream->println(sat_info[i].EPOCH_ORBIT);

    plogw->ostream->print(" Up Freq ");
    plogw->ostream->print(sat_info[i].up_f0);
    plogw->ostream->print(" - ");
    plogw->ostream->print(sat_info[i].up_f1);
    plogw->ostream->print(" ");
    plogw->ostream->println(sat_info[i].up_mode);
    plogw->ostream->print(" Dn Freq ");
    plogw->ostream->print(sat_info[i].dn_f0);
    plogw->ostream->print(" - ");
    plogw->ostream->print(sat_info[i].dn_f1);
    plogw->ostream->print(" ");
    plogw->ostream->println(sat_info[i].dn_mode);

    plogw->ostream->print(" Beacon ");
    plogw->ostream->println(sat_info[i].bc_f0);
  }
}

void print_sat_info(char *sat_name) {
  int i;
  i = find_satname(sat_name);
  if (i == -1) {

    if (!plogw->f_console_emu) {
      plogw->ostream->print(sat_name);
      plogw->ostream->println(" Not found");
    }
    return;
  }
  print_sat_info_by_index(i);
}




int compare_satinfo_aos(const void *a, const void *b) {
  DateTime timea, timeb;
  timea = sat_info[*(int *)a].nextaos;
  timeb = sat_info[*(int *)b].nextaos;
  return (timea.unixtime() - timeb.unixtime());
}

int compare_datetime(DateTime a, DateTime b) {
  return (a.unixtime() - b.unixtime());
}


// display list of aos los maxel
void print_sat_info_aos() {
  // create list of index in satidx_sort
  int nsat;
  nsat = 0;
  for (int i = 0; i < N_SATELLITES; i++) {
    if (sat_info[i].YEAR == 0) continue;
    satidx_sort[nsat] = i;
    nsat++;
  }
  int j;
  if (!plogw->f_console_emu) {
    plogw->ostream->print("nsat=");
    plogw->ostream->println(nsat);
  }
  qsort(satidx_sort, nsat, sizeof(int), compare_satinfo_aos);
  char buf[30];
  strcpy(dp->lcdbuf, "");
  int count;
  count = 0;
  for (int i = 0; i < nsat; i++) {
    j = satidx_sort[i];
    sprintf(buf, "%-7s", sat_info[j].name);
    if (!plogw->f_console_emu) plogw->ostream->print(buf);

    if ((count >= plogw->nextaos_showidx) && (count <= plogw->nextaos_showidx + 6)) {
      // print to LCD
      strcat(dp->lcdbuf, buf);
      sprintf(buf, "%02d:%02d-%02d:%02d ", sat_info[j].nextaos.hour(),
              sat_info[j].nextaos.minute(),
              sat_info[j].nextlos.hour(),
              sat_info[j].nextlos.minute());

      strcat(dp->lcdbuf, buf);
      dtostrf(sat_info[j].maxel, 2, 0, buf);
      strcat(dp->lcdbuf, buf);
      strcat(dp->lcdbuf, "\n");
    }
    if (!plogw->f_console_emu) {
      plogw->ostream->print("AOS ");
      print_datetime(sat_info[j].nextaos);
      plogw->ostream->print(" LOS ");
      print_datetime(sat_info[j].nextlos);
      plogw->ostream->print(" EL ");
      plogw->ostream->print(sat_info[j].maxel);
      plogw->ostream->println("");
    }
    count++;
  }
  upd_display_info_flash(dp->lcdbuf);
}



void init_sat() {
  // allocate buffers in heap
  buff = NULL;
  buff1 = NULL;
  tle1_line = NULL;
  tle2_line = NULL;
  satname_line = NULL;
}


void allocate_sat() {
  if (tle1_line == NULL) {
    Serial.println("allocate_sat(): buff buff1 tle1 tle2 satname line memory allocation");    
    buff = (uint8_t *)malloc(sizeof(uint8_t) * 256);
    buff1 = (uint8_t *)malloc(sizeof(uint8_t) * 256);
    tle1_line = (char *)malloc(sizeof(uint8_t) * 128);
    tle2_line = (char *)malloc(sizeof(uint8_t) * 128);
    satname_line = (char *)malloc(sizeof(uint8_t) * 128);
  } else {
    Serial.println("allocate_sat(): no need for allocation");
  }
}

// free memory used in sat tle data base
void release_sat() {
  Serial.println("releasing sat related buff, buff1 .. and force non satellite op");
  plogw->sat = 0;  // force normal operation
  free(buff);
  buff = NULL;
  free(buff1);
  buff1 = NULL;
  free(tle1_line);
  tle1_line = NULL;
  free(tle2_line);
  tle2_line = NULL;
  free(satname_line);
  satname_line = NULL;
}

int find_satname(char *satname) {
  //  plogw->ostream->print("find_satname()");
  int i;
  for (i = 0; i < N_SATELLITES; i++) {

    if (sat_names[i][0] == '\0') break;
    if (strcmp(satname, sat_names[i]) == 0) {
      plogw->ostream->print("found ");
      plogw->ostream->print(i); plogw->ostream->print(":"); plogw->ostream->print(satname); plogw->ostream->print(":"); plogw->ostream->print(sat_names[i]); plogw->ostream->println(":");
      return i;
    }
  }
  return -1;
}

void readtlefile() {
  if (buff == NULL) {
    allocate_sat();    
    //    return;
  }
  plogw->ostream->printf("opening tlefile %s\n",tlefilename);  
  tlefile = SD.open(tlefilename, FILE_READ);
  if (!tlefile) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("opening TLE file");
      plogw->ostream->println(tlefilename);
      plogw->ostream->print(" failed");
    }
    return;
  }
  sprintf(dp->lcdbuf, "Read TLE file \n");
  upd_display_info_flash(dp->lcdbuf);

  if (!plogw->f_console_emu) {
    plogw->ostream->print("printing contents of ");
    plogw->ostream->println(tlefilename);
  }
  
  int count;
  int stat;
  stat = 0;
  count = 0;
  //  allocate_sat();
  while (tlefile.available()) {
    // read line and store data into memory
    //    char c;
    //    c = tlefile.read();
    //    plogw->ostream->print(c);
    int c = tlefile.readBytesUntil(0x0a, buff, 128);
    if (c > 0) {
      count++;      
      //plogw->ostream->print("c="); plogw->ostream->println(c);
      buff[c] = '\0';
      // replace stray 0x0d
      for (int i = 0; i < strlen((char *)buff); i++) {
        if (buff[i] == 0x0d) buff[i] = '\0';
      }

      // the first line is unix time of the tle file
      if (count==1) {
	sscanf((const char *)buff,"%ld",&plogw->tle_unixtime);
	plogw->ostream->print("tle file unixtime=");
	plogw->ostream->println(plogw->tle_unixtime);
	continue;
      }
      switch (stat) {
        case 0:  // name
          strcpy(satname_line, (char *)buff);
          stat = 1;
          break;
        case 1:  // TLE1
          if (buff[0] == '1') {
            strcpy(tle1_line, (char *)buff);
            stat = 2;
          } else {
            stat = 0;
          }
          break;
        case 2:  // TLE2
          if (buff[0] == '2') {
            strcpy(tle2_line, (char *)buff);

            // check contents
            if (verbose & 8) {
              plogw->ostream->println("Satellite entry");
              plogw->ostream->write((uint8_t *)satname_line, strlen(satname_line));
              plogw->ostream->println("");
              plogw->ostream->write((uint8_t *)tle1_line, strlen(tle1_line));
              plogw->ostream->println("");
              plogw->ostream->write((uint8_t *)tle2_line, strlen(tle2_line));
              plogw->ostream->println("");
            }
            int i;
            if ((i = find_satname(satname_line)) != -1) {
              sprintf(dp->lcdbuf, "Read TLE file \nSat.\n%s\nRead.", satname_line);
              upd_display_info_flash(dp->lcdbuf);

              // this is the satellite to read into sat_info database
              if (verbose & 8) {
                plogw->ostream->print("Satellite info:");
                plogw->ostream->println(satname_line);
              }
              strcpy(sat_info[i].name, satname_line);
              char buf[20];
              buf[0] = '\0';
              strncat(buf, tle1_line + 18, 2);
              sat_info[i].YEAR = atoi(buf) + 2000;
              buf[0] = '\0';
              strncat(buf, tle1_line + 20, 12);
              sat_info[i].EPOCH = atof(buf);
              buf[0] = '\0';
              strncat(buf, tle2_line + 8, 8);
              sat_info[i].INCLINATION = atof(buf);
              buf[0] = '\0';
              strncat(buf, tle2_line + 17, 8);
              sat_info[i].RAAN = atof(buf);
              buf[0] = '0';
              buf[1] = '.';
              buf[2] = '\0';
              strncat(buf + 2, tle2_line + 26, 7);
              sat_info[i].ECCENTRICITY = atof(buf);

              buf[0] = '\0';
              strncat(buf, tle2_line + 34, 8);
              sat_info[i].ARGUMENT_PEDIGREE = atof(buf);
              buf[0] = '\0';
              strncat(buf, tle2_line + 43, 8);
              sat_info[i].MEAN_ANOMALY = atof(buf);

              buf[0] = '\0';
              strncat(buf, tle2_line + 52, 11);
              sat_info[i].MEAN_MOTION = atof(buf);

              buf[0] = '\0';
              strncat(buf, tle1_line + 33, 10);
              sat_info[i].TIME_MOTION_D = atof(buf);

              buf[0] = '\0';
              strncat(buf, tle2_line + 63, 5);
              sat_info[i].EPOCH_ORBIT = atoi(buf);


              //ISS (ZARYA)
              //          1         2         3         4         5         6
              //0123456789012345678901234567890123456789012345678901234567890123456789
              //1 25544U 98067A   19181.39493521 -.00156611  00000-0 -26708-2 0  9993
              //k lllllm bbyyyp   AABBBBBBBBBBBB IIIIIIIIII yyyyyyyy pppppppp b rrrrg

              //          1         2         3         4         5         6
              //0123456789012345678901234567890123456789012345678901234567890123456789
              //2 25544  51.6396 295.7455 0007987  98.4040   9.2643 15.51194651177305
              //b rrrrr CCCCCCCC DDDDDDDD EEEEEEE FFFFFFFF GGGGGGGG HHHHHHHHHHHJJJJJb


              //String  BIRD=               "ISS (ZARYA)";
              //#define YEAR                2019            // 20AA
              //#define EPOCH               181.39493521    // BBBBBBBBBBBB
              //#define INCLINATION         51.6396         // CCCCCCCC
              //#define RAAN                295.7455        // DDDDDDDD
              //#define ECCENTRICITY        0.0007987       // 0.EEEEEE
              //#define ARGUMENT_PEDIGREE   98.404          // FFFFFFFF
              //#define MEAN_ANOMALY        9.2643          // GGGGGGGG
              //#define MEAN_MOTION         15.51194651     // HHHHHHHHHHH
              //#define TIME_MOTION_D       -0.00156611     // IIIIIIIIII
              //#define EPOCH_ORBIT         17730           // JJJJJ
              //#define ONEPPM              1.0e-6
              //#define ONEPPTM             1.0e-7
              //              p13.setElements(YEAR, EPOCH, INCLINATION, RAAN, ECCENTRICITY * ONEPPTM, ARGUMENT_PEDIGREE,
              //                              MEAN_ANOMALY, MEAN_MOTION, TIME_MOTION_D, EPOCH_ORBIT + ONEPPM, 0);

              set_sat_info2(satname_line);
              print_sat_info(satname_line);

              stat = 0;
            } else {
              // sprintf(dp->lcdbuf,"Read TLE file \nSat.\n%s\nSkip.",satname_line);	upd_display_info_flash(dp->lcdbuf);
              if (verbose & 8) {
                plogw->ostream->print(satname_line);
                plogw->ostream->println(":not found");
              }
              stat = 0;
            }
            break;
	  }
          //PrintHex<uint8_t>(c, 0x80);plogw->ostream->print(" ");

          //if (count>=32) {
          //  plogw->ostream->println("");
          //  count=0;
          //}
      }
    }
  }
  tlefile.close();
  release_sat();
  strcpy(dp->lcdbuf, "Read TLE file \nFinished");
  upd_display_info_flash(dp->lcdbuf);
  if (!plogw->f_console_emu) plogw->ostream->println("\nend printing.");
}

// get tle information from network and store to tle file in SD memory.

void getTLE() {
  // this fails by wifisecureclient related
  // --> make menuconfig -> component config ESP-TLS check Enabel PSK verification will compile
  int counttimeout;
  counttimeout = 0;
  plogw->ostream->println("getTLE()");

  //  if (buff1==NULL) return ;
  //  plogw->ostream->println("getTLE()0");

  if (f_sat_updated) return;
  //  plogw->ostream->println("getTLE()1");
  //  if ((WiFi.status() == WL_CONNECTED)) {
  if (wifi_status == 1) {
    //    plogw->ostream->println("getTLE()2");

    tlefile = SD.open(tlefilename, FILE_WRITE);

    if (!tlefile) {
      if (!plogw->f_console_emu) {
        plogw->ostream->print("opening TLE file");
        plogw->ostream->println(tlefilename);
        plogw->ostream->print(" failed");
      }
      return;
    }
    sprintf(dp->lcdbuf, "Connect TLE \nServer");
    upd_display_info_flash(dp->lcdbuf);

    if (!plogw->f_console_emu) plogw->ostream->print("[HTTP] GET begin...\n");
    // configure traged server and url
    http.begin("http://www.amsat.org/tle/current/nasabare.txt");

    if (!plogw->f_console_emu) plogw->ostream->print("[HTTP] GET...\n");
    // start connection and send HTTP header
    int httpCode = http.GET();
    unsigned long index = 0;  // received total number of bytes

    int count_lines = 0;
    // httpCode will be negative on error
    if (httpCode > 0) {
      // HTTP header has been send and Server response header has been handled
      if (!plogw->f_console_emu) plogw->ostream->printf("[HTTP] GET... code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK) {
        allocate_sat();
        // get tcp stream
        WiFiClient *stream = http.getStreamPtr();

        // get lenght of document (is -1 when Server sends no Content-Length header)
        int len = http.getSize();
        if (!plogw->f_console_emu) plogw->ostream->printf("[HTTP] Content-Length=%d\n", len);
	// write current time to tlefile
	tlefile.println(rtctime.unixtime());	
        int stat;
        stat = 0;  // to skip status line
        buff1[0] = '\0';
        // read all data from server
        while (http.connected() && (len > 0 || len == -1)) {
          // get available data size
          size_t size = stream->available();

          if (size > 0) {
            int c = stream->readBytesUntil(0x0a, buff, 256);
            if (len > 0) {
              len -= c;
            }
            if (c == 0) {
              counttimeout++;
              if (counttimeout > 1000) {
                plogw->ostream->print("timeout");
                break;
              }
              delay(10);
            } else {
              counttimeout = 0;
            }
            if (c > 0) {
              index += c;  // index is total number of bytes
              if (c == sizeof(buff)) {
                if (!plogw->f_console_emu) plogw->ostream->println("#buffer full for the line");
                buff[0] = '\0';
                continue;
              } else {
                buff[c] = '\0';  // terminate line
              }
              if (stat == 0) {
                //                plogw->ostream->print("skip this line:");plogw->ostream->write(buff,strlen((char *)buff));plogw->ostream->println("");
                stat = 2;  // post skip concatnate line
                continue;
              }
              // replace stray 0x0d
              for (int i = 0; i < strlen((char *)buff); i++) {
                if (buff[i] == 0x0d) buff[i] = '\0';
              }
              if (stat == 2) {  // concat line
                // concatenate to buff1
                if (strlen((char *)buff) + strlen((char *)buff1) > 128) {
                  // ignore
                  if (!plogw->f_console_emu) plogw->ostream->println("Ignore line ");
                  buff1[0] = '\0';
                  continue;
                } else {
                  strcat((char *)buff1, (char *)buff);
                }
                stat = 1;  // normal line
              } else {
                strcpy((char *)buff1, (char *)buff);
              }
              int l1;
              l1 = strlen((char *)buff1);
              if (l1 == 0) {
                if (!plogw->f_console_emu) plogw->ostream->println("empty record encountered");
                // empty line is end records
                f_sat_updated = 1;

                break;
              }
              // check received line
              // plogw->ostream->print("CHK:");plogw->ostream->print(l1);plogw->ostream->print(":");plogw->ostream->write(buff1, strlen((char *)buff1));plogw->ostream->println(":");
              sprintf(dp->lcdbuf, "TLE received\nLine %d", count_lines);
              upd_display_info_flash(dp->lcdbuf);
              count_lines++;

              if (buff1[0] == '1') {
                if (l1 != 69) {  // may be cut for status line
                  stat = 0;      // skip next line and concatenate
                  continue;
                } else {
                  if (verbose & 8) plogw->ostream->print("TLE1:");
                  strcpy(tle1_line, (char *)buff1);
                }
              } else if (buff1[0] == '2') {
                if (l1 != 69) {  // may be cut for status line
                  stat = 0;      // skip next line and concatenate

                  continue;
                } else {
                  if (verbose & 8) plogw->ostream->print("TLE2:");
                  strcpy(tle2_line, (char *)buff1);
                  // write to file
                  tlefile.write((uint8_t *)satname_line, strlen(satname_line));
                  tlefile.println("");
                  tlefile.write((uint8_t *)tle1_line, strlen(tle1_line));
                  tlefile.println("");
                  tlefile.write((uint8_t *)tle2_line, strlen(tle2_line));
                  tlefile.println("");
                }
              } else {
                if (verbose & 8) plogw->ostream->print("Name:");
                strcpy(satname_line, (char *)buff1);
              }
              // print received buffer
              if (verbose & 8) {
                plogw->ostream->print(index);
                plogw->ostream->print(":");
                plogw->ostream->print(l1);
                plogw->ostream->print(":");
                plogw->ostream->write(buff1, strlen((char *)buff1));
                plogw->ostream->print(":");
                plogw->ostream->print(stat);
                plogw->ostream->println("");
                plogw->ostream->flush();
              }
            }
          }
        }
      } else {
        http.end();
      }
    } else {
      http.end();
      plogw->ostream->printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    plogw->ostream->printf("end of getTLE\n");
    http.end();
    tlefile.close();
    release_sat();
    // read sd file to get satellite information from tle.txt
  }
  readtlefile();
}



void plan13_test() {
  p13.setFrequency(435300000, 145920000);  //AO-51  frequency
  p13.setLocation(-64.375, 45.8958, 20);   // Sackville, NB
  p13.setTime(2009, 10, 1, 19, 5, 0);      //Oct 1, 2009 19:05:00 UTC
  p13.setElements(2009, 232.55636497, 98.0531, 238.4104, 83652 * 1.0e-7, 290.6047,
                  68.6188, 14.406497342, -0.00000001, 27022, 180.0);  //fairly recent keps for AO-51 //readElements();



  //ISS (ZARYA)
  //          1         2         3         4         5         6
  //0123456789012345678901234567890123456789012345678901234567890123456789
  //1 25544U 98067A   19181.39493521 -.00156611  00000-0 -26708-2 0  9993
  //k lllllm bbyyyp   AABBBBBBBBBBBB IIIIIIIIII yyyyyyyy pppppppp b rrrrg

  //          1         2         3         4         5         6
  //0123456789012345678901234567890123456789012345678901234567890123456789
  //2 25544  51.6396 295.7455 0007987  98.4040   9.2643 15.51194651177305
  //b rrrrr CCCCCCCC DDDDDDDD EEEEEEE FFFFFFFF GGGGGGGG HHHHHHHHHHHJJJJJb


  //void setElements(double YE_in, double TE_in, double IN_in, double
  //         RA_in, double EC_in, double WP_in, double MA_in, double MM_in,
  //    double M2_in, double RV_in, double ALON_in );


  p13.initSat();
  p13.satvec();
  p13.rangevec();
  p13.printdata();
  plogw->ostream->println();
  plogw->ostream->println("Should be:");
  plogw->ostream->println("AZ:57.07 EL: 4.05 RX 435301728 TX 145919440");
}

void save_satinfo() {
  char fnbuf[30];
  strcpy(fnbuf, "/satinfo.txt");
  //f = SPIFFS.open(fnbuf, FILE_WRITE);
  f = SD.open(fnbuf, FILE_WRITE);
  for (int i = 0; i < N_SATELLITES; i++) {
    if (sat_info[i].name[0] == '\0') continue;
    f.print(sat_info[i].name);
    f.print(" ");
    f.println(sat_info[i].offset_freq);
    plogw->ostream->print(sat_info[i].name);
    plogw->ostream->print(" ");
    plogw->ostream->println(sat_info[i].offset_freq);
  }
  f.close();
}

void print_datetime(DateTime time) {
  char buf[30];
  sprintf(buf, "%02d/%02d %02d:%02d",
          time.month(),
          time.day(),
          time.hour(),
          time.minute());
  if (!plogw->f_console_emu) plogw->ostream->print(buf);
}

DateTime add_datetime(DateTime time, int seconds) {
  int year, month, day, hr, min, sec;

  year = time.year();
  month = time.month();
  day = time.day();
  hr = time.hour();
  min = time.minute();
  sec = time.second();

  sec += seconds;
  while (sec >= 60) {
    min++;
    sec -= 60;
  }
  while (min >= 60) {
    hr++;
    min -= 60;
  }
  while (hr >= 24) {
    day++;
    hr -= 24;
  }
  return DateTime(year, month, day, hr, min, sec);
}

void start_calc_nextaos() {
  //  plogw->ostream->println("NEXTAOS");
  //		plogw->nextaos_satidx=find_satname(radio->callsign+2+7);
  int i;
  for (i = 0; i < N_SATELLITES; i++) {
    if (*sat_info[i].name != '\0') break;
  }
  if (i < N_SATELLITES) {
    plogw->nextaos_satidx = i;
    plogw->f_nextaos = 1;
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("no satellite data");
    plogw->f_nextaos = 0;
  }
}

// satinfo[satidx] についてnextaos を調べ格納する。
void sat_find_nextaos_sequence() {
  int i;
  int satidx;
  satidx = plogw->nextaos_satidx;
  if (satidx == -1) return;
  char buf[20];

  switch (plogw->f_nextaos) {
    case 0:
      break;
    case 1:  //
      // start calculation
      set_sat_info_calc();
      // check if this sat_info has valid information

      if (sat_info[satidx].YEAR == 0) {
        if (!plogw->f_console_emu) {
          plogw->ostream->print("Not valid sat_info idx= ");
          plogw->ostream->print(satidx);
          plogw->ostream->println(" .. skip");
        }
        plogw->f_nextaos = 5;
        break;
      }
      if (verbose & 8) {
        plogw->ostream->print("nextaos calc sat information ");
        plogw->ostream->println(satidx);
      }
      print_sat_info_by_index(satidx);
      // check rtctime and los to see whether need for calculation
      if (compare_datetime(sat_info[satidx].nextlos, rtctime) < 0) {
        //		  plogw->ostream->print("need to recalc AOS-LOS for satidx:");
        //		  plogw->ostream->println(satidx);
        sat_info[satidx].nextaos = rtctime;
        plogw->nextaos_count = 0;
        plogw->f_nextaos = 2;
      } else {
        //		  plogw->ostream->print("no need to recalc AOS-LOS for satidx:");
        //		  plogw->ostream->println(satidx);
        plogw->f_nextaos = 5;
      }
      break;

    case 2:  // search aos

      sat_calc_position(satidx, sat_info[satidx].nextaos);
      // print result
      if (verbose & 8) {
        plogw->ostream->print("\nNEXTAOS satidx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextaos);
        //p13.printdata();

        plogw->ostream->print(" cnt=");
        plogw->ostream->print(plogw->nextaos_count);
        plogw->ostream->print(" AZ=");
        plogw->ostream->print(p13.AZ);

        plogw->ostream->print(" EL=");
        plogw->ostream->println(p13.EL);
      }

      if (p13.EL > 0) {
        // found aos
        plogw->f_nextaos = 3;
        plogw->nextaos_count = 0;
        sat_info[satidx].nextlos = sat_info[satidx].nextaos;
        sat_info[satidx].maxel = p13.EL;
      } else {
        // move to next candidate aos time
        if (p13.EL > -10) {
          sat_info[satidx].nextaos =
            add_datetime(sat_info[satidx].nextaos, 30);
        } else {
          sat_info[satidx].nextaos =
            add_datetime(sat_info[satidx].nextaos, 60 * 3);
        }

        plogw->nextaos_count++;
        if (plogw->nextaos_count > 900) {
          plogw->f_nextaos = 3;
        }
      }
      break;
    case 3:  // found aos and search los
      sat_calc_position(satidx, sat_info[satidx].nextlos);
      // print result
      if (verbose & 8) {
        plogw->ostream->print("\nNEXTLOS satidx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextlos);
        //p13.printdata();

        plogw->ostream->print(" cnt=");
        plogw->ostream->print(plogw->nextaos_count);
        plogw->ostream->print(" AZ=");
        plogw->ostream->print(p13.AZ);

        plogw->ostream->print(" EL=");
        plogw->ostream->println(p13.EL);
      }
      if (p13.EL < 0) {
        // found los
        plogw->f_nextaos = 4;
      } else {
        // move to next candidate aos time
        if (sat_info[satidx].maxel < p13.EL) {
          sat_info[satidx].maxel = p13.EL;
        }
        sat_info[satidx].nextlos =
          add_datetime(sat_info[satidx].nextlos, 30);

        plogw->nextaos_count++;
        if (plogw->nextaos_count > 900) {
          plogw->f_nextaos = 4;
        }
      }
      break;
    case 4:  // found nextlos
      if (!plogw->f_console_emu) {
        plogw->ostream->print("\nNEXTAOS idx=");
        plogw->ostream->print(plogw->nextaos_satidx);
        plogw->ostream->print(" ");
        plogw->ostream->print(sat_info[satidx].name);
        plogw->ostream->print(" ");
        print_datetime(sat_info[satidx].nextaos);
        plogw->ostream->print(" NEXTLOS ");
        print_datetime(sat_info[satidx].nextlos);
        plogw->ostream->println("");
      }
      plogw->f_nextaos = 5;
      break;
    case 5:  //
      // to the next satellite
      plogw->nextaos_satidx++;
      if (plogw->nextaos_satidx >= N_SATELLITES) {
        // no more satellite to calc
        plogw->f_nextaos = 0;
        plogw->nextaos_showidx = 0;
        print_sat_info_aos();  // print results

      } else {
        plogw->f_nextaos = 1;  // start calc for the next satellite
      }
      break;
  }
}

void sat_calc_position(int satidx, DateTime time) {
  int i;
  i = satidx;
  p13.setElements(
    sat_info[i].YEAR,
    sat_info[i].EPOCH,
    sat_info[i].INCLINATION,
    sat_info[i].RAAN,
    sat_info[i].ECCENTRICITY * ONEPPTM,
    sat_info[i].ARGUMENT_PEDIGREE,
    sat_info[i].MEAN_ANOMALY,
    sat_info[i].MEAN_MOTION,
    sat_info[i].TIME_MOTION_D,
    sat_info[i].EPOCH_ORBIT,
    180);

  set_location_gl_calc(plogw->grid_locator_set);
  // JST->UTC conv just by -9 to hour
  p13.setTime(
    time.year(),
    time.month(),
    time.day(),
    time.hour() - 9,
    time.minute(),
    time.second());


  p13.initSat();
  p13.satvec();
  p13.rangevec();
}
void sat_process() {
  if (plogw->sat) {
    struct radio *radio;

    radio = radio_selected;

    sat_find_nextaos_sequence();

    set_sat_info_calc();
    set_location_gl_calc(plogw->grid_locator_set);

    // JST->UTC conv just by -9 to hour
    p13.setTime(rtctime.year(), rtctime.month(), rtctime.day(),
                rtctime.hour() - 9, rtctime.minute(), rtctime.second());

    p13.initSat();
    p13.satvec();
    p13.rangevec();

    // set rotator_target
    plogw->rotator_target_az = p13.AZ;

    set_sat_freq_calc();

    if (verbose & 8) {
      p13.printdata();
      plogw->ostream->println("");
    }
    upd_display_sat();
  }
}
