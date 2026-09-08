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

#ifndef FILE_SO2R_H
#define FILE_SO2R_H
#include "Arduino.h"
#include "decl.h"
#include "hardware.h"
#include "variables.h"

#include "mcp.h"
#include "ui.h"
#include "cw_keying.h"
#include "display.h"
#include "cat.h"
#include "dupechk.h"
#include "cmd_interp.h"
//#include "AudioPlayer.h"

class SO2R {
public:
  enum RadioMode {
    RADIO_MODE_SO1R = 0,
    RADIO_MODE_SAT = 1,
    RADIO_MODE_SO2R = 2
  };
  
  enum SequenceMode {Manual, Repeat_Func, SO2R_Repeat_Func, SO2R_CQSandP, SO2R_ALTCQ, SO2R_2BSIQ };    // sequencing mode
  enum SequenceStat {Default, Sending_Msg, Sending_Msg_Finished, Timer_Started, Timer_Expired, Timer2_Started, Timer2_Expired } ; // status
  enum QsoStat {SendCQ,StandBy,SendCall, SendCallExch, SendExch, SendTU }; // status of ongoing QSO in each radio direction for the next action

  // SequenceStat 遷移
  // 
  // msg radio set to the focused radio when Enter pressed (action being taken)
  // SO2R_CQSandP
  //   focus alternate when Sending_Msg 
  //   Default -- (need send message) - Sending_Msg - (PTT ON->OFF or CW finished ) - Sendning_Msg_Finished - set focus to msg_radio - Default
  // SO2R_Repeat_Func
  //   (if QSOstat = SendCQ)  -- (Send CQ) - Sending_Msg (TX in msg radio, RX in other radio ) - (PTT ON->OFF or CW finished ) - Sendning_Msg_Finished
  //  -(RX and focus to msg radio) -(start timer) Timer_Started - TimerExpired -- (Response term finished)
  //  (QSOstat is set SendCQ when SendTU is sent finished.)
  //   (if QSOstat not SendCQ) - (keep Default)
  // SO2R_ALTCQ
  //   (if QSOstat of msg_radio is SendCQ) Send CQ - focus on the other radio - Sending_Msg_Finished - set focus to msg radio and msg_radio goes to the other radio
  //                               SendTU and send tu then Sending_msg_finished then set SendCQ on the msg_radio and msg_radio goes to the other radio
  // QsoStat 遷移 what to do next
  //  focused_radio がCQ でcall欄 でenter 何も入力されていない場合SendCQ  なにか入力されていたら SendCallExchに遷移 exchange 欄で有効なナンバーが入力されてenter が押されたらSendTUに遷移
  // TUが送出終わったら- SendCQ pattern
  //  focused radio がS&P  call欄が入力 SendCall - exchge有効が入力 SendExch 

  
private:
  
  int tx_;   // transmit radio 
  int rx_;   // receive  radio
  int stereo_; // status of stereo


  
  enum SequenceMode sequence_mode_;  // sequencing mode 
  //  int f_sequence_stat_;     // control variable for sequencing
  enum SequenceStat sequence_stat_;     // control variable for sequencing
  enum QsoStat qso_stat_[3]; // 各radio でのQSO進行状況
  
  int sequence_stat_changed_; // flag to indicate sequence_stat has changed 
  
  int msg_tx_radio_; // radio index corresponding to the radio to send / or sending message (like function keys) 
  int qso_process_radio_; // radio # for the currently processing qso ( may be different from focused during So2r

  int f_chgstat_tx_;  // request to change tx, rx (set_rx() and set_tx() takes long time so not call in interrupt
  int f_chgstat_rx_;

  volatile int repeat_timer_;   // control variables for repeating functions 
  int repeat_timer_set_;
  int msg_key_;        // key number for the message
  int repeat_cq_radio_; // memorize radio to perform repeat cq 

  int focused_radio_,focused_radio_prev_; // radio index corresponding to input entry window

  // The receive/focus radio selected by the operator before a message starts.
  // set_rx_in_sending_msg() temporarily moves focus to the other receiver, so
  // focused_radio_prev_ is not a reliable return target: set_rx() updates it
  // again while the TX sequence is running.
  int tx_return_focus_radio_;
  bool tx_return_focus_valid_;

  // One persistent symmetric SO2R pair.  Only these two radios alternate:
  // when either member transmits, the other member is selected for receive.
  int so2r_pair_a_;
  int so2r_pair_b_;

  int timeout_monitor; // timeout for monitoring string queue
  int queue_monitor_status;

  // どのRADIOが受信状態にあるかは必ずfocused_radio_ を参照する。
  // 同様にどのRADIOで送信を行っているかについてもmsg_tx_radio_を参照する。
  // これらを変更してからハードウェアの設定変更までは遅れがあるため、focused_radio_ が変わったときにはf_chgstat_rx_を通じてrx_
  // msg_tx_radio_が変わるときにはf_chgstat_tx_を通じてtx_及びハードウェアを変更する。

public:
  
  SO2R() : tx_(0), rx_(0),stereo_(0),sequence_mode_(Manual),sequence_stat_(Default), qso_stat_{SendCQ,SendCQ,SendCQ}, sequence_stat_changed_(0),
	   msg_tx_radio_(0),f_chgstat_tx_(0),f_chgstat_rx_(0),
	   repeat_timer_(0),repeat_timer_set_(3000),msg_key_(0),repeat_cq_radio_(-1),focused_radio_(0),focused_radio_prev_(0),
           tx_return_focus_radio_(0),tx_return_focus_valid_(false),so2r_pair_a_(0),so2r_pair_b_(1),
	   timeout_monitor(0),queue_monitor_status(0),debug(0),
	   radio_mode(0)

