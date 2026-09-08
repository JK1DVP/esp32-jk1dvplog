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
#include "cat.h"
#include "log.h"
#include "display.h"
#include "edit_buf.h"
#include "zserver.h"
#include "dupechk.h"
#include "callhist_fd.h"
//#include "callhist.h"
#include "callhist_remote.h"
#include "callhist_mem.h"
#include "misc.h"
#include "processes.h"
#include "so2r.h"


void print_band_mask(struct radio *radio)
{
  
  sprintf(dp->lcdbuf, "Radio_Bands M%04X R%1d\n", radio->band_mask&((1<<(N_BAND-1))-1),radio->rig_idx);
  // concat binary bit print
  print_bin(dp->lcdbuf+strlen(dp->lcdbuf),reverse_bits(~radio->band_mask,N_BAND-1),N_BAND-1);
  //  strcat(dp->lcdbuf," En\n1521415221731\n");
  //  strcat(dp->lcdbuf,     "0...340814 ..\n");
  //  strcat(dp->lcdbuf,     "G64204     58\n");
  strcat(dp->lcdbuf," En\n1371225141251\n");
  strcat(dp->lcdbuf,     ".. 418043...0\n");
  strcat(dp->lcdbuf,     "85     40246G\n");
  upd_display_info_flash(dp->lcdbuf);
}
// mode code used in dupe checking
int freq2bandid(unsigned int freq) {
  if ((freq >= (1790000/FREQ_UNIT)) && (freq < (1920000/FREQ_UNIT))) return 1;             //1.9
  if ((freq >= (3490000/FREQ_UNIT)) && (freq < (3810000/FREQ_UNIT))) return 2;             // 3.5
  if ((freq >=  (6900000/FREQ_UNIT)) && (freq <  (7210000/FREQ_UNIT))) return 3;             // 7
  if ((freq >= (13900000/FREQ_UNIT)) && (freq < (14360000/FREQ_UNIT))) return 4;           // 14
  if ((freq >= (20900000/FREQ_UNIT)) && (freq < (21500000/FREQ_UNIT))) return 5;           // 21
  if ((freq >= (27900000/FREQ_UNIT)) && (freq < (29800000/FREQ_UNIT))) return 6;           // 28
  if ((freq >= (49900000/FREQ_UNIT)) && (freq < (55000000/FREQ_UNIT))) return 7;           // 50
  if ((freq >=(143900000/FREQ_UNIT)) && (freq <(146100000/FREQ_UNIT))) return 8;         // 144
  if ((freq >=(429500000/FREQ_UNIT)) && (freq <(441000000/FREQ_UNIT))) return 9;         // 430
  if ((freq>= (1199000000UL/FREQ_UNIT)) &&(freq <    (1301000000UL/FREQ_UNIT))) return 10;  // 1200
  if ((freq >=(2399000000UL/FREQ_UNIT)) && (freq < (2451000000UL/FREQ_UNIT))) return 11;  // 2400
  if ((freq >= (5649000000ULL/FREQ_UNIT)) && (freq < (5851000000ULL/FREQ_UNIT))) return 12;  // 5600
  if ((freq >= (9990000000ULL/FREQ_UNIT)) && (freq < (10501000000ULL/FREQ_UNIT))) return 13;  // 10G
  // WARC bands
  if ((freq >=  (10000000/FREQ_UNIT)) && (freq <  (10200000/FREQ_UNIT))) return 14;             // 10 MHz
  if ((freq >=  (18000000/FREQ_UNIT)) && (freq <  (18200000/FREQ_UNIT))) return 15;             // 18 MHz
  if ((freq >=  (24800000/FREQ_UNIT)) && (freq <  (25000000/FREQ_UNIT))) return 16;             // 24 MHz
  return 0;                                                        // default
}

const char *band_str[N_BAND] = { "1.8", "3.5", "7", "14", "21", "28", "50",     "144", "430", "1200", "2400","5600","10G", "10",  "18","24" };
const char *band_str_adif[N_BAND] = { "160M","80M","40M","20M","15M","10M","6M","2M", "70CM", "23CM","12CM", "6CM", "2.4CM","30M","17M","12M" };
		      

// set up file expression of a QSO from logw information


// modeid: index name corresponding to each opmode (CW, CW-R, LSB ...)

