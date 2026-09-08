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
#include "hardware.h"
#include "cw_keying.h"
#include "Ticker.h"

#include "variables.h"

#include "so2r.h"
#include "cat.h"
#include "main.h"
#include "dac-adc.h"
#include "qso.h"
#include "misc.h"
#include "processes.h"
#include "cmd_interp.h"
#include "usb_host.h"


//////////////////////////////
/// cw keying related routines
// cw sending is dual buffer cw_send_buf and cw_on_off_buf
// where cw_send_buf may be cleared and edited before sent to cw_on_off_buf in send_char

char cw_send_buf[LEN_CW_SEND_BUF];
char cw_send_current = '\0';
int wptr_cw_send_buf, rptr_cw_send_buf;
int cw_send_update = 0;


int f_cw_code_type=0; // 0: english  1: japanese (wabun)
int f_cw_code_shift=0; // f_cw_codetype==1(wabun) :   store previous character

int cw_bkin_state=-1; // break in state used in tone_keying -1 TX OFF non_zero:TX ON timeout counter if zero make TX OFF

//void send_char(byte cw_char, byte omit_letterspace);


#define LEN_CW_ONOFF_BUF 30  // no need more than element in a characters + space
int16_t cw_onoff_buf[LEN_CW_ONOFF_BUF];
int wptr_cw_onoff_buf = 0, rptr_cw_onoff_buf = 0;

//int f_so2r_chgstat_tx = 0;  // nonzero if changing so2r transmit requested
//int f_so2r_chgstat_rx = 0;  // nonzero if changing so2r receive requested
int f_transmission = 0;     // 0 nothing   1 force transmission on active trx 2 force stop transmission on active trx
static volatile uint32_t cw_xit_start_hold_until_ms = 0;
volatile bool f_rtty_usb_lead_pending = false; // start USB RTTY lead only after PTT ON is issued

// Manual keyboard CW must not follow the automatic SO2R sequence TX radio.
// -1 means normal/automatic operation: use so2r.radio_tx().
static volatile int manual_cw_radio = -1;

void set_manual_cw_radio(int radio_idx)
{
  if (radio_idx >= 0 && radio_idx < N_RADIO) {
    manual_cw_radio = radio_idx;
  }
}

void clear_manual_cw_radio()
{
  manual_cw_radio = -1;
}

// Return the radio that the CW keying layer will actually use right now.
// Manual keyboard CW overrides the automatic SO2R TX selection.
int effective_cw_radio()
{
  if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R &&
      manual_cw_radio >= 0 && manual_cw_radio < N_RADIO) {
    return manual_cw_radio;
  }
  return so2r.tx();
}

#define CW_MS_ELEMENT 1200
int cw_spd = 24;  // wpm

void clamp_cw_speed() {
  if (cw_spd < CW_SPEED_MIN_WPM) cw_spd = CW_SPEED_MIN_WPM;
  if (cw_spd > CW_SPEED_MAX_WPM) cw_spd = CW_SPEED_MAX_WPM;
}
int cw_dah_ratio_bunshi = 30;
int cw_duty_ratio = 10; //  cw pulse width  10  pulse:gap = 1:1  15 pulse:gap = 15:5  5 pulse:gap = 5:15   // 未実装 24/7/21
int cw_ratio_bunbo = 10;
int cw_ratio_bunbo2 = 100;
int cw_count_ms = 0;
int rtty_figures = 0;  // rtty FIGS 1/LTRS 0 flag

//void send_baudot(byte ascii, int *figures);

//// tone keying
// use first channel of 16 channels (started from zero)
#define LEDC_CHANNEL_0 0
// use 12 bit precission for LEDC timer
#define LEDC_TIMER_12_BIT 12
// use 5000 Hz as a LEDC base frequency
#define LEDC_BASE_FREQ 600
void set_tone(int note,int on) {
#if JK1DVPLOG_HWVER >=2  
  return;
#else
  if (on) {
    //
    switch(note) {
    case 0: // CW 600Hz
      //      dac_frequency_set(2, 49);      
      dac_cosine(DAC_CHANNEL_1,1);
      //      dac_cosine(DAC_CHANNEL_2,1);
      dac_frequency_set(7, 37);
      dac_output_enable(DAC_CHANNEL_1);
      //      dac_output_enable(DAC_CHANNEL_2);	      
      break;
    case 1: // RTTY 2130Hz
      dac_cosine(DAC_CHANNEL_1,1);
      //      dac_cosine(DAC_CHANNEL_2,1);
      
      dac_frequency_set(2, 49);
      dac_output_enable(DAC_CHANNEL_1);	
      break;
    case 2: // RTTY 2295Hz
      dac_cosine(DAC_CHANNEL_1,1);
      //      dac_cosine(DAC_CHANNEL_2,1);
    
      dac_frequency_set(2, 53);
      dac_output_enable(DAC_CHANNEL_1);	
      break;
    }    
  } else {
    // stop note
    // set note 0 rather than output disable
    //    dac_output_disable(DAC_CHANNEL_1);
    //    dac_output_disable(DAC_CHANNEL_2);
    dac_cosine(DAC_CHANNEL_1,0);
    //    dac_cosine(DAC_CHANNEL_2,0);    
  }
#endif
}

void set_tone_keying(struct radio *radio) {
#if JK1DVPLOG_HWVER >=2  
  return ;
#else
  if (radio->f_tone_keying) {
    dac_scale_set(DAC_CHANNEL_1,0x03);
    //    dac_scale_set(DAC_CHANNEL_2,0x03);     
    dac_cosine(DAC_CHANNEL_1,1);
    //    dac_cosine(DAC_CHANNEL_2,1);
    dac_output_enable(DAC_CHANNEL_1);        
    //    dac_output_enable(DAC_CHANNEL_2);

    //	xTaskCreate(dactask, "dactask", 1024*3, NULL, 10, NULL);    
    
    //    ledcSetup(LEDC_CHANNEL_0, LEDC_BASE_FREQ, LEDC_TIMER_12_BIT);
    //    ledcWrite(LEDC_CHANNEL_0, 0);  // off
    //    ledcAttachPin(LED, LEDC_CHANNEL_0);
    // the following does not seem to work !?
    //      Serial3.end();
    //	  pinMode(27,OUTPUT);
    //	  ledcAttachPin(27,LEDC_CHANNEL_0); // software serial port ?
    console->println("set_tone_keying() enable");
  } else {
    //    ledcDetachPin(LED);
    dac_scale_set(DAC_CHANNEL_1,0x03);    
    dac_cosine(DAC_CHANNEL_1,0);
    dac_output_disable(DAC_CHANNEL_1);
    console->println("set_tone_keying() disable");    
  }
#endif
}


static void keying_port(int port, int on)
{
  switch (port) {
  case 0: digitalWrite(LED, on); break;
  case 1: digitalWrite(CW_KEY1, on); break;
  case 2: digitalWrite(CW_KEY2, on); break;
  case 3: case 4: usb_keying_request((uint8_t)port, on != 0); break;
  }
}

void keying(int on)
// set key in appropriate port / SO2Rmini

{
  struct radio *radio;
  //  switch (plogw->radio_mode) {
  switch (so2r.radio_mode) {  
  case SO2R::RADIO_MODE_SO1R:  // SO1R
  case SO2R::RADIO_MODE_SAT:  // SAT
      radio = so2r.radio_selected();
      switch (radio->rig_spec->cwport) {
        case 0:  // LED
          digitalWrite(LED, on);
          break;
        case 1:  // CW_KEY1
          digitalWrite(CW_KEY1, on);
          break;
        case 2:  // CW_KEY2
          digitalWrite(CW_KEY2, on);
          break;
        case 3:  // USB CDC ACM DTR
        case 4:  // USB CDC ACM RTS
          usb_keying_request(radio->rig_spec->cwport, on);
          break;
      }
      // make sure all other hardware ports are off
      break;
  case SO2R::RADIO_MODE_SO2R:  // SO2R
      // Automatic CW follows the SO2R sequence TX radio.  Manual keyboard
      // CW follows the radio that was focused when the character was entered.
      if (manual_cw_radio >= 0 && manual_cw_radio < N_RADIO) {
        radio = &radio_list[manual_cw_radio];
      } else {
        radio = so2r.radio_tx();
      }
      if (plogw->f_so2rmini) {
        // make sure all hardware ports are off
        digitalWrite(LED, 0);
        digitalWrite(CW_KEY1, 0);
        digitalWrite(CW_KEY2, 0);
      } else {
        switch (radio->rig_spec->cwport) {
          case 0:  // LED
            digitalWrite(LED, on);
            break;
          case 1:  // CW_KEY1
            digitalWrite(CW_KEY1, on);
	    //	    console->print("1");
            break;
          case 2:  // CW_KEY2
            digitalWrite(CW_KEY2, on);
	    //	    console->print("2");	    
            break;
          case 3:  // USB CDC ACM DTR
          case 4:  // USB CDC ACM RTS
            usb_keying_request(radio->rig_spec->cwport, on);
            break;
        }
      }
      break;
  }
}