  {


  }

  bool debug;  
  int radio_mode ; // radio_mode formerly in decl.h plogw

  void set_timeout_monitor(int interval)
  {
    timeout_monitor=millis()+interval;
  }

  void set_queue_monitor_status(int value)
  {
    queue_monitor_status=value;
    // Start queue polling with a finite delay.  Without this, timeout_monitor
    // may remain zero and task() can spin continuously while playback is active.
    if (value) {
      set_timeout_monitor(500);
    } else {
      timeout_monitor=0;
    }
  }

  int query_queue_monitor_status()
  {
    return queue_monitor_status;
  }
  

  void play_string_macro(char *exp_src) // play string written in exp_src
  {
    char exp_buf[100];
    char exp_buf1[100];
    char *s1, *s;
    expand_macro_string(exp_buf,sizeof(exp_buf),exp_src);
    s1=expand_macro_string(exp_buf1,sizeof(exp_buf),exp_buf);  // expand macro twice
    s=s1;
    int flag=0;
    while (*s) {
      if (*s=='_') {
	flag=1;
	s++;
	continue;
      }
      if (*s=='?') *s='/';
      if (*s=='\"') *s='/';
      if (!flag) {
	*s=tolower(*s);
      }
      flag=0;	      
      s++;
    }
    if (verbose&4) console->println(s1);
    play_string_cmd(s1);
  }
  
