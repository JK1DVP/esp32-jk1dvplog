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

// memo
// behavior in So2r keep cq alternating radio rather than (modal)
// even in ESM mode
// may be effective switch rig always when sending response
// S&P sending cancel cqing in the main rig and send message
// these transition logic should be well defined 
// we need to enable radio to switch
// show bandmap back to cq mode
// rig does not set 2nd rig ???  serial is not initialized correctly by loading
// bandmask is strange for IC-9700
// sub main rig choise should depend on the bandmask for each rig

// selecting ic rigs select Radio4 ... strange

// interesting on am carrier frequency change
// mode change send band scope width! --> inplemented need to test

#include "Arduino.h"
#include "main.h"
#include "decl.h"
#include "variables.h"
#include "so2r.h"
#include "cat.h"
#include "cw_keying.h"
#include "bandmap.h"
#include "ui.h"
#include "cw_keying.h"
#include "settings.h"
#include "display.h"
#include "edit_buf.h"
#include "dupechk.h"
#include "multi.h"
#include "multi_process.h"
#include "qso.h"
#include "log.h"
#include "misc.h"
#include "cluster.h"
#include "callhist.h"
#include "callhist_remote.h"
#include <hidboot.h>
#include "SD.h"
#include "contest.h"
#include "user_contest_md.h"
#include "keyboard.h"
#include "satellite.h"
#include "sd_files.h"
#include "cmd_interp.h"
#include "network.h"
#include "main.h"
#include "zserver.h"
#include "cp932_utf8.h"
#include "antenna.h"
#include "cty_chk.h"
#include "iambic_keyer.h"
#include "mux_transport.h"
#include "timekeep.h"

// User-initiated focus changes should refresh the bandmap immediately.
// SO2R sequencing continues to call so2r.change_focused_radio() directly,
// so temporary TX/RX focus changes do not switch the displayed bandmap.
static void change_focused_radio_manual(int new_focus)
{
  if (new_focus < 0 || new_focus >= N_RADIO) return;

  so2r.change_focused_radio(new_focus);

  struct radio *radio = &radio_list[new_focus];
  if (radio->bandid > 0) {
    radio->bandid_bandmap = radio->bandid;
  }

  bandmap_disp.f_update = 1;
  upd_display_bandmap();
}

struct HelpPage {
  const char *line[6];
};

// Pages 0..6 preserve the existing HELP order and wording.
// Later pages contain only CALLSIGN commands or only shortcut keys.
static const HelpPage help_pages[] = {
  {{"A-Home:SW_RIG", "A-End:Tgl_Rig", "A-x:Tgl_Xvtr", "A-m:SW_Mode", "A-t:Xmit,-b:BMapSrt", "\\:Focus Radio"}},
  {{"SATELLITE", "NEW/READ/MAIL/", "DUMPQSOLOG", "MAKEDUPE,MEMSTAT", "LISTDIR,SAVE/LOAD", "HELP/HELPR"}},
  {{"C-r,c,s:Rem,Cl,RST", "C-f:Rem->Mul,-v:Tgl_Voice", "C-j,p,n,o:Cur,Prev,Next,Last", "C-1,2:C/PDupe,Contest", "C-3,t,y:MaskBandmap,MultiShow", "C-S-2:Prev Contest"}},
  {{"C-'-':editQSO", "C-4:callhist", "C-5:SO1R/SAT/SO2R", "C-u:Score", "PGUP/DN:CW SPD", "C-v:Voice"}},
  {{"A-'-':Scope", "A-q:CW/S&P", "A-a:nextAOS", "A-p:pick sat AOS", "A-b:beacon ", "A-c:CW/RTTY edit"}},
  {{"A-d,n:bandmap del/add.", "A-g:tone key", "A-Spc:pick spot", "A-DEL:show rig info", "A-f:set center freq.", "A-r:vfo mode sw"}},
  {{"A-w:wipe QSO", "C-w:clear field", "A-<=>:bandmap sel.", "A-m:mode", "A-<>:band", "C-l:QSLcard"}},

  // CALLSIGN command index. Common prefixes/suffixes are compacted.
  {{"NEW/READ/MAIL-QSOLOG", "DUMPQSOLOG/LISTQSOFILE", "SWITCHLOG/ZMERGE", "MAKEDUPE/DUPERESET", "RESTARTLOG/EXITEMU", "SAVE/LOAD"}},
  {{"LOAD/SAVE/RESETRIGS", "NEXTRIG/PREVRIG", "ENABLE/DISABLE-RIG", "BAND/RADIO", "BANDEN/BANDMASK/BANDMAP", "AUTOOFFnn/NATTO"}},
  {{"DISPTYPE0/1/2", "RESETDISP", "DISPCLOCKJST/UTC", "OLDESTnn", "ANTENNA[STATUS]", "ANTENNAON/OFF"}},
  {{"ESM/ALTCQ/2BSIQ", "OFF/ONCONTEST", "CONTEST", "KEY/STRAIGHT", "TOGGLEPTT", "CWJQF/CWNORMAL"}},
  {{"KBDJP/US", "PADDLENOR/REV", "IAMBICA/B", "MUX/NOMUXTRANS", "CALLHIST<file>", "CALLHISTMAIN/SUB"}},
  {{"WIFI", "SUBCPURESET", "VERBOSEnn/DEBUG", "LISTDIR/MEMSTAT/ADCSTAT", "HELP/HELPR", "SATELLITE"}},
  {{"XITON", "XITOFF", "XITRESET", "UP/DN:+/-20Hz S&P", "Yaesu:TX-only CLAR", "SAVE stores offset"}},

  // Shortcut supplement only; no CALLSIGN commands on this page.
  {{"A-s:track mode", "\\:Focus Radio", "C-S-2:Prev Contest", "C-S-t/y:Multi prev/next", "C-S-c/r:Contest/RIG", "ESC:Cancel TX"}},
};

static constexpr int HELP_PAGE_COUNT =
    sizeof(help_pages) / sizeof(help_pages[0]);

#include "processes.h"
#include "so2r.h"
#include "dac-adc.h"
#include "morse_decoder_simple.h"

static void trace_band_key_event(const char *name, int key,
                                 const MODIFIERKEYS &modkey,
                                 struct radio *radio)
{
  if (!(verbose & 16) || !radio) return;
  console->printf(
      "KEYEV t=%lu src=%s radio=%d key=0x%02X "
      "LAlt=%d RAlt=%d LCtrl=%d RCtrl=%d LShift=%d RShift=%d "
      "actual=%u/b%d pending=%d target=%u/b%d\n",
      (unsigned long)millis(), name, radio->rig_idx, key,
      modkey.bmLeftAlt, modkey.bmRightAlt,
      modkey.bmLeftCtrl, modkey.bmRightCtrl,
      modkey.bmLeftShift, modkey.bmRightShift,
      radio->freq, radio->bandid,
      radio->f_freqchange_pending,
      radio->freq_target, radio->bandid_target);
}

#include "esp32_flasher.h"



static bool restart_subcpu_and_enter_mux() {
  static const char ack[] = "go_mux accepted";
  size_t ack_pos = 0;
  uint32_t start_ms;
  uint32_t next_send_ms;

  f_mux_transport = 0;

  /* Discard bytes left from the previous MUX session before reset. */
  while (Serial2.available()) Serial2.read();

  loader_port_reset_target_func();
  start_ms = millis();
  next_send_ms = start_ms + 500;

  /*
   * SUBCPU setup includes Bluetooth/UART initialization.  A fixed 800 ms
   * delay is not sufficient on every boot, so keep sending the raw command
   * until the SUBCPU explicitly acknowledges it.
   */
  while ((uint32_t)(millis() - start_ms) < 8000) {
    uint32_t now = millis();
    if ((int32_t)(now - next_send_ms) >= 0) {
      Serial2.print("\r\ngo_mux\r\n");
      Serial2.flush();
      next_send_ms = now + 250;
    }

    while (Serial2.available()) {
      char c = (char)Serial2.read();
      if (c == ack[ack_pos]) {
        ack_pos++;
        if (ack_pos == sizeof(ack) - 1) {
          /* Drain the trailing CR/LF before enabling the binary parser. */
          delay(20);
          while (Serial2.available()) Serial2.read();
          f_mux_transport = 1;

          uint32_t ready_until = millis() + 300;
          while ((int32_t)(millis() - ready_until) < 0) {
            mux_transport.recv_pkt();
            delay(1);
          }
          return true;
        }
      } else {
        ack_pos = (c == ack[0]) ? 1 : 0;
      }
    }
    delay(1);
  }

  return false;
}

int ui_cursor_next_entry_partial_check (struct radio *radio)
{
  // partial check cursor move to the next entry
  radio->check_entry_list.cursor++;
  if (radio->check_entry_list.cursor>=radio->check_entry_list.nentry) {
    radio->check_entry_list.cursor=0;
  }
  display_partial_check(radio);
  return 0;
}

int ui_pick_partial_check(struct radio *radio)
{
  plogw->ostream->println("ui_pick_partial_check()");
  set_callsign_and_request_dupe(
      radio,
      radio->check_entry_list.entryl[radio->check_entry_list.cursor].callsign,
      true);
  strcpy(radio->recv_exch + 2, radio->check_entry_list.entryl[radio->check_entry_list.cursor].exch);
  radio->recv_exch[1]=strlen(radio->recv_exch+2);
  upd_display();
  return 0;
}

int ui_toggle_partial_check_flag()
{
  // toggle partial check
  if (plogw->f_partial_check & PARTIAL_CHECK_CALLSIGN_AUTO) {
    plogw->f_partial_check &=~PARTIAL_CHECK_CALLSIGN_AUTO;
  } else {
    plogw->f_partial_check |=PARTIAL_CHECK_CALLSIGN_AUTO;		
  }
  sprintf(dp->lcdbuf,"partial chk=%d\n",plogw->f_partial_check);
  upd_display_info_flash(dp->lcdbuf);	      
  return 0;
}

int ui_perform_exch_partial_check(struct radio *radio)
{

  if (plogw->f_partial_check&PARTIAL_CHECK_CALLSIGN_AUTO) {
    //partial check

    // 3 characters
    // partial check
    int nentry;
    int flag;
		  
    // check
    radio->check_entry_list.cursor=0; // reset cursor to top
    radio->check_entry_list.nmax_entry=5;
    exch_partial_check(radio,radio->recv_exch+2,bandmode(radio),plogw->mask,1,&radio->check_entry_list);
    display_partial_check(radio);
  }
  return 0;

}


int ui_perform_partial_check(struct radio *radio)
{
  //  log_d(VERBOSE_UI,"ui_perform_partial_check()\n");
  if (plogw->f_partial_check&PARTIAL_CHECK_CALLSIGN_AUTO) {
    //partial check

    // 3 characters
    // partial check
    int nentry;
    int flag;
		  
    if (strlen(radio->callsign + 2) >=3) {
      if (strcmp(radio->callsign_prev+2,radio->callsign+2)==0) {
      } else {
	// new check
	radio->check_entry_list.cursor=0; // reset cursor to top
	strcpy(radio->callsign_prev+2,radio->callsign+2); // store current callsign to callsign_prev
	radio->check_entry_list.nmax_entry=5;
	dupe_partial_check(radio->callsign+2,bandmode(radio),plogw->mask,1,&radio->check_entry_list);
      }
      display_partial_check(radio);		

    }
  }
  return 0;
}


int ui_response_call_and_move_to_exch(struct radio *radio)
{
  so2r.send_call_exch();
  return 0;
}  




int ui_send_mycall(struct radio *radio)
{
  // repeat function off
  so2r.cancel_msg_tx();
      
  so2r.set_msg_tx_to_focused();
  // Match the normal function-key transmission path.  In SO2R the message
  // radio and the hardware TX radio are separate state variables; without
  // this switch, an ESM MyCALL from radio 1 is queued while tx_ can still be
  // radio 0.
  so2r.set_rx_in_sending_msg();
  so2r.set_tx_to_msg_tx();
  so2r.sequence_stat(SO2R::Sending_Msg);
  if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying) ) { 
    // send my callsign
    strcpy(radio->callsign_previously_sent, radio->callsign + 2);
    append_cwbuf_string(plogw->cw_msg[3] + 2);  // F4 my callsign
    append_cwbuf('$');     
  } else if (radio->modetype== LOG_MODETYPE_PH) {
    if (plogw->voice_memory_enable>=2) {
      send_voice_memory(radio, 4);  // F4 my callsign
    }
  } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
    // send call and exchange , then move to my exch entry
    set_rttymemory_string(radio, 4, plogw->rtty_msg[3] + 2);  // set rtty memory on rig
    // and send it on the air
    //                          delay(200);
    //                          send_rtty_memory(radio, 5);
  }

  return 0;
}


void show_cw_spd()
{
      sprintf(dp->lcdbuf, "CW_SPD=%d\n", cw_spd);
      upd_display_info_flash(dp->lcdbuf);
      if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R) {
	if (plogw->f_so2rmini) {
	  //            tmpbuf[0] = 0x02;
	  //            tmpbuf[1] = cw_spd;
	  //            SO2Rsend((uint8_t *)tmpbuf, 2);
	}
      }
  
}

int modkey_shift(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftShift == 1) || (modkey.bmRightShift == 1)) {  
    return 1;
  } else {
    return 0;
  }
}

int modkey_ctrl(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftCtrl == 1) || (modkey.bmRightCtrl == 1) || (f_capslock)) {
    return 1;
  } else {
    return 0;
  }
}
int modkey_alt(MODIFIERKEYS modkey)
{
  if ((modkey.bmLeftAlt == 1) || (modkey.bmRightAlt == 1)) {
    return 1;
  } else {
    return 0;
  }
}

void send_single_char_radio(struct radio *radio,char c)
{
  if (radio->modetype == LOG_MODETYPE_CW || (radio->f_tone_keying)) {
    // Manual keyboard CW: remember the actual LCD/input radio.  The physical
    // keying layer will use this instead of the SO2R sequence TX radio.
    set_manual_cw_radio(radio->rig_idx);
    // CW
#if JK1DVPLOG_HWVER >=2
    if (radio->f_tone_keying) {
      char buf[5];
      c=append_cwbuf_convchar(c);
      sprintf(buf,"%c",c);
      play_wpm_set();
      play_cw_cmd(buf);
    } else {
      append_cwbuf(c);
    }
#else
      append_cwbuf(c);    
#endif
  } else if (radio->modetype == LOG_MODETYPE_PH) {
    // phone
    char buf[5];
    if (c=='?') c='/';
    if (c=='\"') c='/';
    sprintf(buf,"%c",tolower(c));
    play_string_cmd(buf);
  }
}

static char *current_edit_buffer(struct radio *radio) {
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    return plogw->cw_msg[radio->ptr_curr - 10];
  }
  if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
    return plogw->rtty_msg[radio->ptr_curr - 30];
  }

  switch (radio->ptr_curr) {
  case 0:  return radio->callsign;
  case 1:  return radio->recv_exch;
  case 2:  return radio->sent_rst;
  case 3:  return radio->recv_rst;
  case 4:  return plogw->my_callsign;
  case 5:  return plogw->sent_exch;
  case 6:  return radio->remarks;
  case 7:  return plogw->sat_name;
  case 8:  return plogw->grid_locator;
  case 9:  return plogw->jcc;
  case 20: return radio->rig_name;
  case 21: return plogw->cluster_name;
  case 22: return plogw->email_addr;
  case 23: return plogw->cluster_cmd;
  case 24: return plogw->power_code;
  case 25: return plogw->wifi_ssid;
  case 26: return plogw->wifi_passwd;
  case 27: return radio->rig_spec_str;
  case 28: return plogw->zserver_name;
  case 29: return plogw->my_name;
  case 40: return plogw->contest_name;
  case 41: return plogw->cluster2_name;
  case 42: return plogw->cluster2_cmd;
  case 43: return plogw->hostname;
  default: return nullptr;
  }
}

static void clear_current_edit_field(struct radio *radio) {
  char *pwin = current_edit_buffer(radio);
  if (pwin == nullptr) {
    if (verbose & 1) {
      plogw->ostream->printf("Ctrl-W: no editable field for ptr_curr=%d\n", radio->ptr_curr);
    }
    return;
  }

  clear_buf(pwin);

  // Keep derived QSO indicators consistent with the field that was cleared,
  // without wiping any of the other QSO fields.
  if (radio->ptr_curr == 0) {
    radio->dupe = 0;
  } else if (radio->ptr_curr == 1) {
    radio->multi = -1;
  }

  if (verbose & 1) {
    plogw->ostream->printf("Ctrl-W: cleared field ptr_curr=%d\n", radio->ptr_curr);
  }
}

