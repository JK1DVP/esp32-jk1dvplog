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
#include <U8g2lib.h>
#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_r(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_l(U8G2_R0, /* reset=*/U8X8_PIN_NONE);


#include "main.h"
#include "display.h"
#include "multi_process.h"
#include "multi.h"
#include "log.h"
#include "qso.h"
#include "bandmap.h"
#include "dupechk.h"
#include "cw_keying.h"

uint8_t *dispbuf_r, *dispbuf_l;

#define WCOL_STR "                 "
void display_cw_buf_lcd(char *buf) {

#define LCD_CW_POSY (24 + 13 + 13)
  strncpy(dp->lcdbuf, buf, 20);
  //  sprintf(lcdbuf, "%-20s", buf);
  //  plogw->ostream->println(lcdbuf);
  u8g2_r.setDrawColor(0);
  u8g2_r.drawBox(0, LCD_CW_POSY, dp->wcol, dp->hcol[0]);
  u8g2_r.setDrawColor(1);
  u8g2_r.drawStr(0, LCD_CW_POSY, dp->lcdbuf);
  u8g2_r.sendBuffer();  // transfer internal memory to the display
  if (plogw->f_console_emu) {
    char buf[20], buf1[40];
    sprintf(buf, "\033[%d;%dH", int(LCD_CW_POSY / dp->hcol[0]) + 1, 40);
    plogw->ostream->print(buf);
    *buf1='\0';
    strncat(buf1,dp->lcdbuf,18);
    strcat(buf1,"\033[K"); // clear to the end of the line
      //    sprintf(buf1, "%-18s", dp->lcdbuf);
    //		plogw->ostream->print(dp->lcdbuf);
    plogw->ostream->print(buf1);
    
  }
}
// switch buffer memory to update right display
void select_left_display() {
  u8g2_l.setBufferPtr(dispbuf_l);
}
// switch buffer memory to update right display
void select_right_display() {
  u8g2_r.setBufferPtr(dispbuf_r);
}

// print string to the specified column in the display
void display_printStr(char *s, byte ycol) {

  if (ycol >= 10) {
    // select left
    select_left_display();
    u8g2_l.setDrawColor(0);
    u8g2_l.drawBox(0, (ycol - 10) * dp->hcol[1], dp->wcol, dp->hcol[1]);
    u8g2_l.setDrawColor(1);
    u8g2_l.drawStr(0, (ycol - 10) * dp->hcol[1], s);
    if (plogw->f_console_emu) {
      char buf[20], buf1[40];
      sprintf(buf, "\033[%d;%dH", ycol - 10 + 1, 0);
      plogw->ostream->print(buf);
      sprintf(buf1, "%-17s", s);
      plogw->ostream->print(buf1);
      
      //		plogw->ostream->print(s);
    }
  } else {
    select_right_display();
    u8g2_r.setDrawColor(0);
    u8g2_r.drawBox(0, ycol * dp->hcol[0], dp->wcol, dp->hcol[0]);
    u8g2_r.setDrawColor(1);
    u8g2_r.drawStr(0, ycol * dp->hcol[0], s);
    if (plogw->f_console_emu) {
      char buf[20], buf1[40];
      sprintf(buf, "\033[%d;%dH", ycol + 1, 40);
      plogw->ostream->print(buf);
      sprintf(buf1, "%-17s", s);
      plogw->ostream->print(buf1);
      //		plogw->ostream->print(s);
    }
  }
}

// store \n delimited string to s (may be dp->lcdbuf) and printed to left display
void upd_display_info_flash(char *s) {
  select_left_display();
  u8g2_l.clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }
  char *p;
  int count;
  count = 0;
  p = strtok(s, "\n");
  if (p != NULL) {
    display_printStr(p, 10 + count);
    count++;
  }
  while (p != NULL) {
    p = strtok(NULL, "\n");
    if (p != NULL) {
      display_printStr(p, 10 + count);
      count++;
    }
  }
  info_disp.show_info = INFO_DISP_FLASH;
  u8g2_l.sendBuffer();  // transfer internal memory to the display
  // set timer
  info_disp.timer = 5000;
}

void upd_display_tm() {
  select_right_display();
  // yyyy/mm/dd-hh:mm:ss
  // 0123456789012345678
  char buf[40];
  struct radio *radio;
  int x, y, line_flag;
  line_flag = 0;
  radio = radio_selected;
  if (plogw->show_smeter) {
    if (radio->smeter_stat >= 1) {
      // show peak  with underline
      if (radio->smeter_peak != SMETER_MINIMUM_DBM) {
        sprintf(buf, "%1d %-8s S%4d", plogw->focused_radio, plogw->tm + 9, radio->smeter_peak/SMETER_UNIT_DBM);
        y = dp->hcol[0] * 2 - 2;  // 2nd line
        x = 8 * 11;
        line_flag = 1;
      } else {
        sprintf(buf, "%1d %-8s S----", plogw->focused_radio, plogw->tm + 9);
      }
    } else {
      if (radio->smeter != SMETER_MINIMUM_DBM) {
        sprintf(buf, "%1d %-8s S%4d", plogw->focused_radio, plogw->tm + 9, radio->smeter/SMETER_UNIT_DBM);
      } else {
        sprintf(buf, "%1d %-8s S----", plogw->focused_radio, plogw->tm + 9);
      }
    }
  } else {
    if (plogw->show_qso_interval) {
      sprintf(buf, "%1d %-8s  %4d", plogw->focused_radio, plogw->tm + 9, plogw->qso_interval_timer/1000);
    } else {
      if (radio->qsodata_loaded) {
	sprintf(buf, "%1c %-8s  %4d", 'E', radio->tm_loaded + 9, radio->seqnr_loaded);
      } else {
	sprintf(buf, "%1d %-8s  %4d", plogw->focused_radio, plogw->tm + 9, plogw->seqnr);
      }
    }
  }
  // we still have some space to add some more information
  //display_printStr(plogw->tm + 3, 1);
  display_printStr(buf, 1);
  if (line_flag) u8g2_r.drawHLine(x, y, 8 * 5);
}