int modeid_string(const char *s) {
  // output index to given mode string
  int i;
  int nmode;
  for (i = 0; i < NMODEID; i++) {
    if (strcmp(s, mode_str[i]) == 0) {
      return i;
    }
  }
  return -1;
}

// modetype: type name contest operation CW/PH/DG  0 is default and not defined
// previously modetype_rig2mode
int modetype_string(const char *s) {
  int idx;
  idx = modeid_string(s);
  if (idx != -1) return modetype[idx];
  return 0;
}

const char *switch_rigmode() {
  struct radio *radio;
  radio = so2r.radio_selected();
  // check next mode in the rig
  // LSB -> USB -> ...
  int i;
  int imode;
  for (i = 0; i < NMODEID; i++) {
    if (strcmp(radio->opmode, mode_str[i]) == 0) {
      imode = i;
      imode++;  // next mode
      if (imode >= NMODEID) imode = 0;
      return mode_str[imode];
    }
  }

  return "CW";  // return default mode
}

void wipe_log_entry_radio(struct radio *radio) {
  // clear callsign and number
  clear_buf(radio->callsign);
  clear_buf(radio->recv_exch);
  clear_buf(radio->remarks);
  radio->qsodata_loaded = 0;
  radio->f_qsl=0; // reset to noqsl
  set_log_rst(radio);
  radio->dupe = 0;
  radio->multi = -1;
  radio->ptr_curr = 0;  // go back to call sign entry
}

// clear log entry
void wipe_log_entry() {
  struct radio *radio;
  radio = so2r.radio_selected();
  //  radio = so2r.radio_qso_process();
  // clear callsign and number
  clear_buf(radio->callsign);
  clear_buf(radio->recv_exch);
  clear_buf(radio->remarks);
  radio->qsodata_loaded = 0;
  radio->f_qsl=0; // reset to noqsl
  set_log_rst(radio);
  radio->dupe = 0;
  radio->multi = -1;
  radio->ptr_curr = 0;  // go back to call sign entry
}

// wipe and seqnr++
void new_log_entry() {
  wipe_log_entry();
  // next sequence number
  plogw->seqnr++;
}

void set_log_rst(struct radio *radio) {
  // struct radio *radio;
  // radio = radio_selected;
  switch (radio->modetype) {
  case LOG_MODETYPE_CW:
  case LOG_MODETYPE_DG:
    // CW
    if (!plogw->f_console_emu) plogw->ostream->println("CW");
    strcpy(radio->sent_rst + 2, "599");
    strcpy(radio->recv_rst + 2, "599");
    break;
  case LOG_MODETYPE_PH:
  case LOG_MODETYPE_UNDEF:    
    if (!plogw->f_console_emu) plogw->ostream->println("PH");
    strcpy(radio->sent_rst + 2, "59");
    strcpy(radio->recv_rst + 2, "59");
    break;
  }
}


void enable_radios(int idx_radio, int radio_cmd) {
  struct radio *radio;

  // Any explicit enable/disable/toggle of this radio cancels a pending
  // Alt-I temporary-disable timer for the same radio.  Alt-I sets the
  // temporary state again immediately after calling this function.
  if (temporarily_disabled_radio == idx_radio) {
    temporarily_disabled_radio = -1;
    timeout_rig_disable_temporally = 0;
  }

  radio = &radio_list[idx_radio];
  //  if (!plogw->f_console_emu) plogw->ostream->println("enable/disable  main rig");
  // Alt-Shift-HOME sub rig change
  radio = &radio_list[idx_radio];  // main rig
  switch (radio_cmd) {
  case -1:  //toggle radio enable
    radio->enabled = 1 - radio->enabled;
    break;
  case 0:  // disable
    radio->enabled = 0;
    break;
  case 1:  // enable
    radio->enabled = 1;
    break;
  }
  print_radio_info(idx_radio);
  upd_disp_rig_info();
}


void switch_bands(struct radio *radio) {

  int tmp, count;
  count = 0;
  tmp = radio->bandid;
  while (count < N_BAND) {
    tmp++;
    if (tmp >= N_BAND) tmp = 1;
    if (!plogw->f_console_emu) {
      plogw->ostream->print("bandid:");
      plogw->ostream->print(tmp);
      plogw->ostream->print(" count:");
      plogw->ostream->println(count);
    }
    if (((1 << (tmp - 1)) & radio->band_mask) == 0) {
      // ok to change
      band_change(tmp, radio);
      break;
    }
    count++;
  }
}