void on_key_down(MODIFIERKEYS modkey, uint8_t key, uint8_t c) {

if (key == 0x1f) {
  plogw->ostream->printf(
      "KEY2 rawmod=%02X ctrl=%d shift=%d alt=%d caps=%d\n",
      *((uint8_t *)&modkey),
      modkey_ctrl(modkey),
      modkey_shift(modkey),
      modkey_alt(modkey),
      f_capslock);
}
  
  plogw->count_autopoweroff=0; // reset auto powerdown counter
  //  verbose=1;
  if (verbose & 1) {
    plogw->ostream->print("Key=<");
    PrintHex<uint8_t>(key, 0x80);
    plogw->ostream->print(">c=");
    plogw->ostream->print((char)c);
    plogw->ostream->print("<");
    PrintHex<uint8_t>(c, 0x80);
    plogw->ostream->println(">");
    plogw->ostream->print("f_caps:");
    plogw->ostream->println(f_capslock);
  }
  if (f_printkey) {
    sprintf(dp->lcdbuf,"on key down\nKey %d\nc %c <%02x>\ncaps %d\n",
	    key,c,c,f_capslock);
    upd_display_info_flash(dp->lcdbuf);
  }
  if (key == 0x39) {
    // check capslock for another modifier
    f_capslock = 1;
  }
  //  console->print ("caps=");
  //  console->println (f_capslock);      

  uint8_t tmpbuf[2];

  struct radio *radio;
  radio = so2r.radio_selected();

  // Handle Ctrl-Shift-2 before the general Shift-key dispatcher.
  // This also covers CapsLock-as-Ctrl through modkey_ctrl().
  // Use the HID usage code (0x1f) rather than the translated character,
  // because Shift+2 produces different characters on different keyboards.
  if (modkey_shift(modkey) && modkey_ctrl(modkey) &&
      !modkey_alt(modkey) && key == 0x1f) {
    alternate_contest();
    return;
  }

  // the following is mixed with function in logw_handler() ... unify sometime 21/10/24 ea
  // Shift + alphanumeric key will be sent to cw keying regardless of keyer mode .. 21/11/22 ea
  // Ctrl-Shift shortcuts must be handled by the Ctrl branch below.
  // Previously the Shift branch consumed them before Ctrl-Shift-T/Y could run.
  if (modkey_shift(modkey) && !modkey_ctrl(modkey)) {
    ///// shift+
    if (!modkey_alt(modkey) && !modkey_ctrl(modkey)) {
      // only shift + --> basically cw keying
    
      /*      case 0x1f:
	      console->print ("shift+2 pressed caps=");
	      console->print (f_capslock);      
	      if ((modkey.bmLeftCtrl == 1) || (modkey.bmRightCtrl == 1) || (f_capslock==1) ) {
	      console->println("shift+2+ctrl pressed ");	
	      // shift-ctrl-2  // switch contest	
	      plogw->contest_id--;
	      if (plogw->contest_id <0 ) plogw->contest_id = 0;
	      set_contest_id();
	      }
	      break;
      */


      switch(key) {
	
      case 0x28:   // Enter + SHIFT
	process_enter(1);
	break;
      case 0x2b:  // TAB + SHIFT
      case 0x2c:  // SPACE + SHIFT
	switch (radio->keyer_mode) {
	case 0:
	  switch_logw_entry(1);
	  break;
	case 1: // keyer
	  //	  plogw->f_repeat_func_stat = 0;
	  so2r.suspend();
	  send_single_char_radio(radio,c);
	  break;
	}
	break;
      
      case 0x51:  // DOWN + SHIFT
	if (plogw->sat) {      
	  adjust_sat_offset(-100);
	} else { // adjust frequency of a station for the mode
	  adjust_frequency(-freq_width_mode(radio->opmode));	  
	}
	upd_display();      
	break;
      case 0x52:  // UP + SHIFT
	if (plogw->sat) {    
	  adjust_sat_offset(100);
	} else {
	  adjust_frequency(freq_width_mode(radio->opmode));	  
	}
	upd_display();            
	break;

	// shift + left / right  cursor in bandmap change
      case 0x50:  // left
	select_bandmap_entry(-1,radio->bandid_bandmap);
	break;
      case 0x4f:  //right
	select_bandmap_entry(+1,radio->bandid_bandmap);
	break;

      case 0x2a:  // BS + SHift
	delete_cwbuf();
	break;

      case 0x31:  // \ backslash to switch focused rig and RX to hear(on SO2R)
	switch (so2r.focused_radio()) {
	case 0:
	  //	  select_radio(1);
	  change_focused_radio_manual(1);
	  break;
	case 1:
	  //	  select_radio(2);
	  change_focused_radio_manual(2);	  
	  break;
	case 2:
	  //	  select_radio(0);
	  change_focused_radio_manual(0);	  
	  break;
	}

	break;
	//

      default:
	// shift+
	// function key  --> start rx switching in SO2R 
	if ((key >= 0x3a) && (key <= 0x45)) {
	  //	  plogw->f_repeat_func_stat = 0;
	  so2r.suspend();
	  so2r.radio_mode = SO2R::RADIO_MODE_SO2R; // force go to SO2R 
	  if (modkey_ctrl(modkey)) {
	    so2r.sequence_mode(SO2R::SO2R_Repeat_Func);
	  } else {
	    if (key==0x3a) { // CQ
	      so2r.sequence_mode(SO2R::SO2R_CQSandP);
	    }
	  }
	  so2r.cancel_msg_tx();	
	  so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
	  so2r.set_rx_in_sending_msg();
	  function_keys(key, c); 
	  break;
	}
	if (verbose &4) {
	console->print("key=");
	console->println ((int)key);
	}
	// shift only (somehow come here) leads to mulfunction of SO2R messaging, so check and pass through
	if (key==229 || key==225) {
	  if (verbose&4) console->println("shift only pass thru");
	  break;
	}
	
	// list ptr_curr Shift+ keyer mode interfere setting the window characters
	// sent_exch, Remarks, rig_name, cluster_name, email-addr, cluster_cmd, wifi_ssid, wifi_passwd, rig_spec, zserver_name, my_name
	if (!((radio->ptr_curr == 5) || (radio->ptr_curr == 6) || (radio->ptr_curr == 20) || (radio->ptr_curr == 21) || (radio->ptr_curr == 22) || (radio->ptr_curr == 23) || (radio->ptr_curr == 25) || (radio->ptr_curr == 26) || (radio->ptr_curr == 27) || (radio->ptr_curr == 28) || (radio->ptr_curr == 29) || (radio->ptr_curr == 40) || (radio->ptr_curr == 41) || (radio->ptr_curr == 42) || (radio->ptr_curr == 43) || ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) || ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)))) {
	  if (key == 0x37) {
	    // Shift-.  Shift-, manual band change up/down
	    radio->bandid_bandmap++;
	    if (radio->bandid_bandmap > N_BAND)
	      radio->bandid_bandmap = 1;  // allow bandid_bandmap == N_BAND
	    upd_display_bandmap();
	    break;
	  }
	  
	  if (key == 0x36) {
	    radio->bandid_bandmap--;
	    if (radio->bandid_bandmap < 1)
	      radio->bandid_bandmap = N_BAND ; // allow bandid_bandmap == N_BAND
	    upd_display_bandmap();
	    break;
	  }
	  // cluster edit mode send all characters

	} else {
	  logw_handler(key, c);  // in cw msg edit mode , send 'SPC' to editor (use TAB to switch entry)
	  break;
	}
	if (key >= 0x1e && key <= 0x26) {
	  c = ((int)key - 0x1e) + '1';
	}
	if (key == 0x27) c = '0';
	if (key == 0x87) c = '/';  // ad-hoc remapping next to ?/ key in Japanese keyboard but not so in US.
	//	plogw->f_repeat_func_stat = 0;
	if (so2r.sequence_stat()==SO2R::Sending_Msg) {
	  so2r.cancel_current_tx_message();
	  so2r.suspend();	  
	}
	so2r.set_msg_tx_to_focused();
	send_single_char_radio(radio,c);
	break;
      }
    } // !alt ! ctrl
    else {
      if (modkey_alt(modkey) && !modkey_ctrl(modkey)) {
	// shift + alt
	if (key == 0x04 ) {  // Shift-Alt-A show next aos
	  if (plogw->sat) {
	    plogw->nextaos_showidx += 5;
	    if (plogw->nextaos_showidx >= N_SATELLITES) {
	      plogw->nextaos_showidx = 0;
	    }
	    print_sat_info_aos();
	  }
	}
	else if (key==0x07) {
	  // Shift+ Alt-D   delete entry in the bandmap on_frequenry
	  delete_on_freq_entry_bandmap();
	  upd_display_bandmap();
	}
	else if (key==0x4a) {    // Shift-Alt-HOME   switch rig (sub)
	  if (!plogw->f_console_emu) plogw->ostream->println("select subrig");
	  switch_radios(1, -1);
	}
	else if (key== 0x4d) {  // Shift-Alt-END   enable/disable rig
	  if (!plogw->f_console_emu) plogw->ostream->println("enable/disable  subrig");
	  enable_radios(1, -1);
	}
	
      } // shift alt !ctrl 

    }
  } else if (modkey_alt(modkey)) {
    // ALT+
    // ALT
    // 0           1               2
    // 456778abcdef0123456789abcdef01234567
    // ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890
    if (modkey_ctrl(modkey)) {
      // Alt-Ctrl-
      switch (key) {
      case 0x4a:  // HOME   switch rig (sub)

	if (!plogw->f_console_emu) plogw->ostream->println("select 3rd rig ");
	// Alt-Shift-HOME sub rig change
	switch_radios(2, -1);
	break;
      case 0x4d:  // END   enable/disable rig
	if (!plogw->f_console_emu) plogw->ostream->println("enable/disable  3rd rig");
	enable_radios(2, -1);
	break;
      }
      return;
    }
	
    if (key == 0x38) { // alt-'/' antenna rotator sweep suspension 
      if (plogw->f_rotator_sweep_suspend>0) {
	plogw->f_rotator_sweep_suspend=0;
      } else {
	plogw->f_rotator_sweep_suspend=1;
      }
      //plogw->f_rotator_sweep_suspend = 1-plogw->f_rotator_sweep_suspend;
      plogw->ostream->print("rotator sweep suspend=");
      plogw->ostream->println((int)plogw->f_rotator_sweep_suspend);
    }
    if (key == 0x2e) { // Alt-'=' antenna relay1 toggling
      alternate_antenna_relay();
      return;
    }
    if (key == 0x0c) {  // Alt-i temporarily disable rig for 10 seconds
      int idx_radio = so2r.focused_radio();
      radio = &radio_list[idx_radio];
      if (radio->enabled) {
	enable_radios(idx_radio,0);
	temporarily_disabled_radio = idx_radio;
	timeout_rig_disable_temporally = millis()+10000;
      } else {
	// Alt-I on a disabled rig enables it immediately.  If this radio was
	// temporarily disabled by Alt-I, enable_radios() also cancels its timer.
	enable_radios(idx_radio,1);
      }
      return;
    }
    if (key == 0x2d) {  // Alt-'-'  recenter spectrum scope
      recenter_scope();
      return;
    }
    /*  if (key == 0x19) { // Alt-v 0x19 // this is removed to avoid unaware invoking verbose condition
	if (verbose) {
	verbose = 0;
	} else {
	verbose = 1;
	}
	return;
	}
    */
    if (key == 0x17) {  // Alt-T tx/rx toggle ptt --> tune command
      if (radio->ptt==0) {
	if (radio->power >0) {
	  radio->power_bak = radio->power;
	}
	set_power(radio, 8);
      }
      radio->ptt = 1 - radio->ptt;
      set_ptt_rig(radio, radio->ptt);
      if (radio->ptt==0) {
	if (radio->power_bak >0) {
	  set_power(radio, radio->power_bak);
	}
      }
      return;
    }
    if (key == 0x1b) {  // Alt-X transverter toggle
      switch_transverter();
      return;
    }

    if (key == 0x1e) {  // Alt-1  // set priority to this rig and band
      set_current_rig_priority();
      return;
    }

    if (key == 0x1d) {  // Alt-Z: switch the explicit CW <=> PH memory bank
      int target_modetype;
      switch (radio->modetype) {
      case LOG_MODETYPE_CW:
        target_modetype = LOG_MODETYPE_PH;
        break;
      case LOG_MODETYPE_PH:
        target_modetype = LOG_MODETYPE_CW;
        break;
      default:
        // Do not transition digital/undefined modes implicitly.
        return;
      }

      // Save the current bank, then recall exactly the requested bank.
      // recall_freq_mode_filt() normally follows modetype_bank and could
      // otherwise restore the bank that we have just left.
      const int source_modetype = radio->modetype;
      save_freq_mode_filt(radio);

      if (verbose & 16)
        console->printf("CWPH_SWITCH b=%d from_mt=%d to_mt=%d op=%s\n",
                        radio->bandid, source_modetype,
                        target_modetype, radio->opmode);

      recall_freq_mode_filt_for_modetype(radio, target_modetype);

      // Manual rigs have no CAT response that would trigger a later right-side
      // redraw.  Schedule both displays explicitly and keep the key handler
      // free of synchronous OLED transfers.
      request_display_update_on_demand();
      request_bandmap_update_on_demand();
      // CQ/S&P state remains the state remembered in the selected bank.
      return;
    }

    if (key == 0x14) {  // alt-q switch CQ / S&P
      // memorize current frequency to freqbank cq freqnency
      save_freq_mode_filt(radio);
      if (radio->cq[radio->modetype] == LOG_CQ) {
	// from cq to s&p
	radio->cq[radio->modetype] = LOG_SandP;
      } else {  // S&P
	// from s&p to cq
	radio->cq[radio->modetype] = LOG_CQ;
      }

      // XIT is effective only in S&P; CQ always forces zero offset.
      apply_xit_for_operating_mode(radio);

      if (radio->cq[radio->modetype]== LOG_CQ) {
	console->println("CQ");
      } else {
	console->println("SandP");	
      }
	
      // update cq_bank so that new frequency in cq state is recovered 
      radio->cq_bank[radio->bandid][radio->modetype]=radio->cq[radio->modetype];
      // recall previous memory of S&P
      recall_freq_mode_filt(radio);
      
      if (verbose & 1) {
	plogw->ostream->print("cq:");
	plogw->ostream->println(radio->cq[radio->modetype]);
      }
      upd_display_stat();
      //      u8g2_r.sendBuffer();  // transfer internal memory to the display
      right_display_sendBuffer();
      bandmap_disp.f_update = 0;
      upd_display_bandmap();
      upd_display();
      if (radio->cq[radio->modetype]== LOG_CQ) {
	console->println("CQ");
      } else {
	console->println("SandP");	
      }
      
      return;
    }
    if (key == 0x04) {  // Alt-A show next aos
      if (plogw->sat) {
	strcpy(dp->lcdbuf, "Calc next AOS-LOS\n");
	upd_display_info_flash(dp->lcdbuf);
	start_calc_nextaos();
      }
    }
    if (key == 0x13) {  // alt-p pick satellite of closest next aos
      if (plogw->sat) {

	if (*sat_info[satidx_sort[0]].name != '\0') {
	  strcpy(plogw->sat_name + 2, sat_info[satidx_sort[0]].name);
	  if (!plogw->f_console_emu) {
	    plogw->ostream->print("Picked satellite:");
	    plogw->ostream->println(plogw->sat_name + 2);
	  }
	  sat_name_entered();
	}

      } else {
	//if (key == 0x13) {
	// alt-p change verbose level of printing cluster input to serial
	cluster_verbose_level[0]++;
	if (cluster_verbose_level[0] >= 4) cluster_verbose_level[0] = 0;
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("Alt-P: cluster 1 verbose level = ");
	  plogw->ostream->println(cluster_verbose_level[0]);
	}
      }
      // }
    }
    if (key == 0x05) {
      // alt-b set sat beacon frequency
      // alt-b bandmap sorting change

      if (plogw->sat) {
	set_sat_beacon_frequency();
      } else {
	bandmap_disp.sort_type++;
	if (bandmap_disp.sort_type >= 2) bandmap_disp.sort_type = 0;
	// sort and update display
	//plogw->ostream->print("sort and update bandmap "); plogw->ostream->println(bandmap_disp.sort_type);
	//sort_bandmap(plogw->bandid_bandmap, bandmap_disp.sort_type);
	upd_display_bandmap();
      }
    }

    if (key == 0x06) {  // Alt-c to edit cw mode
      if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
	// current CW
	radio->ptr_curr = 30;  // RTTY message
      } else {
	if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
	  // rtty
	  radio->ptr_curr = 0;
	} else {
	  radio->ptr_curr = 10;
	  if (verbose & 1) plogw->ostream->println("CW Function key message edit mode");
	}
      }
      upd_display();
      return;
    }

    if (key == 0x07) {
      // Alt-D   delete entry in the bandmap on the cursor
      delete_entry_bandmap();
      upd_display_bandmap();
    }

    if (key == 0x11) {
      // Alt-n   add current frequency and callsign in the bandmap
      int len;
      len = strlen(radio->callsign + 2);
      if (len > 3) {
	register_current_callsign_bandmap();
      }
      upd_display_bandmap();
    }
    
    if (key == 0x08) {  // Alt-E: USB keying usage hint
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "USB keying via RIG CW:\n3=DTR 4=RTS");
      upd_display_info_flash(dp->lcdbuf);
    }

    if (key == 0x0a) {  // Alt-g enable disable tone_keying (radio->f_tone_keying)
      //      if (radio->modetype== LOG_MODETYPE_PH) {
      radio->f_tone_keying = 1 - radio->f_tone_keying;
      set_tone_keying(radio);  // initialize for tone_keying (temporally LED pin later use DAC to generate clean sinusoidal tone)
      set_tone(0,0);  // tone keying status is changed, stop keying anyway
      keying(0); // 
      clear_cwbuf(); // and also clear unsent cw message	
      if (!plogw->f_console_emu) {
	plogw->ostream->print("tone keying=");
	plogw->ostream->println(radio->f_tone_keying);
      }
      if (radio->f_tone_keying==1) {
	radio->modetype=LOG_MODETYPE_CW; // force set modetype to be CW
	
      } else {
	radio->modetype = modetype_string(radio->opmode);

      }
      set_log_rst(radio);      
      sprintf(dp->lcdbuf, "Tone keying=%d\n", radio->f_tone_keying);
      upd_display_info_flash(dp->lcdbuf);

    }

    if (key == 0x2c) {
      // Alt-Space pick spot on the bandmap cursor
      pick_entry_bandmap();
      // ＊＊＊＊bandmap_bandidがどのradio のものであろうと、運用中のradio のうち選択されたバンドの運用中のものがあれば、そちらのradio のs&p に選んだ局、周波数、局名をセットし、focused radioをそちらにスイッチすべきである。
      //そうしないと、複数のradio が同じバンドに設定されてしまうこととなる。現状は今のradio にセットしてしまっている。
      //      // dupe check here
      //      plogw->ostream->print("dupechk here");      
      //      if (dupe_check(radio->callsign + 2, bandmode(), plogw->mask, 1)) {  // cw/ssb both ok ... 0xff cw/ssb not ok 0xff-3
      //	radio->dupe = 1;
      //      } else {
      //	radio->dupe = 0;
      //      }
      upd_display();
      upd_display_bandmap();
    }

    if (key == 0x4a) {  // alt-HOME will switch selected radios
      switch_radios(0, -1);
    }
    if (key == 0x4c) {  // Alt-Del show rig info
      upd_disp_rig_info();
    }
    if (key == 0x4d) {  // ALT-END enable /disable main rig
      enable_radios(0, -1);
    }

    if (key == 0x09) {  // alt-f  set up/down sat frequency to the center of op band
      if (plogw->sat) {
	set_sat_center_frequency();
      }
    }

    if (key == 0x15) {  // alt-r  switch sat_vfo_mode
      if (plogw->sat) {
	switch (plogw->sat_vfo_mode) {
	case SAT_VFO_SINGLE_A_TX:  //SAT_VFO_SINGLE_A_TX
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_RX;
	  break;
	case SAT_VFO_SINGLE_A_RX:
	  plogw->sat_vfo_mode =
	    SAT_VFO_MULTI_TX_0;
	  break;
	case SAT_VFO_MULTI_TX_0:
	  plogw->sat_vfo_mode = SAT_VFO_MULTI_TX_1;
	  // may need to force transmit on radio #0 using so2r_tx function
	  break;
	case SAT_VFO_MULTI_TX_1:
	  plogw->sat_vfo_mode = SAT_VFO_SINGLE_A_TX;
	  break;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("Switched sat_vfo_mode ");
	  plogw->ostream->println(plogw->sat_vfo_mode);
	}
	upd_display_sat();
      }
    }
    if (key == 0x16) {  // alt-s satellite tracking mode switch
      if (plogw->sat) {
	switch (plogw->sat_freq_tracking_mode) {
	case SAT_RX_FIX:  // rx fix
	  plogw->sat_freq_tracking_mode = SAT_TX_FIX;
	  break;
	case SAT_TX_FIX:  // tx fix
	  plogw->sat_freq_tracking_mode = SAT_SAT_FIX;
	  break;
	case SAT_SAT_FIX:  // sat fix
	  plogw->sat_freq_tracking_mode = SAT_NO_TRACK;
	  break;
	case SAT_NO_TRACK:  // sat no_track
	  plogw->sat_freq_tracking_mode = SAT_RX_FIX;
	  break;
	}
      }
      upd_display_sat();
    }
    if (key == 0x1a) {
      // Alt-W normally wipes the whole QSO.  WIPEKEYSWAP exchanges the
      // Alt-W and Ctrl-W actions for operators who prefer the opposite pair.
      if (plogw->wipe_key_swap)
        clear_current_edit_field(radio);
      else
        wipe_log_entry();
      upd_display();
    }
    if (key == 0x37) {
      // Alt->,  Alt-<. manual band change up/down
      trace_band_key_event("KEY-BAND-UP", key, modkey, radio);
      switch_bands_from(radio, "KEY-BAND-UP");
    }
    if (key == 0x36) {
      // Alt->  Alt-< manual band change up/down
      int tmp, count;
      count = 0;
      tmp = (radio->f_freqchange_pending && radio->bandid_target > 0)
                ? radio->bandid_target
                : radio->bandid;
      while (count < N_BAND) {
	tmp--;
	if (tmp == 0) tmp = N_BAND - 1;
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("bandid:");
	  plogw->ostream->print(tmp);
	  plogw->ostream->print(" count:");
	  plogw->ostream->println(count);
	}
	if (((1 << (tmp - 1)) & radio->band_mask) == 0) {
	  // ok to change
	  trace_band_key_event("KEY-BAND-DOWN", key, modkey, radio);
	  band_change(tmp, radio);
	  break;
	}
	count++;
      }
      // band_change() already requests both right-display and bandmap
      // updates through the coalescing on-demand path.

    }
    // shift + left / right  cursor in bandmap change
    if (key == 0x52) {  // alt-left  --> remap to alt-up 0x52
      select_bandmap_entry(-1,radio->bandid_bandmap);
    }
    if (key == 0x51) {  //alt-right  ---> remap to alt-down 0x51
      select_bandmap_entry(+1,radio->bandid_bandmap);
    }
    if (key == 0x50) {  // Alt-DOWN  --> remap to alt-left 0x50
      radio->bandid_bandmap--;
      if (radio->bandid_bandmap < 1)
	radio->bandid_bandmap = N_BAND - 1;
      upd_display_bandmap();
    }
    if (key == 0x4f) {  // Alt-UP  --> remap to alt-right 0x4f
      radio->bandid_bandmap++;
      if (radio->bandid_bandmap >= N_BAND)
	radio->bandid_bandmap = 1;
      upd_display_bandmap();
    }

    if (key == 0x10) {
      trace_band_key_event("KEY-ALT-M", key, modkey, radio);
      //      plogw->ostream->print("before radio->modetype=");
      //      plogw->ostream->println(radio->modetype);      
      
      // alt-m to manually switch modes
      const char *mode;
      
      mode = switch_rigmode();
      
      int filt;
      int target_modetype = modetype_string(mode);
      if (target_modetype <= 0) target_modetype = radio->modetype;
      filt = radio->filtbank[radio->bandid]
                            [radio->cq[target_modetype]]
                            [target_modetype];
      if (filt == 0) {
        filt = default_filt(mode);
      }
      request_mode_change_radio(mode, filt, radio);

      //      plogw->ostream->print("after radio->modetype=");
      //      plogw->ostream->println(radio->modetype);      
      
      upd_display();
    }

    // abcdefghijklmnopqrstuvwxyz
    // 456789abcdef0123456789abcd
    // others
    if ((key >= 0x04) && (key <= 0x1d)) {
      if (verbose & 1) {
	plogw->ostream->print("<Alt-");
	plogw->ostream->print((char)(key - 0x04 + 'A'));
	plogw->ostream->print(">");
      }
    } else {
      if (verbose & 1) {
	print_key(modkey, key);
      }
    }
	
  } // end of alt+
  else if (modkey_ctrl(modkey)) {
    // Ctrl+Enter changes the persistent SO2R receive partner without running
    // ESM. Ctrl+Shift+Enter selects the previous enabled radio.
    if (!modkey_alt(modkey) && key == 0x28 && radio->ptr_curr == 0) {
      const int tx_radio = so2r.focused_radio();
      const int direction = modkey_shift(modkey) ? -1 : 1;
      if (so2r.select_so2r_pair_for_radio(tx_radio, direction)) {
        const int partner = so2r.so2r_pair_radio(tx_radio);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SO2R PAIR SET\nR%d <-> R%d\nSaved",
                 tx_radio, partner);
        // Update the normal right/left status screens as well.  The left-side
        // redraw is held until the confirmation flash expires, so the new pair
        // is shown immediately and the underlying status is also refreshed.
        request_display_update_on_demand();
        request_bandmap_update_on_demand();
        upd_display_info_flash(dp->lcdbuf);
        save_settings("");
      } else {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SO2R PAIR ERROR\nTX R%d\nNo partner",
                 tx_radio);
        upd_display_info_flash(dp->lcdbuf);
      }
      return;
    }

    // Ctrl+
    // 0           1               2
    // 456789abcdef0123456789abcdef01234567
    // ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890
    if ((key >= 0x3a) && (key <= 0x45)) {
      // Ctrl + function keys = repeat function
      plogw->ostream->println("repeat function");

      switch (so2r.radio_mode) {
      case SO2R::RADIO_MODE_SAT:
      case SO2R::RADIO_MODE_SO1R:
	// repeat 
	so2r.sequence_mode(SO2R::Repeat_Func);
	break;
      case SO2R::RADIO_MODE_SO2R:
	so2r.sequence_mode(SO2R::SO2R_Repeat_Func);
	break;
      }

      so2r.cancel_msg_tx();	      
      //      plogw->repeat_func_radio = plogw->focused_radio;
      //      so2r.msg_tx_radio(so2r.focused_radio());
      //      SO2R_settx(plogw->repeat_func_radio);
      //      so2r.set_tx(so2r.msg_tx_radio());

      

      //      plogw->repeat_func_key = key;
      //      so2r.msg_key(key);
      //      modkey.bmLeftCtrl = 1;                     // force set bmLeftCtrl in modkey
      //      plogw->repeat_func_key_modifier = modkey;  // repeat func設定した際のmodifier を憶えておく。

      //      function_keys(key, modkey, c);

      so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
      so2r.set_rx_in_sending_msg();
      so2r.repeat_cq_radio(so2r.msg_tx_radio()); // memorize      
      function_keys(key,c);
    }

    // key code check for special shortcut
    if ((key >= 0x04) && (key <= 0x34)) {

      if (verbose & 1) {
	plogw->ostream->print("<Ctrl-");
	plogw->ostream->print((char)(key - 0x04 + 'A'));
	plogw->ostream->print(">");
      }
      int pos;
      switch (key) {
      case 0x05: // ctrl-b exch partial check
	ui_perform_exch_partial_check(radio);
	break;
      case 0x08:  // ctrl-e direct move to recv_exch
	switch_logw_entry(5);
	break;
	
      case 0x06:  // Ctrl-C: Callsign / Ctrl-Shift-C: Contest
        if (modkey_shift(modkey)) {
          switch_logw_entry(8);
        } else {
          switch_logw_entry(3);
        }
	break;
	// enable/disable voice cq on/off ? if set, TU message voice record will be sent on Enter 220116
      case 0x16:  // ctrl-s direct move to his RST
	switch_logw_entry(4);
	break;
      case 0x04: // ctrl-a direct move to sent RST
	switch_logw_entry(6);
	break;
      case 0x0a: // ctrl-g find entity information from callsign
	//	console->println("ctrl-g");
	if (strlen(radio->callsign+2)>2) {
	  show_entity_info(radio->callsign+2);
	}
	break;
      case 0x09:  // ctrl-f find multi from remarks or numbers 
	switch (radio->ptr_curr) {
	case 6: // in remarks, reverse search jcc/jcg from city name 
	  reverse_search_multi();
	  break; 
	case 1: // in exchange input, search multi from the number (multi_check)
	  radio->multi = multi_check_option(radio->recv_exch+2,radio->bandid,1);
	  //	  serial.print("multi checked ");
	  //	  serial.print(radio->multi);	  
	  upd_display_info_contest_settings(radio);
	  break;
	}
	break;
      case 0x14: // ctrl-q switch QSL specification
	radio->f_qsl++;
	if (radio->f_qsl>=3) {
	  radio->f_qsl=0;
	}
	switch(radio->f_qsl) {
	case 0: // NOQSL
	  sprintf(dp->lcdbuf, "Card=NoQSL\n");
	  break;
	case 1: // JARL
	  sprintf(dp->lcdbuf, "Card=JARL\n");
	  break;
	case 2: // hQSL
	  sprintf(dp->lcdbuf, "Card=hQSL\n");
	  break;
	}
	upd_display_info_flash(dp->lcdbuf);
	break;
      case 0x0e:  // ctrl-k keyer mode switch

	if (radio->keyer_mode == 1) {
	  radio->keyer_mode = 0;
	} else {
	  radio->keyer_mode = 1;
	  // Keep already queued CW.  Ctrl-K changes only the input mode;
	  // subsequent manual characters must be appended to the existing
	  // transmit buffer rather than discarding pending text.
	}
	// need to update info line
	upd_display_stat();
	//          u8g2_r.sendBuffer();  // transfer internal memory to the display
	right_display_sendBuffer();

	if (verbose & 1) {
	  plogw->ostream->print("keyer_mode:");
	  plogw->ostream->println(radio->keyer_mode);
	}
	break;
      case 0x15:  // Ctrl-R: Remarks / Ctrl-Shift-R: RIG
        if (modkey_shift(modkey)) {
          switch_logw_entry(7);
        } else {
          switch_logw_entry(2);
        }
	break;
      case 0x1a:  // Ctrl-W: normally clear field; optionally swap with Alt-W
        if (plogw->wipe_key_swap)
          wipe_log_entry();
        else
          clear_current_edit_field(radio);
        upd_display();
        break;
      case 0x1d: // ctrl-z send Remarks to Z-server
	if (verbose & 1) {
	  plogw->ostream->printf("CTRL-Z ptr_curr=%d zserver_connected=%d state=%s remarks=<%s>\n",
	                         radio->ptr_curr,
	                         zserver_is_connected() ? 1 : 0,
	                         zserver_connection_state(),
	                         radio->remarks + 2);
	}
	if (radio->ptr_curr == 6) {
	  // Remarks is only about 80 bytes.  Do not reserve two 1 KiB
	  // NCHR_ZSERVER_CMD buffers on the ESP-IDF main task stack.
	  // UTF-8 -> CP932 never expands the payload, and the ASCII command
	  // prefix needs only a small fixed allowance.
	  static constexpr size_t ZMSG_SIZE = sizeof(radio->remarks) + 32;
	  char zmsg_utf8[ZMSG_SIZE];
	  char zmsg_cp932[ZMSG_SIZE];
	  snprintf(zmsg_utf8, sizeof(zmsg_utf8),
	           "#ZLOG# PUTMESSAGE %s", radio->remarks + 2);
	  size_t sjis_len = utf8_to_cp932(zmsg_utf8,
	                                  strlen(zmsg_utf8),
	                                  (uint8_t *)zmsg_cp932,
	                                  sizeof(zmsg_cp932));
	  if (verbose & 1) {
	    plogw->ostream->print("CTRL-Z calling zserver_send() UTF8=<");
	    plogw->ostream->print(zmsg_utf8);
	    plogw->ostream->printf("> CP932-bytes=%u\n", (unsigned)sjis_len);
	  }
	  zserver_send(zmsg_cp932);
	} else if (verbose & 1) {
	  plogw->ostream->println("CTRL-Z not in Remarks");
	}
	break;
      case 0x19:  // Ctrl-v to toggle voice memory enable
	plogw->voice_memory_enable++;
	if (plogw->voice_memory_enable>=4) {
	  plogw->voice_memory_enable=0;
	}
	//	if (plogw->voice_memory_enable) {
	//	  plogw->voice_memory_enable = 0;
	//	} else {
	//	  plogw->voice_memory_enable = 1;
	//	}
	sprintf(dp->lcdbuf, "VoiceMemory=%d\n%s\n", plogw->voice_memory_enable,
		(plogw->voice_memory_enable==0 ? "Disabled" :
		 (plogw->voice_memory_enable==1 ? "Enabled(NoTU)":
		  (plogw->voice_memory_enable==2 ? "Enabled(TU)":"Enabled(voice gen)")
		  )));

	upd_display_info_flash(dp->lcdbuf);
	plogw->ostream->print(dp->lcdbuf);	
	break;
	/// qso entry display related
	//      case 0x0d:  //ctrl-j to show current qso entry --> remap to ctrl-i
      case 0x0d:  // ctrl-j to toggle romaji conv
	switch (radio->f_romaji) {
	case 0: // non romaji
	  radio->f_romaji=1;
	  g_pre.s[0]='\0';g_pre.len=0;//clear preedit 
	  break;
	case 1: // in romaji -> notify flush_preedit()
	  radio->f_romaji=2; break;
	}
	upd_display();
	break;
      case 0x0c:  //ctrl-i to show current qso entry --> remap from ctrl-j
	upd_display_info_qso(0);
	break;
      case 0x13:;  // ctrl-p to show previous qso entry
	info_disp.pos -= sizeof(qso.all);
	if (info_disp.pos < 0) info_disp.pos = 0;
	upd_display_info_qso(0);
	break;
      case 0x11:;  // ctrl-n to show next qso entry
	info_disp.pos += sizeof(qso.all);
	pos = qsologf.position();

	if (info_disp.pos > pos - sizeof(qso.all)) info_disp.pos = pos - sizeof(qso.all);
	upd_display_info_qso(0);
	break;
      case 0x12:;  // ctrl -o to the last recorded qso entry and show
	pos = qsologf.position();
	info_disp.pos = pos - sizeof(qso.all);
	if (info_disp.pos < 0) {
	  info_disp.pos = 0;
	} else {
	  upd_display_info_qso(0);
	}
	break;
      case 0x2d:  // 		Ctrl-'-'  to load current (shown by Ctrl-j) into editing buffer
	if (!plogw->f_console_emu) {
	  plogw->ostream->println("qso SD-> edit");
	}
	upd_display_info_qso(1);
	upd_display();
	break;

      case 0x2f: // Ctrl-'[' toggle partial check flag
	ui_toggle_partial_check_flag();
	break;

      case 0x34: // ctrl-'\'' (apostroph) pick partial check
	ui_pick_partial_check(radio);
	break;
	      

	// contest related settings
	
      case 0x1e:  // ctrl-1  // switch dupe cw/phone
	if (plogw->mask == 0xff) {
	  plogw->mask = 0xff - 3;
	} else {
	  plogw->mask = 0xff;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("mask:");
	  plogw->ostream->println(plogw->mask);
	}
	sync_dupechk_mask_subcpu(plogw->mask);
	upd_display_info_contest_settings(radio);
	break;
      case 0x1f:  // Ctrl-2 / Ctrl-Shift-2: switch contest
        if (modkey_shift(modkey)) {
          // Alternate between the current and previous contest.
          alternate_contest();
        } else {
          // Advance to the next registered contest.
          plogw->contest_id++;
          if (plogw->contest_id >= N_CONTEST) plogw->contest_id = 0;
          set_contest_id();
        }
	break;
      case 0x20:  // ctrl-3  bandmap mask toggle for current operating band
	if (bandmap_mask & (1 << (radio->bandid - 1))) {
	  bandmap_mask &= ~(1 << (radio->bandid - 1));
	} else {
	  bandmap_mask |= (1 << (radio->bandid - 1));
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("bandmap_mask=");
	  plogw->ostream->println(bandmap_mask, BIN);
	}
	print_bandmap_mask();
	break;


      case 0x21: {  // ctrl-4 callhistory search toggle
	if (plogw->enable_callhist) {
	  plogw->enable_callhist = 0;
	} else {
	  plogw->enable_callhist = 1;
	}
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("callhist en =");
	  plogw->ostream->println(plogw->enable_callhist);
	}
	sprintf(dp->lcdbuf, "callhist en:%d\nStart.\n", plogw->enable_callhist);
	upd_display_info_flash(dp->lcdbuf);
	int n = 0;
	if (plogw->enable_callhist) {
	  if (!plogw->f_console_emu) plogw->ostream->println("open callhist");
	  if (callhist_at == 1) {
	    if (load_callhist_subcpu(callhistfn)) {
	      n = get_callhist_subcpu_count();
	    }
	  } else {
	    n = read_callhist_list(callhistfn);
	    print_callhist_list((const char **)callhist_list, n);
	  }
	} else {
	  if (!plogw->f_console_emu) plogw->ostream->println("close callhist");
	  //	  close_callhist();
	}
	sprintf(dp->lcdbuf, "callhist en:%d\nDone.\n", plogw->enable_callhist);
	upd_display_info_flash(dp->lcdbuf);
	break;
      }

      case 0x22:  // ctrl-5 radio_mode switch
	switch (so2r.radio_mode) {
	case 0:  // current so1r
	  {
	  so2r.radio_mode = SO2R::RADIO_MODE_SAT;
	  }
	  break;

	case 1:  // sat wro radio
	  {
	  so2r.radio_mode = SO2R::RADIO_MODE_SO2R;
	  }
	  break;
	case 2:  // so2r
	  {
	  so2r.radio_mode = SO2R::RADIO_MODE_SO1R;
	  }
	  break;
	}
	sprintf(dp->lcdbuf, "radio_mode =%d\n%s\n", so2r.radio_mode, (so2r.radio_mode == SO2R::RADIO_MODE_SO1R) ? "SO1R" : (so2r.radio_mode == SO2R::RADIO_MODE_SAT) ? "SAT 0 TX 1 RX "
		: "SO2R");
	upd_display_info_flash(dp->lcdbuf);
	break;

	// multi display
      case 0x17:  // ctrl-t: selected multiplier worked status on each band
        if (modkey_shift(modkey)) {
          // Ctrl-Shift-T: move the retained multiplier index backward.
          if (info_disp.multi_ofs > 0) info_disp.multi_ofs--;
          int saved_multi = radio->multi;
          radio->multi = -1;  // force display from retained index
          upd_display_info_multi_bands(radio);
          radio->multi = saved_multi;
        } else {
          radio->multi = multi_check_option(radio->recv_exch + 2, radio->bandid, 1);
          upd_display_info_multi_bands(radio);
        }
        break;

      case 0x1c:  // ctrl-y: nearby multipliers on the current band
        if (modkey_shift(modkey)) {
          // Ctrl-Shift-Y: move the retained multiplier index forward.
          int band_index = radio->bandid - 1;
          if (band_index >= 0 && band_index < N_BAND &&
              multi_list.multi[band_index] != NULL &&
              info_disp.multi_ofs + 1 < multi_list.n_multi[band_index]) {
            info_disp.multi_ofs++;
          }
          int saved_multi = radio->multi;
          radio->multi = -1;  // force display from retained index
          upd_display_info_multi_bands(radio);
          radio->multi = saved_multi;
        } else {
          radio->multi = multi_check_option(radio->recv_exch + 2, radio->bandid, 1);
          upd_display_info_multi_nearby(radio);
        }
        break;

      case 0x1b:  // ctrl-x: contest summary and nearby band status
        upd_display_info_contest_band_nearby(radio);
        break;
	// score summary
      case 0x18:  // ctrl-u    to work number of stations and worked stations/multis page show
	if (info_disp.show_info == INFO_DISP_SUMMARY &&  info_disp.timer > 2000) { 
	  plogw->bandmap_summary_idx += 6;
	  if (plogw->bandmap_summary_idx >= N_BAND) plogw->bandmap_summary_idx = 0;
	}
	upd_display_info_to_work_bandmap();	    
	break;
      }
    } // 0x04-0x34
    else {
      print_key(modkey, key);
    }
  } // ctrl *

  else {
    /////////////////////////////////////////////////////////////////////
    // no modifier keys
    // check with special keys
    
    switch (key) {
    case 0x28:  // Enter
      process_enter(0);
      break;
      //break;
    case 0x2c:  // SPACE
      switch (radio->keyer_mode) {
      case 0:
	if (check_edit_mode() == 1) {
	  if (verbose & 1) plogw->ostream->println("check_edit_mode==1");
	  logw_handler(key, c);  // in cw msg edit mode , send 'SPC' to editor (use TAB to switch entry)
	} else {
	  if (radio->ptr_curr == 1) { // exchange input space will make callsign window move
	    radio->ptr_curr=0;
	    request_display_update_on_demand();
	  } else {
	    if (verbose & 1) plogw->ostream->println("switch_logw_entry(0)");
	    switch_logw_entry(0);
	  }
	}
	break;
      case 1: // in keyer mode
	so2r.cancel_msg_tx();
	so2r.set_msg_tx_to_focused();
	send_single_char_radio(radio,c);
	break;
      }
      break;

    case 0x29:  // ESC cancel messaging
      if (verbose & 1) plogw->ostream->print("(ESC)");
      so2r.cancel_msg_tx();
      switch(so2r.radio_mode) {
      case SO2R::RADIO_MODE_SO2R:
        // ESC bypasses sending_msg_finished(), so explicitly restore the
        // operator focus saved when this message transmission started.
        so2r.request_restore_focus_after_message(so2r.msg_tx_radio());
	so2r.sequence_mode(SO2R::SO2R_CQSandP);
	break;
      case SO2R::RADIO_MODE_SO1R:
      case SO2R::RADIO_MODE_SAT:	
	so2r.sequence_mode(SO2R::Manual);
	break;
      }
      so2r.sequence_stat(SO2R::Default);

      /* looks like the following is done in cancel_msg_tx() -> cancel_current_tx_message() -> cancel_keying()
      if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying)) {
	// clear cw buffer (stop CW keying)
	clear_cwbuf();
      } else if (radio->modetype== LOG_MODETYPE_PH) {
	if (plogw->voice_memory_enable) {
	  struct radio *radio1;
	  radio1=so2r.radio_msg_tx();
	  send_voice_memory(radio1, 0); // need stop msg tx radio
	}
      } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
	//            send_rtty_memory(radio, 0); // this will not stop sending ! (22/7/23)
	if (radio->f_tone_keying) {
	  //              ledcWrite(LEDC_CHANNEL_0, 0);  // stop sending tone
	} else {
	  //digitalWrite(LED, 0);
	  keying(0);
	}
	set_ptt_rig(radio, 0);
      }
      */
      break;

    case 0x2a:  // BS
      if (verbose & 1) plogw->ostream->print("(BS)");
      if (radio->keyer_mode) {
	delete_cwbuf();
      } else {
	logw_handler(key, c);
      }
      break;
    case 0x2b:  // TAB
      // exchange current entry
      switch_logw_entry(0);
      break;
    case 0x35:  // ` to switch stereo/mono on SO2R
      switch (so2r.radio_mode) {
      case 0:
      case 1:
      case 2:
	so2r.setstereo(1 - so2r.stereo());
      }
      break;

    case 0x31:  // \ backslash to switch focused rig and RX to hear(on SO2R)

      switch (so2r.focused_radio()) {
      case 0:
	change_focused_radio_manual(1);
	break;
      case 1:
	change_focused_radio_manual(0);
	break;
      case 2:
	change_focused_radio_manual(0);
	break;
      }
      //

      break;


    case 0x4b:  // PGUP
      // cw keying speed change
      cw_spd++;
      clamp_cw_speed();
      show_cw_spd();
      break;

    case 0x4e:  // PGDN
      cw_spd--;
      clamp_cw_speed();
      show_cw_spd();
      break;
      // editing related keys

    case 0x4c:  // DEL
    case 0x4f:  // RIGHT
    case 0x50:  // LEFT
      logw_handler(key, c);
      break;

      // frequency control keys
    case 0x51:  // DOWN
    case 0x52: { // UP
      struct radio *xr = so2r.radio_selected();
      const int delta = (key == 0x52) ? 20 : -20;
      if (xr != NULL && xr->rit_xit_adjust_target == 1 &&
          xr->rit_enabled && rit_control_supported(xr)) {
        adjust_rit_xit_offset(xr, delta);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "RIT %+d Hz", xr->rit_offset_hz);
        upd_display_info_flash(dp->lcdbuf);
        info_disp.timer = 1000;
      } else if (xr != NULL && xr->rit_xit_adjust_target == 2 &&
                 xr->xit_enabled && xit_control_supported(xr)) {
        adjust_rit_xit_offset(xr, delta);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "XIT %+d Hz", xr->xit_offset_hz);
        upd_display_info_flash(dp->lcdbuf);
        info_disp.timer = 1000;
      } else {
        adjust_frequency((key == 0x52 ? +100 : -100) / FREQ_UNIT);
        upd_display();
      }
      break;
    }

    case 0x46:  // PRTSC
    case 0x49:  // INS
    case 0x4a:  // HOME
    case 0x4d:  // END

      if (verbose & 1) {
	plogw->ostream->print("Key=<");
	PrintHex<uint8_t>(key, 0x80);
	plogw->ostream->print(">");
      }
      // frequency adjustment


      // Ctrl +Down , Up  get next spot higher in frequency
      // Ctrl Alt Down , Up get next spot higher in multiplyer (not implemented)
      // Alt+Q CQ/S&P switch
      // Ctrl-W wipe
      // Space/Tab jump
    default:
      // no modifier
      // F9-F12 are local operating controls. F1-F7 remain message memories.
      if (key >= 0x42 && key <= 0x45) {
        struct radio *xr = so2r.radio_selected();

        if (key == 0x42) { // F9: RIT / RX CLAR toggle
          if (xr == NULL || !rit_control_supported(xr)) {
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "F9 RIT\nUNSUPPORTED");
          } else {
            set_rit_control(xr, !xr->rit_enabled);
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                     "RIT %s\nXIT %s",
                     xr->rit_enabled ? "ON" : "OFF",
                     xr->xit_enabled ? "ARMED" : "OFF");
          }
        } else if (key == 0x43) { // F10: XIT / TX CLAR toggle
          // CI-V RIT/XIT uses the same command family.  In particular,
          // IC-9700 already passes rit_control_supported(), so do not reject
          // F10 merely because an older per-rig XIT capability test says no.
          const bool icom_rit_xit =
              xr != NULL && xr->rig_spec != NULL &&
              xr->rig_spec->cat_type == CAT_TYPE_CIV &&
              rit_control_supported(xr);
          if (xr == NULL ||
              (!icom_rit_xit && !xit_control_supported(xr))) {
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "F10 XIT\nUNSUPPORTED");
          } else {
            xr->xit_enabled = !xr->xit_enabled;
            if (xr->xit_enabled) {
              select_xit_adjust_target(xr);
              apply_xit_for_operating_mode(xr);
              snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                       "XIT ON\n%+d Hz\n%s",
                       xr->xit_offset_hz,
                       xr->cq[xr->modetype] == LOG_SandP
                           ? "S&P ACTIVE" : "CQ: 0 Hz");
            } else {
              set_xit_control(xr, false);
              set_xit_offset_hz(xr, 0);
              if (xr->rit_xit_adjust_target == 2)
                xr->rit_xit_adjust_target = 0;
              snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                       "XIT OFF\nsaved %+d Hz", xr->xit_offset_hz);
            }
          }
        } else if (key == 0x44) { // F11: receiver filter / WIDTH cycle
          const int val = xr ? cycle_rx_filter(xr) : -1;
          if (val < 0)
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                     "F11 FILTER\nUNSUPPORTED");
          else if (xr->rig_spec->cat_type == CAT_TYPE_CIV)
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                     "RX FILTER\nFIL%d", val);
          else
            snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                     "RX WIDTH\n%d Hz", val);
        } else { // 0x45 F12: receiver AGC cycle
          const int agc = xr ? cycle_rx_agc(xr) : -1;
          const char *name =
              agc == 1 ? "FAST" :
              agc == 2 ? "MID" :
              agc == 3 ? "SLOW" :
              agc == 4 ? "AUTO" : "UNSUPPORTED";
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                   "AGC\n%s", name);
        }

        upd_display_info_flash(dp->lcdbuf);
        info_disp.timer = 1000;
        break;
      }

      // function keys
      if ((key >= 0x3a) && (key <= 0x45)) {
	so2r.cancel_msg_tx();	
	so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
	so2r.set_rx_in_sending_msg();	
	function_keys(key, c);
      } else {
	// key input in the repeat function radio window will stop repeating until concluding interrupted qso 
	if (so2r.sequence_mode() == SO2R::Repeat_Func || so2r.sequence_mode() == SO2R::SO2R_Repeat_Func) {
	  if (so2r.msg_tx_radio() == so2r.focused_radio()) {
	    if (radio->ptr_curr == 0 || radio->ptr_curr == 1) {
	      so2r.suspend();
	    }
	  }
	} else if (so2r.sequence_mode() == SO2R::SO2R_ALTCQ || so2r.sequence_mode() == SO2R::SO2R_2BSIQ) {
	  so2r.suspend();
	  
	}
	// other alphabetical / numerical keys
	//  OnKeyPressed(c);
	//	plogw->ostream->println("ptr_curr:");
	//	plogw->ostream->print(radio->ptr_curr);	
	//	plogw->ostream->println("*");
	
	switch (radio->keyer_mode) {
	case 0:
	  if ((radio->ptr_curr == 0)||(radio->ptr_curr == 1) ) { // 25/3/23 same response in call/exchange ; 
	    // additional check for ; (send F5 and move to my exch window)
	    if (c == ';') {
	      ui_response_call_and_move_to_exch(radio);
	      break;
	    } else if (c== '\'') {  // apostroph
	      ui_cursor_next_entry_partial_check (radio);
	      break;
	    }
	  }
	  logw_handler(key, c);
	  break;
	case 1:
	  // Ctrl-K manual keyer mode: append characters to the existing CW
	  // transmit buffer.  Do NOT call cancel_msg_tx() here: that routine
	  // eventually calls cancel_keying() -> clear_cwbuf(), so invoking it
	  // for every typed character discards previously queued manual CW.
	  if (c == ';') {
	    ui_response_call_and_move_to_exch(radio);
	    break;
	  }
	  send_single_char_radio(radio,c);
	  break;
	} // keyer mode
      }	    // other alphabetical / numerical keys
    } // switch key
  } // no modifier
}