char statstr_sub[8];
void upd_display_stat() {
  select_right_display();

  struct radio *radio;
  radio = radio_selected;

  const char *statstr;
  statstr=statstr_sub;
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    sprintf(statstr_sub, "CW F%-d", radio->ptr_curr - 10 + 1);
    statstr = statstr_sub;
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // rtty msg
      sprintf(statstr_sub, "RT F%-d", radio->ptr_curr - 30 + 1);
      statstr = statstr_sub;
    } else {
      switch (radio->ptr_curr) {
        case 0:
        case 1:
          statstr = "Rcv";
          break;
        case 2:
        case 3:
          statstr = "RST";
          break;
        case 4:
        case 5:
          statstr = "Snt";
          break;
        case 6:
          statstr = "Rmks";
          break;
        case 7:
          statstr = "Sat.";
          break;
        case 8:
          statstr = "G.L.";
          break;
        case 9:
          statstr = "JCCG";
          break;
        case 20:
          statstr = "RIG";
          break;
        case 21:
          statstr = "Clst";
          break;
        case 22:
          statstr = "Mail";
          break;
        case 23:
          statstr = "CCmd";
          break;
        case 24:
          statstr = "Pow";
          break;
        case 25:
          statstr = "SSID";
          break;
        case 26:
          statstr = "Pass";
          break;
        case 27:
          statstr = "Spec";
          break;
        case 28:
          statstr = "zSvr";
          break;
        case 29:
          statstr = "Name";
          break;
      }
    }
  }

  char c;
  if (radio->dupe == 1) {
    if (radio->multi < 0) {
      c = '!';  // invalid multi and dupe as well
    } else {
      c = 'D';
    }
  } else {
    if (radio->multi < 0) {
      c = 'M';  // invalid multi
    } else {
      c = ' ';
    }
  }

  sprintf(dp->lcdbuf, "%-4s %1s %-3s %-3s%c%c",
          statstr,
          radio->keyer_mode == 1 ? ( f_cw_code_type == 1 ? "J": "K" ): (f_cw_code_type ==1? "j":" "),
          radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
          plogw->sat == 1 ? "Sat" : "",
	  radio->f_qsl==0 ? ' ':(radio->f_qsl==1 ? 'J': (radio->f_qsl==2 ? 'h': ' ')),
          c);
  // CQ/S&P Keyer Dupe info
  display_printStr(dp->lcdbuf, 2);
}
int upd_cursor_calc(int cursor, int wsize)
// cursor position 0 left most ..  wsize : window size (num of chars)
{
  // cursor position *cursor
  //plogw->ostream->print("cursor:");plogw->ostream->println(cursor);
  if (cursor >= wsize-1) {
    //plogw->ostream->print("* ");plogw->ostream->println((wsize-1));
    return (wsize-1) * 8;
    // show string from cursor - wsize +2    01234567  wsize==8
    //                                       JK1DVP/_  c==7
  } else {
    //plogw->ostream->print("- ");plogw->ostream->println((cursor));
    return (cursor * 8);
  }
}

void upd_display_put_lcdbuf(char *s, int cursor, int wsize, int lcdpos) {
  // lcdpos is where the window starts in lcdbuf
  // store string in *s so that cursor is located within the window (of size wsize)
  // sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
  int ofs;
  if (cursor >= wsize-1 ) {
    // cursor need to be at the last position
    ofs = cursor - (wsize - 1);
  } else {
    ofs = 0;
  }
  // sprintf(dp->lcdbuf,"%-s", s+ofs);
  //strncpy(dp->lcdbuf + lcdpos, s + ofs, wsize);
  // overwrite
  char *p0, *p1;
  p0 = dp->lcdbuf + lcdpos;
  p1 = s + ofs;
  for (int i = 0; i < wsize; i++) {
    if (*p1 == '\0') break;
    *p0++ = *p1++;
  }
  //plogw->ostream->print("ofs:");plogw->ostream->println(ofs);
}