void band_change(int bandid, struct radio *radio) {
  // change to bandid
  if (bandid <= 0) bandid = N_BAND - 1; // last bandid = N_BAND -1
  if (bandid >= N_BAND) bandid = 1; // 
  save_freq_mode_filt(radio);
  radio->bandid = bandid;
  radio->cq[radio->modetype] = LOG_SandP;  // force move to S&P
  recall_freq_mode_filt(radio);
  //  send_mode_set_civ(mode_str[bandmap_disp.on_cursor_modeid], radio->filtbank[radio->cq[radio->modetype]][radio->modetype]); // cw phone switch filter selection!
  //set_frequency(bandid2freq(radio->bandid));
  //set_frequency_rig(bandid2freq(radio->bandid));
  request_display_update_on_demand();

  // notify changed band to zserver
  sprintf(buf,"#ZLOG# BAND %d",zserver_bandid_freqcodes_map[radio->bandid]);
  zserver_send(buf);
  

  // One coalescing request is sufficient; do not also leave the legacy
  // interval flag set, which would redraw the same bandmap twice.
  request_bandmap_update_on_demand();
}

void init_logwindow() {

  plogw = &logw;
  plogw->ostream = console;  // set default output stream

  plogw->f_partial_check=PARTIAL_CHECK_CALLSIGN_AUTO;// partial check enabled by default

  plogw->ostream->print("size of logwindow struct:");
  plogw->ostream->println(sizeof(struct logwindow));

  //plogw = (struct logwindow *)malloc(sizeof (struct logwindow));
  plogw->f_esm=0;
  plogw->wipe_key_swap=0;
  plogw->show_smeter = 1;
  plogw->show_qso_interval=0;
  plogw->qso_interval_timer=0;

  plogw->autopoweroff=0;
  plogw->count_autopoweroff=0;

  //  init_sequence_manager();

  plogw->f_antalt_switched = 0;
  plogw->f_antalt = 0;
  plogw->antalt_count = 0;
  plogw->f_show_signal = 0;
  plogw->relay[0] = 0;
  plogw->relay[0] = 0;
  plogw->help_idx = 0;
  plogw->f_straightkey = 0;
  plogw->f_toggle_ptt_mode = 0;  // ptt control in phone by right shift key
  //  plogw->radio_mode = 0;  // 0 so1r 1 sat two radio (main tx sub receive

  
  // 2 so2r (box connected)

  // nextaos related
  plogw->f_nextaos = 0;
  plogw->nextaos_satidx = 0;
  plogw->nextaos_showidx = 0;

  plogw->f_console_emu = 0;

  // rotator control
  plogw->f_rotator_enable = 0;
  plogw->f_rotator_track = 0;
  plogw->rotator_target_az = -1;           // <0 indicate not yet set
  plogw->rotator_target_az_tolerance = 5;  //
  plogw->rotator_sweep_step_default=2;
  plogw->f_rotator_sweep_suspend=0;
  plogw->f_rotator_sweep=0;
  //plogw->rotator_sweep_step=2;
  
  // tcp comdbuf, ringbuf init
  plogw->tcp_ringbuf.buf = tcp_ringbuf_buf;
  plogw->tcp_ringbuf.len = NCHR_TCP_RINGBUF;
  plogw->tcp_ringbuf.wptr = 0;
  plogw->tcp_ringbuf.rptr = 0;

  plogw->tcp_cmdbuf_ptr = 0;
  plogw->tcp_cmdbuf_len = NCHR_TCP_CMD;
  memset(plogw->tcp_cmdbuf, '\0', NCHR_TCP_CMD + 1);

  plogw->f_off_contest=0; // default off the contest flag 
  
  plogw->mask = 0xff;  // cw/ssb both ok ... 0xff cw/ssb not ok 0xff-3
  //plogw->bandid_bandmap = 0;
  plogw->cmd_ptr = 0;

  plogw->enable_callhist = 0;  // default OFF; enable explicitly when Call History is configured
  plogw->bandmap_summary_idx = 0;

  plogw->contest_id = 0;
  plogw->multi_type = 0;
  plogw->voice_memory_enable = 0;
  strcpy(plogw->contest_name+2, "");

  init_buf(plogw->my_callsign, LEN_CALL_WINDOW);
  init_buf(plogw->email_addr, LEN_EMAIL_ADDR_WINDOW);
  init_buf(plogw->my_name, LEN_CALL_WINDOW);
  init_buf(plogw->cluster_name, LEN_HOST_NAME);
  init_buf(plogw->hostname, LEN_HOST_NAME);  
  init_buf(plogw->zserver_name, LEN_HOST_NAME);
  init_buf(plogw->cluster_cmd, LEN_CLUSTER_CMD);
  init_buf(plogw->cluster2_name, LEN_HOST_NAME);
  init_buf(plogw->cluster2_cmd, LEN_CLUSTER_CMD);
  init_buf(plogw->sent_exch, LEN_SENT_EXCH_WINDOW);
  init_buf(plogw->contest_name,LEN_CONTEST_NAME);

  plogw->sent_exch_abbreviation_level=0; // control how abbreviate sent_exch 
  init_buf(plogw->jcc, LEN_JCC_WINDOW);
  init_buf(plogw->sat_name, LEN_SATNAME_WINDOW);
  init_buf(plogw->grid_locator, LEN_GL);
  //  init_buf(plogw->grid_locator_set, LEN_GL);   // grid_locator_set is not edit buffer but just string holder
  init_buf(plogw->power_code, LEN_POWER_WINDOW);

  init_buf(plogw->wifi_ssid, LEN_WIFI_SSID_WINDOW);
  init_buf(plogw->wifi_passwd, LEN_WIFI_PASSWD_WINDOW);

  // set default values
  strcpy(plogw->hostname+2,"dvplogger");  
  strcpy(plogw->wifi_ssid+2,"testap");
  strcpy(plogw->wifi_passwd+2,"testtest");


  strcpy(plogw->sent_exch + 2, "11");   // sent_exch describe rule of the sent exchange including power code, and frequency dependent 
  //  strcpy(plogw->my_callsign + 2, "JK1DVP/1");
  strcpy(plogw->my_callsign + 2, "CallSign");  
  // default power_code
  strcpy(plogw->power_code + 2, "MMMMMMMMMPPP");
  //                             137122514125
  //                             85 418043...
  //                                    40246

  //  strcpy(plogw->email_addr + 2, "jk1dvp@gmail.com");
  strcpy(plogw->email_addr + 2, "email@address");  
  strcpy(plogw->grid_locator + 2, "PM95UP");
  strcpy(plogw->grid_locator_set,plogw->grid_locator+2);
  
  strcpy(plogw->my_name + 2, "NoName");
  strcpy(plogw->cluster_name + 2, "arc.jg1vgx.net:7000");                 // default ip address for cluster
  strcpy(plogw->cluster_cmd + 2, "set dx fil cty=ja and SpotterCty=ja");  // default additional command for cluster
  strcpy(plogw->cluster2_name + 2, "");  // disabled until configured
  strcpy(plogw->cluster2_cmd + 2, "set dx fil cty=ja and SpotterCty=ja");

  plogw->seqnr = 0;
  for (int i=0;i<N_BAND;i++) {
    plogw->seqnr_band[i]=0;
  }
  
  plogw->qsoid=0;
  plogw->txnum=7;// txnum is unique to the logbox determined fron hostname (last digit=0 7 1->8 2->9) for zLog
  
  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
  for (int i = 0; i < N_CWMSG; i++) {
    init_buf(plogw->cw_msg[i], LEN_CWMSG_WINDOW);
    // set default cw message
    char *p;
    switch (i) {
      case 0:  // F1
        p = "$U $I $I $T"; // $U=CQ $T=TEST
        break;
      case 1:  // F2 send exchange
        p = "$V$W$P";
        break;
      case 2:  // F3 TU
        p = "$A $I $T"; // $A=TU $T=TEST
        break;
      case 3:  // F4 send my callsign
        p = "$I";
        break;
      case 4:  // F5 send callsign and number (ESM)
        p = "$C $V$W$P";
        break;
      case 5:  // F6  send callsign and number with JCC code
        p = "$C $V$J$P";
        break;
      case 6:          // F7  send number with JCC code
        p = "$V$J$P";  // $P power_code
        break;
      default:
        p = "";
        break;
        break;
    }
    strcpy(plogw->cw_msg[i] + 2, p);

    init_buf(plogw->rtty_msg[i], LEN_CWMSG_WINDOW);
    // set default cw message
    switch (i) {
      case 0:  // F1
        p = "CQ $I $I CQ";
        break;
      case 1:  // F2
        p = "$V $W $W $I";
        break;
      case 2:  // F3
        p = "TU $I CQ";
        break;
      case 3:  // F4
        p = "$I $I";
        break;
      case 4:  // F5
        p = "$C $V $W $W $C";
        break;
      default:
        p = "";
        break;
        break;
    }
    strcpy(plogw->rtty_msg[i] + 2, p);
  }
  plogw->sat = 0;            // 1 satellite op mode 0 contest op mode
  plogw->sat_freq_mode = 0;  // default is indipendent

  plogw->freq_tolerance = 1;  // freq adjustement over freq_tolerance (3Hz)
  plogw->up_f = 0;
  plogw->dn_f = 0;
  plogw->up_f_prev = 0;
  plogw->dn_f_prev = 0;

  plogw->tle_unixtime=0; // tle information unixtime
}