//void function_keys(uint8_t key, MODIFIERKEYS modkey, uint8_t c) {
void function_keys(uint8_t key, uint8_t c) {
  uint8_t tmpbuf[2];

  struct radio *radio;
  so2r.cancel_msg_tx();
  radio=so2r.radio_msg_tx(); // transmit always in the radio of msg_tx
  so2r.set_tx_to_msg_tx();

  // function key
  if (verbose & 1) {
    plogw->ostream->print("<F");
    plogw->ostream->print(key - 0x3a + 1);
    plogw->ostream->print(">");
  }
  if ((key >= 0x3a) && (key <= 0x3a + N_CWMSG - 1)) {
    if (key == 0x3a) {
      so2r.msg_key(key); // memorize only in cq (for repeating functions)
      // F1 CQ  go to CQ mode in the current frequency
      if (radio->cq[radio->modetype] == LOG_SandP) {
        radio->cq[radio->modetype] = LOG_CQ;
      }
      apply_xit_for_operating_mode(radio);
      // memorize current frequency to freqbank cq freqnency
      save_freq_mode_filt(radio);
      // abcdef012345
      //F123456
    }
    if (key == 0x3d) {  // F4 callsign
      if (!radio->cq[radio->modetype]) {
        // S & P  start getting his S meter reading
        radio->smeter_stat = 1;
        radio->smeter_peak = SMETER_MINIMUM_DBM;

        //
        int len;
        len = strlen(radio->callsign + 2);
        if (len == 0) {  // no existing callsign entry
          pick_onfreq_station();
        } else {
          if (len > 3) {
            register_current_callsign_bandmap();
          }
        }
      }
    }
    if (key == 0x3b) {  // F2 send number
      if (!radio->cq[radio->modetype]) {
        if (radio->smeter_stat == 1)
          radio->smeter_stat = 2;  // finished getting s-meter statistics
      }
    }

    // check ctrl-key repeat function するか？
    //    plogw->repeat_func_timer = 0;
    so2r.cancel_repeat_timer();

    so2r.sequence_stat(SO2R::Sending_Msg);
    // send corresponding memory
    if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying)) {
      append_cwbuf_string(plogw->cw_msg[key - 0x3a] + 2);  // send CW msg
      append_cwbuf('$');  // repeat command
    } else if (radio->modetype== LOG_MODETYPE_PH) {
	if (plogw->voice_memory_enable) {
	  if (plogw->voice_memory_enable == 3) {
	    // SubCPU voice generation using metan.wav.
	    // Match the behavior of the ESM path.
	    so2r.play_string_macro(
				   plogw->cw_msg[key - 0x3a] + 2);
	  } else {
	    // Rig built-in voice memory.
	    send_voice_memory(
			      radio, key - 0x3a + 1);
	  }
	}
    } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
      set_rttymemory_string(radio, key - 0x3a + 1, plogw->rtty_msg[key - 0x3a] + 2);
      // set_rttymemory_string() queues the local Baudot/FSK generator for
      // every rig.  rig_spec->cwport selects GPIO0/1/2, DTR, or RTS.
    }
  }
}