void upd_cursor() {
  int x, y;
  char cursor_char=' ';
  select_right_display();

  struct radio *radio;
  radio = radio_selected;

  y = dp->hcol[0] * 4 - 1;
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    //x = (plogw->cw_msg[radio->ptr_curr - 10][1]) * 8;
    x = upd_cursor_calc(plogw->cw_msg[radio->ptr_curr - 10][1], 16);
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // rtty
      x = upd_cursor_calc(plogw->rtty_msg[radio->ptr_curr - 30][1], 16);
    } else {
      switch (radio->ptr_curr) {
        case 0:  // callsign
          //x = radio->callsign[1] * 8;
          x = upd_cursor_calc(radio->callsign[1], 9);
	  if (radio->callsign[1]< strlen(radio->callsign+2)) {
	    cursor_char=(radio->callsign+2)[(int)radio->callsign[1]];
	  }
          break;
        case 1:  // my exch (received exch)
          //          x = radio->recv_exch[1] * 8 + 70;
          x = upd_cursor_calc(radio->recv_exch[1], 7) + 8 * 9;
	  if (radio->recv_exch[1]< strlen(radio->recv_exch+2)) {	  
	    cursor_char=(radio->recv_exch+2)[(int)radio->recv_exch[1]];
	  }
          break;
        case 2:  // his rst (sent)
          x = (radio->sent_rst[1] + 4) * 8;
          break;
        case 3:  // my rst (recv)
          x = (radio->recv_rst[1] + 12) * 8;
          break;
        case 4:  // my callsign
          x = plogw->my_callsign[1] * 8;
          break;
        case 5:  // his callsign
          x = plogw->sent_exch[1] * 8 + 8 * 9;
          break;
        case 6:  // remarks
          x = upd_cursor_calc(radio->remarks[1], 16);
	  if (radio->remarks[1]< strlen(radio->remarks+2)) {
	    cursor_char=(radio->remarks+2)[(int)radio->remarks[1]];
	  }
          break;
        case 7:  // satellite name
          x = plogw->sat_name[1] * 8;
          break;
        case 8:  // grid locator
          x = plogw->grid_locator[1] * 8;
          break;
        case 9:  // jcc
          x = plogw->jcc[1] * 8;
          break;
        case 20:  // rig_name
          x = radio->rig_name[1] * 8;
          break;
        case 21:  // cluster_name
          //x = plogw->cluster_name[1] * 8; break;
          x = upd_cursor_calc(plogw->cluster_name[1], 16);
          break;
        case 22:  // email_addr
          x = upd_cursor_calc(plogw->email_addr[1], 16);
          break;
        case 23:  // cluster_cmd
          x = upd_cursor_calc(plogw->cluster_cmd[1], 16);
          break;
        case 24:  // power_code
          x = upd_cursor_calc(plogw->power_code[1], 16);
          break;
        case 25:  // wifi_ssid
          x = upd_cursor_calc(plogw->wifi_ssid[1], 16);
          break;
        case 26:  // wifi_passwd
          x = upd_cursor_calc(plogw->wifi_passwd[1], 16);
          break;
        case 27:  // rig_spec
          x = upd_cursor_calc(radio->rig_spec_str[1], 16);
          break;
        case 28:  // zserver_name
          x = upd_cursor_calc(plogw->zserver_name[1], 16);
          break;
        case 29:  // my_name
          x = upd_cursor_calc(plogw->my_name[1], 16);
          break;

        default:
          return;
      }
    }
  }
  u8g2_r.drawHLine(x, y, 8);
  if (plogw->f_console_emu) {
    char buf[40];
    sprintf(buf,"\033[%d;%dH\033[4m%c\033[0m",y/dp->hcol[0]+1,x/8+40,cursor_char);
    plogw->ostream->print(buf);
  }
}

void upd_display_freq(unsigned int freq, char *opmode, int col) {
  //
  unsigned int tmp1, tmp2;
  struct radio *radio;
  radio = radio_selected;
  //  tmp2 = (freq % 1000) / 10;   // 100Hz, 10Hz
  //  tmp1 = freq / 1000;  // KHz 
  //  tmp1 = tmp1 % 1000;  // kHz
  tmp2= (freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT); // below kHz
  tmp1 = freq / (1000/FREQ_UNIT); 
  tmp1 = tmp1 % 1000 ;  // kHz

  sprintf(dp->lcdbuf, "%3d.%03d.%02d%c%-6s", freq / (1000000/FREQ_UNIT), tmp1, tmp2, radio->transverter_in_use ? '*' : ' ', opmode);
  display_printStr(dp->lcdbuf, col);
}