/// partial check
void display_partial_check(struct radio * radio)
{
  int flag;
  int nentry;
  int dupe; // flag shows this callsign IS dupe
  /// dupe 1 exact match flagged in partial check shown by 'D'
  // dupe 2  part match station worked in the bandmode shown by 'd'
  int worked;  // flag shows this callsign has been WORKED during the contest
  char exch[30];
  int bandmode1;
  int mask;
  bandmode1=bandmode(radio);
  mask=plogw->mask;
  nentry=radio->check_entry_list.nentry;
  if (nentry>0) {
    char buf[100];
    *dp->lcdbuf='\0';
    for(int i=0;i<nentry;i++) {
      flag=radio->check_entry_list.entryl[i].flag;
      exch[0]='\0';
      dupe=0;
      worked=0;
      
      // check flags
      if (flag&CHECK_ENTRY_FLAG_DISPLAYED) continue;
      if (flag&CHECK_ENTRY_FLAG_DUPE) {
	dupe|=1;
	strcpy(exch,radio->check_entry_list.entryl[i].exch);  // overwrite exchange with duped one 
      }
      if (flag&CHECK_ENTRY_FLAG_DUPECHECK_LIST) {
	worked=1;
	if (!(dupe&1)) {
	  strcpy(exch,radio->check_entry_list.entryl[i].exch); // exchange may be different in other mode/band
	}
	if ((radio->check_entry_list.entryl[i].bandmode & mask) == (bandmode1 & mask)) {
	  // part match in dupecheck_list and possible for dupe
	  dupe|=2;
	}
      }
      if (!dupe && !worked) {
	strcpy(exch,radio->check_entry_list.entryl[i].exch); // only if 
      }
      // check if the entry callsign appears in later entries (among dupecheck_list and )
      for (int j=i+1;j<nentry;j++) {
	if (strcmp(radio->check_entry_list.entryl[i].callsign,radio->check_entry_list.entryl[j].callsign)==0) {
	  // the same callsign 
	  if (radio->check_entry_list.entryl[j].flag &CHECK_ENTRY_FLAG_DUPECHECK_LIST) {
	    worked=1;
	    if (!(dupe&1)) {
	      strcpy(exch,radio->check_entry_list.entryl[j].exch); // exchange may be different in other mode/band
	    }
	    if ((radio->check_entry_list.entryl[j].bandmode & mask) == (bandmode1 & mask)) {
	      // part match in dupecheck_list and possible for dupe
	      dupe|=2;
	    }
	  } else {
	    if (!worked) {
	      strcpy(exch,radio->check_entry_list.entryl[j].exch); // exchange may be different in other mode/band	      
	    }
	  }
	  if (radio->check_entry_list.entryl[j].flag & CHECK_ENTRY_FLAG_DUPE) {
	    dupe |= 1;
	    strcpy(exch,radio->check_entry_list.entryl[j].exch); // exchange may be different in other mode/band	      	    
	  }
	  radio->check_entry_list.entryl[j].flag|=CHECK_ENTRY_FLAG_DISPLAYED;
	}
      }
      sprintf(buf,"%-9s %-8s %c%c\n",radio->check_entry_list.entryl[i].callsign,
	      exch,
	      (dupe&1) ? 'D': ((dupe&2) ? 'd': ( worked ? 'W':' ')),
	      radio->check_entry_list.cursor==i ? '<':' '
	      );
      strcat(dp->lcdbuf,buf);
    }
  } else {
    strcpy(dp->lcdbuf,"none found\n");
  }
  upd_display_info_flash(dp->lcdbuf);
  
}