void print_off_contest()
{
  sprintf(dp->lcdbuf, "OFF Contest = %d\n",plogw->f_off_contest);
  if (plogw->f_off_contest) {
    strcat(dp->lcdbuf,"No Dupe/Multi/Score\n");
  } else {
    strcat(dp->lcdbuf,"Contest:");
    strcat(dp->lcdbuf,plogw->contest_name+2);
    strcat(dp->lcdbuf,"\n");    
  }
  upd_display_info_flash(dp->lcdbuf);

}

void set_contest_from_name()
{
    if (is_user_md_contest_name(plogw->contest_name + 2)) {
      // Route User MD contests through the same contest-selection path as
      // built-in contests.  set_contest_id() records the current contest as
      // previous before switching, so ordinary selection establishes the
      // current/previous pair used by contest alternate and dual-contest QSO.
      plogw->contest_id = USER_MD_CONTEST_ID;
      set_contest_id();
      return;
    }
    search_contest_id_from_name() ;
    switch(plogw->multi_type) {
    case MULTI_TYPE_NORMAL:      sprintf(buf,"NORMAL"); break;
    case MULTI_TYPE_CQWW:        sprintf(buf,"NORMAL/CQWW"); break;      
    case MULTI_TYPE_JARL_PWR:    sprintf(buf,"JARL PWR"); break;      
    case MULTI_TYPE_UEC:    sprintf(buf,"UEC"); break;      
    case MULTI_TYPE_JARL_PWR_NOMULTICHK:    sprintf(buf,"JARL PWR \nNOMULTICHK"); break;      
    case MULTI_TYPE_NOCHK_LASTCHR:    sprintf(buf,"IGN LASTCHR"); break;      
    case MULTI_TYPE_GL_NUMBERS:    sprintf(buf,"GL/NUMBER"); break;      
    case MULTI_TYPE_ARRLDX:    sprintf(buf,"ARRLDX"); break; 
    case MULTI_TYPE_ARRL10M:    sprintf(buf,"ARRL10M"); break;
    case MULTI_TYPE_AA:    sprintf(buf,"AllAsia"); break;         
    default:sprintf(buf,"N/A"); break;
    }
    
    sprintf(dp->lcdbuf, "contest \n%s\nselected.\nD:%s\nMult:%s",plogw->contest_name+2,plogw->mask == 0xff ? "OK C/P" : "NG C/P",buf);
    
    upd_display_info_flash(dp->lcdbuf);
  
}