void upd_display() {
  struct radio *radio;
  radio = radio_selected;


  // frequency and mode in the top line
  select_right_display();
  if (plogw->f_console_emu) {
    clear_display_emu(0);
  }
  
  //strcpy(radio->opmode, opmode);
  if (verbose & 1) {
    plogw->ostream->print("Freq:");
    plogw->ostream->print(radio->freq);
    plogw->ostream->print(" Mode:");
    plogw->ostream->println(radio->opmode);
  }

  if (radio->qsodata_loaded) {
    upd_display_freq(radio->freq_loaded, radio->opmode_loaded, 0);
  } else {
    upd_display_freq(radio->freq, radio->opmode, 0);
  }
  // second line: time
  upd_display_tm();
  // fill
  strcpy(dp->lcdbuf, "                ");

  // show log window and number  third line
  if ((radio->ptr_curr >= 10) && (radio->ptr_curr <= 10 + N_CWMSG - 1)) {
    // cw msg
    //sprintf(dp->lcdbuf, "%-s", radio->cw_msg[radio->ptr_curr - 10] + 2);
    upd_display_put_lcdbuf(plogw->cw_msg[radio->ptr_curr - 10] + 2, plogw->cw_msg[radio->ptr_curr - 10][1], 16, 0);
  } else {
    if ((radio->ptr_curr >= 30) && (radio->ptr_curr <= 30 + N_CWMSG - 1)) {
      // cw msg
      //sprintf(dp->lcdbuf, "%-s", radio->cw_msg[radio->ptr_curr - 10] + 2);
      upd_display_put_lcdbuf(plogw->rtty_msg[radio->ptr_curr - 30] + 2, plogw->rtty_msg[radio->ptr_curr - 30][1], 16, 0);
    } else {
      switch (radio->ptr_curr) {
        case 0:  // callsign
        case 1:  // my exch
          //          sprintf(dp->lcdbuf, "%-8s %-7s", radio->callsign + 2, radio->recv_exch + 2);
          // better to allocate space to show 9 characters (??????/?_=9 chrs)
          upd_display_put_lcdbuf(radio->callsign + 2, radio->callsign[1], 9, 0);
          upd_display_put_lcdbuf(radio->recv_exch + 2, radio->recv_exch[1], 7, 8 + 1);
          break;
        case 2:  // sent rst
        case 3:  // rcv  rst
          strcpy(dp->lcdbuf, "Snt     Rcv    ");
          upd_display_put_lcdbuf(radio->sent_rst + 2, radio->sent_rst[1], 4, 4);
          upd_display_put_lcdbuf(radio->recv_rst + 2, radio->recv_rst[1], 4, 12);

          break;
        case 4:  // edit my_callsign
        case 5:  // edit sent_exch
          //          sprintf(dp->lcdbuf, "%-8s %-7s", plogw->my_callsign + 2, plogw->sent_exch + 2);
          upd_display_put_lcdbuf(plogw->my_callsign + 2, plogw->my_callsign[1], 9, 0);
          upd_display_put_lcdbuf(plogw->sent_exch + 2, plogw->sent_exch[1], 7, 8 + 1);
          break;
        case 6:  // editing remarks
          //sprintf(dp->lcdbuf, "%-s", plogw->remarks + 2);
          upd_display_put_lcdbuf(radio->remarks + 2, radio->remarks[1], 16, 0);
          break;
        case 7:  // editing satellite name
          sprintf(dp->lcdbuf, "%-s", plogw->sat_name + 2);
          break;
        case 8:  // editing grid locator
          sprintf(dp->lcdbuf, "%-s", plogw->grid_locator + 2);
          break;
        case 9:  // jcc
          sprintf(dp->lcdbuf, "%-s", plogw->jcc + 2);
          break;
        case 20:  // rig_name
          sprintf(dp->lcdbuf, "%-s", radio->rig_name + 2);
          break;
        case 21:  // cluster_name
          //sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
          upd_display_put_lcdbuf(plogw->cluster_name + 2, plogw->cluster_name[1], 16, 0);
          break;
        case 22:  // email_addr
          upd_display_put_lcdbuf(plogw->email_addr + 2, plogw->email_addr[1], 16, 0);
          break;
        case 23:  // cluster_cmd
          //sprintf(dp->lcdbuf, "%-s", plogw->cluster_name + 2);
          upd_display_put_lcdbuf(plogw->cluster_cmd + 2, plogw->cluster_cmd[1], 16, 0);
          break;
        case 24:  // power code
          upd_display_put_lcdbuf(plogw->power_code + 2, plogw->power_code[1], 16, 0);
          break;
        case 25:  // wifi_ssid
          upd_display_put_lcdbuf(plogw->wifi_ssid + 2, plogw->wifi_ssid[1], 16, 0);
          break;
        case 26:  // wifi_passwd
          upd_display_put_lcdbuf(plogw->wifi_passwd + 2, plogw->wifi_passwd[1], 16, 0);
          break;
        case 27:  // rig_spec_str
          upd_display_put_lcdbuf(radio->rig_spec_str + 2, radio->rig_spec_str[1], 16, 0);
          break;
        case 28:  // zserver_name
          upd_display_put_lcdbuf(plogw->zserver_name + 2, plogw->zserver_name[1], 16, 0);
          break;
        case 29:  // my_name
          upd_display_put_lcdbuf(plogw->my_name + 2, plogw->my_name[1], 16, 0);
          break;
      }
    }
  }
  display_printStr(dp->lcdbuf, 3);

  // draw underline to the cursor
  upd_cursor();

  upd_display_stat();

  u8g2_r.sendBuffer();  // transfer internal memory to the display
  // u8g2_l.sendBuffer();          // transfer internal memory to the display
}


void right_display_sendBuffer()
{
    u8g2_r.sendBuffer();  // transfer internal memory to the display

}
void left_display_sendBuffer()
{
    u8g2_l.sendBuffer();  // transfer internal memory to the display

}

void left_display_clearBuffer()
{
  u8g2_l.clearBuffer();  // clear the internal memory
}
void right_display_clearBuffer()
{
  u8g2_r.clearBuffer();  // clear the internal memory
}