int exch_partial_check(struct radio *radio,char *exch,unsigned char bandmode,unsigned char mask,int callhist_check,struct check_entry_list *entry_list)
{
  //  struct radio *radio;
  //  radio = so2r.radio_selected();
  // search partial from exchange
  struct check_entry *entry;
  entry_list->nentry=0;// number of matched entries
  entry_list->dupe=0;// number of matched entries
  int len;
  len=strlen(exch);
  char *exchange,*part;

  // check all qso in dupechk
  for (int i = 0; i < dupechk->ncallsign; i++) {
    entry=&entry_list->entryl[entry_list->nentry];
    entry->flag=0;
    exchange=dupechk->exch[i];
    part=strstr(exchange,exch);

    
    if (part!=NULL) { // substring exch found in target exchange in dupechk list
      strcpy(entry->callsign,dupechk->callsign[i]);
      strcpy(entry->exch,dupechk->exch[i]);
      entry->bandmode=dupechk->bandmode[i];
      entry->flag|=CHECK_ENTRY_FLAG_DUPECHECK_LIST;
      entry_list->nentry++;

      if (strlen(dupechk->callsign[i]) == strlen(radio->callsign+2)) {
	// exact match
	entry->flag|=CHECK_ENTRY_FLAG_EXACT_MATCH;
	if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
	  // dupe = worked
	  entry->flag|=CHECK_ENTRY_FLAG_DUPE;
	  entry_list->dupe++;
	} 
      }
      
      // number of found entry in range?
      if ((entry_list->nentry>=10)|| (entry_list->nentry>=entry_list->nmax_entry)) {
	//
	if (entry_list->nentry<=entry_list->cursor) {
	  entry_list->cursor=entry_list->nentry-1;
	}
	if (entry_list->cursor<0) {
	  entry_list->cursor=0;
	}
	return entry_list->nentry; // end of search
      }
    }
  }
  // end of dupechk search

  if (callhist_check && callhist_at == 0) {
    // callhist_list search
    char *callsign;    
    int i=0;
    char callsign_exch[20];//,*exch;
    while (1) {
      entry=&entry_list->entryl[entry_list->nentry];
      //    entry=&check_entry_list[entry_list->nentry];
      entry->flag=0;
    
      strcpy(callsign_exch,callhist_list[i]); // process by strtok so need to copy
      if (*callsign_exch=='\0') {
	// end of the callhist_list to search
	break;
      }
      callsign=strtok(callsign_exch," ");
      if (callsign!=NULL) {
	exchange=strtok(NULL," ");
	if (exchange!=NULL) {
	  // check the exchange by strstr
	  part=strstr(exchange,exch);
	  if (part!=NULL) { // substring call found in target callsign in dupechk list
	    // check if the same entry (s) already exists in the list
	    for (int j=0;j<entry_list->nentry;j++) {
	      if (strcmp(entry_list->entryl[j].callsign,callsign)==0) {
		goto end_check_callhist1;
	      }
	    }
	    // if check passed
	    strcpy(entry->callsign,callsign);
	    strcpy(entry->exch,exchange);
	    entry->flag|=CHECK_ENTRY_FLAG_CALLHIST_LIST;
	    entry_list->nentry++;
	    if (strlen(callsign) == strlen(radio->callsign+2)) {
	      // exact match with callsign window
	      entry->flag|=CHECK_ENTRY_FLAG_EXACT_MATCH;
	      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
		// dupe = worked
		entry->flag|=CHECK_ENTRY_FLAG_DUPE;
		entry_list->dupe ++;
	      } 
	    }
	    // number of found entry in range?
	    if ((entry_list->nentry>=10)|| (entry_list->nentry>=entry_list->nmax_entry)) {
	      if (entry_list->nentry<=entry_list->cursor) {
		entry_list->cursor=entry_list->nentry-1;
	      }
	      if (entry_list->cursor<0) {
		entry_list->cursor=0;
	      }
	      return entry_list->nentry; // end of search
	    }
	    
	  }
	}
      }
    end_check_callhist1:
      i++; // callhist_list index i; to the next entry
    }
  }
  if (entry_list->nentry<=entry_list->cursor) {
    entry_list->cursor=entry_list->nentry-1;
  }
  if (entry_list->cursor<0) {
    entry_list->cursor=0;
  }
  return entry_list->nentry;  // not dupe
  
}