// true if current contest checks multiplier validity
int check_multi_contest()
{
  //  console->print(plogw->contest_name+2);  
  if (strncasecmp(plogw->contest_name+2,"NOMULTI",7)==0) return 0;
  //if (strncasecmp(plogw->contest_name+2,"AAtest",6)==0) return 0;
  return 1;
}

// Split the editable receive exchange into primary/secondary parts.
// The first ',' or '/' is the dual-contest separator.  The edit buffer itself
// is never modified here.
static bool split_dual_recv_exch(const char *raw,
                                 char *primary, size_t primary_size,
                                 char *secondary, size_t secondary_size,
                                 char *separator_char)
{
  if (primary && primary_size) primary[0] = '\0';
  if (secondary && secondary_size) secondary[0] = '\0';
  if (separator_char) *separator_char = '\0';
  if (!raw) return false;

  const char *sep = strpbrk(raw, ",/");
  if (!sep) {
    if (primary && primary_size) strlcpy(primary, raw, primary_size);
    return false;
  }

  if (separator_char) *separator_char = *sep;
  if (primary && primary_size) {
    size_t n = (size_t)(sep - raw);
    if (n >= primary_size) n = primary_size - 1;
    memcpy(primary, raw, n);
    primary[n] = '\0';
  }
  if (secondary && secondary_size) strlcpy(secondary, sep + 1, secondary_size);
  return primary && secondary && primary[0] != '\0' && secondary[0] != '\0';
}

// 入力欄でEnter を押したときの処理
static bool normalize_mdns_hostname(char *name)
{
  if (!name || !*name) return false;
  const size_t len = strlen(name);
  if (len == 0 || len > LEN_HOST_NAME) return false;
  if (name[0] == '-' || name[len - 1] == '-') return false;
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (c >= 'A' && c <= 'Z') {
      name[i] = (char)(c - 'A' + 'a');
      c = (unsigned char)name[i];
    }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return true;
}

static void report_hostname_setting(const char *prefix)
{
  if (!prefix) prefix = "hostname";
  plogw->ostream->printf("%s: %s (mDNS: %s.local)\n",
                         prefix, plogw->hostname + 2, plogw->hostname + 2);
}