void init_display() {
  dp = &disp;

  if (display_flip) {
    u8g2_r.setI2CAddress(0x3c * 2); // flip
  } else {
    u8g2_r.setI2CAddress(0x3d * 2); //non flip
  }

  plogw->ostream->println("getBufferSize=");
  plogw->ostream->print(u8g2_r.getBufferSize());
  dispbuf_r = (uint8_t *)malloc(u8g2_r.getBufferSize());
  select_right_display();


  u8g2_r.initDisplay();
  if (display_flip) {  
    u8g2_r.setFlipMode(1);
  }
  u8g2_r.clearDisplay();
  u8g2_r.setPowerSave(0);
  u8g2_r.clearBuffer();  // clear the internal memory
  u8g2_r.setFont(u8g2_font_8x13_mf);
  dp->hcol[0] = u8g2_r.getFontAscent() - u8g2_r.getFontDescent();
  plogw->ostream->print("u8g2_r hcol =");
  plogw->ostream->println(dp->hcol[0]);
  u8g2_r.setFontRefHeightExtendedText();
  u8g2_r.setFontPosTop();

  if (display_flip) {
    u8g2_l.setI2CAddress(0x3d * 2);   // flip    
  } else {
    u8g2_l.setI2CAddress(0x3c * 2); // non flip
  }

  dispbuf_l = (uint8_t *)malloc(u8g2_l.getBufferSize());

  select_left_display();
  u8g2_l.initDisplay();

  if (display_flip) {  
    u8g2_l.setFlipMode(1);
  }
  u8g2_l.clearDisplay();
  u8g2_l.setPowerSave(0);
  u8g2_l.clearBuffer();  // clear the internal memory
  //  u8g2_l.setFont(u8g2_font_8x13_tf);
  u8g2_l.setFont(u8g2_font_t0_11_mf);
  u8g2_l.setFontRefHeightExtendedText();
  u8g2_l.setFontPosTop();

  dp->hcol[1] = u8g2_l.getFontAscent() - u8g2_l.getFontDescent();
  plogw->ostream->print("u8g2_l hcol =");
  plogw->ostream->println(dp->hcol[1]);
  //  w = u8g2_r.getStrWidth(lcdbuf);
  dp->wcol = 128;  // the whole line

  u8g2_l.drawStr(0, 0, "JK1DVP log");
  u8g2_l.drawStr(0, 13, "Initializing");
  u8g2_l.drawStr(0, 23, "test 24/6/21");  

  u8g2_l.sendBuffer();  // transfer internal memory to the display
  Serial.print("disp initialized.");
}


void upd_disp_rig_info() {
  char buf[80];
  *dp->lcdbuf = '\0';
  switch (plogw->radio_mode) {
    case 0:  // SO1R
      strcat(dp->lcdbuf, "SO1R ");
      break;
    case 1:  // SAT
      strcat(dp->lcdbuf, "SAT ");
      break;
    case 2:  // SO2R
      strcat(dp->lcdbuf, "SO2R ");
      break;
  }

  //  if ((WiFi.status() == WL_CONNECTED)) {
  if (wifi_status == 1) {
    localip_to_string(buf);
    strcat(dp->lcdbuf, buf);
    strcat(dp->lcdbuf, "\n");
  } else {
    strcat(dp->lcdbuf, "\n");
  }
  for (int i = 0; i < N_RADIO; i++) {
    sprintf(buf, "Rig%d:%s %c", radio_list[i].rig_idx, radio_list[i].rig_spec->name, radio_list[i].enabled ? '*' : ' ');
    strcat(dp->lcdbuf, buf);
    // add SO1R/SAT/SO2R specific infos
    switch (plogw->radio_mode) {
      case 0:  // SO1R
      case 1:  // SAT
        sprintf(buf, " %c%c %c\n", ' ', ' ', radio_list[i].f_tone_keying ? 't' : ' ');
        break;
      case 2:  // SO2R
        sprintf(buf, " %c%c %c\n", plogw->so2r_tx == i ? 'T' : ' ', plogw->so2r_rx == i ? 'R' : ' ', radio_list[i].f_tone_keying ? 't' : ' ');
        break;
    }
    strcat(dp->lcdbuf, buf);
  }
  // append callhistfn settingsfn
  strcat(dp->lcdbuf, "callhist:");
  strcat(dp->lcdbuf, callhistfn);
  strcat(dp->lcdbuf, "\n");
  strcat(dp->lcdbuf, "settings:");
  strcat(dp->lcdbuf, settingsfn);
  strcat(dp->lcdbuf, "\n");
  upd_display_info_flash(dp->lcdbuf);
}
/////////////////////////////////////

void init_info_display() {
  info_disp.timer = 0;
  info_disp.show_info = 0;
  info_disp.show_info_prev = 0;
  info_disp.pos = 0;
  info_disp.multi_ofs = 0;
}
void upd_display_info_signal()
{
  // display information related with signal received
  // antenna selected
  // and rotator azimuth ( if enabled )
  struct radio *radio;

  radio = radio_selected;
  char buf[60],buf1[15];
  *dp->lcdbuf = '\0';
  sprintf(buf, "%6s F:%d kHz\nRelay :%d ", radio->rig_spec->name , radio->freq / 1000, plogw->relay[0] + plogw->relay[1] * 2);
  strcat(dp->lcdbuf, buf);
  if (plogw->f_rotator_enable) {
    sprintf(buf, "Az %03d\n", plogw-> rotator_az);
    strcat(dp->lcdbuf, buf);
  } else {
    strcat(dp->lcdbuf, "\n");
  }
  for (int i = 0; i < 4; i++) {
    if (radio->smeter_record[i] != 0 ) {
	  dtostrf(radio->smeter_record[i]/(SMETER_UNIT_DBM*1.0),4,0,buf1);
	  if (radio->smeter_azimuth[i] != -1) {
        sprintf(buf, "ANT %1d AZ %3d S %s\n", i, radio->smeter_azimuth[i],buf1 );
      } else {
        sprintf(buf, "ANT %1d AZ --- S %s\n", i, buf1);
      }
      strcat(dp->lcdbuf, buf);
    }
  }
  upd_display_info_flash(dp->lcdbuf);

  // print to serial
  plogw->ostream->print("SIG: ");
  *dp->lcdbuf = '\0';
  sprintf(buf, "%-17s %6s F:%d kHz Ant:%d ", plogw->tm, radio->rig_spec->name , radio->freq / 1000, plogw->relay[0] + plogw->relay[1] * 2);
  strcat(dp->lcdbuf, buf);
  if (plogw->f_rotator_enable) {
    sprintf(buf, "Az %03d, ", plogw-> rotator_az);
    strcat(dp->lcdbuf, buf);
  } else {
    strcat(dp->lcdbuf, ", ");
  }
  for (int i = 0; i < 4; i++) {
    if (radio->smeter_record[i] != 0 ) {
	  dtostrf(radio->smeter_record[i]/(SMETER_UNIT_DBM*1.0),6,1,buf1);
      if (radio->smeter_azimuth[i] != -1) {
        sprintf(buf, "ANT %1d AZ %3d S %s , ", i, radio->smeter_azimuth[i], buf1);
      } else {
        sprintf(buf, "ANT %1d AZ --- S %s , ", i, buf1);
      }
      strcat(dp->lcdbuf, buf);
    }
  }
	  if (strlen(radio->remarks+2))
		strcat(dp->lcdbuf,radio->remarks+2);
  plogw->ostream->println(dp->lcdbuf);
}