void interrupt_cw_send() {
  // timer decriment
  if (cw_count_ms > 0) {
    cw_count_ms--;
  }

  // query response timers
  struct radio *radio;
  for (int i = 0; i < N_RADIO; i++) {
    radio = &radio_list[i];
    if (radio->freqchange_timer > 0) {
      radio->freqchange_timer--;
    }
    if (radio->freqchange_program_guard > 0) {
      radio->freqchange_program_guard--;
    }

    if (radio->civ_response_timer > 0) {
      radio->civ_response_timer--;
      if (radio->civ_response_timer == 0) {
        radio->f_civ_response_expected = 0;
      }
    }
  }
  // rotator response timer ?


  if (info_disp.timer > 0) {
    info_disp.timer--;
  }

  //  sequence_manager_repeat_timer_tick()  ;
  so2r.repeat_timer_tick();
  
  // qso interval timer increment
  plogw->qso_interval_timer++;
  if (plogw->qso_interval_timer > 1200000) plogw->qso_interval_timer=1200000;

  // SO2R support は下記の方針で考える。22/7/10
  // cw のon/off instruction にSO2R のreceive/ transmit 切り替えを含める。
  // ms = 2000 + 0:  TX 1 RX 1  1: TX 1 RX 2    2: TX 2 RX 1 3 : TX 2 RX 2
  // のように。
  // これによって、CQ出したらRX をCQでない方に切り替え、終わったらRXをCQ出した方に戻す。などが実装可能。
  // F1押したとき: TX設定　CQ出す。RXは変えない
  // shift+F1: TX設定 RX CQじゃない方。 CQ 出す 終わったらRX CQ だしたものに。
  // 上記から、TXを設定できる必要は必ずしもない。

  //  radio = &radio_list[plogw->so2r_tx];  // set tx radio
  if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R &&
      manual_cw_radio >= 0 && manual_cw_radio < N_RADIO) {
    radio = &radio_list[manual_cw_radio];
  } else {
    radio = so2r.radio_tx();
  }

  // For transient Yaesu TX CLAR, give the queued CAT command a short window
  // to reach the rig before the first CW element.  This does not block loop().
  if (cw_xit_start_hold_until_ms != 0) {
    uint32_t now = millis();
    if ((int32_t)(now - cw_xit_start_hold_until_ms) < 0) return;
    cw_xit_start_hold_until_ms = 0;
  }

  // USB RTTY defers ETX until the last scheduled 45.45-baud cell has
  // actually completed.  Turn PTT off only after loop_usb() reports done.
  if (usb_rtty_take_tx_done()) {
    f_transmission = 2;
  }

  int ms;
  if (cw_count_ms == 0) {
    // load the next instruction if available
    if (rptr_cw_onoff_buf != wptr_cw_onoff_buf) {
      ms = cw_onoff_buf[rptr_cw_onoff_buf];


      // cw bkin process in on f_tone keying
      if (radio->f_tone_keying and cw_bkin_state<0 ) {  // currently not PTT ON

	  if (ms>0 && ms<8000 ) {
	    // key on instruction
	    keying(1); // immediately key on (PTT ON) and pause bkin period (100ms)
	    f_transmission=1; // force transmission
	    ms=-200; // space 200ms
	    cw_bkin_state=200; // count down starts when no keying instruction given
	    // does not advance instruction counter
	  } else {
	    rptr_cw_onoff_buf = (rptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
	  }
	  
      } else {
	rptr_cw_onoff_buf = (rptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      }

      // hook SO2R commands
      switch (ms) {
	//      case CW_MSCMD_SETRX1:  // set RX 1
	//	f_so2r_chgstat_rx = 1;
	//	break;
	//      case CW_MSCMD_SETRX2:  // set RX 2
	//	f_so2r_chgstat_rx = 2;
	//	break;
	//      case CW_MSCMD_SETRX3:  // set RX 3
	//	f_so2r_chgstat_rx = 3;
	//	break;
      case CW_MSCMD_END_OF_MSG:  // end of a message reached
        request_finish_xit_cw_message(radio);
	so2r.tx_msg_finished();
	break;
	// RTTY stx/etx
      case CW_MSCMD_RTTY_STX: {           // STX start transmission in active TX (in SO2R)
	f_transmission = 1;  // request PTT ON in the main loop
        if (rtty_ptt_lead_ms < 0) rtty_ptt_lead_ms = 0;
        if (rtty_ptt_lead_ms > 2000) rtty_ptt_lead_ms = 2000;
        const uint8_t fskport = (uint8_t)rig_fsk_port(radio->rig_spec);
        if (!radio->f_tone_keying && (fskport == 3 || fskport == 4)) {
          // Do not start the MARK lead here.  PTT is only issued later by
          // Control_TX_process() in the main loop.  Starting the lead here
          // can consume the whole lead interval while the main loop is busy,
          // clipping the beginning of the callsign/exchange.
          f_rtty_usb_lead_pending = true;
          // Hold the CW/RTTY producer until Control_TX_process() has issued
          // PTT ON and starts the real lead interval.
          cw_count_ms = 2000;
        } else {
          const bool line_asserted = true ^ rig_rtty_invert(radio->rig_spec);
          keying_port(fskport, line_asserted ? 1 : 0);
          cw_count_ms = rtty_ptt_lead_ms;
        }
	break;
      }
      case CW_MSCMD_RTTY_ETX: {           // ETX end transmission in active TX (in SO2R)
        const uint8_t fskport = (uint8_t)rig_fsk_port(radio->rig_spec);
        if (!radio->f_tone_keying && (fskport == 3 || fskport == 4)) {
          // Keep PTT on until the USB scheduler has completed the final
          // 0.5-stop-bit MARK cell.
          if (!usb_rtty_end_request(fskport)) f_transmission = 2;
        } else {
	  f_transmission = 2;
        }
	break;
      }

	// tone keying PTT
      case CW_MSCMD_TONEKEY_ON: // in tone keying breakin ptt on
	keying(1);
	break;
      case CW_MSCMD_TONEKEY_OFF: // in tone keying breakin ptt off
	keying(0);
	break;
	  
      default:
	if (ms <= 0) {
	  // key off
	  switch (radio->modetype) {
	  case LOG_MODETYPE_PH:
	    break;
	  case LOG_MODETYPE_DG:
	    if (radio->f_tone_keying) {
	      // space
	      //                  ledcChangeFrequency(LEDC_CHANNEL_0, 2295, LEDC_TIMER_12_BIT);
	      //                  ledcWrite(LEDC_CHANNEL_0, 2047);  // 50% duty tone

	      set_tone(2,1);

	    } else {
              const uint8_t fskport = (uint8_t)rig_fsk_port(radio->rig_spec);
              if (fskport == 3 || fskport == 4)
                usb_rtty_symbol_request(fskport, false, (uint32_t)(-ms) * 1000UL);
              else {
                const bool line_asserted = false ^ rig_rtty_invert(radio->rig_spec);
                keying_port(fskport, line_asserted ? 1 : 0);
              }
	    }

	    break;
	  default:	    
	  case LOG_MODETYPE_CW:
	    if (radio->f_tone_keying) {   // phone F2A, tone keying
	      set_tone(0,0);
	    } else {
	      //digitalWrite(LED, 0);
	      keying(0);
	    }
	    break;
	  }
	  cw_count_ms = -ms;
	} else {
	  // key on
	  // Acm.SetControlLineState(1); //
	  //             Acm.SetControlLineState(3); // RTS 1 DTR 1
	  //		キーイングするとUSB接続の受信が出来なくなっているので、RTS DTR ONにしていないとシリアル通信がが働かない等があるのかもしれない。
	  switch (radio->modetype) {
	  case LOG_MODETYPE_PH: 
	    break;
	  case LOG_MODETYPE_DG:
	    if (radio->f_tone_keying) {
	      // mark
	      //                  ledcChangeFrequency(LEDC_CHANNEL_0, 2125, LEDC_TIMER_12_BIT);
	      //ledcWrite(LEDC_CHANNEL_0, 2047);  // 50% duty tone
	      set_tone(1,1);
	    } else {
              const uint8_t fskport = (uint8_t)rig_fsk_port(radio->rig_spec);
              if (fskport == 3 || fskport == 4)
                usb_rtty_symbol_request(fskport, true, (uint32_t)ms * 1000UL);
              else {
                const bool line_asserted = true ^ rig_rtty_invert(radio->rig_spec);
                keying_port(fskport, line_asserted ? 1 : 0);
              }
	    }

	    break;
	  default:
	  case LOG_MODETYPE_CW:
	    if (radio->f_tone_keying) {
	      // phone F2A, tone keying	      
	      set_tone(0,1);
	    } else {
	      //digitalWrite(LED, 1);
	      keying(1);
	    }
	    break;
	  }
	  cw_count_ms = ms;
	}
	break;
      }
    }
  }
  // check cw_send_buf has available input instruction
  if (rptr_cw_onoff_buf == wptr_cw_onoff_buf) {
    if (cw_send_current != '\0') {
      cw_send_current = '\0';
      cw_send_update = 1;
    }
    // if no more dit dah to send
    if (wptr_cw_send_buf != rptr_cw_send_buf) {
      // there is next character to send in the buffer
      //      radio = so2r.radio_selected();
      if (so2r.radio_mode == SO2R::RADIO_MODE_SO2R &&
          manual_cw_radio >= 0 && manual_cw_radio < N_RADIO) {
        radio = &radio_list[manual_cw_radio];
      } else {
        radio = so2r.radio_tx();
      }
      if (radio->modetype == LOG_MODETYPE_DG) {
        // RTTY send
        send_baudot(cw_send_buf[rptr_cw_send_buf], &rtty_figures);
        cw_send_update = 1;
      } else {
        // CW send
        send_char(cw_send_buf[rptr_cw_send_buf], 0);
      }

      rptr_cw_send_buf = (rptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
    } else {
      // no more character to send
      if (radio->f_tone_keying) {
	if (cw_bkin_state > 0) {
	  cw_bkin_state--;
	}
	if (cw_bkin_state==0) { 
	  keying(0); // PTT off now
	  // request ptt off
	  f_transmission=2; // force stop transmission
	  cw_bkin_state=-1; // reset bkin_state
	}
      }
    }
  }
}

Ticker cw_sender;

void init_cw_keying() {
  // init first buffer pointers
  rptr_cw_send_buf = 0;
  wptr_cw_send_buf = 0;

  // init cw on off buffer
  for (int i = 0; i < LEN_CW_ONOFF_BUF; i++) {
    cw_onoff_buf[LEN_CW_ONOFF_BUF] = 0;
  }

  // attach interrupt handler
  cw_sender.attach_ms(1, interrupt_cw_send);
}

int send_the_dits_and_dahs(const char *cw_to_send) {
  //  int p, p1;
  const char *cp;
  cp = cw_to_send;
  char c;
  int ms_elem, ms;
  ms_elem = CW_MS_ELEMENT / cw_spd;
  while (1) {
    c = *cp;
    cp++;

    ms = 0;
    // check SO2R commands and transmission control commands
    switch (c) {
    case '!':  // RX1
      ms = CW_MSCMD_SETRX1;
      break;
    case '@':  // RX2
      ms = CW_MSCMD_SETRX2;
      break;
    case '#':  // RX 3
      ms = CW_MSCMD_SETRX3;
      break;

    case '$':  // end of a message and standby for repeating if configured
      ms = CW_MSCMD_END_OF_MSG;
      break;

      //    case '%': // tone keying PTT on
      //      ms=8020;
      //      break;
      //    case '^': // tone keying PTT off
      //      ms=8021;
      //      break;
	
    default:  // non
      ms = 0;
      break;
    }

    if (ms != 0) {
      //while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      //        ;
      // No wait for buffer space here.
      // cw_onoff_buf is sized to hold more than the maximum number of
      // elements generated by a single character.  Busy-waiting here is
      // unsafe because this function may be called from interrupt_cw_send().
      cw_onoff_buf[wptr_cw_onoff_buf] = ms;
      //    plogw->ostream->println(ms);
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      // return here , not to read the following
      return 0;
    }
    
    // dit and dar, space
    //    plogw->ostream->print("sending:"); plogw->ostream->print(c); plogw->ostream->print("/"); plogw->ostream->print((int)c); plogw->ostream->println("/");
    if (!c) {
      // end of characters
      //      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      //        ;
      cw_onoff_buf[wptr_cw_onoff_buf] = -ms_elem * 2;
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      return 0;
    }

    
    switch (c) {
    case '.':  // dit
      ms = (ms_elem*(cw_duty_ratio))/cw_ratio_bunbo;
      break;
    case '-':  // dah
      ms = (ms_elem * cw_dah_ratio_bunshi*cw_duty_ratio)/cw_ratio_bunbo2;
      break;
    case ' ':  // (unit) space 
      ms = -ms_elem;
      break;
    case '_': // (half) space
      ms = -ms_elem/2;
      break;
    default:  // faulty
      plogw->ostream->println("?????");
      ms = -ms_elem;
      break;
    }
    //
    //    while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
    //      ;
    cw_onoff_buf[wptr_cw_onoff_buf] = ms;
    wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;

    //
    if (c!=' ' && c!='_') {
      //      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      //	;
      cw_onoff_buf[wptr_cw_onoff_buf] = -(ms_elem*(20-cw_duty_ratio))/cw_ratio_bunbo;
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
    }
  }
}

// Planned Baudot-cell diagnostic.  Record exact logical cells generated by
// send_bits() and dump them later from the command interpreter.
struct RttyBaudotDiagCell {
  uint8_t code;
  uint8_t bit_index;  // 0=start, 1..5=D0..D4, 6=stop, 7=half-stop
  uint8_t mark;
  uint16_t duration_ms;
};
static constexpr uint16_t RTTY_BAUDOT_DIAG_LEN = 256;
static RttyBaudotDiagCell rtty_baudot_diag[RTTY_BAUDOT_DIAG_LEN];
static volatile uint16_t rtty_baudot_diag_w = 0;
static volatile uint16_t rtty_baudot_diag_n = 0;

void RTTYbaudotDiagReset() { rtty_baudot_diag_w = 0; rtty_baudot_diag_n = 0; }

static void rtty_baudot_diag_add(uint8_t code, uint8_t bit_index, bool mark, uint16_t duration_ms) {
  const uint16_t w = rtty_baudot_diag_w;
  rtty_baudot_diag[w] = { (uint8_t)(code & 0x1f), bit_index, (uint8_t)(mark ? 1 : 0), duration_ms };
  rtty_baudot_diag_w = (uint16_t)((w + 1) % RTTY_BAUDOT_DIAG_LEN);
  if (rtty_baudot_diag_n < RTTY_BAUDOT_DIAG_LEN) ++rtty_baudot_diag_n;
}

void RTTYbaudotDiagDump(Print *out) {
  if (!out) out = console;
  const uint16_t n = rtty_baudot_diag_n;
  uint16_t idx = (uint16_t)((rtty_baudot_diag_w + RTTY_BAUDOT_DIAG_LEN - n) % RTTY_BAUDOT_DIAG_LEN);
  out->printf("RTTY BAUDOT planned cells=%u (S=SPACE M=MARK)\n", n);
  for (uint16_t i=0; i<n; ++i) {
    const RttyBaudotDiagCell &c = rtty_baudot_diag[idx];
    char bitname[8];
    const char *name = bitname;
    if (c.bit_index == 0) name = "START";
    else if (c.bit_index >= 1 && c.bit_index <= 5) { snprintf(bitname, sizeof(bitname), "D%u", c.bit_index - 1); }
    else if (c.bit_index == 6) name = "STOP";
    else name = "STOP.5";
    out->printf("BAUDOT %u code=%02u bit=%s state=%c duration=%u ms\n",
                i, c.code, name, c.mark ? 'M' : 'S', c.duration_ms);
    idx = (uint16_t)((idx + 1) % RTTY_BAUDOT_DIAG_LEN);
  }
}

// send baudot bits to the key
void send_bits(byte code, int fig, int *figures) {
  // check figures
  if (fig != *figures) {
    if (fig) {
      *figures = 1;
      send_bits(27, 1, figures);  // FIGS
    } else {
      *figures = 0;
      send_bits(31, 0, figures);  // LTRS
    }
  }
  const uint8_t ita2_code = code & 0x1f;
  code = (code << 1) | 0b1000000;  // add start bit (at LSB) and stop bit // start bit is 0(space) stop bit is 1(mark)
  int ms_elem, ms;
  ms_elem = 22;  // single element of 45.45 baud = 22ms

  for (int i = 0; i < 7; i++) {
    // check LSB
    const bool mark = (code & 0x1) != 0;
    if (mark) ms = ms_elem;
    else ms = -ms_elem;
    rtty_baudot_diag_add(ita2_code, (uint8_t)i, mark, (uint16_t)ms_elem);
    code = code >> 1;  // shift code for the next bit
    //    while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
    //      ;
    cw_onoff_buf[wptr_cw_onoff_buf] = ms;
    wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
  }

  //  while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
  //    ;
  rtty_baudot_diag_add(ita2_code, 7, true, (uint16_t)(ms_elem / 2));
  cw_onoff_buf[wptr_cw_onoff_buf] = ms_elem / 2;  // add half bit stop (mark)
  wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
}


void send_baudot(byte ascii, int *figures)
// figures : state of letters 0 and figures 1
{

  switch (ascii) {
    // Diagnostic-only explicit LTRS character.
    case 0x1f:
      *figures = 0;
      send_bits(31, 0, figures);
      break;
    // followings are letters
    case 0x00: send_bits(0, *figures, figures); break;
    case 'E': send_bits(1, 0, figures); break;
    case 0x0a: send_bits(2, *figures, figures); break;
    case 'A': send_bits(3, 0, figures); break;
    case ' ': send_bits(4, *figures, figures); break;
    case 'S': send_bits(5, 0, figures); break;
    case 'I': send_bits(6, 0, figures); break;
    case 'U': send_bits(7, 0, figures); break;
    case 0x0d: send_bits(8, *figures, figures); break;
    case 'D': send_bits(9, 0, figures); break;
    case 'R': send_bits(10, 0, figures); break;
    case 'J': send_bits(11, 0, figures); break;
    case 'N': send_bits(12, 0, figures); break;
    case 'F': send_bits(13, 0, figures); break;
    case 'C': send_bits(14, 0, figures); break;
    case 'K': send_bits(15, 0, figures); break;
    case 'T': send_bits(16, 0, figures); break;
    case 'Z': send_bits(17, 0, figures); break;
    case 'L': send_bits(18, 0, figures); break;
    case 'W': send_bits(19, 0, figures); break;
    case 'H': send_bits(20, 0, figures); break;
    case 'Y': send_bits(21, 0, figures); break;
    case 'P': send_bits(22, 0, figures); break;
    case 'Q': send_bits(23, 0, figures); break;
    case 'O': send_bits(24, 0, figures); break;
    case 'B': send_bits(25, 0, figures); break;
    case 'G': send_bits(26, 0, figures); break;
    //		case 'O': send_bits(27); break;
    case 'M': send_bits(28, 0, figures); break;
    case 'X': send_bits(29, 0, figures); break;
    case 'V': send_bits(30, 0, figures); break;
    //		case 'O': send_bits(31); break;
    // followings are figures
    //	case 0x00: send_bits(0,1,figures);break;
    case '3': send_bits(1, 1, figures); break;
    //	case 0x0d: send_bits(2,1,figures); break;
    case '-': send_bits(3, 1, figures); break;
    //		case ' ': send_bits(4,1,figures); break;
    //		case ' ': send_bits(5,1,figures); break;
    case '8': send_bits(6, 1, figures); break;
    case '7': send_bits(7, 1, figures); break;
    //	case 0x0a: send_bits(8,1,figures); break;
    case '$': send_bits(9, 1, figures); break;
    case '4': send_bits(10, 1, figures); break;
    case '\'': send_bits(11, 1, figures); break;
    case ',': send_bits(12, 1, figures); break;
    case '!': send_bits(13, 1, figures); break;
    case ':': send_bits(14, 1, figures); break;
    case '(': send_bits(15, 1, figures); break;
    case '5': send_bits(16, 1, figures); break;
    case '\"': send_bits(17, 1, figures); break;
    case ')': send_bits(18, 1, figures); break;
    case '2': send_bits(19, 1, figures); break;
    //		case BEL: send_bits(20); break;
    case '6': send_bits(21, 1, figures); break;
    case '0': send_bits(22, 1, figures); break;
    case '1': send_bits(23, 1, figures); break;
    case '9': send_bits(24, 1, figures); break;
    case '?': send_bits(25, 1, figures); break;
    case '&': send_bits(26, 1, figures); break;
    case '.': send_bits(28, 1, figures); break;
    case '/': send_bits(29, 1, figures); break;
    case ';': send_bits(30, 1, figures); break;
    case CW_MSCMD_RTTY_STX_CHR:
      //      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      //        ;
      cw_onoff_buf[wptr_cw_onoff_buf] = CW_MSCMD_RTTY_STX;  // set special transmission control command  to start transmission
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      break;
    case CW_MSCMD_RTTY_ETX_CHR:
      //      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      //        ;
      cw_onoff_buf[wptr_cw_onoff_buf] = CW_MSCMD_RTTY_ETX;  // set special transmission control command  to start transmission
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      break;
      //		 FIGURE SHIFT 27
      //		 LETTER SHIFT 31
      // RTTY 45 45.45 baud = 22 ms /symbol
      // start bit + 5bit code + stop bit(1.5bit long+) (opposite sense)
  }
}



void send_char(byte cw_char, byte omit_letterspace) {
  int finish_eval=0;
  if (f_cw_code_type==1) {
    finish_eval=1;
    // wabun japanese (kunrei-shiki)

    //https://nomulabo.com/k3ng_keyer/
    //    ホレ [DO] で和文符号を開始、ラタ [SN] で和文終了。ラタ は訂正にも使う。
    //    和文中にアルファベットを入れるには下向き括弧 KK と上向き括弧 RR で括る。
    //    拗音と促音は規定なし
    
    switch(cw_char) {
    case ' ':send_the_dits_and_dahs(" "); f_cw_code_shift='\0'; break;      
    case '-':send_the_dits_and_dahs(".--.-"); f_cw_code_shift='\0';break;
    case 'A':
      switch(f_cw_code_shift) {
      case 'K':send_the_dits_and_dahs(".-.."); f_cw_code_shift='\0';break;
      case 'G':send_the_dits_and_dahs(".-.. .."); f_cw_code_shift='\0';break;
	
      case 'S':send_the_dits_and_dahs("-.-.-"); f_cw_code_shift='\0';break;
      case 'Z':send_the_dits_and_dahs("-.-.- .."); f_cw_code_shift='\0';break;
	
      case 'T':send_the_dits_and_dahs("-."); f_cw_code_shift='\0';break;            
      case 'D':send_the_dits_and_dahs("-. .."); f_cw_code_shift='\0';break;

      case 'N':send_the_dits_and_dahs(".-."); f_cw_code_shift='\0';break;            
      case 'H':send_the_dits_and_dahs("-..."); f_cw_code_shift='\0';break;
      case 'B':send_the_dits_and_dahs("-... .."); f_cw_code_shift='\0';break;
      case 'P':send_the_dits_and_dahs("-... ..--."); f_cw_code_shift='\0';break;

      case 'M':send_the_dits_and_dahs("-..-");f_cw_code_shift='\0'; break;            
      case 'Y':send_the_dits_and_dahs(".--"); f_cw_code_shift='\0';break;            
      case 'R':send_the_dits_and_dahs("..."); f_cw_code_shift='\0';break;            
      case 'W':send_the_dits_and_dahs("-.-"); f_cw_code_shift='\0';break;

      default: send_the_dits_and_dahs("--.--"); f_cw_code_shift='\0';break;
      }
      break;
    case 'I':
      switch(f_cw_code_shift) {
      case 'K':send_the_dits_and_dahs("-.-.."); f_cw_code_shift='\0';break;
      case 'G':send_the_dits_and_dahs("-.-.. .."); f_cw_code_shift='\0';break;
      case 'S':send_the_dits_and_dahs("--.-."); f_cw_code_shift='\0';break;
      case 'Z':send_the_dits_and_dahs("--.-. .."); f_cw_code_shift='\0';break;
      case 'T':send_the_dits_and_dahs("..-."); f_cw_code_shift='\0';break;            
      case 'D':send_the_dits_and_dahs("..-. .."); f_cw_code_shift='\0';break;
      case 'N':send_the_dits_and_dahs("-.-."); f_cw_code_shift='\0';break;            
      case 'H':send_the_dits_and_dahs("--..-"); f_cw_code_shift='\0';break;
      case 'B':send_the_dits_and_dahs("--..- .."); f_cw_code_shift='\0';break;	
      case 'P':send_the_dits_and_dahs("--..- ..--."); f_cw_code_shift='\0';break;
      case 'M':send_the_dits_and_dahs("..-.-"); f_cw_code_shift='\0';break;            
      case 'Y':break;            
      case 'R':send_the_dits_and_dahs("--."); f_cw_code_shift='\0';break;            
      case 'W':break;            

      default: send_the_dits_and_dahs(".-"); f_cw_code_shift='\0';break;
      }
      break;
    case 'U':
      switch(f_cw_code_shift) {
      case 'K':send_the_dits_and_dahs("...-"); f_cw_code_shift='\0';break;
      case 'G':send_the_dits_and_dahs("...- .."); f_cw_code_shift='\0';break;		
      case 'S':send_the_dits_and_dahs("---.-"); f_cw_code_shift='\0';break;
      case 'Z':send_the_dits_and_dahs("---.- .."); f_cw_code_shift='\0';break;
      case 'T':send_the_dits_and_dahs(".--."); f_cw_code_shift='\0';break;            
      case 'D':send_the_dits_and_dahs(".--. .."); f_cw_code_shift='\0';break;
      case 'N':send_the_dits_and_dahs("...."); f_cw_code_shift='\0';break;            
      case 'H':send_the_dits_and_dahs("--.."); f_cw_code_shift='\0';break;
      case 'B':send_the_dits_and_dahs("--.. .."); f_cw_code_shift='\0';break;	
      case 'P':send_the_dits_and_dahs("--.. ..--."); f_cw_code_shift='\0';break;
      case 'M':send_the_dits_and_dahs("-"); f_cw_code_shift='\0';break;            
      case 'Y':send_the_dits_and_dahs("-..--"); f_cw_code_shift='\0';break;                        
      case 'R':send_the_dits_and_dahs("-.--."); f_cw_code_shift='\0';break;            
      case 'W':f_cw_code_shift='\0';break;                        

      default: send_the_dits_and_dahs("..-"); f_cw_code_shift='\0';break;
      }
      break;
    case 'E':
      switch(f_cw_code_shift) {
      case 'K':send_the_dits_and_dahs("-.--"); f_cw_code_shift='\0';break;
      case 'G':send_the_dits_and_dahs("-.-- .."); f_cw_code_shift='\0';break;	
      case 'S':send_the_dits_and_dahs(".---."); f_cw_code_shift='\0';break;
      case 'Z':send_the_dits_and_dahs(".---. .."); f_cw_code_shift='\0';break;
      case 'T':send_the_dits_and_dahs(".-.--"); f_cw_code_shift='\0';break;            
      case 'D':send_the_dits_and_dahs(".-.-- .."); f_cw_code_shift='\0';break;
      case 'N':send_the_dits_and_dahs("--.-"); f_cw_code_shift='\0';break;            
      case 'H':send_the_dits_and_dahs("."); f_cw_code_shift='\0';break;
      case 'B':send_the_dits_and_dahs(". .."); f_cw_code_shift='\0';break;
      case 'P':send_the_dits_and_dahs(". ..--."); f_cw_code_shift='\0';break;
      case 'M':send_the_dits_and_dahs("-...-"); f_cw_code_shift='\0';break;            
      case 'Y':f_cw_code_shift='\0';break;
      case 'R':send_the_dits_and_dahs("---"); f_cw_code_shift='\0';break;            
      case 'W':f_cw_code_shift='\0';break;                        

      default: send_the_dits_and_dahs("-.---"); f_cw_code_shift='\0';break;
      }
      break;
    case 'O':
      switch(f_cw_code_shift) {
      case 'K':send_the_dits_and_dahs("----"); f_cw_code_shift='\0';break;
      case 'G':send_the_dits_and_dahs("---- .."); f_cw_code_shift='\0';break;
      case 'S':send_the_dits_and_dahs("---."); f_cw_code_shift='\0';break;
      case 'Z':send_the_dits_and_dahs("---. .."); f_cw_code_shift='\0';break;
      case 'T':send_the_dits_and_dahs("..-..");f_cw_code_shift='\0'; break;            
      case 'D':send_the_dits_and_dahs("..-.. .."); f_cw_code_shift='\0';break;
      case 'N':send_the_dits_and_dahs("..--"); f_cw_code_shift='\0';break;            
      case 'H':send_the_dits_and_dahs("-.."); f_cw_code_shift='\0';break;
      case 'B':send_the_dits_and_dahs("-.. .."); f_cw_code_shift='\0';break;	
      case 'P':send_the_dits_and_dahs("-.. ..--."); f_cw_code_shift='\0';break;
      case 'M':send_the_dits_and_dahs("-..-."); f_cw_code_shift='\0';break;            
      case 'Y':send_the_dits_and_dahs("--"); f_cw_code_shift='\0';break;
      case 'R':send_the_dits_and_dahs(".-.-"); f_cw_code_shift='\0';break;            
      case 'W':send_the_dits_and_dahs(".---"); f_cw_code_shift='\0';break;                                  
      default: send_the_dits_and_dahs(".-..."); f_cw_code_shift='\0';break;
      }
      // NN send  only shifting
      break;
    case 'N':
      switch(f_cw_code_shift) {      
      case 'N':send_the_dits_and_dahs(".-.-."); f_cw_code_shift='\0';break;
      default: f_cw_code_shift=cw_char;break;
      }
      //  only shifting
      break;
    case 'K':f_cw_code_shift=cw_char ;break;
    case 'S':f_cw_code_shift=cw_char ;break;
    case 'T':f_cw_code_shift=cw_char ;break;
    case 'H':f_cw_code_shift=cw_char ;break;
    case 'M':f_cw_code_shift=cw_char ;break;
    case 'Y':f_cw_code_shift=cw_char ;break;
    case 'R':f_cw_code_shift=cw_char ;break;
    case 'W':f_cw_code_shift=cw_char ;break;
    case 'G':f_cw_code_shift=cw_char ;break;      
    case 'D':f_cw_code_shift=cw_char ;break;
    case 'Z':f_cw_code_shift=cw_char ;break;
    case 'B':f_cw_code_shift=cw_char ;break;      
    case 'P':f_cw_code_shift=cw_char ;break;
    case ')':f_cw_code_type=0;f_cw_code_shift='\0'; break;  // go to English
    case '}':send_the_dits_and_dahs("...-.");f_cw_code_type=0;f_cw_code_shift='\0'; break;  // send RATA and go to English
    default: finish_eval=0;break; // evaluate possible english codes
    }
  }
  if (!finish_eval) {
    switch (cw_char) {
    case ' ': send_the_dits_and_dahs(" "); break;
    case 'A': send_the_dits_and_dahs(".-"); break;
    case 'B': send_the_dits_and_dahs("-..."); break;
    case 'C': send_the_dits_and_dahs("-.-."); break;
    case 'D': send_the_dits_and_dahs("-.."); break;
    case 'E': send_the_dits_and_dahs("."); break;
    case 'F': send_the_dits_and_dahs("..-."); break;
    case 'G': send_the_dits_and_dahs("--."); break;
    case 'H': send_the_dits_and_dahs("...."); break;
    case 'I': send_the_dits_and_dahs(".."); break;
    case 'J': send_the_dits_and_dahs(".---"); break;
    case 'K': send_the_dits_and_dahs("-.-"); break;
    case 'L': send_the_dits_and_dahs(".-.."); break;
    case 'M': send_the_dits_and_dahs("--"); break;
    case 'N': send_the_dits_and_dahs("-."); break;
    case 'O': send_the_dits_and_dahs("---"); break;
    case 'P': send_the_dits_and_dahs(".--."); break;
    case 'Q': send_the_dits_and_dahs("--.-"); break;
    case 'R': send_the_dits_and_dahs(".-."); break;
    case 'S': send_the_dits_and_dahs("..."); break;
    case 'T': send_the_dits_and_dahs("-"); break;
    case 'U': send_the_dits_and_dahs("..-"); break;
    case 'V': send_the_dits_and_dahs("...-"); break;
    case 'W': send_the_dits_and_dahs(".--"); break;
    case 'X': send_the_dits_and_dahs("-..-"); break;
    case 'Y': send_the_dits_and_dahs("-.--"); break;
    case 'Z': send_the_dits_and_dahs("--.."); break;

    case '0': send_the_dits_and_dahs("-----"); break;
    case '1': send_the_dits_and_dahs(".----"); break;
    case '2': send_the_dits_and_dahs("..---"); break;
    case '3': send_the_dits_and_dahs("...--"); break;
    case '4': send_the_dits_and_dahs("....-"); break;
    case '5': send_the_dits_and_dahs("....."); break;
    case '6': send_the_dits_and_dahs("-...."); break;
    case '7': send_the_dits_and_dahs("--..."); break;
    case '8': send_the_dits_and_dahs("---.."); break;
    case '9': send_the_dits_and_dahs("----."); break;

    case '=': send_the_dits_and_dahs("-...-"); break;
    case '/': send_the_dits_and_dahs("-..-."); break;
    case '*': send_the_dits_and_dahs("-...-"); break;  // remap 22/11/15
    case '.': send_the_dits_and_dahs("."); break;
    case ',': send_the_dits_and_dahs("--..--"); break;
    case '\'': send_the_dits_and_dahs(".----."); break;  // apostrophe
    case '(': send_the_dits_and_dahs("-.--."); break;
    case ')': send_the_dits_and_dahs("-.--.-"); break;
    case '&': send_the_dits_and_dahs(".-..."); break;

    case '+': send_the_dits_and_dahs(".-.-."); break;
    case '-': send_the_dits_and_dahs("-...-"); break;  // remap 22/11/15
    case '~': send_the_dits_and_dahs("-...-"); break;  // mapped 24/12/2
    case '_': send_the_dits_and_dahs("_"); break;  // remap 22/11/15 // remap to send half space 24/10/15
    case '"':
      send_the_dits_and_dahs("-..-.");
      break;                                            // same as / ... above ?
      //    case '$': send_the_dits_and_dahs("...-..-"); break;
      //    case '{': send_the_dits_and_dahs(".-.-."); break;   // AR  // remap 23/1/2
      //    case '}': send_the_dits_and_dahs("...-.-"); break;  // SK //  remap 23/1/2
    case '<': send_the_dits_and_dahs(".-.-."); break;   // AR  // ignored becase mapped to bandmap change
    case '>': send_the_dits_and_dahs("...-.-"); break;  // SK // ignored becase mapped to bandmap change
    case '?': send_the_dits_and_dahs("..--.."); break;
    case '{': send_the_dits_and_dahs("-..---"); f_cw_code_type = 1; break; // hore: go to japanese
    case '!':
      send_the_dits_and_dahs("! ");
      cw_char = '\0';
      break;  // SO2R special command RX1
    case '@':
      send_the_dits_and_dahs("@ ");
      cw_char = '\0';
      break;  // SO2R special command RX2
    case '#':
      send_the_dits_and_dahs("# ");
      cw_char = '\0';
      break;   // SO2R special command RX3
    case '$':  // end of a message and repeat timer start request
      send_the_dits_and_dahs("$");
      cw_char = '\0';
      break;
      //    case '{': // send keyon(1)      
      //    case '[': // send keyon(1)
      //      send_the_dits_and_dahs("% ");
      //      cw_char = '\0';
      //      break;
      //    case '%': // send keyon(1)
      //      send_the_dits_and_dahs("% ");
      //      cw_char = '\0';
      //      break;
      //    case '}': // send keyoff(1)            
      //    case '^': // send keyoff(1)
      //      send_the_dits_and_dahs("^");
      //      cw_char = '\0';
      //      break;
    default:
      cw_char = '\0';
      break;
    }
  }

  // set to currently sent character
  cw_send_current = cw_char;
  cw_send_update = 1;
}

void set_tx_to_manual_radio(int radio_idx) {
  if (radio_idx < 0 || radio_idx >= N_RADIO) return;

  // Manual CW must override any TX change queued by the automatic SO2R
  // sequence.  Otherwise task() can restore the sequence TX immediately
  // after set_tx(), making manual keying appear to ignore the operator focus.
  so2r.cancel_pending_tx_change();

  if (so2r.tx() != radio_idx) {
    if (!plogw->f_console_emu) {
      plogw->ostream->printf("manual CW: switch TX %d -> %d\n",
                             so2r.tx(), radio_idx);
    }

    clear_cwbuf();

    // Finish the element already being keyed before changing the hardware TX
    // path.  New queued characters were cleared above.
    while ((cw_count_ms != 0) ||
           (rptr_cw_onoff_buf != wptr_cw_onoff_buf)) {
      delay(1);
    }
    delay(100);
    so2r.set_tx(radio_idx);

    // set_tx() is synchronous, but clear again defensively in case another
    // sequence request was posted while the old CW element was finishing.
    so2r.cancel_pending_tx_change();
  }
}

void set_tx_to_focused() {
  set_tx_to_manual_radio(so2r.focused_radio());
}

char append_cwbuf_convchar(char c)
{
  if (!isPrintable(c)) return '\0';
  if (c >= 'a' && c <= 'z') c += 'A' - 'a';
  if (c == '"') c = '/';  // remap to '/'
  return c;
}

static void append_cwbuf_raw(uint8_t c) {
  cw_send_buf[wptr_cw_send_buf] = (char)c;
  wptr_cw_send_buf = (wptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
  cw_send_update = 1;
}

void append_rtty_test1(char ch, int n) {
  // Diagnostic path: bypass append_cwbuf_convchar()/macro expansion so that
  // the non-printable ITA2 LTRS control code (0x1f) reaches send_baudot().
  clear_manual_cw_radio();
  append_cwbuf_raw((uint8_t)CW_MSCMD_RTTY_STX_CHR);
  append_cwbuf_raw(0x1f);
  append_cwbuf_raw(0x1f);
  append_cwbuf_raw(0x1f);
  for (int i = 0; i < n; ++i) append_cwbuf_raw((uint8_t)ch);
  append_cwbuf_raw((uint8_t)CW_MSCMD_RTTY_ETX_CHR);
}

void append_rtty_test_text(const char *s) {
  if (s == NULL) return;
  clear_manual_cw_radio();
  append_cwbuf_raw((uint8_t)CW_MSCMD_RTTY_STX_CHR);
  while (*s) {
    unsigned char c = (unsigned char)*s++;
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    // Keep the diagnostic command intentionally literal and Baudot-safe.
    // CR/LF are accepted by send_baudot(); other control bytes are skipped.
    if (c == '\r' || c == '\n' || (c >= 0x20 && c <= 0x7e))
      append_cwbuf_raw(c);
  }
  append_cwbuf_raw((uint8_t)CW_MSCMD_RTTY_ETX_CHR);
}

void append_cwbuf(char c) {
  c=append_cwbuf_convchar(c);
  if (c=='\0') return ;
  //  if (plogw->f_repeat_func_stat != 1) {
  //    set_tx_to_focused();
  //  }

  //  struct radio *radio;
  //  radio = &radio_list[plogw->so2r_tx];
  //  radio=so2r.radio_tx();
  //  if (radio->f_tone_keying) {
  //    cw_send_buf[wptr_cw_send_buf] = '%'; // PTT on 
  //    wptr_cw_send_buf = (wptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
  //  }
  cw_send_buf[wptr_cw_send_buf] = c;
  wptr_cw_send_buf = (wptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
  cw_send_update = 1;
  if (so2r.radio_mode == 2) {
    // also send the character to SO2Rmini box --> deprecated 
    // send char excluding special SO2R function characters '@' and '!','#'
    //    if (c != '!' && c != '@' && c != '#' && (plogw->radio_mode == 2)) SO2Rsend((uint8_t *)&c, 1);
  }
}

// static variable for return value
char cw_num_ret[40];
char *cw_num_abbreviation(char *s, int level) {
  char *p;
  p = cw_num_ret;
  char c;
  while ((c = *s++)) {
    switch (level) {
      case 3:  // '1' and '9', '0'->'T'
        if (c == '0') c = 'T';
        if (c == '1') c = 'A';
        if (c == '9') c = 'N';
	break;
      case 2:  // '1' and '9', '0'->'O'
        if (c == '1') c = 'A';
        if (c == '0') c = 'O';
        if (c == '9') c = 'N';
	break;
      case 1:  // only for '9'
        if (c == '9') c = 'N';
	break;
      case 0:  // no abbrev
      default:
        break;
    }
    *p++ = c;
  }
  *p = '\0';
  return cw_num_ret;
}

// rtty memory string buffer
char rtty_memory_string[51];  // yaesu max 50 char icom max 70 chars

void set_rttymemory_string(struct radio *radio, int num, char *s)
// set rtty data memory num (1~8)
{
  char c;
  (void)radio;
  (void)num;
  // clear buffer
  *rtty_memory_string = '\0';
  char *sp;
  // set buffer (recursively)
  set_rttymemory_string_buf(s);

  if (!plogw->f_console_emu) {
    plogw->ostream->print("rtty_memory_string:");
    plogw->ostream->println(rtty_memory_string);
  }

  // All radios use the DVPlogger local Baudot/FSK generator.
  // The physical MARK/SPACE output is selected by FSK:0..4.
  // If FSK is omitted, legacy rigs fall back to CW:0..4.
  // '[' and ']' are the existing local RTTY STX/ETX markers.
  append_cwbuf('[');  // start transmission
  sp = rtty_memory_string;
  while (1) {
    c = *sp++;
    if (c == 0x00) break;
    // CR/LF are valid ITA2 control characters, but append_cwbuf()
    // deliberately drops non-printable characters.  Preserve them
    // so $L can reach send_baudot().
    if (c == '\r' || c == '\n')
      append_cwbuf_raw((uint8_t)c);
    else
      append_cwbuf(c);
  }
  append_cwbuf(']');  // end transmission
}

void set_rttymemory_string_buf(char *s) {
  struct radio *radio;
  //  radio = so2r.radio_selected();
  radio=so2r.radio_tx();
  
  int stat = 0;  // 0 normal append 1 $ sequence
  int idx;
  char tmp_buf[10];
  idx = strlen(rtty_memory_string);
  if (idx >= 50) return;
  char c;
  while (*s) {
    c = *s;
    s++;
    if (c == '$') {
      stat = 1;
      continue;
    }
    if (stat == 1) {
      // $ sequence
      switch (c) {
        // macro similar to CTESTWIN
        case 'I':  // mycall insertion
          set_rttymemory_string_buf(plogw->my_callsign + 2);
          break;
        case 'C':  // his callsign
          set_rttymemory_string_buf(radio->callsign + 2);
          break;
        case 'W':  // send my contest exchange (band-dependent token expansion)
          {
            char exch_buf[100];
            expand_sent_exch(exch_buf, sizeof(exch_buf));
            set_rttymemory_string_buf(exch_buf);
          }
          break;
        case 'J':  // jcc nr send
          set_rttymemory_string_buf(plogw->jcc + 2);
          break;
        case 'P':  // band dependent power string
          set_rttymemory_string_buf(power_code(radio->bandid));
          break;
        case 'V':  // send his rst
          set_rttymemory_string_buf(radio->sent_rst + 2);
          break;
      case 'S': // send SEQNR
	sprintf(tmp_buf,"%d",plogw->seqnr);
	set_rttymemory_string_buf(tmp_buf); // without abbreviation
	break;
	  
        case 'R':  // send remarks content
          set_rttymemory_string_buf(radio->remarks + 2);
          break;
        case 'N':  // send my name
          set_rttymemory_string_buf(plogw->my_name + 2);
          break;
        case 'L':  // new line (RTTY): CR + LF
          set_rttymemory_string_buf("\r\n");
          break;
      }
      idx = strlen(rtty_memory_string);

      stat = 0;
    } else {
      // append (leave one byte for the terminating NUL)
      if (idx >= (int)sizeof(rtty_memory_string) - 1) break;
      rtty_memory_string[idx++] = c;
      rtty_memory_string[idx] = '\0';
    }
  }
}


char power_code_return[2];
char *power_code(int bandid) {
  if (bandid == 0) {
    return "M";
  }

  // bandid = 1 ... idx 0
  if (strlen(plogw->power_code + 2) >= bandid) {
    *power_code_return = *(plogw->power_code + 2 + bandid - 1);
  } else {
    *power_code_return = *(plogw->power_code + 2 + strlen(plogw->power_code + 2) - 1);
  }
  power_code_return[1] = '\0';
  if (verbose & 64) {
    plogw->ostream->print("power_code = ");
    plogw->ostream->println(power_code_return);
  }
  return power_code_return;
}

void japanize_number(char *s1, const char *s2)
{
  char c;
  while (*s2) {
    c=*s2++;
    if (isdigit(c)) {
      *s1++='_';
    }
    *s1++=c;
  }
  *s1='\0';
}

char *expand_macro_string(char *p,size_t p_size, const char *s) { // expand macro string s to p
  // p_size available buffer size from p  ( be careful to supply expand_macro_strin(p, p_overall_size - strlen(p), s)) 
  struct radio *radio;
  //  radio = so2r.radio_selected(); // this can be
  radio = so2r.radio_msg_tx();
  if (verbose&4) 	{
    console->print("expand_macro_string:");
    console->print(s);
    console->print(" radio=");
    console->println(radio->rig_idx);
  }
  int stat = 0;  // 0 normal append 1 $ sequence
  char c;
  size_t len=0;
  char *s1;

  *p='\0';

  while (*s) {
    c = *s;
    s++;
    if (c == '$') {
      stat = 1;
      continue;
    }
    if (stat == 1) {
      const char *expansion="";
      char tmp_buf[100];  tmp_buf[0]='\0';
      char tmp_buf1[100]; const char *s2;
      // $ sequence
      switch (c) {
        // macro similar to CTESTWIN
      case 'I':  // mycall insertion
	expansion=plogw->my_callsign + 2;
	break;
      case 'C':  // his callsign
	expansion=radio->callsign + 2;
	break;
      case 'U': // CQ in CW and CQcontest (_C) in Phone
	if (radio->modetype==LOG_MODETYPE_CW ||radio->modetype==LOG_MODETYPE_DG) {
	  expansion="CQ";
	} else {
	  expansion="_C";
	}
	break;
      case 'T': // TEST in CW and _c(contest) in Phone
	if (radio->modetype==LOG_MODETYPE_CW ||radio->modetype==LOG_MODETYPE_DG) {
	  expansion="TEST";
	} else {
	  expansion="_c";
	}
	break;
      case 'G':
	// memo
	// _D (douzo)
	// _C (CQcontest)
	// _A (arigatougozaimashita)
	// _M (mouitidoonegaishimasu)
	// _H (hokairassyaimasuka)
	// _c (contest)
      case 'A': // TU in CW and _A(arigatou)
	if (radio->modetype==LOG_MODETYPE_CW ||radio->modetype==LOG_MODETYPE_DG) {
	  expansion="TU";
	} else {
	  expansion="_A";
	}
	break;
      case 'W':  // send his exchange
	expand_sent_exch(tmp_buf, sizeof(tmp_buf));
	if (radio->modetype==LOG_MODETYPE_CW ||radio->modetype==LOG_MODETYPE_DG) {
	  expansion= cw_num_abbreviation(tmp_buf,plogw->sent_exch_abbreviation_level);
	} else {
	  // PHONE
	  expansion=cw_num_abbreviation(tmp_buf,0);
	  // number to japanese number (_number) conversion
	  japanize_number(tmp_buf1,expansion); // tmp_buf
	  expansion=tmp_buf1;
	  //	  strcat(p,); // We cound define a new abbrev level to convert 5911 -> '5'+0x40, '9'+0x40 ... to read in Japanese 1: Hito 2: Futa 3: San .... 0:Maru (if follows after a number) Zero not following or following 0
	}
	break;
      case 'P':  // send power code (band dependent)
	expansion= power_code(radio->bandid);
	break;
      case 'J':  // jcc nr send
	expansion= plogw->jcc + 2;
	break;
      case 'V':  // send his rst
	if (radio->modetype==LOG_MODETYPE_CW||radio->f_tone_keying) {
	  expansion= cw_num_abbreviation(radio->sent_rst + 2, 1);// 5NN 
	} else {
	  expansion= cw_num_abbreviation(radio->sent_rst + 2, 0);// no abbrev
	  japanize_number(tmp_buf1,expansion); // tmp_buf
	  expansion=tmp_buf1;
	}
	break;
      case 'S': // send SEQNR
	sprintf(tmp_buf,"%d",plogw->seqnr);
	expansion= cw_num_abbreviation(tmp_buf, 0);
	break;
      case 'Q': // send sequential_number for the band
	sprintf(tmp_buf,"%03d",plogw->seqnr_band[radio->bandid-1]+1);
	expansion=cw_num_abbreviation(tmp_buf, 0);
	break;
      case 'N':  // send my name
	expansion=plogw->my_name + 2;
	break;
      default:
	expansion=""; break;
      }
      // append expansion
      size_t add_len=strlen(expansion);
      if (len + add_len >= p_size - 1) break; // 防御
      strcat(p, expansion);
      len += add_len;
      stat = 0;
    } else {
      if (len < p_size - 1) {
        p[len++] = c;
        p[len] = '\0';
      } else {
        break; // バッファ上限
      }
      //      tmp_buf[0]=c;tmp_buf[1]='\0';
      //      strcat(p,tmp_buf);
    }
  }
  return p;
}



void append_cwbuf_string(const char *s) {
  // This is the automatic/message path (F-key/ESM/CQ).  Ensure a previous
  // manual keyboard-CW target cannot leak into the automatic SO2R sequence.
  clear_manual_cw_radio();

  if (verbose & VERBOSE_SEQUENCE) {
    console->printf("[CWSTART] tx=%d msg=%d focus=%d seq=%d text=\"%.32s\"\n",
                    so2r.tx(), so2r.msg_tx_radio(), so2r.focused_radio(),
                    so2r.sequence_stat(), s ? s : "");
  }
  //  struct radio *radio;
  //  radio = radio_selected;
  //  radio = &radio_list[plogw->so2r_tx];
  //  radio=so2r.radio_tx();
  //  char *s1;
  char *s1;
#define EXP_BUF_SIZE 100
  char exp_buf[EXP_BUF_SIZE];exp_buf[0]='\0';
  char exp_buf1[EXP_BUF_SIZE];exp_buf1[0]='\0';
  expand_macro_string(exp_buf,sizeof(exp_buf),s);
  s1=expand_macro_string(exp_buf1,sizeof(exp_buf1),exp_buf);  // expand macro twice

  struct radio *radio;
  radio=so2r.radio_tx();
  if (prepare_xit_for_cw_message(radio)) {
    // USB CAT is queued, so let loop() service it before the ticker keys CW.
    cw_xit_start_hold_until_ms = millis() + 30;
  }
  // check f_tone  
  if (radio->f_tone_keying ) {
    // F2A
    play_wpm_set();
    play_cw_cmd(s1);
  } else  {
    char c;  
    while (*s1) {
      c = *s1;
      s1++;
      append_cwbuf(c);
    }
  }
}
  

int cw_wptr_cw_send_buf_previous() {
  int p;
  p = wptr_cw_send_buf - 1;
  if (p < 0) p = LEN_CW_SEND_BUF - 1;
  return p;
}

void delete_cwbuf() {
  // delete a character in the cwbuf
  if (wptr_cw_send_buf != rptr_cw_send_buf) {
    wptr_cw_send_buf = cw_wptr_cw_send_buf_previous();
  }
  cw_send_update = 1;
  //  char c;
  //  c = 0x08;
  //  if (plogw->radio_mode == 2) SO2Rsend((uint8_t *)&c, 1);
}

void cancel_keying(struct radio *radio) // here radio indicates currently transmitting radio
{
  cw_xit_start_hold_until_ms = 0;
  request_finish_xit_cw_message(radio);
  clear_cwbuf();
  rptr_cw_onoff_buf = wptr_cw_onoff_buf; // read pointer go to write pointer
  cw_count_ms=0;
  // stop current keying
    switch (radio->modetype) {
    case LOG_MODETYPE_CW:  // CW
      if (radio->f_tone_keying) {
#if JK1DVPLOG_HWVER >=2
	play_stop_cmd();
	console->println("cancel keying tone and CW ");	
#else
	set_tone(1,0);
#endif
      } else {
        keying(0);
	console->println("cancel keying ");		
      }
      clear_cwbuf();
      break;
    case LOG_MODETYPE_PH:  // phone
#if JK1DVPLOG_HWVER >=2
      play_stop_cmd();
      console->println("cancel playing PH ");
#endif
      if (plogw->voice_memory_enable) {
        send_voice_memory(radio, 0);
      }
      break;
    case LOG_MODETYPE_DG:  // RTTY
#if JK1DVPLOG_HWVER >=2
      play_stop_cmd();
      console->println("cancel playing and DG ");                  
#endif
      if (radio->f_tone_keying) {
        //ledcWrite(LEDC_CHANNEL_0, 0);  // stop sending tone
	set_tone(1,0);
      } else {
        //digitalWrite(LED, 0);
        keying(0);
      }
      set_ptt_rig(radio, 0);
      break;
    }
}

void clear_cwbuf() {
  //  char c;
  //  c = 0x0a;  // clear buffer
  //  if (plogw->radio_mode == 2) SO2Rsend((uint8_t *)&c, 1);
  //
  rptr_cw_send_buf = wptr_cw_send_buf;
  cw_send_update = 1;
}

void display_cwbuf() {
  // if any change with rptr and wptr
  if (!cw_send_update) return;
  cw_send_update = 0;

  // show the unsent content of cw buf after currently sending character
  int p, pbuf;
  //  char buf[LEN_CW_SEND_BUF];
  char *buf;
  buf=plogw->cwbuf_display;
  pbuf = 0;
  if (cw_send_current != '\0') {
    buf[pbuf++] = cw_send_current;
    buf[pbuf]='\0';
  }
  char c;
  p = rptr_cw_send_buf;
  while (p != wptr_cw_send_buf) {
    // check characters excluding SO2R special characters '!' and '@'
    c = cw_send_buf[p];
    if (c != '!' && c != '@' && c != '#' && c != '$' && c != '%' && c != '^' ) {
      buf[pbuf++] = c; // do not show special characters
      buf[pbuf]='\0';      
    }
    p = (p + 1) % LEN_CW_SEND_BUF;
    if (pbuf >= LEN_CW_SEND_BUF - 1) break;
  }
  buf[pbuf] = '\0';
  display_cw_buf_lcd(buf);
}


