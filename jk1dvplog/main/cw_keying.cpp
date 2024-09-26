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
#include "hardware.h"
#include "cw_keying.h"

#include "Ticker.h"
#include "decl.h"
#include "variables.h"

#include "so2r.h"
#include "cat.h"
#include "main.h"
#include "dac-adc.h"
#include "misc.h"

//////////////////////////////
/// cw keying related routines
// cw sending is dual buffer cw_send_buf and cw_on_off_buf
// where cw_send_buf may be cleared and edited before sent to cw_on_off_buf in send_char
#define LEN_CW_SEND_BUF 120
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

int f_so2r_chgstat_tx = 0;  // nonzero if changing so2r transmit requested
int f_so2r_chgstat_rx = 0;  // nonzero if changing so2r receive requested

int f_transmission = 0;     // 0 nothing   1 force transmission on active trx 2 force stop transmission on active trx
#define CW_MS_ELEMENT 1200
int cw_spd = 24;  // wpm
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
}

void set_tone_keying(struct radio *radio) {
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
  } else {
    //    ledcDetachPin(LED);
    dac_scale_set(DAC_CHANNEL_1,0x03);    
    dac_cosine(DAC_CHANNEL_1,0);
    dac_output_disable(DAC_CHANNEL_1);
  }
}


void keying(int on)
// set key in appropriate port / SO2Rmini