void upd_display_info_contest_settings() {

  struct radio *radio;

  radio = radio_selected;

  // show information about contest settings  dupe check, name of the contest and multi worked
  if (verbose & 1) {
    plogw->ostream->println("display info contest settings()  ");
  }
  select_left_display();
  u8g2_l.clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // show contest name and dupe category
  sprintf(dp->lcdbuf, "%s D:%s", plogw->contest_name, plogw->mask == 0xff ? "OK C/P" : "NG C/P");
  display_printStr(dp->lcdbuf, 10 + 0);

  // show worked stations and multi
  int nq, nm, np;  // total number of q and m and p point
  nq = 0;
  nm = 0;
  np = 0;
  for (int i = 0; i < N_BAND; i++) {
    nq += score.worked[0][i];
    nq += score.worked[1][i];
    np += score.worked[0][i] * plogw->cw_pts;
    np += score.worked[1][i];
    if (verbose & 1) {
      plogw->ostream->print("Band:");
      plogw->ostream->print(i);
      plogw->ostream->print(" QC:");
      plogw->ostream->print(score.worked[0][i]);
      plogw->ostream->print(" QP:");
      plogw->ostream->print(score.worked[1][i]);
      plogw->ostream->print(" M:");
      plogw->ostream->println(score.nmulti[i]);
    }
    nm += score.nmulti[i];
  }
  sprintf(dp->lcdbuf, "QSO:%d MUL:%d", nq, nm);
  display_printStr(dp->lcdbuf, 10 + 1);

  const char *s;
  if (multi_list.multi[radio->bandid-1] != NULL) {
    if ((radio->multi >= 0) && (multi_list.n_multi[radio->bandid-1] > radio->multi)) {
      s = multi_list.multi[radio->bandid-1]->name[radio->multi];
    } else {
      s = "Not valid";
    }
  } else {
    s = "No CHECK";
  }


  sprintf(dp->lcdbuf, "mult:%s", s);
  display_printStr(dp->lcdbuf, 10 + 2);


  // show multi list below
  if (radio->bandid >= 1) {
  if ( (multi_list.multi[radio->bandid-1] != NULL) ) {

    sprintf(dp->lcdbuf, "Multi in %s MHz", band_str[radio->bandid - 1]);
    display_printStr(dp->lcdbuf, 13);

    char buf1[10];
    int count;
    int len;
    count = 0;
    int countrow;
    countrow = 0;
    *dp->lcdbuf = '\0';


    for (int i = info_disp.multi_ofs; i < multi_list.n_multi[radio->bandid-1]; i++) {
      if (i >= multi_list.n_multi[radio->bandid-1]) break;
      sprintf(buf1, "%c%s ", multi_list.multi_worked[radio->bandid - 1][i] == 1 ? '*' : ' ', multi_list.multi[radio->bandid-1]->mul[i]);
      len = strlen(buf1);
      if (count + len > 16) {  // use next row
        display_printStr(dp->lcdbuf, 14 + countrow);
        *dp->lcdbuf = '\0';
        count = 0;
	// check row
	if (countrow>2) { // no displayable area available
	  break;
	}
        countrow++;
      }
      strcat(dp->lcdbuf, buf1);
      count += len;
    }

    display_printStr(dp->lcdbuf, 14 + countrow);
  }

  }

  u8g2_l.sendBuffer();  // transfer internal memory to the display
  // set timer
  info_disp.timer = 5000;
}

// show non worked bandmap stations and worked QSOs for all non-masked bands
// バンドマップのエントリー数の一覧を見ることによってバンドの状況がチェックできる。
void upd_display_info_to_work_bandmap() {
  struct radio *radio;

  radio = radio_selected;

  // show information about contest settings  dupe check, name of the contest and multi worked
  if (!plogw->f_console_emu) plogw->ostream->println("display info to work bandmap()  ");

  select_left_display();
  u8g2_l.clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // show worked stations and multi
  unsigned int bandmask = 0b1111111111111;
  int nidx = 0;
  int nq, nm, np, nw;  // total number of q and m and p point

  struct bandmap_entry *p;
  int idx;

  nidx = 0;
  int cnt;

  cnt = 0;

  for (int j = 0; j < N_BAND; j++) {
    if (((1 << j) & bandmask) == 0) {
      // not show this band
      continue;
    }
    nq = 0;
    nm = 0;
    np = 0;
    nw = 0;
    cnt++;
    if (cnt <= plogw->bandmap_summary_idx) continue;


    nq = score.worked[0][j];
    nq += score.worked[1][j];
    nm = score.nmulti[j];

    // procedure to check number of non-worked stations for the band
    for (int i = 0; i < bandmap[j].nentry; i++) {
      p = bandmap[j].entry + i;
      if (*p->station == '\0') continue;
      if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) continue;
      nw++;
    }

    // nw: number of to be worked station for the band

    sprintf(dp->lcdbuf, "%s %3dQ>%2d %2dM", bandid_str[j], nq, nw, nm);
    display_printStr(dp->lcdbuf, 10 + nidx);
    // next row
    nidx++;
    if (nidx >= 6) break;
  }
  u8g2_l.sendBuffer();  // transfer internal memory to the display
  // set timer
  info_disp.timer = 5000;
}