void process_enter(int option) {
  // option 0: normal
  //        1: with SHIFT 
  struct radio *radio;
  radio = so2r.radio_selected();
  so2r.set_default_sequence_mode(); // set appropriate operating mode
  so2r.qso_process_radio(so2r.focused_radio());// set qso_process_radio to the focused

  if (verbose &4) {
    console->print("process_enter() process ");
    console->print(so2r.qso_process_radio());
    console->print("focus  ");
    console->print(so2r.focused_radio());
    console->print("tx  ");  
    console->print(so2r.tx());    
    console->print("rx  ");  
    console->println(so2r.rx());
  }
  
  char buf[60];
  char *p;
  if (verbose & 1) plogw->ostream->println("Enter pressed");
  //        switch (plogw->keyer_mode) {
  //          case 0: // non keyer mode
  // 21/11/7 Enter will also be processed similarly in keyer mode
  switch (radio->ptr_curr) {
  case 1: { // number entry
    // A comma or slash means one physical QSO should be recorded for both the
    // active and previous contests.  Parse a copy: do not destroy the edit
    // buffer before both sides have been captured.
    char raw_exch[LEN_DUAL_EXCH_WINDOW + 1] = {0};
    char primary_exch[LEN_EXCH + 1] = {0};
    char secondary_exch[LEN_EXCH + 1] = {0};
    char secondary_contest[sizeof(plogw->contest_name) - 2] = {0};
    char separator_char = 0;
    strlcpy(raw_exch, radio->recv_exch + 2, sizeof(raw_exch));
    const bool split_ok = split_dual_recv_exch(raw_exch,
                                                primary_exch, sizeof(primary_exch),
                                                secondary_exch, sizeof(secondary_exch),
                                                &separator_char);
    bool dual_contest_qso = false;
    int secondary_id = -1;
    bool have_previous_contest = false;

    if (separator_char != 0) {
      have_previous_contest = previous_contest_info(&secondary_id, secondary_contest,
                                                     sizeof(secondary_contest));
      dual_contest_qso = split_ok && have_previous_contest;
      plogw->ostream->printf(
          "DUAL EXCH: raw=<%s> sep=%c primary=<%s> secondary=<%s> "
          "split=%d previous=%d current=<%s> secondary_contest=<%s>\n",
          raw_exch, separator_char, primary_exch, secondary_exch,
          split_ok ? 1 : 0, have_previous_contest ? 1 : 0,
          plogw->contest_name + 2,
          have_previous_contest ? secondary_contest : "");

      if (!split_ok) {
        upd_display_info_flash("2ND EXCH NG\nNeed both sides");
        info_disp.timer = 2000;
      } else if (!have_previous_contest) {
        upd_display_info_flash("2ND CONTEST NG\nNo previous contest");
        info_disp.timer = 2000;
      }
    }

    // Primary QSO always uses only the left side.  No separator means the
    // complete exchange, exactly as before.
    if (separator_char != 0) {
      strlcpy(radio->recv_exch + 2, primary_exch, LEN_DUAL_EXCH_WINDOW + 1);
      radio->recv_exch[1] = strlen(radio->recv_exch + 2);
    }
    // Always interpret the exchange using the currently selected contest,
    // including OFFCONTEST operation.  OFFCONTEST changes scoring/recording
    // policy, not the operator assistance used to interpret the exchange.
    radio->multi = multi_check(radio->recv_exch+2,radio->bandid);
    //   plogw->ostream->println("contest multi checking0");
    if (check_multi_contest() && radio->multi == -1) {
      // In a normal contest an invalid multiplier prevents logging, as before.
      // In OFFCONTEST it is only a warning: keep the exchange visible and
      // allow the QSO to be recorded with C:-, without score/dupe/multi entry.
      if (!plogw->f_console_emu) plogw->ostream->println("not valid multiplier");
      upd_display();
      upd_display_info_contest_settings(radio);
      if (!plogw->f_off_contest) {
	break;
      }
    }

    // ESM is an operating aid, not contest scoring state.  Keep the normal
    // exchange/TU sequence active in OFFCONTEST as well.
    if (plogw->f_esm==1) {
      if (radio->cq[radio->modetype] == LOG_SandP && option!=1) {
	// ESM && S&P --> send exchange
	if (!so2r.send_exch(radio)) return; // this need to be done for the previously focused radio
	// 2bsiq delay so set flag and return
      }
    }
    
    // determine qsoid
    plogw->qsoid=  plogw->txnum* 100000000 + plogw->seqnr *10000 + random(100)*100;
    
    // log current QSO
    print_qso_log();

    if (dual_contest_qso) {
      const int m2 = previous_contest_multi_check(secondary_exch, radio->bandid);
      const bool m2_ok = (m2 >= 0);
      plogw->ostream->printf(
          "DUAL SECONDARY: contest=<%s> id=%d exch=<%s> multi=%d\n",
          secondary_contest, secondary_id, secondary_exch, m2);
      if (!append_secondary_contest_qso(secondary_exch, secondary_contest, m2_ok)) {
        upd_display_info_flash("2ND QSO WRITE NG");
        info_disp.timer = 2000;
      } else if (!m2_ok) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "M2:NG %s\n%s",
                 secondary_contest, secondary_exch);
        upd_display_info_flash(dp->lcdbuf);
        info_disp.timer = 2000;
      }
    }
    if (verbose&4) 	{
      if (!plogw->f_console_emu) plogw->ostream->println("print_qso_log() finished");
    }
    
    if (!plogw->f_off_contest) {              
      // reset qso_interval_timer
      plogw->qso_interval_timer =0;


      if (!radio->dupe) {
	// update statistics in score
	score.worked[radio->modetype == LOG_MODETYPE_CW ? 0 : 1][radio->bandid - 1]++;
	entry_dupechk(so2r.radio_qso_process());
      }

      plogw->seqnr_band[radio->bandid-1]++;

      if (verbose&4) 	{
	if (!plogw->f_console_emu) plogw->ostream->println("dupechk entered");
      }
      
      entry_multiplier(so2r.radio_qso_process());
      if (verbose&4) 	{
	if (!plogw->f_console_emu) plogw->ostream->println("multi entered ");
      }

      // Mark every matching spot that is a DUPE under the current
      // contest band/mode mask.  A station may have several spots at
      // different frequencies, so updating only search_bandmap()'s first
      // match leaves stale unworked entries visible.
      mark_bandmap_call_worked(radio->callsign + 2, radio->bandid,
                               bandmode(radio));

      // bandmap update
      if (radio->cq[radio->modetype] == LOG_SandP) {
	if (strlen(radio->callsign + 2) > 3) {
	  register_current_callsign_bandmap();
	}
      }

      if (verbose&4) 	{
	if (!plogw->f_console_emu) plogw->ostream->println("bandmap updated  ");
      }

      // A full upd_display() follows immediately after the entry is wiped.
      // Avoid drawing the contest information once here and again there.
    } else {
      sprintf(dp->lcdbuf, "OFF Contest=%d !!!\nNo Dupe/Multi/Score\nperformed.",plogw->f_off_contest);
      upd_display_info_flash(dp->lcdbuf);
    }

    // Message sequencing is independent of contest scoring.  OFFCONTEST
    // QSOs still complete the normal ESM/TU operating sequence, while the
    // contest dupe/multiplier/score and bandmap-worked state above remain
    // untouched.
    if (option == 0 && ((plogw->f_esm==0) ||
                       (plogw->f_esm==1 &&
                        (radio->cq[radio->modetype] == LOG_CQ)))) {
      so2r.send_tu();
      if (verbose&4) console->println("end of tu");
    }
    so2r.print_sequence_mode();
      
    
    // prepare new log entry
    //    new_log_entry();
    if (verbose&4)     {
      console->print("after tu qso_process_radio="); console->println(so2r.qso_process_radio());
    }
    wipe_log_entry_radio(&radio_list[so2r.qso_process_radio()]); // up to here need to  use tx radio
    if (verbose &4) {
      console->print("ptr_curr=");console->println(so2r.radio_selected()->ptr_curr);
    }
    plogw->seqnr++;

    // check repeat func and start timer 
    if (so2r.sequence_mode() == SO2R::Repeat_Func || so2r.sequence_mode() == SO2R::SO2R_Repeat_Func) {
      if (so2r.msg_tx_radio() == so2r.focused_radio()) {
	so2r.request_set_rx(so2r.msg_tx_radio());
	if (so2r.sequence_stat()!=SO2R::Sending_Msg) {
	  // no tu or exchange being sent --> need to start timer2 to resume repeat cq 
	  so2r.repeat_timer2_start();
	  if (verbose&4) {
	    console->println("so2r start timer2 on enter");
	  }
	}
      }
    }
    
    // update display by the current focus
    so2r.qso_process_radio(so2r.focused_radio()); // set qso_process_radio to the focused 
    request_display_update_on_demand();
    request_bandmap_update_on_demand();
    break;
  }
  case 8:  // grid locator
    set_grid_locator_information();
    break;
  case 7:  // satellite name
    if (strlen(plogw->sat_name + 2) != 0) {

      // force stop rotator tracking
      plogw->f_rotator_track = 0;

      // satellite name entered
      sat_name_entered();

    } else {
      // go to contest qso
      if (!plogw->f_console_emu) plogw->ostream->println("non satellite");
      plogw->sat = 0;
      // force stop rotator tracking
      plogw->f_rotator_track = 0;
    }
    upd_display();
    break;
  case 20:  // rig entry
    set_rig_from_name(radio);
    //
    
    upd_display();
    break;
  case 21:  // cluster name
    // split port and body and copy to cluster
    set_cluster();
    //    connect_cluster();
    cluster.stat=0;// reset connection status
    upd_display();
    break;
  case 23:  // cluster cmd
    send_cluster_cmd();
    upd_display();
    break;
  case 41:  // cluster 2 name
    set_cluster2();
    upd_display();
    break;
  case 42:  // cluster 2 command
    send_cluster2_cmd();
    upd_display();
    break;
  case 25:  // wifi_ssid
    multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);
    break;
  case 26:  // wifi_passwd
    multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);    
    break;
  case 27: { // rig_spec_str -> set spec to the rig
    sprintf(dp->lcdbuf, "modifying rig_spec... ");
    upd_display_info_flash(dp->lcdbuf);
    char rig_update_err[96];
    if (!update_rig_spec(radio->rig_spec_idx, radio->rig_spec_str + 2,
                         rig_update_err, sizeof(rig_update_err))) {
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%s", rig_update_err);
      upd_display_info_flash(dp->lcdbuf);
      // Restore the edit buffer from the unchanged rig_spec.
      set_rig_spec_str_from_spec(radio);
      break;
    }
    if (verbose&4) {
    console->print("Now edit rig name is ");
    console->print(radio->rig_name+2);
    console->print(" and rig_spec rig name is ");
    console->print(radio->rig_spec->name);
    console->print(" and rig_spec cwport is ");
    console->print(radio->rig_spec->cwport);
    console->print(" and rig_spec_str is ");
    console->println(radio->rig_spec_str+2);
    }
    // update_rig_spec() has already re-selected every radio using this rig.
    if (verbose&4) {
    console->print("After select_rig(), Now edit rig name is ");
    console->print(radio->rig_name+2);
    console->print(" and rig_spec rig name is ");
    console->print(radio->rig_spec->name);
    console->print(" and rig_spec cwport is ");
    console->print(radio->rig_spec->cwport);
    console->print(" and rig_spec_str is ");
    console->println(radio->rig_spec_str+2);
    }
    sprintf(dp->lcdbuf, "modifying rig_spec\nfinished.");
    upd_display_info_flash(dp->lcdbuf);
    break;
  }
  case 28:  // zserver_name -> connect to zserver
    reconnect_zserver();
    break;
  case 43: { // hostname / mDNS name
    char tmp[LEN_HOST_NAME + 1];
    strlcpy(tmp, plogw->hostname + 2, sizeof(tmp));
    if (!normalize_mdns_hostname(tmp)) {
      upd_display_info_flash("Hostname invalid\nUse a-z 0-9 -");
      info_disp.timer = 2500;
      break;
    }
    strlcpy(plogw->hostname + 2, tmp, LEN_HOST_NAME + 1);
    plogw->hostname[1] = strlen(plogw->hostname + 2);
    save_settings("");
    report_hostname_setting("hostname saved");
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "Hostname saved\n%s.local\nRestart required", plogw->hostname + 2);
    upd_display_info_flash(dp->lcdbuf);
    info_disp.timer = 3000;
    break;
  }
  case 40: // contest_name
    set_contest_from_name();
    break;
    
  case 0:  // callsign entry
    // コールサイン入力前でS&Pの際onfreq の局があったらコールサインを取り込み。
    int len;
    int tmp;
    len = strlen(radio->callsign + 2);
    if (len == 0) {
      if (radio->cq[radio->modetype] == LOG_SandP) {
        // First Enter on an empty CALLSIGN may pick the on-frequency station,
        // but must NOT also transmit.  The next Enter runs normal ESM.
        pick_onfreq_station();
        len = strlen(radio->callsign + 2);
        if (len != 0) {
          request_display_update_on_demand();
          break;
        }
      }

      if (len == 0) {
        if (plogw->f_esm == 1) {
          if (radio->cq[radio->modetype] == LOG_SandP) {
            // N1MM+-style ESM behavior: an empty Enter in S&P sends F4
            // (my callsign) while keeping the radio in S&P mode.
            ui_send_mycall(radio);
          } else {
            // In CQ mode, an empty Enter starts/continues CQ as before.
            so2r.cancel_msg_tx();
            so2r.set_msg_tx_to_focused();
            so2r.send_cq();
          }
        }
        break;
      }
    }
    // check for commands
    if (strncmp(radio->callsign + 2, "OLDEST", 6) == 0) {
      const char *arg = radio->callsign + 8;
      char *endp = NULL;
      long minutes = strtol(arg, &endp, 10);
      if (arg != endp && *endp == '\0' && minutes >= 1 && minutes <= 1440) {
        bandmap_lifetime_minutes = (int)minutes;
        save_settings("");
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "Bandmap oldest\n%d min", bandmap_lifetime_minutes);
      } else {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "OLDEST invalid\n1..1440 min");
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "ZSERVERON") == 0 ||
        strcmp(radio->callsign + 2, "ZSERVEROFF") == 0) {
      const int enabled = strcmp(radio->callsign + 2, "ZSERVERON") == 0;
      set_zserver_auto(enabled);
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "ZSERVER\n%s", enabled ? "AUTO" : "OFF");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "CLUSTER1ON") == 0 ||
        strcmp(radio->callsign + 2, "CLUSTER1OFF") == 0) {
      const int enabled = strcmp(radio->callsign + 2, "CLUSTER1ON") == 0;
      set_cluster_auto(1, enabled);
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "CLUSTER 1\n%s", enabled ? "AUTO" : "OFF");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "CLUSTER2ON") == 0 ||
        strcmp(radio->callsign + 2, "CLUSTER2OFF") == 0) {
      const int enabled = strcmp(radio->callsign + 2, "CLUSTER2ON") == 0;
      set_cluster_auto(2, enabled);
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "CLUSTER 2\n%s", enabled ? "AUTO" : "OFF");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DISCONN") == 0) {
      disconnect_cluster();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DISCON2") == 0) {
      disconnect_cluster2();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"PRINTKEY")==0) {
      f_printkey=1-f_printkey;
      sprintf(dp->lcdbuf, "printkey=%d", f_printkey);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "SATELLITE") == 0) {
      // first read tle information from sd file 
      readtlefile();
      // check time
      long int dt;
      //      dt=rtctime.unixtime()-plogw->tle_unixtime;
      dt=my_rtc.unixtime()-plogw->tle_unixtime;      
      sprintf(dp->lcdbuf, "TLE %ld hrs old\n",dt/3600);
      if (dt>84600*3) {
	// need to update from the internet
	strcat(dp->lcdbuf,"Update...\n");
	upd_display_info_flash(dp->lcdbuf);	
	getTLE();
      } else {
	strcat(dp->lcdbuf,"Keep.\n");
	upd_display_info_flash(dp->lcdbuf);
      }
      clear_buf(radio->callsign);
      break;
    }

    if (strncmp(radio->callsign + 2, "NEXTAOS", 7) == 0) {
      // find next aos
      start_calc_nextaos();
      clear_buf(radio->callsign);
      break;
    }
    if (strncmp(radio->callsign + 2, "POWER", 5) == 0) {
      const char *p = radio->callsign + 2 + 5;
      int watts = -1;

      if (*p == '\0' || strcmp(p, "?") == 0) {
        // Query the rig now; the LCD shows the latest cached value
        // immediately and the normal CAT poll updates it again shortly.
        send_power_query_civ(radio);
        if (radio->rig_spec &&
            radio->rig_spec->rig_type == RIG_TYPE_YAESU_FTX1) {
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                   "POWER %dW\nSRC %d", radio->power, radio->power_source);
        } else {
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                   "POWER %dW", radio->power);
        }
        upd_display_info_flash(dp->lcdbuf);
        clear_buf(radio->callsign);
        break;
      }

      if (sscanf(p, "%d", &watts) == 1 && watts >= 0 && watts <= 100) {
        set_power(radio, watts);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "POWER SET\n%d W", watts);
      } else {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "POWER ?\nPOWER5..100");
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "CURSOR") == 0) {
      if (!yaesu_scope_supported(radio)) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "CURSOR\nUNSUPPORTED");
      } else {
        // Yaesu W/F CURSOR (NORMAL): SS0670000;
        // Use the public scope path so FTDX10/FTX-1 can be tested from LCD.
        radio->scope_cursor_restore_pending = 0;
        send_cat_cmd(radio, "SS0670000;");
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "SCOPE\nCURSOR");
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strncmp(radio->callsign + 2, "DLEVEL", 6) == 0) {
      const char *p = radio->callsign + 8;
      if (!yaesu_scope_supported(radio)) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "D-LEVEL\nUNSUPPORTED");
      } else if (*p == '\0' || strcmp(p, "?") == 0) {
        send_yaesu_scope_level_query(radio);
        const int x2 = radio->scope_level_x2;
        if (x2 >= -60 && x2 <= 60)
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "D-LEVEL\n%+.1f dB",
                   x2 / 2.0);
        else
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "D-LEVEL\nUNKNOWN");
      } else {
        float db = 0.0f;
        if (sscanf(p, "%f", &db) == 1 && db >= -30.0f && db <= 30.0f) {
          const int x2 =
              (int)(db * 2.0f + (db >= 0 ? 0.5f : -0.5f));
          set_yaesu_scope_level_x2(radio, x2, true);
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "D-LEVEL SET\n%+.1f dB",
                   x2 / 2.0);
        } else {
          snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "DLEVEL\n-30..+30");
        }
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "IPO") == 0 ||
        strcmp(radio->callsign + 2, "AMP1") == 0 ||
        strcmp(radio->callsign + 2, "AMP2") == 0) {
      if (!yaesu_scope_supported(radio)) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "PREAMP\nUNSUPPORTED");
      } else {
        int preamp = 0;
        if (strcmp(radio->callsign + 2, "AMP1") == 0) preamp = 1;
        if (strcmp(radio->callsign + 2, "AMP2") == 0) preamp = 2;
        set_yaesu_preamp(radio, preamp, true);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "%s\nBAND %d SAVED",
                 preamp == 0 ? "IPO" : (preamp == 1 ? "AMP1" : "AMP2"),
                 radio->bandid);
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strncmp(radio->callsign + 2, "RIGANT", 6) == 0) {
      const char *p = radio->callsign + 2 + 6;

      if (!rig_antenna_supported(radio)) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "RIG ANT\nUNSUPPORTED");
        upd_display_info_flash(dp->lcdbuf);
        clear_buf(radio->callsign);
        break;
      }

      if (*p == '\0' || strcmp(p, "?") == 0) {
        send_rig_antenna_query(radio);
        const int saved =
            (radio->bandid >= 1 && radio->bandid <= N_BAND)
                ? radio->rig_antenna_band[radio->bandid]
                : -1;
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "RIG ANT%d\nBAND ANT%d",
                 radio->rig_antenna > 0 ? radio->rig_antenna : 0,
                 saved > 0 ? saved : 0);
      } else if (strcmp(p, "1") == 0 || strcmp(p, "2") == 0) {
        const int ant = *p - '0';
        set_rig_antenna(radio, ant, true);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "RIG ANT%d\nBAND %d SAVED",
                 ant, radio->bandid);
      } else {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "RIGANT ?\nRIGANT1/2");
      }

      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "SMETER") == 0) {
      plogw->show_smeter++;
      if (plogw->show_smeter >= 3) plogw->show_smeter = 0;
      sprintf(dp->lcdbuf, "show_smeter=%d", plogw->show_smeter);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "INTERVAL") == 0) {
      plogw->show_qso_interval=1-plogw->show_qso_interval;
      sprintf(dp->lcdbuf, "show_qso_interval=%d", plogw->show_qso_interval);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "SEQNR") == 0) {
      plogw->show_smeter=0;
      sprintf(dp->lcdbuf, "show_smeter=%d", plogw->show_smeter);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign +2, "DECODER") == 0 ) {
      decoder.start_i2s_adc_24k_rms_task();
      sprintf(dp->lcdbuf, "CW decoder start\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign +2, "DECODERSTOP") == 0 ) {
      decoder.stop_i2s_adc_24k_rms_task();
      sprintf(dp->lcdbuf, "CW decoder stopped\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if(strncmp(radio->callsign+2,"POWER",5)==0) {
      if (sscanf(radio->callsign+2+5,"%d",&tmp)==1) {
	set_power(radio,tmp);
      }
      clear_buf(radio->callsign);            
      break;
    }
    
    if (strcmp(radio->callsign + 2, "NEWQSOLOG") == 0) {
      // create new QSO log
      create_new_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if(strncmp(radio->callsign+2,"AUTOOFF",7)==0) {
      if (sscanf(radio->callsign+2+7,"%d",&tmp)==1) {
	plogw->autopoweroff=tmp;
      } else {
	//	plogw->autopoweroff=0;
      }
      sprintf(dp->lcdbuf, "AUTOOFF=%d sec\n",plogw->autopoweroff);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strcmp(radio->callsign+2,"LOADRIGS")==0) {
      load_rigs("RIGS");
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"SAVERIGS")==0) {
      save_rigs("RIGS");
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"RESETRIGS")==0) {
      //      save_rigs("RIGS");
      init_rig();
      clear_buf(radio->callsign);      
      break;
    }

    if (strcmp(radio->callsign+2,"MUXTRANS")==0) {
	f_mux_transport_cmd=1; // transition to mux
	sprintf(dp->lcdbuf,"mux_transport->mux\ncurrent=%d",f_mux_transport);
	upd_display_info_flash(dp->lcdbuf);
	clear_buf(radio->callsign);
	break;
    }
    
    if (strcmp(radio->callsign+2,"NOMUXTRANS")==0) {	
      f_mux_transport_cmd=2;
      sprintf(dp->lcdbuf,"mux_transport->no mux\ncurrent=%d",f_mux_transport);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strcmp(radio->callsign + 2, "PADDLENOR")==0) {
      paddle_setting&=~1;
      normal_paddle();      
      sprintf(dp->lcdbuf, "PADDLE\nNORMAL\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "PADDLEREV")==0) {
      paddle_setting|=1;
      //      reverse_paddle();
      set_paddle();
      sprintf(dp->lcdbuf, "PADDLE\nREVERSE\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "IAMBICA")==0) {
      paddle_setting|=2;
      //      paddle_iambic_a();
      set_paddle();
      sprintf(dp->lcdbuf, "IAMBIC_A\n");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "IAMBICB")==0) {
      paddle_setting&=~2;
      //      paddle_iambic_b();
      set_paddle();
      sprintf(dp->lcdbuf, "IAMBIC_B\n");      
      upd_display_info_flash(dp->lcdbuf);
            clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "KBDJP")==0) {
      kbdtype=1;
      Prs.jp();
      Prs1.jp();      
      sprintf(dp->lcdbuf, "Kbd:Japanese\n");      
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "KBDUS")==0) {
      kbdtype=0;
      Prs.us();
      Prs1.us();
      sprintf(dp->lcdbuf, "Kbd:US\n");      
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DISPCLOCKJST") == 0) {
      clock_display_mode = 0;
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "DISPLAY CLOCK\nJST\nSaved");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DISPCLOCKUTC") == 0) {
      clock_display_mode = 1;
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "DISPLAY CLOCK\nUTC\nSaved");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign+2,"RESETDISP")==0) {
      plogw->ostream->print("reset_display()");
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"DISPTYPE0")==0) {
      plogw->ostream->print("reset_display()");
      display_type=0;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }

    if (strcmp(radio->callsign+2,"DISPTYPE1")==0) {
      plogw->ostream->print("reset_display()");
      display_type=1;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"DISPTYPE2")==0) {
      plogw->ostream->print("reset_display()");
      display_type=2;
      init_display();
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strcmp(radio->callsign + 2, "READQSOLOG") == 0) {
      // create new QSO log
      read_qso_log(READQSO_PRINT);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "MAILQSOLOG") == 0) {
      // mail qso log
      //        mail_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "MEMSTAT") == 0) {
      request_memstat_main_subcpu();
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "LISTQSOFILE") == 0) {
      clear_buf(radio->callsign);
      list_qso_backup_files();
      break;
    }

    if (strncmp(radio->callsign + 2, "SWITCHLOG", 9) == 0) {
      const char *arg = radio->callsign + 2 + 9;
      bool valid = strlen(arg) == 3;
      for (int i = 0; valid && i < 3; i++) {
        if (arg[i] < '0' || arg[i] > '9') valid = false;
      }

      if (!valid) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "Usage:\nSWITCHLOGnnn");
        upd_display_info_flash(dp->lcdbuf);
        clear_buf(radio->callsign);
        break;
      }

      int backup_number = (arg[0] - '0') * 100
                        + (arg[1] - '0') * 10
                        + (arg[2] - '0');
      clear_buf(radio->callsign);
      switch_qso_log(backup_number);
      break;
    }

    if (strcmp(radio->callsign + 2, "ZMERGE") == 0) {
      clear_buf(radio->callsign);
      zserver_start_merge(false);
      break;
    }

    if (strcmp(radio->callsign + 2, "ANTENNAON") == 0) {
      antenna_control_enable = 1;
      antenna_settings_changed();
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ANTENNA CTRL\nON\n%s:%d",
               antenna_host, antenna_port);
      upd_display_info_flash(dp->lcdbuf);
      if (!plogw->f_console_emu) {
        plogw->ostream->print("antenna control ON: ");
        plogw->ostream->print(antenna_host);
        plogw->ostream->print(":");
        plogw->ostream->println(antenna_port);
      }
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "ANTENNAOFF") == 0) {
      antenna_control_enable = 0;
      antenna_settings_changed();
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ANTENNA CTRL\nOFF");
      upd_display_info_flash(dp->lcdbuf);
      if (!plogw->f_console_emu)
        plogw->ostream->println("antenna control OFF");
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "ANTENNA") == 0 ||
        strcmp(radio->callsign + 2, "ANTENNASTATUS") == 0) {
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "ANT CTRL:%s\n%s\n%s:%d",
               antenna_control_enable ? "ON" : "OFF",
               antenna_controller_state(),
               antenna_host, antenna_port);
      upd_display_info_flash(dp->lcdbuf);
      if (!plogw->f_console_emu) {
        plogw->ostream->print("antenna control=");
        plogw->ostream->print(antenna_control_enable ? "ON" : "OFF");
        plogw->ostream->print(" state=");
        plogw->ostream->print(antenna_controller_state());
        plogw->ostream->print(" server=");
        plogw->ostream->print(antenna_host);
        plogw->ostream->print(":");
        plogw->ostream->println(antenna_port);
      }
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "RESTARTLOG") == 0) {
      clear_buf(radio->callsign);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Restarting\nDVPlogger...");
      // This command is handled in the MAIN-loop key path, so the flash
      // message is transferred before the short grace period and reset.
      upd_display_info_flash(dp->lcdbuf);
      delay(500);
      ESP.restart();
      break;  // ESP.restart() normally does not return.
    }

    if (strcmp(radio->callsign + 2, "DUPERESET") == 0) {
      clear_buf(radio->callsign);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Resetting SUBCPU\nDUPE database...");
      upd_display_info_flash(dp->lcdbuf);

      bool ok = reset_dupechk_subcpu();
      if (ok) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SUBCPU DUPE RESET\nOK\nRun MAKEDUPE");
      } else {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SUBCPU DUPE RESET\nFAILED\nUse SUBCPURESET");
      }
      upd_display_info_flash(dp->lcdbuf);
      break;
    }

    if (strcmp(radio->callsign + 2, "SUBCPURESET") == 0) {
      clear_buf(radio->callsign);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Resetting SUBCPU...\nPlease wait");
      upd_display_info_flash(dp->lcdbuf);

      bool mux_ok = restart_subcpu_and_enter_mux();
      if (!mux_ok) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SUBCPU RESET DONE\nMUX START NO ACK\nTry again");
        upd_display_info_flash(dp->lcdbuf);
        break;
      }

      bool dupe_reset_ok = reset_dupechk_subcpu();
      if (!dupe_reset_ok) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "SUBCPU RESET DONE\nMUX/DUPE NO REPLY\nCheck connection");
        upd_display_info_flash(dp->lcdbuf);
        break;
      }

      int callhist_count = 0;
      if (plogw->enable_callhist && callhist_at == 1) {
        if (load_callhist_subcpu(callhistfn))
          callhist_count = get_callhist_subcpu_count();
      }

      request_makedupe_rebuild();

      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "SUBCPU RECOVERED\nDUPE rebuild queued\nCALLHIST=%d",
               callhist_count);
      upd_display_info_flash(dp->lcdbuf);
      break;
    }

    if (strcmp(radio->callsign + 2, "MAKEDUPE") == 0) {
      sprintf(dp->lcdbuf, "MAKEDUPE\nRebuild queued...\n");
      upd_display_info_flash(dp->lcdbuf);
      request_makedupe_rebuild();
      clear_buf(radio->callsign);
      break;
    }

    if (strcmp(radio->callsign + 2, "ESM")==0 ) {
      // ESM toggle
      plogw->f_esm = 1- plogw->f_esm;
      sprintf(dp->lcdbuf, "ESM mode = %d\n",plogw->f_esm);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strcmp(radio->callsign + 2, "WIPEKEYSWAP") == 0) {
      plogw->wipe_key_swap = 1 - plogw->wipe_key_swap;
      save_settings("");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "WIPE KEY SWAP=%d\nAlt-W: %s\nCtrl-W: %s",
               plogw->wipe_key_swap,
               plogw->wipe_key_swap ? "clear field" : "wipe QSO",
               plogw->wipe_key_swap ? "wipe QSO" : "clear field");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strcmp(radio->callsign+2,"OFFCONTEST")==0) {
      // toggle on/off contest flag
      plogw->f_off_contest=1;
      print_off_contest();
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign+2,"ONCONTEST")==0) {
      // toggle on/off contest flag
      plogw->f_off_contest=0;
      print_off_contest();
      clear_buf(radio->callsign);
      break;
    }

    if (strncmp(radio->callsign+2,"CONTEST",7)==0) {
      strcpy(plogw->contest_name+2, radio->callsign+2+7);
      set_contest_from_name();
      clear_buf(radio->callsign);
      break;
    }
    if (strncmp(radio->callsign + 2, "KEY", 3) == 0) {
      // change rigspec radio->rig_spec->cwport
      int tmp, tmp1;
      if ((tmp = sscanf(radio->callsign + 5, "%d", &tmp1)) == 1) {
	if (tmp1 >= 0 && tmp1 <= 2) {
	  radio->rig_spec->cwport = tmp1;
	  plogw->ostream->print("cwport -> ");
	  plogw->ostream->println(radio->rig_spec->cwport);
	}
      } else {
	plogw->ostream->println("invalid cwport");
      }
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "STRAIGHT") == 0) {
      // straight key flag toggle
      plogw->f_straightkey = 1 - plogw->f_straightkey;
      plogw->ostream->print("straight key enable = ");
      plogw->ostream->println(plogw->f_straightkey);
      sprintf(dp->lcdbuf, "straight key=%d", plogw->f_straightkey);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "TOGGLEPTT") == 0) {
      // straight key flag toggle
      plogw->f_toggle_ptt_mode = 1 - plogw->f_toggle_ptt_mode;
      plogw->ostream->print("toggle ptt by right shift key = ");
      plogw->ostream->println(plogw->f_toggle_ptt_mode);
      sprintf(dp->lcdbuf, "toggle ptt\nRIGHT SHIFT=%d", plogw->f_toggle_ptt_mode);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strncmp(radio->callsign + 2, "SAVE", 4) == 0) {

      // release other settings  including sat
      release_memory();

      save_settings(radio->callsign + 6);
      if (!plogw->f_console_emu) plogw->ostream->println("save");
      clear_buf(radio->callsign);
      break;
    }
    if (rotator_commands(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
    if (antenna_switch_commands(radio->callsign + 2)) {
      clear_buf(radio->callsign);

      break;
    }
    if (strncmp(radio->callsign + 2, "NEXTRIG", 7) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("next rig");
      switch_radios(radio->rig_idx,-1);
      break;
    }
    if (strncmp(radio->callsign + 2, "PREVRIG", 7) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("prev rig");
      switch_radios(radio->rig_idx,-2);      
      break;
    }
    if (strcmp(radio->callsign + 2, "ENABLERIG") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("enable rig");
      enable_radios(radio->rig_idx,1);
      clear_buf(radio->callsign);      
      break;
    }
    if (strcmp(radio->callsign + 2, "DISABLERIG") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("disable rig");
      enable_radios(radio->rig_idx,0);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strncmp(radio->callsign + 2, "LOAD", 4) == 0) {
      load_settings(radio->callsign + 6);
      if (!plogw->f_console_emu) plogw->ostream->println("load");
      clear_buf(radio->callsign);
      break;
    }
    if (strncmp(radio->callsign + 2, "NATTO", 5) == 0) {
      cw_dah_ratio_bunshi=40;
      cw_ratio_bunbo=10;
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CWJQF", 5) == 0) {
      cw_duty_ratio=13;
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CWNORMAL", 5) == 0) {
      cw_dah_ratio_bunshi=30;
      cw_ratio_bunbo=10;
      cw_duty_ratio=10;      
      clear_buf(radio->callsign);      
      break;
    }
    
    if (strncmp(radio->callsign+2,"VERBOSE",7)==0) {
      if (sscanf(radio->callsign+2+7,"%d",&tmp)==1) {
	verbose=tmp;
      } else {
	verbose=0;
      }
      break;
    }
    if (strncmp(radio->callsign+2,"DEBUG",5)==0) {
      so2r.debug=1-so2r.debug;
      break;
    }
    if (strcmp(radio->callsign+2,"ALTCQ")==0) {
      // alt cq
      so2r.sequence_mode(SO2R::SO2R_ALTCQ);
      so2r.sequence_stat(SO2R::Default);      
      if (verbose &4) {
	console->println("SO2R_ALTCQ");
      }
      clear_buf(radio->callsign);            
      break;
    }
    if (strcmp(radio->callsign+2,"2BSIQ")==0) {
      // 2bsiq
      so2r.sequence_mode(SO2R::SO2R_2BSIQ);
      so2r.sequence_stat(SO2R::Default);      
      if (verbose&4)       console->println("SO2R_2BSIQ");
      clear_buf(radio->callsign);      
      break;
    }

     

    if (strcmp(radio->callsign + 2, "CALLHISTMAIN") == 0) {
      callhist_at = 0;
      plogw->enable_callhist = 1;
      int n = read_callhist_list(callhistfn);
      plogw->ostream->printf("Call History location: MAIN CPU, entries=%d\n", n);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Call History\n%s\n%d entries\nAt %s\n%s",
               callhistfn, n, f_spiram ? "MAIN-PSRAM" : "MAIN-RAM",
               n > 0 ? "Enabled" : "Load failed");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "CALLHISTSUB") == 0) {
      callhist_at = 1;
      plogw->enable_callhist = 1;
      bool fallback = false;
      int n = load_callhist_subcpu_or_main(callhistfn, &fallback);
      plogw->ostream->printf("Call History location: %s, entries=%d%s\n",
                             callhist_at ? "SUB CPU" : "MAIN-PSRAM", n,
                             fallback ? " (fallback)" : "");
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Call History\n%s\n%d entries\n%s\n%s",
               callhistfn, n,
               n > 0 ? (fallback ? "MAIN-PSRAM fallback" : "At SUBCPU")
                     : "SUB load failed",
               n > 0 ? "Enabled" : "No PSRAM / Disabled");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    
    if (strncmp(radio->callsign + 2, "CALLHIST", 8) == 0) {
      // The Callsign field does not accept spaces, so an optional filename
      // starts immediately after CALLHIST.
      const char *arg = radio->callsign + 10; // CALLHIST + filename, no space
      if (*arg != '\0') set_callhistfn((char *)arg);
      plogw->enable_callhist = 1;
      bool fallback = false;
      int n = (callhist_at == 1) ? load_callhist_subcpu_or_main(callhistfn, &fallback)
                                 : read_callhist_list(callhistfn);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "Call History\n%s\n%d entries\nAt %s\n%s",
               callhistfn, n, callhist_at ? "SUBCPU" : (f_spiram ? "MAIN-PSRAM" : "MAIN-RAM"),
               n > 0 ? "Enabled" : "Load failed");
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }

    
    if (strcmp(radio->callsign + 2, "LISTDIR") == 0) {
      listDir(SD, "/", 0);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign+2,"ADCSTAT")==0) {
      adc_statistics();
      break;
    }
    if (strcmp(radio->callsign + 2, "HELP") == 0) {
      // If HELP is still visible, advance to the next page.  After the
      // 10-second timeout, show the last selected page again first.
      if (info_disp.show_info == INFO_DISP_HELP && info_disp.timer > 0) {
        plogw->help_idx = (plogw->help_idx + 1) % HELP_PAGE_COUNT;
      }
      print_help(plogw->help_idx);
      //        clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "HELPR") == 0) {
      // Mirror HELP: move backward only while a HELP page is visible.
      // After timeout, redisplay the current page without changing it.
      if (info_disp.show_info == INFO_DISP_HELP && info_disp.timer > 0) {
        plogw->help_idx =
            (plogw->help_idx + HELP_PAGE_COUNT - 1) % HELP_PAGE_COUNT;
      }
      print_help(plogw->help_idx);
      //        clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "WIFI") == 0) {
      set_wifi_enabled(!wifi_enable);
      sprintf(dp->lcdbuf, "wifi_enable=%d", wifi_enable);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "XITON") == 0 ||
        strcmp(radio->callsign + 2, "XITOFF") == 0 ||
        strcmp(radio->callsign + 2, "XITRESET") == 0) {
      const char *cmd = radio->callsign + 2;
      const bool icom_rit_xit =
          radio->rig_spec != NULL &&
          radio->rig_spec->cat_type == CAT_TYPE_CIV &&
          rit_control_supported(radio);
      if (!icom_rit_xit && !xit_control_supported(radio)) {
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "XIT unsupported\n%s", radio->rig_spec->name);
      } else if (strcmp(cmd, "XITON") == 0) {
        radio->xit_enabled = true;
        apply_xit_for_operating_mode(radio);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "XIT ON\n%+d Hz\n%s", radio->xit_offset_hz,
                 radio->cq[radio->modetype] == LOG_SandP ? "S&P" : "CQ: forced 0");
      } else if (strcmp(cmd, "XITRESET") == 0) {
        radio->xit_offset_hz = 0;
        apply_xit_for_operating_mode(radio);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "XIT RESET\n0 Hz");
      } else {
        radio->xit_enabled = false;
        set_xit_control(radio, false);
        set_xit_offset_hz(radio, 0);
        snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
                 "XIT OFF\nsaved %+d Hz", radio->xit_offset_hz);
      }
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "CLOCKSYNC") == 0) {
      int n = request_icom_clock_sync_all();
      plogw->ostream->printf(
          "CLOCKSYNC requested for %d supported Icom radio(s); "
          "will send at next minute boundary after valid CI-V RX\n", n);
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "CLOCKSYNC\n%d radio(s) armed\nWait for :00", n);
      upd_display_info_flash(dp->lcdbuf);
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "DUMPQSOLOG") == 0) {
      // create new QSO log
      dump_qso_log();
      clear_buf(radio->callsign);
      break;
    }
    if (strcmp(radio->callsign + 2, "EXITEMU") == 0) {
      // exit from console_emu
      plogw->f_console_emu = 0;
      plogw->ostream->println("\nexit from console_emu");
      clear_buf(radio->callsign);
      break;
    }
    //      if (strncmp(radio->callsign + 2, "BAND", 4) == 0) {
    //        switch_bands(radio);
    //        break;
    //      }
    if (strncmp(radio->callsign + 2, "RADIO", 5) == 0) {
      // config ratio command
      // radio radio#+0 disable 1 enable 2 prev_radio 3 next_radio
      int tmp, tmp1;
      tmp1 = sscanf(radio->callsign + 7, "%d", &tmp);
      if (tmp1 == 1) {
	int idx_radio, radio_cmd;
	radio_cmd = tmp % 10;
	idx_radio = radio_cmd / 10;
	plogw->ostream->print("radio cmd idx=");
	plogw->ostream->println(idx_radio);
	plogw->ostream->print(" cmd 0 disable 1 enable 2 sw - 3 sw + =");
	plogw->ostream->println(radio_cmd);
	if (idx_radio < 0 || idx_radio > 2) break;  // invalid radio#
	switch (radio_cmd) {
	case 0:  // disable
	  enable_radios(idx_radio, 0);
	  break;
	case 1:  // enable
	  enable_radios(idx_radio, 1);
	  break;
	case 2:  // setch -
	  switch_radios(idx_radio, -1);
	  break;
	case 3:  // switch +
	  switch_radios(idx_radio, -2);
	  break;
	}
      }
      break;
    }

    if (strncmp(radio->callsign + 2, "BANDEN", 6) == 0) {
      // define bandenable in hex
      if (*(radio->callsign+2+6)!='\0') {
	radio->band_mask = (~strtol(radio->callsign + 2 + 6, NULL, 16))&((1<<(N_BAND-1))-1);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band_mask=");
	plogw->ostream->println(radio->band_mask, HEX);
      }
      print_band_mask(radio);
      break;
    }

    if (strncmp(radio->callsign + 2, "BANDMASK", 8) == 0) {
      // define bandmask in hex
      if (*(radio->callsign+2+8)!='\0') {
	radio->band_mask = strtol(radio->callsign + 2 + 8, NULL, 16);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band_mask=");
	plogw->ostream->println(radio->band_mask, HEX);
      }
      print_band_mask(radio);
      break;
    }
    if (signal_command(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
    if (antenna_alternate_command(radio->callsign + 2)) {
      clear_buf(radio->callsign);
      break;
    }
	  
    if (strncmp(radio->callsign + 2, "BANDMAP", 7) == 0) {
      // define bandmap_mask  in hex
      if (*(radio->callsign+2+7)!='\0') {      
	bandmap_mask = strtol(radio->callsign + 2 + 7, NULL, 16);
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print("bandmap_mask=");
	plogw->ostream->println(bandmap_mask, HEX);
      }
      print_bandmap_mask();
      break;
    }
    // check if mode change, freq change
    if (rig_modenum(radio->callsign + 2) != -1) {
      if (is_manual_rig(radio)) { // manual rig set directly
	set_mode_nonfil(radio->callsign +2,radio);
      } else {
	if (plogw->sat) {
	  set_sat_opmode(radio, radio->callsign + 2);
	} else {
	  if (verbose & 1) {
	    plogw->ostream->print("setting mode filt ");
	    plogw->ostream->println(radio->modetype);
	    plogw->ostream->println(radio->cq[radio->modetype]);
	    plogw->ostream->println(radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype]);
	  }
	  send_mode_set_civ(radio->callsign + 2, radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype]);  // cw phone switch filter selection!
	}
      }
      clear_buf(radio->callsign);
      break;
    }
    // frequency entry
    double frequency;
    frequency = check_frequency((String)(radio->callsign + 2));
    if (frequency != ErrorFloat) {
      if (verbose & 1) {
	plogw->ostream->print("setting frequency(kHz):");
	plogw->ostream->println(frequency);
      }
      if (is_manual_rig(radio)) {
	set_frequency((unsigned int)(frequency * (1000/FREQ_UNIT)), radio);
	radio->f_freqchange_pending=0;
      } else {
	set_frequency_rig((unsigned int)(frequency * (1000/FREQ_UNIT)));  // kHz -> Hz
      }
      clear_buf(radio->callsign);      
      break;
    }

    // A dot is easier to type than slash on some compact keyboards.  Keep it
    // untouched until all CALLSIGN-field commands/mode/frequency parsing has
    // failed, so e.g. "14.074" is still interpreted as a frequency.  Only
    // when the remaining text is going to be treated as a callsign do we use
    // '.' as an alias for '/'.
    bool callsign_dot_alias = false;
    for (char *q = radio->callsign + 2; *q != '\0'; ++q) {
      if (*q == '.') {
        *q = '/';
        callsign_dot_alias = true;
      }
    }
    if (callsign_dot_alias) {
      radio->callsign[1] = strlen(radio->callsign + 2);

      // The live DUPE/CALLHIST/partial query was made while the edit buffer
      // still contained '.'.  Start a fresh query for the normalized call so
      // S&P Enter cannot reuse a stale result for the dot spelling.
      request_async_dupe_partial(radio, true);
      request_dupe_aware_display_update();
    }

    // ESM
    if (plogw->f_esm==1) {
      if (radio->cq[radio->modetype]) {
	// CQ -> send his callsign and number and move to exch
	ui_response_call_and_move_to_exch(radio); // send call and number and move to exch
      } else {
        // S&P: never call an unchecked station.  If the combined query is
        // still running, only this transmit request is deferred; keyboard
        // processing continues and no "unknown" indication is displayed.
        if (request_sp_send_after_dupe(radio)) ui_send_mycall(radio);
      }
    }
    break;
  }
  upd_display();

}