// partial string search by call and obtain list of possible callsign and exchanges with callhist_list or dupechk entries, and dupe
static int dupe_partial_check_local(const char *call,
                                    unsigned char bandmode,
                                    unsigned char mask,
                                    struct check_entry_list *entry_list)
{
  const int len = strlen(call);

  for (int i = 0; i < dupechk->ncallsign; i++) {
    const char *callsign = dupechk->callsign[i];
    if (!dupe_callsign_partial_match(callsign, call)) continue;

    struct check_entry *entry = &entry_list->entryl[entry_list->nentry];
    entry->flag = CHECK_ENTRY_FLAG_DUPECHECK_LIST;
    strcpy(entry->callsign, callsign);
    strcpy(entry->exch, dupechk->exch[i]);
    entry->bandmode = dupechk->bandmode[i];

    if (strchr(call, '-') == NULL && len == strlen(callsign) &&
        dupe_callsign_equal(callsign, call)) {
      entry->flag |= CHECK_ENTRY_FLAG_EXACT_MATCH;
      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
        entry->flag |= CHECK_ENTRY_FLAG_DUPE;
        entry_list->dupe++;
      }
    }

    entry_list->nentry++;
    if ((entry_list->nentry >= 10) ||
        (entry_list->nentry >= entry_list->nmax_entry)) {
      break;
    }
  }

  return entry_list->nentry;
}