void upd_display_info_qso(int option) {
  // option 0: load and print on the left display
  // option 1: load and edit mode
  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;

  // pos = pos - len;  // start from the end record
  //pos = 0; // start from the beginning
  pos = info_disp.pos;
  if (verbose & 1) {
    plogw->ostream->print("pos=");
    plogw->ostream->println(pos);
  }
  if (!qsologf.seek(pos)) {

    if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
    goto end;
  }
  ret = qsologf.read(qso.all, len);
  if (ret != len) {
    //
    if (!plogw->f_console_emu) {
      plogw->ostream->print("qso not read bytes=");
      plogw->ostream->println(ret);
    }
    goto end;
  }
  // check type
  if (qso.entry.type[0] != 'Q') {
    // not vaild qso
    if (!plogw->f_console_emu) plogw->ostream->println("not valid qso encountered");
    //     goto end;
  }
  // print content
  //plogw->ostream->print("Pos:");plogw->ostream->print(pos);
  //plogw->ostream->print(" ");

  reformat_qso_entry();

  struct radio *radio;

  radio = radio_selected;

  switch (option) {
    case 0:
      print_qso_entry();

      // show the qso content
      select_left_display();
      u8g2_l.clearBuffer();  // clear the internal memory
      if (plogw->f_console_emu) {
        clear_display_emu(1);
      }

      upd_disp_info_qso_entry();
      u8g2_l.sendBuffer();  // transfer internal memory to the display
      // set timer
      info_disp.timer = 5000;
      break;

    case 1:
      // copy into edit bufferes
      set_qsodata_from_qso_entry();
      radio->qsodata_loaded = 1;
      break;
  }
end:
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
  }
  return;
}



/// the following are not yet completed 21/11/15  still not in use 22/01/25


void upd_display_info() {
  select_left_display();
  u8g2_l.clearBuffer();  // clear the internal memory
  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }

  // the following should be executed after timeout
  //  if (info_disp.show_info != info_disp.show_info_prev) {
  //  info_disp.show_info = info_disp.show_info_prev;
  //	// go back to previous item   after showing info temporarilly
  // }

  switch (info_disp.show_info) {
    case INFO_DISP_QSO:
      // show previous qso for referring and editing (mark deleted and so on ...)
      // upd_display_info_qso();
      // set next update time
      return;
      break;
    case INFO_DISP_BANDMAP:
      //upd_display_bandmap();
      return;
      break;
    case INFO_DISP_CONTEST_SETTINGS:
      //upd_display_info_contest_settings();
      break;
    case INFO_DISP_FLASH:
      // keep displaying previous
      return;
    case INFO_DISP_SIGNAL:
      // upd_display_info_signal();
      return;
      break;
  }
  u8g2_l.sendBuffer();  // transfer internal memory to the display
}
void clear_display_emu(int side) {
  int ix, iy;
  ix=0;
  switch (side) {
    case 1:  // left
      ix = 1;
      break;
    case 0:  // right  display
      ix = 41;
      break;
  }
  for (int iy = 1; iy <= 7; iy++) {
    char buf[50];
    sprintf(buf, "\033[%d;%dH                               ", iy, ix);
    plogw->ostream->print(buf);
  }
}

void upd_disp_info_qso_entry() {
  sprintf(dp->lcdbuf, "%-14s %4s%c", qso.entry.tm + 3, qso.entry.seqnr, qso.entry.type[0]);  // line 1 time
  display_printStr(dp->lcdbuf, 10);                                                          //
  upd_display_freq((unsigned long)(atoll(qso.entry.freq)/FREQ_UNIT), qso.entry.opmode, 11);                              // line 2 freq
  sprintf(dp->lcdbuf, "%-8s %3s %-s", qso.entry.hiscall, qso.entry.rcvrst, qso.entry.rcvexch);
  display_printStr(dp->lcdbuf, 12);  // line3 his
  sprintf(dp->lcdbuf, "%-8s %3s %-s", qso.entry.mycall, qso.entry.sentrst, qso.entry.sentexch);
  display_printStr(dp->lcdbuf, 13);            // line 4 my
  strncpy(dp->lcdbuf, qso.entry.remarks, 16);  // line 5 remarks
  display_printStr(dp->lcdbuf, 14);            // line 5 remarks
}