int check_edit_mode()  // CW key input and Remarks  return 1 (insert) else return 0 (overwrite edit)
{
  struct radio *radio;
  radio = so2r.radio_selected();
  if (radio->ptr_curr == 0) return 2;   // callsign edit  (　insert mode edit exception)
  if (radio->ptr_curr == 6) return 1;   // Remarks edit
  if (radio->ptr_curr == 9) return 1;   // jcc edit  
  if (radio->ptr_curr == 21) return 1;  // cluster  name edit
  if (radio->ptr_curr == 22) return 1;  // email edit
  if (radio->ptr_curr == 25) return 1;  // wifi ssid
  if (radio->ptr_curr == 26) return 1;  // wifi pass
  if (radio->ptr_curr == 23) return 1;  // cluster cmd edit
  if (radio->ptr_curr == 27) return 1;  // rig_spec_str
  if (radio->ptr_curr == 28) return 1;  // zserver_name
  if (radio->ptr_curr == 29) return 1;  // my_name
  if (radio->ptr_curr == 40) return 0;  // contest_name
  if (radio->ptr_curr == 41) return 1;  // cluster2 name
  if (radio->ptr_curr == 42) return 1;  // cluster2 command
  if (radio->ptr_curr == 43) return 1;  // hostname / mDNS name

  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) return 1;  // CW key msg
  if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) return 1;  // CW key msg
  return 0;
}