  int send_exch(struct radio *radio) 
  {
    if (verbose&4)     console->print("so2r send_exch()");
    cancel_msg_tx();
    set_msg_tx_to_focused();
    set_tx_to_msg_tx();
    set_rx_in_sending_msg(); // this will change focused radio so need to keep dealing with msg_tx_radio
    //    if (&radio_list[0]==radio) msg_tx_radio(0);
    //    else if (&radio_list[1]==radio) msg_tx_radio(1);
    //    else if (&radio_list[2]==radio) msg_tx_radio(2);        
    if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying) ) { 
      // send exchange
      strcpy(radio->callsign_previously_sent, radio->callsign + 2);
      append_cwbuf_string(plogw->cw_msg[1] + 2);  // F2 exchange
      append_cwbuf('$');  // control command
    } else if (radio->modetype== LOG_MODETYPE_PH) {
      if (plogw->voice_memory_enable>=2) {
	send_voice_memory(radio, 2);  // F2 exchange
      }
    } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
      // send call and exchange , then move to my exch entry
      set_rttymemory_string(radio, 2, plogw->rtty_msg[1] + 2);  // set rtty memory on rig
      // and send it on the air
      //                          delay(200);
      //                          send_rtty_memory(radio, 5);
    }
    sequence_stat(Sending_Msg);
    return 1;
  }

  void send_call_exch() {
    struct radio *radio;
    radio=radio_selected();
    if (verbose&4) {
      console->print("send_call_exch() radio_selected=");
      console->println(radio->rig_idx);
    }
    // callsign check
    if (strlen(radio->callsign + 2) > 0) {
      // if anything written as callsign
      if (radio->cq[radio->modetype]==LOG_CQ) {
	// repeat function off
	
	if ((radio->modetype==LOG_MODETYPE_CW) || ((radio->f_tone_keying)) ) {
	  cancel_msg_tx();
	  set_msg_tx_to_focused();
	  set_tx_to_msg_tx();
	  set_rx_in_sending_msg(); // this will change focused radio so need to keep dealing with msg_tx_radio
	  
	  // send call and exchange , then move to my exch entry
	  strcpy(radio->callsign_previously_sent, radio->callsign + 2);
	  append_cwbuf_string(plogw->cw_msg[4] + 2);  // F5 msg send
	  append_cwbuf('$');
	  sequence_stat(Sending_Msg);	  	  
	} else if (radio->modetype== LOG_MODETYPE_PH) {
	  if (plogw->voice_memory_enable==3) {
	    cancel_msg_tx();
	    set_msg_tx_to_focused();
	    set_tx_to_msg_tx();
	    set_rx_in_sending_msg();
	    strcpy(radio->callsign_previously_sent, radio->callsign + 2);
	    play_string_macro(plogw->cw_msg[4]+2);
	    sequence_stat(Sending_Msg);
	  }
	} else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
	  cancel_msg_tx();
	  set_msg_tx_to_focused();
	  set_tx_to_msg_tx();
	  set_rx_in_sending_msg(); // this will change focused radio so need to keep dealing with msg_tx_radio
	  // send call and exchange , then move to my exch entry
	  strcpy(radio->callsign_previously_sent, radio->callsign + 2);
	  set_rttymemory_string(radio, 5, plogw->rtty_msg[4] + 2);  // set rtty memory on rig
	  sequence_stat(Sending_Msg);
	}
	// s-meter statistics
	radio->smeter_stat = 1;     // start getting peak S meter value
	radio->smeter_peak = SMETER_MINIMUM_DBM;  // very small value
      }
      qso_stat(SendCallExch);
      // CQ response must remain immediate.  DUPE/partial/CALLHIST results
      // arrive asynchronously and update the display when ready.
      request_async_dupe_partial(radio, true);

      radio->ptr_curr = 1;
      upd_display();    

      // in dx contest, may show dx entity information here
      check_call_show_dx_entity_info(radio);
    }

  }

  void show_status_lcd() {
    // show so2r related status on LCD if
    if (debug) {
      char buf[100];
      *dp->lcdbuf = '\0';
      switch (radio_mode) {
      case RADIO_MODE_SO1R:
	strcpy(buf,"SO1R");break;
      case RADIO_MODE_SO2R:
	strcpy(buf,"SO2R");break;
      case RADIO_MODE_SAT:
	strcpy(buf,"SAT");break;
      }
      switch (sequence_mode()) {
      case Manual:
	strcat(buf," Manual");break;
      case Repeat_Func:
	strcat(buf," Rep");break;
      case SO2R_Repeat_Func:
	strcat(buf," 2R_Rep");break;	
      case SO2R_CQSandP:
	strcat(buf," 2R_CQSP");break;		
      case SO2R_ALTCQ:
	strcat(buf," 2R_ALT");break;			
      case SO2R_2BSIQ:
	strcat(buf," 2R_2BS");break;
      }
      switch (sequence_stat()) {
      case Default:
	strcat(buf," Default\n");break;
      case Sending_Msg:
	strcat(buf," Sending_Msg\n");break;	
      case Sending_Msg_Finished:
	strcat(buf," Sending_Msg_Fin\n");break;		
      case Timer_Started:
	strcat(buf," Timer_Started\n");break;		
      case Timer_Expired:
	strcat(buf," Timer_Expired\n");break;			
      case Timer2_Started:
	strcat(buf," Timer2_Started\n");break;				
      case Timer2_Expired:
	strcat(buf," Timer2_Expired\n");break;				
      }
      strcat(buf,"QSO:");      
      for (int i=0;i<3;i++) {
	switch (qso_stat(i)) {
	case SendCQ:
	  strcat(buf," SendCQ");break;
	case StandBy:
	  strcat(buf," Stby");break;	  
	case SendCall:
	  strcat(buf," SCall");break;	  	  
	case SendCallExch:
	  strcat(buf," SCallExch");break;	  	  	  
	case SendExch:
	  strcat(buf," SExch");break;	  	  	  	  
	case SendTU:
	  strcat(buf," STU");break;
	}
      }
      sprintf(dp->lcdbuf,"%s\nt:%d r:%d seq_chg:%d\nm_tx_r:%d q_proc_r:%d\nchg_t:%d _r:%d\nfoc:%d f_prv:%d\n",
	      buf,
	      tx_,rx_,
	      sequence_stat_changed_,
	      msg_tx_radio_,
	      qso_process_radio_,
	      f_chgstat_tx_,
	      f_chgstat_rx_,
	      focused_radio_,focused_radio_prev_
	      );
      upd_display_info_flash(dp->lcdbuf);
    }
  }

  void send_tu() {
    struct radio *radio;
    radio=radio_selected();
    if (radio->cq[radio->modetype] && (!radio->qsodata_loaded)) {  // TU sent only in CQ and qsodata is not loaded from memory card.
      // send TU (F3)
      switch (radio->modetype) {
      case LOG_MODETYPE_CW:
	cancel_msg_tx();
	set_msg_tx_to_focused();
	set_tx_to_msg_tx();
	set_rx_in_sending_msg();
	
	// in CW
	// check previously sent call
	if (strcmp(radio->callsign_previously_sent, radio->callsign + 2) != 0) {
	  append_cwbuf_string(radio->callsign + 2);
	  append_cwbuf_string(" ");
	  radio->callsign_previously_sent[0] = '\0';
	}
	append_cwbuf_string(plogw->cw_msg[2] + 2);
	append_cwbuf('$');
	sequence_stat(Sending_Msg);
	break;
	// phone
      case LOG_MODETYPE_PH:
	if (plogw->voice_memory_enable>=2) {
	  cancel_msg_tx();
	  set_msg_tx_to_focused();
	  set_tx_to_msg_tx();
	  set_rx_in_sending_msg();
	  if (plogw->voice_memory_enable==3) {
	    // play with voice generation
	    char exp_src[100];
	    exp_src[0]='\0';
	    if (strcmp(radio->callsign_previously_sent, radio->callsign + 2) != 0) {
	      strcat(exp_src,radio->callsign+2);
	      strcat(exp_src," ");
	      radio->callsign_previously_sent[0] = '\0';
	    }
	    strcat(exp_src,plogw->cw_msg[2]+2); // TU macro
	    play_string_macro(exp_src);
	  } else {
	    send_voice_memory(radio, 3);  // F3 voice memory send
	  }
	  sequence_stat(Sending_Msg);	    
	}
	break;
      case LOG_MODETYPE_DG:
	cancel_msg_tx();
	set_msg_tx_to_focused();
	set_tx_to_msg_tx();
	set_rx_in_sending_msg();
	
	set_rttymemory_string(radio, 3, plogw->rtty_msg[2] + 2);  // set/queue RTTY message
	// Local Baudot/FSK for every rig; CW:0/1/2 select GPIO and
	// CW:3/4 select DTR/RTS through the common keying path.
	sequence_stat(Sending_Msg);	  
	break;
      }
    }

  }
  
  void send_cq() {
    if (verbose&4)     console->print("so2r send_cq()");
    cancel_msg_tx(); // overwrite sending msg

    struct radio *radio; // new
    radio= radio_msg_tx();
    set_tx_to_msg_tx();
    radio->cq[radio->modetype]=LOG_CQ;
    
    repeat_cq_radio_= msg_tx_radio(); // memorize
    if ((radio->modetype==LOG_MODETYPE_CW) || (radio->f_tone_keying) ) { 
      // send CQ
      set_rx_in_sending_msg(); // this will change focused radio so need to keep dealing with msg_tx_radio      
      if (verbose&4)       console->println("so2r cw sending cq");
      strcpy(radio->callsign_previously_sent, radio->callsign + 2);/// ????
      append_cwbuf_string(plogw->cw_msg[0] + 2);  // F1 msg send
      append_cwbuf('$');  // control command
      sequence_stat(Sending_Msg);            
    } else if (radio->modetype== LOG_MODETYPE_PH) {
      if (verbose&4)       console->println("voice ");      
      if (plogw->voice_memory_enable>=1) {
	set_rx_in_sending_msg(); // this will change focused radio so need to keep dealing with msg_tx_radio
	strcpy(radio->callsign_previously_sent, radio->callsign + 2);	// ??? needed here?
	if (plogw->voice_memory_enable==3) {
	  play_string_macro(plogw->cw_msg[0] + 2); 
	} else {
	  send_voice_memory(radio, 1);  // F1 CQ
	}
	sequence_stat(Sending_Msg);
      }
      // nothing
    } else if (radio->modetype== LOG_MODETYPE_DG) {  // RTTY
      // send call and exchange , then move to my exch entry
      set_rttymemory_string(radio, 1, plogw->rtty_msg[0] + 2);  // set rtty memory on rig
      // and send it on the air
      //                          delay(200);
      //                          send_rtty_memory(radio, 5);
      sequence_stat(Sending_Msg);      
    }
    // status update
  }
  
  void send_msg(char *buf) {
    struct radio *radio;
    radio= radio_msg_tx();
    cancel_msg_tx(); // overwrite sending msg
    if ((radio->modetype==LOG_MODETYPE_CW) ||  (radio->f_tone_keying) ) {     
      append_cwbuf_string(buf);
      append_cwbuf('$');
      sequence_stat(Sending_Msg);
    } else {
      if (plogw->voice_memory_enable==3) {
	play_string_macro(buf);
	sequence_stat(Sending_Msg);	
      }
    }
    // PH and RTTY need special implementation to send arbitrary messages 
  }

  // send function key message
  //  void  function_keys(int f_num)  {

  //  }

  void task() {
    // monitoring SO2R status and control
    // switch RX
    if (f_chgstat_rx_ != 0) {
      show_status_lcd();      
      focused_radio_prev_ = focused_radio_;
      focused_radio_ = f_chgstat_rx_ - 1; 
      set_rx(f_chgstat_rx_ - 1);
      f_chgstat_rx_ = 0;
      show_status_lcd();
    }
    // switch TX
    if (f_chgstat_tx_ != 0) {
      show_status_lcd();      
      set_tx(f_chgstat_tx_ - 1);
      f_chgstat_tx_ = 0;
      show_status_lcd();
    }
    
    // watch f_sequence_stat_ changes and control sequence jobs
    if (sequence_stat_changed_) {
      show_status_lcd();      
      sequence_stat_changed_= 0;
      
      switch (sequence_stat_) {
      case Default:
      case Sending_Msg:
	break;
      case Sending_Msg_Finished:
	sending_msg_finished();
	break;
      case Timer_Started:
      case Timer2_Started:	
	break;
      case Timer_Expired:
      case Timer2_Expired:
	onTimerExpiration();
	break;
      }
      show_status_lcd();      
    }

    // if voice / tone key from sub cpu, check queue and print result (in mux handler)

    if (timeout_monitor < millis()) {
      switch(queue_monitor_status) {
      case 0:  // nothing to do
	break;
      case 1: // monitor sub CPU playback queue
	console->print("timeout_monitor=");console->println(timeout_monitor);      
	console->print("queue_monitor_status="); console->print(queue_monitor_status);
	console->print(" sequence_stat="); console->println(sequence_stat());
	// queue_monitor_status itself means that voice/tone playback is active.
	// Terminal playcw/play commands do not necessarily enter Sending_Msg, so
	// do not gate playq polling on sequence_stat().
	play_queue_cmd();
	set_timeout_monitor(500);
	break;
      }
      show_status_lcd();      
    }
  }

  void set_status()
  {
    sequence_stat_changed_=1;
    f_chgstat_tx_=1;
    f_chgstat_rx_=1;
    task();
  }
  

  void request_set_tx(int tx) {
    f_chgstat_tx_ = tx+1;
  }

  // Manual keying is an explicit operator override.  A TX-change request
  // queued by the SO2R sequence must not immediately undo that override on
  // the next task() pass.
  void cancel_pending_tx_change() {
    f_chgstat_tx_ = 0;
  }

  void request_set_rx(int rx) {
    f_chgstat_rx_ = rx+1;
  }



  void remember_focus_before_message_tx() {
    tx_return_focus_radio_ = focused_radio();
    tx_return_focus_valid_ = true;
    if (verbose & VERBOSE_SEQUENCE) {
      console->printf("SO2R TX context save focus=%d tx=%d rx=%d\n",
                      tx_return_focus_radio_, tx_, rx_);
    }
  }

  void clear_message_tx_context() {
    tx_return_focus_valid_ = false;
  }

  void request_restore_focus_after_message(int fallback_radio) {
    int target = fallback_radio;
    if (tx_return_focus_valid_ &&
        tx_return_focus_radio_ >= 0 && tx_return_focus_radio_ <= 2) {
      target = tx_return_focus_radio_;
    }
    if (verbose & VERBOSE_SEQUENCE) {
      console->printf("SO2R TX context restore focus=%d saved_valid=%d msg_tx=%d current_focus=%d\n",
                      target, tx_return_focus_valid_ ? 1 : 0,
                      msg_tx_radio(), focused_radio());
    }
    tx_return_focus_valid_ = false;
    request_set_rx(target);
  }

  void set_msg_tx_to_focused() {
    // Capture the operator-selected radio before set_rx_in_sending_msg()
    // temporarily changes focused_radio_.
    remember_focus_before_message_tx();
    msg_tx_radio(focused_radio());
  }

  // Start a Web-originated message on an explicitly selected radio without
  // changing the operator focus first.  This preserves the true return focus
  // and prevents a queued Web Radio command or SO2R RX switching from changing
  // the physical TX radio while the message is being sent.
  void set_msg_tx_to_radio(int radio) {
    if (radio < 0 || radio >= N_RADIO) return;
    remember_focus_before_message_tx();
    msg_tx_radio(radio);
  }

  void set_tx_to_msg_tx() {
    set_tx(msg_tx_radio());
  }

  void set_rx_to_msg_tx() {
    set_rx(msg_tx_radio());
  }


  void set_default_sequence_mode() {
    switch(radio_mode) {
    case SO2R::RADIO_MODE_SO1R:
      if (sequence_mode()==SO2R::SO2R_Repeat_Func) {
	sequence_mode(SO2R::Repeat_Func);
      } else if (sequence_mode()==SO2R::SO2R_CQSandP ||
		 sequence_mode()==SO2R::SO2R_ALTCQ ||
		 sequence_mode()==SO2R::SO2R_2BSIQ) {
	sequence_mode(SO2R::Manual);	
      }
      break;
    case SO2R::RADIO_MODE_SAT:      
      if (sequence_mode()==SO2R::SO2R_Repeat_Func) {
	sequence_mode(SO2R::Repeat_Func);
      } else if (sequence_mode()==SO2R::SO2R_CQSandP ||
		 sequence_mode()==SO2R::SO2R_ALTCQ ||
		 sequence_mode()==SO2R::SO2R_2BSIQ) {
	sequence_mode(SO2R::Manual);	
      }
      break;
    case SO2R::RADIO_MODE_SO2R:
      if (sequence_mode()==SO2R::Repeat_Func) {
	sequence_mode(SO2R::SO2R_Repeat_Func);
      } else if (sequence_mode()==SO2R::Manual) {
	sequence_mode(SO2R::SO2R_CQSandP);
      }
      break;
    }
  }

  int so2r_pair_a() const { return so2r_pair_a_; }
  int so2r_pair_b() const { return so2r_pair_b_; }

  int *so2r_pair_a_setting_ptr() { return &so2r_pair_a_; }
  int *so2r_pair_b_setting_ptr() { return &so2r_pair_b_; }

  bool so2r_pair_contains(int radio) const {
    return radio == so2r_pair_a_ || radio == so2r_pair_b_;
  }

  int so2r_pair_radio(int tx_radio) const {
    if (tx_radio == so2r_pair_a_) return so2r_pair_b_;
    if (tx_radio == so2r_pair_b_) return so2r_pair_a_;
    return -1;
  }

  int next_enabled_partner(int tx_radio, int direction) const {
    if (tx_radio < 0 || tx_radio > 2) return -1;
    const int step = direction < 0 ? -1 : 1;
    for (int n = 1; n <= 3; ++n) {
      int candidate = (tx_radio + step * n) % 3;
      if (candidate < 0) candidate += 3;
      if (candidate != tx_radio && radio_list[candidate].enabled) return candidate;
    }
    return -1;
  }

  bool set_so2r_pair(int radio_a, int radio_b) {
    if (radio_a < 0 || radio_a > 2 || radio_b < 0 || radio_b > 2 ||
        radio_a == radio_b || !radio_list[radio_a].enabled ||
        !radio_list[radio_b].enabled) {
      return false;
    }
    so2r_pair_a_ = radio_a;
    so2r_pair_b_ = radio_b;
    return true;
  }

  bool select_so2r_pair_for_radio(int radio, int direction) {
    const int partner = next_enabled_partner(radio, direction);
    return partner >= 0 && set_so2r_pair(radio, partner);
  }

  void validate_so2r_pairs() {
    if (so2r_pair_a_ >= 0 && so2r_pair_a_ <= 2 &&
        so2r_pair_b_ >= 0 && so2r_pair_b_ <= 2 &&
        so2r_pair_a_ != so2r_pair_b_ &&
        radio_list[so2r_pair_a_].enabled && radio_list[so2r_pair_b_].enabled) {
      return;
    }

    int first = -1;
    int second = -1;
    for (int i = 0; i < 3; ++i) {
      if (!radio_list[i].enabled) continue;
      if (first < 0) first = i;
      else { second = i; break; }
    }
    if (first >= 0 && second >= 0) {
      so2r_pair_a_ = first;
      so2r_pair_b_ = second;
    }
  }

  void set_rx_in_sending_msg() { // choose rx and set
    // In SO2R, alternate only inside the one persistent symmetric pair.
    if (radio_mode==RADIO_MODE_SO2R) {
      switch(sequence_mode()) {
      case SO2R::Manual:
      case SO2R::Repeat_Func:
	break;
      case SO2R::SO2R_Repeat_Func:
      case SO2R::SO2R_CQSandP:
      case SO2R::SO2R_ALTCQ:
      case SO2R::SO2R_2BSIQ: {
        validate_so2r_pairs();
        const int partner = so2r_pair_radio(msg_tx_radio());
        if (partner >= 0 && partner <= 2 && partner != msg_tx_radio()) {
          set_rx(partner);
        }
	break;
      }
      }
    }
  }
  
  void sending_msg_finished()
  {
    if (verbose&4)     console->println("sending_msg_finished()");
    
    switch (radio_mode) {
    case RADIO_MODE_SAT:
    case RADIO_MODE_SO1R:
      clear_message_tx_context();
      //      if (sequence_mode_ == SEQUENCE_MODE_REPEAT_FUNC) {
      if (sequence_mode() == Repeat_Func) {
	if (verbose&4) 	console->println("repeat func timer start");
	repeat_timer_start();
      }
      break;
    case RADIO_MODE_SO2R:
      //      set_rx(msg_tx_radio_);
      //      change_focused_radio(msg_tx_radio_);
      switch (sequence_mode()) {
	//      case SEQUENCE_MODE_SO2R_REPEAT_FUNC:
      case Manual:
	if (verbose&4) 	console->println("Manual");
        request_restore_focus_after_message(msg_tx_radio());
	sequence_stat(Default);
	break;
      case Repeat_Func:
        request_restore_focus_after_message(msg_tx_radio());
	repeat_timer_start();
	if (msg_tx_radio() == focused_radio()) {
	  if (radio_selected()->callsign[2] != '\0' || radio_selected()->recv_exch[2] !='\0' ) { // any input exists in the window of message sent -> suspend 
	    suspend();
	    if (verbose&4) 	    console->println("repeat func suspended");	    
	    //	    console->println("so2r repeat_func suspended");
	  } else {
	    //	    console->println("so2r repeat_func timer start");
	    if (verbose&4) 	    console->println("repeat func timer start so2r");
	  }
	} 
	break;
      case SO2R_Repeat_Func:
	if (verbose&4) 	console->println("SO2R_Repeat_Func");
        request_restore_focus_after_message(msg_tx_radio());
	repeat_timer_start();	
	if (msg_tx_radio() == focused_radio()) {
	  if (radio_selected()->callsign[2] != '\0' || radio_selected()->recv_exch[2] !='\0' ) {
	    suspend();
	    if (verbose&4) 	    console->println("so2r repeat_func suspended");
	  } else {
	    if (verbose&4) 	    console->println("so2r repeat_func timer start");
	  }
	} 
	break;
      case SO2R_CQSandP:
	if (verbose&4) 	console->println("so2r cqsandp");
        request_restore_focus_after_message(msg_tx_radio());
	sequence_stat(Default);
	break;
      case SO2R_ALTCQ:
        // ALTCQ intentionally selects the next radio; do not restore the
        // operator focus saved for the completed transmission.
        clear_message_tx_context();
	if (verbose&4) 	console->println("altcq");
	// switch to next radio cq 
	switch (msg_tx_radio()) {
	case 0: request_set_tx(1); request_set_rx(0); msg_tx_radio(1); break;
	case 1: request_set_tx(0); request_set_rx(1); msg_tx_radio(0); break;
	}
	task();
	send_cq();
	break;
      case SO2R_2BSIQ:
        // 2BSIQ also owns the next RX/TX selection explicitly.
        clear_message_tx_context();
	//	sequence_stat(Default);
	if (verbose&4) 	console->println("2bsiq");
	// switch radio
	if (qso_stat_focused()==StandBy) {
	  // wait until qso_stat changes to the others
	  break;
	}
	// switch radios
	switch (msg_tx_radio()) {
	case 0:
	  request_set_tx(1); request_set_rx(0); msg_tx_radio(1);
	  break;
	case 1:
	  request_set_tx(0); request_set_rx(1); msg_tx_radio(0);
	  break;
	}
	task();
	switch (qso_stat_focused()) {
	case SendCQ:
	  send_cq();break;
	case StandBy:
	  break;
	case SendCall:
	  break;
	case SendCallExch:
	  //	  send_exch(radio_selected());break; // 
	case SendExch:
	  send_exch(radio_selected());break; // 	  
	  break;
	case SendTU:
	  //	  send_tu();
	  break;
	}
	break;
	//   0     ------ CQ TX -------   rx sel                      (if ; pressed) --- send call+num --  rx sel
	//   1        rx sel       (if ; pressed) ---- send call+num ----      if (enter pressed)        ------- send tu  ------ rx sel 
      }
    
    }
  }


  // watch transmit status
  void onTx_stat_update()
  {
    struct radio *radio;
    radio = radio_msg_tx();

    if (verbose & VERBOSE_SEQUENCE) {
      if (verbose&4)     console->println("onTx_stat_update()");
      print_sequence_stat();
      // debug repeat function
      plogw->ostream->print("sequence: stat ");
      plogw->ostream->print(sequence_stat_);
      plogw->ostream->print(" tmr ");
      plogw->ostream->print(repeat_timer_);
      plogw->ostream->print(" radio ");
      plogw->ostream->print(msg_tx_radio_);
      plogw->ostream->print(" ptt ");
      plogw->ostream->print(radio->ptt);
      plogw->ostream->print(" ptt_stat_prev ");
      plogw->ostream->print(radio->ptt_stat_prev);
      plogw->ostream->print(" ptt_stat ");
      plogw->ostream->println(radio->ptt_stat);
    }
    
    if (radio->modetype != LOG_MODETYPE_CW) {
      // not CW
      //      console->println("onTx_stat_update");
      if ((sequence_stat() == Sending_Msg) && (radio->ptt_stat_prev >= 1) && (radio->ptt_stat == 0)) { // back to receive

	sequence_stat(Sending_Msg_Finished);
      }
    }

  }


  // repeat timer related processes
  void repeat_timer_start()
  {
    sequence_stat(Timer_Started);
    repeat_timer_ = repeat_timer_set_;
  }

  void repeat_timer2_start()
  {
    sequence_stat(Timer2_Started);
    repeat_timer_ = repeat_timer_set_;
  }

  void timer_expired()
  {
    if (sequence_stat() == Timer_Started) {
      sequence_stat(Timer_Expired);  // request next repeat function command
    } else if (sequence_stat() == Timer2_Started) {
      sequence_stat(Timer2_Expired);  // request next repeat function command
    }
  }
  
  void onTimerExpiration()
  {
    if (verbose&4)     console->print("onTimerExpiration");
    switch (radio_mode) {
    case RADIO_MODE_SAT:
    case RADIO_MODE_SO1R:
      clear_message_tx_context();
      //      if (sequence_mode_ == SEQUENCE_MODE_REPEAT_FUNC) {
      if (sequence_mode() == Repeat_Func) {
	sequence_stat(Sending_Msg);
	set_tx(msg_tx_radio());

	function_keys(msg_key(), 0);
      }
      break;
    case RADIO_MODE_SO2R:
      switch (sequence_mode()) {
	//      case SEQUENCE_MODE_SO2R_REPEAT_FUNC:
      case Manual:
	break;
      case Repeat_Func:
	if (verbose&4) 	console->println("repeat func send message(in so2r mode)???");
	sequence_stat(Sending_Msg);
	set_msg_tx_to_focused();
	set_tx_to_msg_tx();
	set_rx_in_sending_msg();
	function_keys(msg_key(), 0);
	break;
      case SO2R_Repeat_Func:
	if (verbose&4) 	console->println("repeat func so2r send message");
	if (repeat_cq_radio() != -1) {
	  if (repeat_cq_radio()!=msg_tx_radio()) { // last message is not in repeat cq radio
	    if (sequence_stat()==Timer_Expired) { 
	      set_rx_to_msg_tx(); // receive response and timer start
	      repeat_timer2_start();
	      if (verbose&4) 	      console->println("watch response before repeat");
	      break;
	    } else {
	      if (verbose&4)  {	      console->print("repeat cq radio=");
		console->println(repeat_cq_radio_);
	      }
	      msg_tx_radio(repeat_cq_radio_);
	    }
	  } else {
	    if (verbose&4) {	    console->print("repeat cq radio=");
	      console->println(repeat_cq_radio_);
	    }
	    msg_tx_radio(repeat_cq_radio_);
	  }
	} else {
	  if (verbose&4) 	  console->println("set msg_tx_to_focused()");
	  set_msg_tx_to_focused();
	}
	set_rx_in_sending_msg();
	function_keys(msg_key(), 0); // repeat message
	break;
      case SO2R_CQSandP:
      case SO2R_ALTCQ:
      case SO2R_2BSIQ:
	break;
      }
    
    }

  }



  void cancel_repeat_timer()
  {
    repeat_timer_ = 0;
  }

  int repeat_timer()
  {
    return repeat_timer_;
  }


  void repeat_timer_tick()
  {
    // repeat function timer countdown during receive after CQ'ing
    if (repeat_timer_ > 0) {
      repeat_timer_--;
      if (repeat_timer_ == 0) {
	// repeat timer expired
	timer_expired();
      }
    }
  }

  


  
  // msg_key(): repeat function key code number
  void msg_key(int key)  { msg_key_ = key;}

  int msg_key()  {return msg_key_;}

  void cancel_current_tx_message() {
    // cancel currently ongoing messages (clear_cwbuf in cw and stop voice memory in phone)
    struct radio *radio;
    radio=radio_tx();
    cancel_keying(radio);
  }

  void print_sequence_mode()
  {
    if (verbose&4) 	{
      switch (sequence_mode()) {
      case Manual:
	console->println("Manual");    	
	break;
      case Repeat_Func:
	console->println("Repeat Func");    		
	break;
      case SO2R_Repeat_Func:
	console->println("SO2R Repeat Func");    		
	break;
      case SO2R_CQSandP:
	console->println("SO2R CQ SandP");    			
	break;
      case SO2R_ALTCQ:
	console->println("SO2R ALTCQ");
	break;
      case SO2R_2BSIQ:
	console->println("SO2R 2BSIQ");	
	break;
      }
    }
  }
  
  void cancel_msg_tx()
  {
    cancel_current_tx_message(); // tx current tx message cancel
    if (verbose&4) 	    console->println("cancel_msg_tx()");
    print_sequence_mode();
    suspend();
  }
  
  void tx_msg_finished()   { sequence_stat(Sending_Msg_Finished); }
  
  int sequence_stat()    { return sequence_stat_;}

  void print_sequence_stat()
  {
    if (verbose&4) 	{    
      switch (sequence_stat_) {
      case Default:
	console->println("Default");
	break;
      case Sending_Msg:
	console->println("Sending_Msg");                  
	break;
      case Sending_Msg_Finished:
	console->println("Sending_Msg_Finished");            
	break;
      case Timer_Started:
	console->println("Timer_Started");      
	break;
      case Timer2_Started:      
	console->println("Timer2_Started");      
	break;
      case Timer_Expired:
	console->println("Timer_Expired");
	break;
      case Timer2_Expired:            
	console->println("Timer2_Expired");
	break;
      }
    }
  }

  void sequence_stat(SequenceStat new_stat) {
    sequence_stat_=new_stat;
    sequence_stat_changed_=1;
    if (verbose&4) 	    console->print("sequence_stat change,");
    print_sequence_stat();
    
  }
  
  void suspend() { // stop repeating until resumed

    if (verbose&4) 	    console->println("suspend() stop timer and sequence Default");
    repeat_timer_=0;// stop timer 
    sequence_stat(Default);
  }
  // variables 

  enum SequenceMode sequence_mode()   {  return sequence_mode_;}
  enum SequenceMode  sequence_mode(enum SequenceMode mode) {
    sequence_mode_=mode;
    return sequence_mode_;
  }

  enum QsoStat qso_stat(int iradio) {
    return qso_stat_[iradio];
  }
  
  enum QsoStat qso_stat(int iradio,enum QsoStat newstat) {
    qso_stat_[iradio]=newstat;
    return qso_stat_[iradio];
  }

  enum QsoStat qso_stat_focused()
  {
    return qso_stat(focused_radio());
  }

  enum QsoStat qso_stat_focused(enum QsoStat newstat)
  {
    return qso_stat(focused_radio(),newstat);
  }
  
  void change_focused_radio(int new_focus)
  {
  
    if (new_focus < 0 || new_focus > 2 ) return;
    focused_radio_prev_ = focused_radio_;

    focused_radio_ = new_focus;
    //      radio_selected = &radio_list[plogw->focused_radio];
    //  if (plogw->radio_mode == 2) SO2R_setrx(plogw->focused_radio);
    switch(radio_mode) {
    case 0: // SO1R
    case 1: // SAT
      set_rx(focused_radio_);
      set_tx(focused_radio_);
      break;
    case 2: // SO2R
      set_rx(focused_radio_);
      break;
    }

    if (!plogw->f_console_emu) {
      plogw->ostream->print("focused radio:");
      plogw->ostream->println(focused_radio_);
    }
    phone_switch_management();
    upd_display();
    // bandmap_disp.f_update = 1; // this switch band map too often so better not switch
  }

  int repeat_cq_radio()
  {
    return repeat_cq_radio_;
  }
  int repeat_cq_radio(int n)
  {
    repeat_cq_radio_=n;
    return repeat_cq_radio_;
  }

  
  int msg_tx_radio()
  {
    return msg_tx_radio_;
  }

  int msg_tx_radio(int msg)
  {
    //    if (msg_tx_radio_ != msg) {
    //      request_set_tx(msg);
    msg_tx_radio_ = msg;
    //    }
    return msg_tx_radio_;
  }

  int find_radio_idx(struct radio *radio) {
    for (int i=0;i<3;i++) {
      if (&radio_list[i]==radio) {
	// this
	return i;
      }
    }
    return 0; // faulty but return this 
  }

  int qso_process_radio()
  {
    return qso_process_radio_;
  }
  
  int qso_process_radio(struct radio *radio) {
    qso_process_radio_ = find_radio_idx(radio);
    return qso_process_radio_;
  }
  
  int qso_process_radio(int msg)
  {
    qso_process_radio_ = msg;
    return qso_process_radio_;
  }

  struct radio *radio_qso_process() {
    return &radio_list[qso_process_radio_];
  }
    
  
  int focused_radio() {
    return focused_radio_;
  }
  
  struct radio *radio_selected() {
    return &radio_list[focused_radio_];
  }
  
  struct radio *radio_msg_tx() {
    return &radio_list[msg_tx_radio_];
  }

  struct radio *radio_tx() {
    return &radio_list[tx_];
  }

  void set_tx(int tx) {
    if (verbose & VERBOSE_SEQUENCE) {
      console->printf("[TXSET ] old=%d new=%d msg=%d focus=%d seq=%d\n",
                      tx_, tx, msg_tx_radio(), focused_radio(), sequence_stat());
    }
    // need to stop cw keying before changing so2r_tx 25/5/6
    keying(0);
    
    tx_ = tx;
    // MIC switch
    mic_switch(tx_);
    plogw->ostream->print("mic:");
    plogw->ostream->println(tx_);

    upd_display();
  }


  void set_rx(int rx) {
    rx_ = rx;
    if (focused_radio_ != rx) {
      focused_radio_prev_ = focused_radio_;
    }
    focused_radio_ = rx;

    // switch on-board relay to switch RX phone
    phone_switch_management();

    upd_display();      
  }

  /// control peripherals
  void phone_switch_management() {
    // STEREO, selected_radio の状況に応じ、phoneを選択する。
    int left, right;
    left = 0;
    right = 0;
    if (!plogw->f_console_emu) {
      plogw->ostream->print("stereo:");
      plogw->ostream->print(stereo_);
      plogw->ostream->print(" focused_radio:");
      plogw->ostream->print(focused_radio_);
      plogw->ostream->print(" focused_radio_prev:");
      plogw->ostream->print(focused_radio_prev_);
      plogw->ostream->print(" so2r_rx:");
      plogw->ostream->println(rx_);
    }
    switch (stereo_) {
    case 1:// stereo
      left =1; // listen always radio 0 on the left
      if (focused_radio_ == 2) {
	right = 4;
      } else {
	right = 2;
      }
      break;
    case 0:  // not stereo
      switch (focused_radio_) {
      case 0:
	left = 1;
	right = 1;
	break;
      case 1:
	left = 2;
	right = 2;
	break;
      case 2:
	left = 4;
	right = 4;
	break;
      }
      break;
    }
    phone_switch(left, right);
  }
  
  int stereo()
  {
    return stereo_;
  }

  void setstereo(int stereo) {
    stereo_ = stereo;
    switch (radio_mode) {
    case 0:
    case 1:
    case 2:
      phone_switch_management();
      break;
    }
  }
  
  int tx(){
    return tx_;
  }
  int rx() {
    return rx_;    
  }


};

extern SO2R so2r;

#endif