static int dupe_partial_check_history(const char *call,
                                      unsigned char bandmode,
                                      unsigned char mask,
                                      struct check_entry_list *entry_list)
{
  if (dupechk->dupechk_at == 1) {
    return query_dupechk_partial_subcpu(call, bandmode, mask, entry_list);
  }

  return dupe_partial_check_local(call, bandmode, mask, entry_list);
}

// partial string search by call and obtain list of possible callsign and exchanges with callhist_list or dupechk entries, and dupe
int dupe_partial_check(const char *call,unsigned char bandmode,unsigned char mask, int callhist_check,struct check_entry_list *entry_list)
{
  struct check_entry *entry;
  entry_list->nentry=0;// number of matched entries
  entry_list->dupe=0;// number of matched entries
  int len;
  len=strlen(call);
  char *callsign,*part;

  if (verbose&4) {
    console->print("dupe_partial_check(): bandmode=");console->println(bandmode,HEX);
  }

  // Search the QSO history using the database owner.  The remainder of this
  // function merges those results with the call history database on main CPU.
  dupe_partial_check_history(call, bandmode, mask, entry_list);

  if ((entry_list->nentry >= 10) ||
      (entry_list->nentry >= entry_list->nmax_entry)) {
    if (entry_list->nentry <= entry_list->cursor) {
      entry_list->cursor = entry_list->nentry - 1;
    }
    if (entry_list->cursor < 0) entry_list->cursor = 0;
    return entry_list->nentry;
  }
  // end of dupechk search

  if (callhist_check && callhist_at == 0) {
    // callhist_list search
    int i=0;
    char callsign_exch[20],*exch;
    while (1) {
      entry=&entry_list->entryl[entry_list->nentry];
      //    entry=&check_entry_list[entry_list->nentry];
      entry->flag=0;
    
      strcpy(callsign_exch,callhist_list[i]); // process by strtok so need to copy
      if (*callsign_exch=='\0') {
	// end of the callhist_list to search
	break;
      }
      callsign=strtok(callsign_exch," ");
      if (callsign!=NULL) {
	exch=strtok(NULL," ");
	if (exch!=NULL) {
	  // Ordinary entry keeps substring matching; RTTY '-' is a one-character
	  // wildcard over the complete callsign.
	  part = dupe_callsign_partial_match(callsign, call) ? callsign : NULL;
	  if (part!=NULL) { // partial/wildcard call found
	    // check if the same entry (s) already exists in the list
	    for (int j=0;j<entry_list->nentry;j++) {
	      if (strcmp(entry_list->entryl[j].callsign,callsign)==0) {
		goto end_check_callhist;
	      }
	    }
	    // if check passed
	    strcpy(entry->callsign,callsign);
	    strcpy(entry->exch,exch);
	    entry->flag|=CHECK_ENTRY_FLAG_CALLHIST_LIST;
	    entry_list->nentry++;
	    if (strchr(call, '-') == NULL && len == strlen(callsign) &&
              dupe_callsign_equal(callsign, call)) {
	      // exact match
	      entry->flag|=CHECK_ENTRY_FLAG_EXACT_MATCH;
	    }
	    // number of found entry in range?
	    if ((entry_list->nentry>=10)|| (entry_list->nentry>=entry_list->nmax_entry)) {
	      if (entry_list->nentry<=entry_list->cursor) {
		entry_list->cursor=entry_list->nentry-1;
	      }
	      if (entry_list->cursor<0) {
		entry_list->cursor=0;
	      }
	      return entry_list->nentry; // end of search
	    }
	    
	  }
	}
      }
    end_check_callhist:
      i++; // callhist_list index i; to the next entry
    }
  }
  if (entry_list->nentry<=entry_list->cursor) {
    entry_list->cursor=entry_list->nentry-1;
  }
  if (entry_list->cursor<0) {
    entry_list->cursor=0;
  }
  return entry_list->nentry;  // not dupe
}