int ptr_curr_req_uppercase(struct radio *radio) {
  if ((radio->ptr_curr != 6) && (radio->ptr_curr != 20) &&(radio->ptr_curr != 21) && (radio->ptr_curr != 22) && (radio->ptr_curr != 23) && (radio->ptr_curr != 25) && (radio->ptr_curr != 26) && (radio->ptr_curr != 27) && (radio->ptr_curr != 28) && (radio->ptr_curr != 29) && (radio->ptr_curr != 40) && (radio->ptr_curr != 41) && (radio->ptr_curr != 42) && (radio->ptr_curr != 43) )   {
    return 1;
  } else {
    return 0;
  }
  
}

int ptr_curr_req_nospace(struct radio *radio) {
  if (
      (!((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)))
      && (!((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)))
      && (radio->ptr_curr != 6)
      && (radio->ptr_curr != 23)
      && (radio->ptr_curr != 42)
      && (radio->ptr_curr != 28)
      && (radio->ptr_curr != 29)
      && (radio->ptr_curr != 9)
      && (radio->ptr_curr != 25)
      ) {
    // JCC and Wi-Fi SSID allow space
    return 1;
  } else {
    return 0;
  }
}

int ptr_curr_req_callsign_exch_chr(struct radio *radio) {
  switch(radio->ptr_curr) {
  case 0:
  case 1:
  case 4:
    //  case 5: // now allow macro expansion so allow non callsign exchange chr for sent_exch
    return 1;
  default:
    return 0;
  }
}


int ptr_curr_req_num(struct radio *radio) {
  switch(radio->ptr_curr) {
  case 2:
  case 3:
    return 1;
  default:
    return 0;
  }
}


void logw_handler(char key, char c)
// key: key code
// c: ascii code 0x00 --> non character keys
{

  struct radio *radio;
  radio = so2r.radio_selected();
  char callsign_before[LEN_CALL_WINDOW + 1];
  bool callsign_content_changed = false;
  bool defer_display_for_dupe = false;
  if (radio->ptr_curr == 0) {
    strncpy(callsign_before, radio->callsign + 2, LEN_CALL_WINDOW);
    callsign_before[LEN_CALL_WINDOW] = '\0';
  } else {
    callsign_before[0] = '\0';
  }
  char *pwin;  // pointer to the window
  // handle input character  in window
  // ptr_curr: 10-14(N_CWMSG-1)  cw_msg window
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    pwin = plogw->cw_msg[radio->ptr_curr - 10];
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // cw msg
      pwin = plogw->rtty_msg[radio->ptr_curr - 30];
    } else {
      switch (radio->ptr_curr) {
      case 0:  // call sign window
	pwin = radio->callsign;
	break;
      case 1:  // my exchange
	pwin = radio->recv_exch;
	break;
      case 2:  // his  rst
	pwin = radio->sent_rst;
	break;
      case 3:  // his rst (recv_rst)
	pwin = radio->recv_rst;
	break;
      case 4:  // my callsign
	pwin = plogw->my_callsign;
	break;
      case 5:  // his exchange
	pwin = plogw->sent_exch;
	break;
      case 6:  // remarks
	//plogw->ostream->println("remarks");
	pwin = radio->remarks;
	break;
      case 7:  // sat name
	pwin = plogw->sat_name;
	break;
      case 8:  // grid locator
	pwin = plogw->grid_locator;
	break;
      case 9:  // jcc
	pwin = plogw->jcc;
	break;
      case 20:  // rig_name
	pwin = radio->rig_name;
	break;
      case 21:  // cluster_name
	pwin = plogw->cluster_name;
	break;
      case 22:  // email_addr
	pwin = plogw->email_addr;
	break;
      case 23:  // cluster_cmd
	pwin = plogw->cluster_cmd;
	break;
      case 24:  // power_code`
	pwin = plogw->power_code;
	break;
      case 25:  // wifi_ssid
	pwin = plogw->wifi_ssid;
	break;
      case 26:  // wifi_passwd
	pwin = plogw->wifi_passwd;
	break;
      case 27:  // rig_spec
	pwin = radio->rig_spec_str;
	break;
      case 28:  // zserver_name
	pwin = plogw->zserver_name;
	break;
      case 29:  // my_name
	pwin = plogw->my_name;
	break;
      case 40:  // contest_name
	pwin = plogw->contest_name;
	break;
      case 41:  // cluster2_name
	pwin = plogw->cluster2_name;
	break;
      case 42:  // cluster2_cmd
	pwin = plogw->cluster2_cmd;
	break;
      case 43:  // hostname / mDNS name
        pwin = plogw->hostname;
        break;
	
      default:
	// do nothing and ignore
	if (verbose & 1) {
	  plogw->ostream->print("ptr_curr!?=");
	  plogw->ostream->println(radio->ptr_curr);
	}
	return;
      }
    }
  }
  // check keys
  if (c == 0x00) {
    switch (key) {
    case CHR_BS:
      // backspace character
      if (verbose & 1) plogw->ostream->println("(BS)");
      backspace_buf(pwin);
      break;
    case CHR_DEL:
      if (verbose & 1) plogw->ostream->println("(DEL)");
      delete_buf(pwin);
      break;
    case 0x4f:  // RIGHT
      if (verbose & 1) plogw->ostream->println("(RIGHT)");
      right_buf(pwin);
      break;
    case 0x50:  // LEFT
      if (verbose & 1) plogw->ostream->println("(LEFT)");
      left_buf(pwin);
      break;
    default:
      break;
    }
  } else {
    //plogw->ostream->println("chk");
    // other character change to upper case
    // list ptr_curr in which cases matters 
    // not cluster address (need small characters to allow input )
    if (ptr_curr_req_uppercase(radio)) {
      if (c >= 'a' && c <= 'z') c += 'A' - 'a';
    }
    
    if (ptr_curr_req_callsign_exch_chr(radio)) {
      if (!(isalnum(c)||(c=='/')||(c=='.')||(c=='-')||
            (radio->ptr_curr == 1 && c==','))) { // callsign/exchange characters
        return ;
      }
    } else {
      if (ptr_curr_req_num(radio)) {
	if (!isdigit(c)) {
	  return;
	}
      }
    }
    if (ptr_curr_req_nospace(radio)) {
      // do not allow space
      if (isSpace(c)) return;
    }
    if (!isPrintable(c)) return;
    //plogw->ostream->println("chk1");
    if (radio->ptr_curr == 6) {
      switch(radio->f_romaji) {
      case 0:
	insert_buf(c, pwin);  // insert to buffer in cw msg edit mode
	break;
      case 1:
	romaji_input_char(c,pwin);
	break;
      case 2:
	flush_preedit(pwin);
	insert_buf(c,pwin);
	radio->f_romaji=0;
	g_pre.s[0]='\0';g_pre.len=0; // clear preedit
	break;
      } 
    } else {
      if (check_edit_mode()) {
	insert_buf(c, pwin);  // insert to buffer in cw msg edit mode
      } else {
	overwrite_buf(c, pwin);  // overwriting is more appropriate than insert in logging
      }
    }
    // activity in log window leads to memorize frequency if S&P
    if (radio->cq[radio->modetype] == LOG_SandP) {
      // memorize current frequency to freqbank s and p freqnency if s&p mode
      save_freq_mode_filt(radio);
    }

  }

  
  // Expensive DUPE/partial checks are needed only when the callsign text
  // actually changed. Cursor movement and ignored keys used to repeat both
  // synchronous sub-CPU queries with exactly the same input.
  if (radio->ptr_curr == 0) {
    callsign_content_changed =
      strcmp(callsign_before, radio->callsign + 2) != 0;
  }

  // on-demand processes after editing
  switch (radio->ptr_curr) {
  case 0:  // call sign window
    if (!callsign_content_changed) break;
    // Do not block keyboard handling on subcpu searches.  One combined
    // asynchronous request supplies DUPE, exact-match EXCH and partial data.
    request_async_dupe_partial(radio, true);
    request_dupe_aware_display_update();
    defer_display_for_dupe = true;
    break;
  case 1: { // recv_exch
    if (strlen(radio->recv_exch + 2) >= 1) {
      char primary_exch[LEN_EXCH + 1] = {0};
      char secondary_exch[LEN_EXCH + 1] = {0};
      char sep = 0;
      split_dual_recv_exch(radio->recv_exch + 2,
                           primary_exch, sizeof(primary_exch),
                           secondary_exch, sizeof(secondary_exch), &sep);
      // While editing a dual exchange, the current contest's live multiplier
      // check is always performed on the left/primary side only.
      radio->multi = multi_check(primary_exch, radio->bandid);
      if (radio->multi >= 0) {
	upd_display_info_multi_bands(radio);
      }
    }
    break;
  }
  }


  if (!defer_display_for_dupe) request_display_update_on_demand();
}

void 	check_call_show_dx_entity_info(struct radio *radio) {
  if (strlen(radio->callsign+2)>2) {
    switch (plogw->multi_type) {
    case MULTI_TYPE_CQWW:
    case MULTI_TYPE_AA:
      show_entity_info(radio->callsign+2);
      break;
    }
  }
}



// switch from a entry to the other with TAB or SPC
void switch_logw_entry(int option) {
  struct radio *radio;
  radio = so2r.radio_selected();
  switch (option) {
  case 3:  // direct move to Callsign
    radio->ptr_curr = 0;
    break;
  case 2:  // direct move to Remarks
    radio->ptr_curr = 6;
    break;
  case 4:  // direct move to Rcv rst
    radio->ptr_curr = 3;
    if (strlen(radio->recv_rst+2)>=2) {
      radio->recv_rst[1]=1; // cursor move to S position
    }
    break;
  case 5:  // direct move to recv_exch
    radio->ptr_curr = 1;
    break;
  case 6: // direct move to Sent rst 
    radio->ptr_curr = 2 ;
    if (strlen(radio->sent_rst+2)>=2) {
      radio->sent_rst[1]=1; // cursor move to S position
    }
    break;
  case 7:  // direct move to RIG name, cursor at the first character
    radio->ptr_curr = 20;
    radio->rig_name[1] = 0;
    break;
  case 8:  // direct move to Contest name, cursor at the first character
    radio->ptr_curr = 40;
    plogw->contest_name[1] = 0;
    break;
  case 0:  // forward
    if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func forward");
      radio->ptr_curr = radio->ptr_curr + 1;
      if (radio->ptr_curr >= 10 + N_CWMSG) radio->ptr_curr = 10;
      break;
    }
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func forward");
      radio->ptr_curr = radio->ptr_curr + 1;
      if (radio->ptr_curr >= 30 + N_CWMSG) radio->ptr_curr = 30;
      break;
    }

    switch (radio->ptr_curr) {
    case 0:  // his callsign
      // check callsign for callhist, my history and fill in my exchange
      // dupe check
      if (strlen(radio->callsign + 2) >= 3) {
        request_async_dupe_partial(radio, true);
        check_call_show_dx_entity_info(radio);
      }
      radio->ptr_curr = 1;
      break;
    case 1:  // my exchange
      radio->ptr_curr = 2;
      break;
    case 2:  // my rst (sent rst)
      radio->ptr_curr = 3;
      break;
    case 3:  // his rst (recv rst)
      radio->ptr_curr = 4;
      break;
    case 4:  // my_callsign
      radio->ptr_curr = 5;
      break;
    case 5:  // his exchange
      radio->ptr_curr = 6;
      break;
    case 6:  // remarks
      radio->ptr_curr = 40;
      break;
    case 40: // contest_name
      radio->ptr_curr = 7;
      break;
    case 7:  // sat name
      radio->ptr_curr = 8;
      break;
    case 8:  // grid locator
      radio->ptr_curr = 9;
      break;
    case 9:  // jcc
      radio->ptr_curr = 29;
      break;
    case 29:  // my_name (next to jcc)
      radio->ptr_curr = 20;
      break;
    case 20:  // rig_name
      radio->ptr_curr = 27;
      break;
    case 27:  // rig_spec_str (next to rig_name)
      radio->ptr_curr = 21;
      break;
    case 21:  // cluster_name
      radio->ptr_curr = 23;
      break;
    case 23:  // cluster_cmd
      radio->ptr_curr = 41;
      break;
    case 41:  // cluster2_name
      radio->ptr_curr = 42;
      break;
    case 42:  // cluster2_cmd
      radio->ptr_curr = 22;
      break;
    case 22:  // email_addr
      radio->ptr_curr = 24;
      break;
    case 24:  // power
      radio->ptr_curr = 25;
      break;
    case 25:  // wifi_ssid
      radio->ptr_curr = 26;
      break;
    case 26:  // wifi_passwd
      radio->ptr_curr = 28;
      break;
    case 28:  // zserver_name
      radio->ptr_curr = 43;
      break;
    case 43:  // hostname
      radio->ptr_curr = 0;
      break;
    }
    break;
  case 1:  // backward  easier to go back to exchange
    if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func backward");
      radio->ptr_curr = radio->ptr_curr - 1;
      if (radio->ptr_curr < 10) radio->ptr_curr = 10 + N_CWMSG - 1;
      break;
    }
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // CW F1-F5 msg
      if (verbose & 1) plogw->ostream->println("Func backward");
      radio->ptr_curr = radio->ptr_curr - 1;
      if (radio->ptr_curr < 30) radio->ptr_curr = 30 + N_CWMSG - 1;
      break;
    }
    switch (radio->ptr_curr) {
    case 0:  // his callsign
      // check callsign for callhist, my history and fill in my exchange
      radio->ptr_curr = 1;
      break;
    case 1:  // my exchange
      radio->ptr_curr = 0;
      break;
    case 2:  // my rst
      radio->ptr_curr = 1;
      break;
    case 3:  // his rst
      radio->ptr_curr = 2;
      break;
    case 4:  // my_callsign
      radio->ptr_curr = 3;
      break;
    case 5:  // his exchange
      radio->ptr_curr = 4;
      break;
    case 6:  // remarks
      radio->ptr_curr = 5;
      break;
    case 40: // contest_name
      radio->ptr_curr = 6;
      break;
    case 7:  // sat name
      radio->ptr_curr = 40;
      break;
    case 8:  // grid locator
      radio->ptr_curr = 7;
      break;
    case 9:  // jcc
      radio->ptr_curr = 8;
      break;
    case 29:  // my_name (next to jcc)
      radio->ptr_curr = 9;
      break;
    case 20:  // rig_name
      radio->ptr_curr = 29;
      break;
    case 27:  // rig_spec_str
      radio->ptr_curr = 20;
      break;
    case 21:  // cluster_name
      radio->ptr_curr = 27;
      break;
    case 23:  // cluster_cmd
      radio->ptr_curr = 21;
      break;
    case 22:  // email_addr
      radio->ptr_curr = 42;
      break;
    case 42:  // cluster2_cmd
      radio->ptr_curr = 41;
      break;
    case 41:  // cluster2_name
      radio->ptr_curr = 23;
      break;
    case 24:  // power_code
      radio->ptr_curr = 22;
      break;
    case 25:  // wifi_ssid
      radio->ptr_curr = 24;
      break;
    case 26:  // wifi_passwd
      radio->ptr_curr = 25;
      break;
    case 28:  // zserver_name
      radio->ptr_curr = 26;
      break;
    case 43:  // hostname
      radio->ptr_curr = 28;
      break;
    }
    break;
  }
  if (verbose & 1) {
    plogw->ostream->print("ptr_curr:");
    plogw->ostream->println(radio->ptr_curr);
    plogw->ostream->print("\t");
  }
  request_display_update_on_demand();
}


void sat_name_entered() {
  plogw->sat = 1;
  // check sat_name
  //#ifdef notdef
  // check if tlefile read
  if (plogw->tle_unixtime==0) {
    readtlefile();
  }
  if (strcmp(plogw->sat_name + 2, plogw->sat_name_set) != 0) {
    load_satinfo();
    // set new satellite
    if (find_satname(plogw->sat_name + 2) != -1) {
      strcpy(plogw->sat_name_set, plogw->sat_name + 2);
      //        set_sat_info_calc(plogw->sat_name_set);
      set_sat_index(plogw->sat_name_set);

      char auto_reason[160];
      auto_select_sat_vfo_mode(auto_reason, sizeof(auto_reason));
      if (verbose & 8) {
        plogw->ostream->print("SAT Auto VFO: ");
        plogw->ostream->println(auto_reason);
      }
      struct radio *radio;
      radio=so2r.radio_selected();
      // set satellite opmode
      //set_sat_opmode(plogw->opmode);
      set_sat_opmode(radio, "CW");
    }
  }
  //#endif
}


// print help on the left screen
void print_help(int option) {
  if (option < 0 || option >= HELP_PAGE_COUNT) option = 0;

  dp->lcdbuf[0] = '\0';
  for (int line = 0; line < 6; ++line) {
    if (line == 5) {
      char numbered_line[96];
      snprintf(numbered_line, sizeof(numbered_line), "%s\x1f%d/%d",
               help_pages[option].line[line], option + 1, HELP_PAGE_COUNT);
      strlcat(dp->lcdbuf, numbered_line, sizeof(dp->lcdbuf));
    } else {
      strlcat(dp->lcdbuf, help_pages[option].line[line], sizeof(dp->lcdbuf));
    }
    if (line != 5) strlcat(dp->lcdbuf, "\n", sizeof(dp->lcdbuf));
  }
  upd_display_info_flash(dp->lcdbuf);
  // HELP is temporary, but longer-lived than ordinary flash messages.
  // INFO_DISP_HELP also lets HELP/HELPR distinguish a visible page from a
  // page that has already timed out and returned to the bandmap.
  info_disp.show_info = INFO_DISP_HELP;
  info_disp.timer = 10000;
}