{
  struct radio *radio;
  switch (plogw->radio_mode) {
    case 0:  // SO1R
    case 1:  // SAT
      radio = radio_selected;
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
      }
      // make sure all other hardware ports are off
      break;
    case 2:  // SO2R
      radio = &radio_list[plogw->so2r_tx];
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
            break;
          case 2:  // CW_KEY2
            digitalWrite(CW_KEY2, on);
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

  // repeat function timer countdown during receive after CQ'ing
  if (plogw->repeat_func_timer > 0) {
    plogw->repeat_func_timer--;
    if (plogw->repeat_func_timer == 0) {
      // repeat timer expired
      if (plogw->f_repeat_func_stat == 2) {
        plogw->f_repeat_func_stat = 3;  // request next repeat function command
      }
    }
  }
  
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

  if (enable_usb_keying) return;  // do not perform the following keying process if keying from usb

  radio = &radio_list[plogw->so2r_tx];  // set tx radio

  int ms;
  if (cw_count_ms == 0) {
    // load the next instruction if available
    if (rptr_cw_onoff_buf != wptr_cw_onoff_buf) {
      ms = cw_onoff_buf[rptr_cw_onoff_buf];

      // cw break process in on f_tone keying
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
      case 8000:  // set RX 1
	//			SO2R_setrx(0);
	f_so2r_chgstat_rx = 1;
	break;
      case 8001:  // set RX 2
	//		    SO2R_setrx(1);
	f_so2r_chgstat_rx = 2;
	break;
      case 8002:  // set RX 3
	f_so2r_chgstat_rx = 3;
	break;
      case 8003:  // end of a message and repeat counter start request

	if (plogw->f_repeat_func_stat == 1) {
	  plogw->f_repeat_func_stat = 2;
	  plogw->repeat_func_timer = plogw->repeat_func_timer_set;
	}
	break;

	// RTTY stx/etx
      case 8010:             // STX start transmission in active TX (in SO2R)
	f_transmission = 1;  // start transmission
	break;
      case 8011:             // ETX end transmission in active TX (in SO2R)
	f_transmission = 2;  // stop transmission
	break;

	// tone keying PTT
      case 8020: // in tone keying breakin ptt on
	keying(1);
	break;
      case 8021: // in tone keying breakin ptt off
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
	      // digitalWrite(LED, 0);
	      keying(0);
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
	      //digitalWrite(LED, 1);
	      keying(1);
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
      radio = radio_selected;
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

int send_the_dits_and_dahs(char *cw_to_send) {
  //  int p, p1;
  char *cp;
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
      ms = 8000;
      break;
    case '@':  // RX2
      ms = 8001;
      break;
    case '#':  // RX 3
      ms = 8002;
      break;

    case '$':  // end of a message and standby for repeating if configured
      ms = 8003;
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
      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
        ;
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
      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
        ;
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
    default:  // faulty
      plogw->ostream->println("?????");
      ms = -ms_elem;
      break;
    }
    //
    while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      ;
    cw_onoff_buf[wptr_cw_onoff_buf] = ms;
    wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;

    //
    if (c!=' ') {
      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
	;
      cw_onoff_buf[wptr_cw_onoff_buf] = -(ms_elem*(20-cw_duty_ratio))/cw_ratio_bunbo;
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
    }
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
  code = (code << 1) | 0b1000000;  // add start bit (at LSB) and stop bit // start bit is 0(space) stop bit is 1(mark)
  int ms_elem, ms;
  ms_elem = 22;  // single element of 45.45 baud = 22ms

  for (int i = 0; i < 7; i++) {
    // check LSB
    if (code & 0x1) {
      ms = ms_elem;
    } else {
      ms = -ms_elem;
    }
    code = code >> 1;  // shift code for the next bit
    while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
      ;
    cw_onoff_buf[wptr_cw_onoff_buf] = ms;
    wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
  }

  while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
    ;
  cw_onoff_buf[wptr_cw_onoff_buf] = ms_elem / 2;  // add half bit stop (mark)
  wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
}


void send_baudot(byte ascii, int *figures)
// figures : state of letters 0 and figures 1
{

  switch (ascii) {
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
    case '[':
      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
        ;
      cw_onoff_buf[wptr_cw_onoff_buf] = 8010;  // set special transmission control command  to start transmission
      wptr_cw_onoff_buf = (wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF;
      break;
    case ']':
      while ((wptr_cw_onoff_buf + 1) % LEN_CW_ONOFF_BUF == rptr_cw_onoff_buf)
        ;
      cw_onoff_buf[wptr_cw_onoff_buf] = 8011;  // set special transmission control command  to start transmission
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
    case '_': send_the_dits_and_dahs("-...-"); break;  // remap 22/11/15
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

void set_tx_to_focused() {
  // to make manual operation always happens to the radio which is current focus
  // check current so2r_tx and focused_radio if different, clear buffer and switch tx ---> this should be taken care of outside the append_cwbuf
  if (plogw->so2r_tx != plogw->focused_radio) {
    if (!plogw->f_console_emu) plogw->ostream->println("clear buf and switch tx");
    // need clear buffer
    clear_cwbuf();
    // wait until current character to be sent completely
    while ((cw_count_ms != 0) || (rptr_cw_onoff_buf != wptr_cw_onoff_buf)) {
      delay(1);
    }
    delay(100);
    // need switch to the currently focused radio
    SO2R_settx(plogw->focused_radio);
    if (!plogw->f_console_emu) plogw->ostream->println("   .. done");
  }
}

void cancel_current_tx_message() {
  // cancel currently ongoing messages (clear_cwbuf in cw and stop voice memory in phone)
  struct radio *radio;
  radio = &radio_list[plogw->so2r_tx];
  switch (radio->modetype) {
    case LOG_MODETYPE_CW:  // CW
      clear_cwbuf();
      break;
    case LOG_MODETYPE_PH:  // phone
      if (plogw->voice_memory_enable) {
        send_voice_memory(radio, 0);
      }
      break;
    case LOG_MODETYPE_DG:  // RTTY
      //            send_rtty_memory(radio, 0); // this will not stop sending ! (22/7/23)
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


void append_cwbuf(char c) {
  if (!isPrintable(c)) return;
  if (c >= 'a' && c <= 'z') c += 'A' - 'a';
  if (c == '"') c = '/';  // remap to '/'

  if (plogw->f_repeat_func_stat != 1) {
    set_tx_to_focused();
  }

  struct radio *radio;
  radio = &radio_list[plogw->so2r_tx];
  //  if (radio->f_tone_keying) {
  //    cw_send_buf[wptr_cw_send_buf] = '%'; // PTT on 
  //    wptr_cw_send_buf = (wptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
  //  }
  cw_send_buf[wptr_cw_send_buf] = c;
  wptr_cw_send_buf = (wptr_cw_send_buf + 1) % LEN_CW_SEND_BUF;
  cw_send_update = 1;
  if (plogw->radio_mode == 2) {
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
  char buf[100];

  char c;
  // clear buffer
  *rtty_memory_string = '\0';
  char *sp;
  // set buffer (recursively)
  set_rttymemory_string_buf(s);
  // issue set command to set string to id
  // icom
  // 1A 02 CH TEXT(2~71)  total of 73 bytes max

  if (!plogw->f_console_emu) {
    plogw->ostream->print("rtty_memory_string:");
    plogw->ostream->println(rtty_memory_string);
  }
  switch (radio->rig_spec->cat_type) {
    case 0:  // icom
      // ic-705 cannot set rtty memory buffer so send them to cwbuf as CW
      // special character 0x1　STX will start transmission and 0x2 ETX will stop transmission
      append_cwbuf('[');  // start transmission
      sp = rtty_memory_string;
      while (1) {
        c = *sp++;
        if (c == 0x00) break;
        append_cwbuf(c);
      }
      append_cwbuf(']');  // end transmission
      break;
    case 1:  // cat
      sprintf(buf, "EM0%d%s;", num, rtty_memory_string);
      if (!plogw->f_console_emu) plogw->ostream->println(buf);
      send_cat_cmd(radio, buf);
      break;
  }
}

void set_rttymemory_string_buf(char *s) {
  struct radio *radio;
  radio = radio_selected;
  int stat = 0;  // 0 normal append 1 $ sequence
  int idx;
  char tmp_buf[10];
  idx = strlen(rtty_memory_string);
  //  plogw->ostream->print("rtty_string_buf():"); plogw->ostream->println(s);
  // plogw->ostream->print("rttymemory_idx:"); plogw->ostream->println(idx);
  // plogw->ostream->println("");
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
        case 'W':  // send his exchange
          set_rttymemory_string_buf(plogw->sent_exch + 2);
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
        case 'L':  // new line (RTTY) CR (or LF?)
          set_rttymemory_string_buf("\n");
          break;
      }
      idx = strlen(rtty_memory_string);

      stat = 0;
    } else {
      // append
      //   plogw->ostream->println(idx);
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


void append_cwbuf_string(char *s) {
  struct radio *radio;
  radio = radio_selected;
  int stat = 0;  // 0 normal append 1 $ sequence
  char c;
  char tmp_buf[10];

  while (*s) {
    c = *s;
    s++;
    //  plogw->ostream->print("append_cwbuf_string():");
    //  plogw->ostream->println(c);
    if (c == '$') {
      stat = 1;
      continue;
    }
    if (stat == 1) {
      // $ sequence
      char tmpbuf[40];	        
      switch (c) {
        // macro similar to CTESTWIN
        case 'I':  // mycall insertion
          append_cwbuf_string(plogw->my_callsign + 2);
          break;
        case 'C':  // his callsign
          append_cwbuf_string(radio->callsign + 2);
          break;
        case 'W':  // send his exchange
	  if (radio->bandid>=11) {
	    copy_token(tmpbuf,plogw->sent_exch + 2,1,"/,;");
	  } else {
	    copy_token(tmpbuf,plogw->sent_exch + 2,0,"/,;");	    
	  }
	  append_cwbuf_string(cw_num_abbreviation(tmpbuf,plogw->sent_exch_abbreviation_level));
	  //          append_cwbuf_string(plogw->sent_exch + 2);
          break;
        case 'P':  // send power code (band dependent)
          if (plogw->multi_type == 3 || plogw->multi_type == 1) {
            append_cwbuf_string(power_code(radio->bandid));
          } else {
            // ignore $P
          }
          break;
        case 'J':  // jcc nr send
          append_cwbuf_string(plogw->jcc + 2);
          break;
        case 'V':  // send his rst
          append_cwbuf_string(cw_num_abbreviation(radio->sent_rst + 2, 1));
          break;
      case 'S': // send SEQNR
	sprintf(tmp_buf,"%d",plogw->seqnr);
	append_cwbuf_string(cw_num_abbreviation(tmp_buf, 0)); // without abbreviation
	break;
        case 'N':  // send my name
          append_cwbuf_string(plogw->my_name + 2);
          break;
      }
      stat = 0;
    } else {
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
  char buf[LEN_CW_SEND_BUF];
  pbuf = 0;
  if (cw_send_current != '\0') {
    buf[pbuf++] = cw_send_current;
  }
  char c;
  p = rptr_cw_send_buf;
  while (p != wptr_cw_send_buf) {
    // check characters excluding SO2R special characters '!' and '@'
    c = cw_send_buf[p];
    if (c != '!' && c != '@' && c != '#' && c != '$' && c != '%' && c != '^' ) buf[pbuf++] = c; // do not show special characters
    p = (p + 1) % LEN_CW_SEND_BUF;
    if (pbuf >= LEN_CW_SEND_BUF - 1) break;
  }
  buf[pbuf] = '\0';
  display_cw_buf_lcd(buf);
}