// ESP32のRAM帯域ざっくり判定（必要なら調整）
static inline bool is_valid_ram_ptr(const void* p) {
    uintptr_t a = (uintptr_t)p;
    // 内部DRAM
    if (a >= 0x3FF80000 && a < 0x40000000) return true;
    // PSRAM（使っているなら）
    if (a >= 0x3F800000 && a < 0x3FC00000) return true;
    return false;
}

    
// check dupe and obtain exchange in the call history; use in cluster
// return with number of
int dupe_callhist_check(const char *call,unsigned char bandmode, unsigned char mask,int callhist_check, char **exch_history) {
  // **exch_history copy the pointer of the exch string in dupechk structure
  int i;
  int ret;
  //
  //  struct radio *radio;
  //  radio = so2r.radio_selected();
  *exch_history=NULL;  
  if (verbose &4) console->println("dupe_callhist_check()");
  //  return 0; 

  // 入力の健全性check
  if (!call || !is_valid_ram_ptr(call)) {
    ESP_LOGE("DUPE", "bad call ptr: %p", call);
    return 0;
  }
  

  // When the large QSO history/CALLHIST lives on the SubCPU, use the
  // unified query.  The old code below only searches the main CPU's
  // callhist_list and therefore could not mark CALLHIST-derived new multis.
  if (dupechk->dupechk_at == 1) {
    static char remote_exch[LEN_EXCH + 1];
    ret = dupe_check_with_exch(call, bandmode, mask,
                               remote_exch, sizeof(remote_exch));
    if (remote_exch[0]) *exch_history = remote_exch;
    return ret;
  }

  int f_callhist = 1;
  // check all qso
  ret = 0;
  for (i = 0; i < dupechk->ncallsign; i++) {
    if ((ret == 0) || (f_callhist == 1)) {
      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
        // current band and mode
        if (dupe_callsign_equal(dupechk->callsign[i], call)) {
          // dupe
          ret = 1;
        }
      } else if (f_callhist) {
        // other band and mode
        if (dupe_callsign_equal(dupechk->callsign[i], call)) {
          // hit !
          *exch_history=dupechk->exch[i];
          f_callhist = 0;  // no longer need to search for history
        }
      }
    } else {
      break;
    }
  }
  if ((ret != 1) && (f_callhist == 1) && (callhist_check)) {
    // not dupe and not yet worked in other band
    // if enabled search for the history
    if (plogw->enable_callhist) {
      // flash stored callhist_list search
      search_callhist_list_exch((const char **)callhist_list,call,0,exch_history);
      //      search_callhist_list(callhist_list,call,0); // if 1, match callsign body (before /?) 0, full callsign match
    }
  }
  return ret;  // not dupe
  
}
  