void upd_display_bandmap() {
  // plogw->ostream->println("upd_display_bandmap()");
  // plogw->ostream->print("top_column:");
  // plogw->ostream->println(bandmap_disp.top_column);
  //  plogw->ostream->print("cursor:");
  //  plogw->ostream->println(bandmap_disp.cursor );

  // show current bandmap structure for the current band
  int idx;
  struct bandmap_entry *p;

  struct radio *radio;
  radio = radio_selected;

  int bandid;
  bandid = radio->bandid_bandmap;
  if (verbose & 128) {
    plogw->ostream->print("bandid bandmap=");
    plogw->ostream->println(bandid);
  }
  if (bandid == 0) {
    return;
  }


  // maintenances display bandmap
  // delete old entry
  // delete_old_entry(bandid, 20); // now performed in minute interval jobs
  // sort the entry

  sort_bandmap(bandid);

  select_left_display();

  left_display_clearBuffer();

  if (plogw->f_console_emu) {
    clear_display_emu(1);
  }
  // place the on the frequency bandmap entry on the top if exists
  int i_onfreq;
  int f_worked;

  f_worked = 0;
  // print on frequency station on top every time if we have
  i_onfreq = find_on_freq_bandmap(radio->bandid, radio->freq, 100/FREQ_UNIT);
  idx = radio->bandid - 1;
  if ((i_onfreq != -1) && (idx >= 0)) {
    bandmap_disp.f_onfreq = 1;
    p = bandmap[idx].entry + i_onfreq;
    strncpy(bandmap_disp.on_freq_station, p->station, 10);
    bandmap_disp.on_freq_modeid = p->mode;


    // check if already worked this station
    //    if (dupe_check_nocallhist(p->station, bandid * 4 + modetype[p->mode], plogw->mask)) {
    if (dupe_check_nocallhist(p->station, bandmode_param(radio->bandid,modetype[p->mode]), plogw->mask)) {    
      // already worked
      f_worked = 1;

      if (!plogw->f_console_emu) {
        plogw->ostream->print(p->station);
        plogw->ostream->println(" is worked before");
      }

    } else {
      if (!plogw->f_console_emu) {
        plogw->ostream->print(p->station);
        plogw->ostream->println(" is NOT worked before");
      }

    }
    if (plogw->f_console_emu) {
      // underlined
      plogw->ostream->print("\033[4m");
      if (f_worked) {
        // yellow color
        plogw->ostream->print("\033[33m");
      }
    }
    upd_display_bandmap_show_entry(p, -1);
    if (plogw->f_console_emu) {
      // restore
      plogw->ostream->print("\033[0m\033[39m");
    }
    // put underline
    u8g2_l.drawHLine(0, dp->hcol[1] - 1, 128);
    if (f_worked) {
      // middle line
      u8g2_l.drawHLine(0, dp->hcol[1] - 5, 128);
    }

  } else {
    bandmap_disp.f_onfreq = 0;
  }



  int count;
  count = 0;

  // show entry normally
  idx = bandid - 1;
  if (verbose & 64) {
    plogw->ostream->print("show bandmap entry for idx=");
    plogw->ostream->println(idx);
  }

  if (idx >= 0) {
    // clear on_cursor information
    bandmap_disp.on_cursor_station[0] = '\0';

    // check entries

    // first , check number of displayable entries

    int i;
    //    int count1;
    bandmap_disp.ncount1 = 0;
    for (i = 0; i < bandmap[idx].nentry; i++) {
      p = bandmap[idx].entry + i;
      if (*p->station == '\0') {
        if (verbose & 64) {
          plogw->ostream->print(".");
        }

        continue;
      }
      if (p->flag & BANDMAP_ENTRY_FLAG_ONFREQ) {
        if (verbose & 64) {
          plogw->ostream->print("O");
        }

        continue;
      }
      if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) {
        if (verbose & 64) {
          plogw->ostream->print("W");
        }

        continue;
      }
      if (verbose & 64) {
        plogw->ostream->print("+");
      }

      bandmap_disp.ncount1++;
    }
    if (verbose & 64) {
      plogw->ostream->print("\nbandmap count1=");
      plogw->ostream->println(bandmap_disp.ncount1);
    }
    if (bandmap_disp.ncount1 > 0) {

      // print
      if (verbose & 64) {
        plogw->ostream->print("bandmap ");
        plogw->ostream->print(idx);
        plogw->ostream->print(" nentry ");
        plogw->ostream->println(bandmap[idx].nentry);
      }

      for (i = 0; i < bandmap[idx].nentry; i++) {

        if (verbose & 128) {
          plogw->ostream->print("count ");
          plogw->ostream->print(count);
          plogw->ostream->print(" i ");
          plogw->ostream->print(i);
          plogw->ostream->print(" top_column ");
          plogw->ostream->println(bandmap_disp.top_column);
        }


        if (count - bandmap_disp.top_column >= 5 + bandmap_disp.f_onfreq) {
          if (verbose & 64) plogw->ostream->println("break");
          break;  // could not show more than this
        }
        p = bandmap[idx].entry + i;
        if (*p->station == '\0') continue;
        // check if on the freq flag
        if (p->flag & BANDMAP_ENTRY_FLAG_ONFREQ) {
          if (verbose & 64) {
            plogw->ostream->print((String)p->station);
            plogw->ostream->print(" f=");
            plogw->ostream->print(p->freq);
            plogw->ostream->println(" onfreq not show");
          }
          continue;
        }
        if (p->flag & BANDMAP_ENTRY_FLAG_WORKED) {
          if (verbose & 64) {
            plogw->ostream->print((String)p->station);
            plogw->ostream->print(" f=");
            plogw->ostream->print(p->freq);
            plogw->ostream->println(" worked not show");
          }
          continue;
        }
        if (count - bandmap_disp.top_column >= 0) {
          if (verbose & 64) {
            plogw->ostream->print("show entry  ");
          }
          upd_display_bandmap_show_entry(p, count);
        }
        count++;
      }
    }
  }

  if ((count + bandmap_disp.f_onfreq )== 0) {
    sprintf(dp->lcdbuf, "Bandmap %s", bandid_str[idx]);  
    display_printStr(dp->lcdbuf, 10);
  }

  u8g2_l.sendBuffer();  // transfer internal memory to the display
}

