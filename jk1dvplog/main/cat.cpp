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
#include "Ticker.h"
#include "decl.h"
#include "variables.h"
#include "hardware.h"
#include "cat.h"
#include "cw_keying.h"
#include "usb_host.h"
#include "display.h"
#include "log.h"
#include "edit_buf.h"
#include "main.h"
#include "mcp.h"

Ticker civ_reader;
// software serial
#include <SoftwareSerial.h>
SoftwareSerial Serial3;

// ci-v related variables
long freq;
char opmode[8];
// DECIMAL -> BCD
byte dec2bcd(byte data) {
  return (((data / 10) << 4) + (data % 10));
}

// BCD -> DECIMAL
byte bcd2dec(byte data) {
  return (((data >> 4) * 10) + (data % 16));
}



// mode name string to rig mode number
int rig_modenum(const char *opmode) {
  int mode;
  if (verbose & 1) {
    plogw->ostream->print("rig_modenum():");
    plogw->ostream->print(opmode);
    plogw->ostream->println(":");
  }
  if (strcmp(opmode, "LSB") == 0) mode = 0;
  else if (strcmp(opmode, "USB") == 0) mode = 1;
  else if (strcmp(opmode, "AM") == 0) mode = 2;
  else if (strcmp(opmode, "CW") == 0) mode = 3;
  else if (strcmp(opmode, "RTTY") == 0) mode = 4;
  else if (strcmp(opmode, "FM") == 0) mode = 5;
  else if (strcmp(opmode, "WFM") == 0) mode = 6;
  else if (strcmp(opmode, "CW-R") == 0) mode = 7;
  else if (strcmp(opmode, "RTTY-R") == 0) mode = 8;
  else if (strcmp(opmode, "DV") == 0) mode = 0x17;
  else mode = -1;
  return mode;
}

char *opmode_string(int modenum) {
  switch (modenum) {
    case 0: return "LSB"; break;
    case 1: return "USB"; break;
    case 2: return "AM"; break;
    case 3: return "CW"; break;
    case 4: return "RTTY"; break;
    case 5: return "FM"; break;
    case 6: return "WFM"; break;
    case 7: return "CW-R"; break;
    case 8: return "RTTY-R"; break;
    case 0x17: return "DV"; break;
    default: return ""; break;
  }
}

// set frequency to radio
void set_frequency_rig_radio(unsigned int freq, struct radio *radio) {
  if (!radio->enabled) return;
  // check manual radio
  if (radio->rig_spec->cat_type == 3) {
    // manual rig
    set_frequency(freq, radio);
    radio->f_freqchange_pending=0;
    return;
  }

  send_freq_set_civ(radio, freq);  // send to civ port
  radio->freqchange_timer = 200;   // timeout for resending in the unit of ms
  radio->f_freqchange_pending = 1;
  radio->freq_target = freq;
}


// set frequency to radio_selected
void set_frequency_rig(unsigned int freq) {
  struct radio *radio;
  radio = radio_selected;
  set_frequency_rig_radio(freq, radio);
}

// internal memory buffer for civ 
byte civ_buf[65];
int civ_buf_idx = 0;

void add_civ_buf(byte c) {
  if (civ_buf_idx < 64) {
    civ_buf[civ_buf_idx++] = c;
  } else {
    return;
  }
}

void attach_interrupt_civ()
{
  // attach interrupt handler
  civ_reader.attach_ms(1, receive_civport_1);

}

void send_head_civ(struct radio *radio) {
  // header part sending
  clear_civ_buf();
  add_civ_buf((byte)0xfe);  // $FE
  add_civ_buf((byte)0xfe);
  add_civ_buf((byte)radio->rig_spec->civaddr);
  add_civ_buf((byte)0xe0);
}

void send_tail_civ(struct radio *radio) {
  // tail part sending
  add_civ_buf((byte)0xfd);
  send_civ_buf(radio->rig_spec->civport);
}


// ptt control
void set_ptt_rig(struct radio *radio, int on) {
  // set hardware PTT1 or 2
  switch (radio->rig_idx) {
  case 0: // MIC1 PTT ON
    ptt_switch(0,on);break;
  case 1: // MIC2 PTT ON
    ptt_switch(1,on);break;
  }
  
  switch (radio->rig_spec->cat_type) {
    case 0:  // ci-v

      send_head_civ(radio);
      add_civ_buf((byte)0x1c);  //
      add_civ_buf((byte)0x00);  //

      if (on) {
        add_civ_buf((byte)0x01);  //

      } else {
        add_civ_buf((byte)0x00);  //
      }
      send_tail_civ(radio);
      break;
    case 1:  // cat
      if (on) {
        send_cat_cmd(radio, "TX1;");
      } else {
        send_cat_cmd(radio, "TX0;");
      }
      break;
    case 2:  // kenwood cat ( QCX special command and not of Kenwood general)
      if (on) {
        send_cat_cmd(radio, "TQ1;");
      } else {
        send_cat_cmd(radio, "TQ0;");
      }
      break;
  }

  // if CW mode  also key on / off
  if (radio->modetype == LOG_MODETYPE_CW) {
    if (on) {
      // key on
      //  digitalWrite(LED, 1);
      keying(1);
      if (verbose & 1) {
        plogw->ostream->print("Key On");
      }
    } else {
      // key on
      // digitalWrite(LED, 0);
      keying(0);
      if (verbose & 1) {
        plogw->ostream->print("Key Off");
      }
    }
  }
}
void send_voice_memory(struct radio *radio, int num)
// num 0 cancel send 1  F1 CQ  2 F2 number 3 F3 TU  4 F4 CALLSIGN 5 F5 CALL+NUM
{
  char buf[10];
  switch (radio->rig_spec->cat_type) {
    case 0:
      if (!plogw->f_console_emu) {
        plogw->ostream->print("voice ");
        plogw->ostream->println(num);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x28);
      add_civ_buf((byte)0x00);
      add_civ_buf((byte)num);
      send_tail_civ(radio);
      break;
    case 1:  // cat
      sprintf(buf, "PB0%d;", num);
      send_cat_cmd(radio, buf);
      break;
  }
}

void send_rtty_memory(struct radio *radio, int num)
// num 0 cancel send 1  F1 CQ  2 F2 number 3 F3 TU  4 F4 CALLSIGN 5 F5 CALL+NUM
{
  char buf[100];
  switch (radio->rig_spec->cat_type) {
    case 0:
      if (!plogw->f_console_emu) {
        plogw->ostream->print("voice ");
        plogw->ostream->println(num);
      }
      send_head_civ(radio);
      add_civ_buf((byte)0x28);
      add_civ_buf((byte)0x00);
      add_civ_buf((byte)num);
      send_tail_civ(radio);
      break;
    case 1:  // cat
      if (!plogw->f_console_emu) plogw->ostream->println("rtty encoder");
      sprintf(buf, "EN0%d;", num);
      send_cat_cmd(radio, buf);
      break;
  }
}


void clear_civ_buf() {
  civ_buf_idx = 0;
}

void send_civ_buf(Stream *civport) {
  if (civport == NULL) {
    // send to Ftdi USB serial port
    // send to USB host serial adapter
    usb_send_civ_buf() ;

  } else {
    // normal serial port write
    //		for (int i=0;i<civ_buf_idx;i++) {
    //			civport->write(civ_buf[i]);
    //		}
    civport->write(civ_buf, civ_buf_idx);
  }
  clear_civ_buf();
}

void send_cat_cmd(struct radio *radio, char *cmd) {
  //  if (radio->rig_spec->cat_type != 1) return ;
  // check port
  if (radio->rig_spec->civport != NULL) {
    // send to serial
    radio->rig_spec->civport->print(cmd);
    if (verbose & 1) {
      plogw->ostream->print("send cat cmd serial:");
      plogw->ostream->println(cmd);
    }
    return;
  } else {
    usb_send_cat_buf(cmd);
  }
}

void receive_cat_data(struct radio *radio) {


  if (!radio->enabled) return;

  // check port

  if (radio->rig_spec->civport != NULL) {

    //    if (verbose & 1) plogw->ostream->println("receive_cat_data()");
    // receive from serial
    char c;
    int count;
    count=0;
    while (radio->rig_spec->civport->available()) {
      c = radio->rig_spec->civport->read();
      if (!isprint(c)) continue;
      radio->bt_buf[radio->w_ptr] = c;
      radio->w_ptr++;
      radio->w_ptr %= 256;
      count++;
    }
    if (count>0) {
      //      plogw->ostream->print("receive_cat_data():");
      //      plogw->ostream->println(count);
    }
    //    // record max number of received characters during the civport interrupt for debugging
    //    if (count>receive_civport_count) receive_civport_count=count;
    
    return;
  } else {
    usb_receive_cat_data(radio);
  }
}


// return with mode specific frequency width 
int freq_width_mode(char *opmode)
{
  int mode;
  int span;
  mode = rig_modenum(opmode);
  //  Serial.println(opmode);
  //  Serial.println(mode);
  span =1000/FREQ_UNIT; //default
      switch (mode) {
      case 3:  // CW
          span = 500/FREQ_UNIT;  // 500Hz
	  break;
      case 7:
          span = 500/FREQ_UNIT;  // 500Hz 
          break;
      case 0: // SSB
          span = 1000/FREQ_UNIT;  // 3 k
	  break;
      case 1: 
          span = 1000/FREQ_UNIT;  // 3 k
          break;
      case 5:              // FM
	span = 20000/FREQ_UNIT;  // 20k
	break;
      case 6:              // WFM
	span = 20000/FREQ_UNIT;  // 20k
	break;
      }
      return span;
}

void set_scope() {
  int mode;
  int span;
  struct radio *radio;
  radio = radio_selected;
  char buf[20];
  mode = rig_modenum(radio->opmode);
  span=0;
  switch (radio->rig_spec->cat_type) {
    case 0:  // icom

      send_head_civ(radio);
      add_civ_buf((byte)0x27);
      add_civ_buf((byte)0x15);
      switch (mode) {
        case 3:  // CW
        case 7:
          span = 0x10000;  // 10k
          break;
        case 0:
        case 1:
          span = 0x25000;  // 25 k
          break;
        case 5:              // FM
        case 6:              // WFM
          span = 0x0250000;  // 250k
          break;
      }
      add_civ_buf((byte)0x00);
      for (int i = 0; i < 5; i++) {
        add_civ_buf((byte)(span & 0xff));
        span = span >> 8;
      }
      send_tail_civ(radio);

      break;
    case 1:  // cat
      switch (mode) {
        case 3:  // CW
        case 7:
          span = 3;  // 10k
          break;
        case 0:
        case 1:
          span = 5;  // 50k
          break;
        case 5:      // FM
        case 6:      // WFM
          span = 6;  // 100k
          break;
      }
      sprintf(buf, "SS05%d0000;", span);
      send_cat_cmd(radio, buf);
      break;
  }
}

void send_rit_setting(struct radio *radio, int rit, int xit) {
  int type;
  int sign;
  type = radio->rig_spec->cat_type;
  char buf[30];
  switch (type) {
    case 1:  // Yaesu
      if (freq < 0) return;
      sprintf(buf, "CF000%c%c000;", (rit == 1) ? '1' : '0', (xit == 1) ? '1' : '0');
      send_cat_cmd(radio, buf);
      return;
    case 2:
      // kenwood TS480
      return;
    case 0:  // icom ci-v
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // RIT
      add_civ_buf((byte)0x01);  //  on/off
      add_civ_buf(rit ? 0x01 : 0x00);
      send_tail_civ(radio);
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // XIT
      add_civ_buf((byte)0x02);  //  on/off
      add_civ_buf(xit ? 0x01 : 0x00);
      send_tail_civ(radio);
      return;
  }
}


// RIT/dTX frequency set to the rig (freq offset in freq)
void send_rit_freq_civ(struct radio *radio, int freq) {
  int type;
  int sign;
  type = radio->rig_spec->cat_type;
  char buf[30];
  
  freq=freq*FREQ_UNIT; 
  if (freq < 0) {
    sign = 1;
    freq = -freq;
  } else {
    sign = 0;
  }

  switch (type) {
    case 1:  // Yaesu
      if (freq < 0) return;
      sprintf(buf, "CF001%c%04d;", (sign == 0) ? '+' : '-', freq);
      send_cat_cmd(radio, buf);
      return;
    case 2:
      // kenwood TS480
      return;
    case 0:  // icom ci-v
      send_head_civ(radio);
      add_civ_buf((byte)0x21);  // RIT frequency
      add_civ_buf((byte)0x00);  //

      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 10 Hz
      add_civ_buf((byte)dec2bcd(freq % 100));
      freq = freq / 100;  // 1kHz
      add_civ_buf((byte)sign);
      send_tail_civ(radio);
      return;
  }
}

void send_freq_set_civ(struct radio *radio, unsigned int freq) {


  // check transverter frequency
  for (int i = 0; i < NMAX_TRANSVERTER; i++) {
    if (radio->rig_spec->transverter_freq[i][0] == 0) continue;  // not defined
    if (!radio->rig_spec->transverter_enable[i]) continue;
    if ((freq >= radio->rig_spec->transverter_freq[i][0]) && (freq <= radio->rig_spec->transverter_freq[i][1])) {
      // transverter frequency
      freq = freq - (radio->rig_spec->transverter_freq[i][0] - radio->rig_spec->transverter_freq[i][2]);
      break;
    }
  }
  if (!plogw->f_console_emu) {  
    Serial.print("FREQ=");
    Serial.println(freq);
  }
  int type;
  type = radio->rig_spec->cat_type;
  char buf[30];
  switch (type) {
    case 1:
      if (freq < 0) return;
      sprintf(buf, "FA%09lld;", ((long long)freq*FREQ_UNIT));
      send_cat_cmd(radio, buf);
      return;
    case 2:
      // kenwood TS480 uses 11 digit
      if (freq < 0) return;
      sprintf(buf, "FA%011lld;", ((long long)freq*FREQ_UNIT));
      send_cat_cmd(radio, buf);
      return;
    case 3:
      return;
  }

  // plogw->ostream->print("send_freq_set_civ()"); plogw->ostream->println(freq);
  if (freq <= 0) {
    // ignore
    if (!plogw->f_console_emu) plogw->ostream->println("<0, ignore sending freq");
    return;
  }


  send_head_civ(radio);
  add_civ_buf((byte)0x05);  // Frequency !
  add_civ_buf((byte)dec2bcd(freq % (100/FREQ_UNIT)));
  freq = freq / (100/FREQ_UNIT);  // 10 Hz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 1kHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 100kHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 10MHz
  add_civ_buf((byte)dec2bcd(freq % 100));
  freq = freq / 100;  // 1GHz
  send_tail_civ(radio);
}




void send_mode_set_civ(const char *opmode, int filnr) {
  struct radio *radio;
  radio = radio_selected;

  int mode;
  mode = rig_modenum(opmode);
  if (mode < 0) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("No matching mode");
      plogw->ostream->println(opmode);
    }
    return;
  }

  switch (radio->rig_spec->cat_type) {
    case 1:
      // cat
      switch (mode) {
        case 0:  // LSB
          send_cat_cmd(radio, "MD01;");
          break;
        case 1:  // USB
          send_cat_cmd(radio, "MD02;");
          break;
        case 2:  // AM
          send_cat_cmd(radio, "MD05;");
          break;
        case 3:  // CW
          send_cat_cmd(radio, "MD03;");
          break;
        case 4:  // RTTY-LSB
          send_cat_cmd(radio, "MD06;");
          break;
        case 5:  // FM
          send_cat_cmd(radio, "MD04;");
          break;
        case 6:  // WFM
          send_cat_cmd(radio, "MD0B;");
          break;
        case 7:  // CW-R
          send_cat_cmd(radio, "MD07;");
          break;
        case 8:  // RTTY-R
          send_cat_cmd(radio, "MD09;");
          break;
        case 0x17:  // DV
          break;
      }
      break;
    case 2:
      // kenwood TS-480
      switch (mode) {
        case 3:                         // CW
          send_cat_cmd(radio, "MD3;");  // OK?
          break;
        case 0:  // LSB
          send_cat_cmd(radio, "MD1;");  // OK?	  
          break;
        case 1:  // USB
          send_cat_cmd(radio, "MD2;");  // OK?	  
          break;
        case 2:  // AM
          send_cat_cmd(radio, "MD5;");  // OK?	  
          break;
        case 4:  // RTTY-LSB
          send_cat_cmd(radio, "MD6;");  // OK?	  
          break;
        case 5:  // FM
          send_cat_cmd(radio, "MD4;");  // OK?	  
          break;
        case 6:  // WFM
          break;
        case 7:  // CW-R
          send_cat_cmd(radio, "MD7;");  // OK?	  
          break;
        case 8:  // RTTY-R
          send_cat_cmd(radio, "MD9;");  // OK?	  
          break;
        case 0x17:  // DV
          break;
      }
      break;
    case 0:
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // Frequency !
      add_civ_buf((byte)mode);  // mode data
      if (filnr <= 0) filnr = 1;
      add_civ_buf((byte)filnr);  // filter data;
      send_tail_civ(radio);
      break;
  }
}

void send_freq_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:
    case 2:
      send_cat_cmd(radio, "IF;");
      return;
    case 3:
      return;
  }
  // icom
  // query frequency
  send_head_civ(radio);
  add_civ_buf((byte)0x03);  // Frequency !
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response

  // query mode
  //  send_head_civ();
  // civport->write((byte)0x04);                  // mode
  // send_tail_civ();
}

void send_mode_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:  // yaesu cat
      send_cat_cmd(radio, "IF;");
      return;
    case 2:
    case 3:
      return;
  }
  // icom
  // mode frequency
  send_head_civ(radio);
  add_civ_buf((byte)0x04);  // mode !
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_rotator_head_civ(int from, int to) {
  Serial2.write((byte)0xfe);  // $FE
  Serial2.write((byte)0xfe);
  Serial2.write((byte)to);    // 2
  Serial2.write((byte)from);  // 3 to
}

void send_rotator_tail_civ() {
  Serial2.write((byte)0xfd);  // 7 tail
}

void send_rotator_az_query_civ() {
  // rotator is always connected to civ port Serial2
  // header part sending
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);  // 4 rotator command
  Serial2.write((byte)0x00);  //  5 rotator sub command query
  send_rotator_tail_civ();
  if (verbose & 2) plogw->ostream->println("send_rotator_az_query_civ()");
}

void send_rotator_az_set_civ(int az) {
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);               // 4 rotator command
  Serial2.write((byte)0x01);               //  5 rotator sub command query
  Serial2.write((byte)dec2bcd(az % 100));  //  6 data0
  az = az / 100;
  Serial2.write((byte)dec2bcd(az % 100));  //  7 data1
  send_rotator_tail_civ();
}
// rotator commands
// 00 query az
// 01 set target az
// 02 set north
// 03 set south
// 04 enable tracking
// 05 set type to the second arg. (0 Yaesu 1 DC motor)

void send_rotator_command_civ(byte *cmds, int n) {
  send_rotator_head_civ(0xe0, 0x91);
  Serial2.write((byte)0x31);  // 4 rotator command
  for (int i = 0; i < n; i++) {
    Serial2.write((byte)*cmds);  //  5 rotator sub command query
    cmds++;
  }
  send_rotator_tail_civ();
}


// check tx/r
void send_ptt_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:
      send_cat_cmd(radio, "TX;");
      return;
    case 2:  // do nothing  and get status from IF query result
      return;
  }

  if (radio->modetype != LOG_MODETYPE_CW) {
    // set TX status output for phone
    send_head_civ(radio);
    add_civ_buf((byte)0x24);  //
    add_civ_buf((byte)0x00);
    add_civ_buf((byte)0x00);  // set TX status output
    add_civ_buf((byte)0x01);  // on
    send_tail_civ(radio);
    if (verbose & 256) plogw->ostream->println("TX query");
    radio->f_civ_response_expected = 1;
    radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
  }
}

void send_preamp_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:
      send_cat_cmd(radio, "PA0;");
      return;
    case 2:
      return;
    case 3:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x16);  //
  add_civ_buf((byte)0x02);  // preamp query
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}

void send_identification_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:
      send_cat_cmd(radio, "ID;");  //0670 FT991A
      return;
    case 2:
      return;
    case 3:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x19);  // id query  //5416725060A2FD
  add_civ_buf((byte)0x00);  // id query
  //
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


void send_att_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  switch (type) {
    case 1:
      send_cat_cmd(radio, "RA0;");
      return;
    case 2:
      return;
    case 3:  // manual radio do nothing
      return;
  }

  send_head_civ(radio);
  add_civ_buf((byte)0x11);  // att query
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


void send_smeter_query_civ(struct radio *radio) {
  if (!radio->enabled) return;
  int type;
  type = radio->rig_spec->cat_type;
  
  switch (type) {
  case 1: // yeasu
  case 2: // kenwood
    send_cat_cmd(radio, "SM0;");
    return;
  case 3:  // manual radio do nothing
    return;
  }

  //  plogw->ostream->println("send_smeter_query_civ()");
  send_head_civ(radio);
  add_civ_buf((byte)0x15);  //
  add_civ_buf((byte)0x02);  // smeter level 0000 S0 0120 S9 0241 S9+60
  send_tail_civ(radio);
  radio->f_civ_response_expected = 1;
  radio->civ_response_timer = 50;  // 0.1 s wait for (any) response
}


// set frequency (received from rig) to the logging system
void set_frequency(int freq, struct radio *radio) {


  if (verbose & 1) plogw->ostream->println("set_frequency()");
  if ((radio->f_freqchange_pending) && !((plogw->sat) || (radio->rig_spec->cat_type == 3))) {  // not sat and pending
    //if (1 == 0) {
    if (verbose & 1) plogw->ostream->println("freq_change_pending");
    // wait until received freq is the same as the target plogw->freq (already changed by the program)
    if (radio->freq_target == freq) {
      // frequency set to rig completed
      radio->f_freqchange_pending = 0;
      radio->freqchange_timer = 0;
      radio->freqchange_retry = 0;
      radio->freq = radio->freq_target;
      radio->freq_prev = radio->freq;  // make the frequency tracking as normal
    } else {
      if (radio->freqchange_timer == 0) {
        // timeout
        radio->freqchange_retry++;
        if (radio->freqchange_retry < 4) {
          set_frequency_rig(radio->freq_target);
        } else {
          radio->f_freqchange_pending = 0;  // abandon
          radio->freqchange_retry = 0;
          return;
        }
      }
    }
  } else {
    radio->f_freqchange_pending = 0;  // adhoc
    // set received frequency
    radio->freq_prev = radio->freq;
    radio->freq = freq;
    radio->bandid = freq2bandid(radio->freq);
    if (radio->bandid != radio->bandid_prev) {
      // band change
      radio->bandid_prev = radio->bandid;
      // also move the bandmap display band
      radio->bandid_bandmap = radio->bandid;
      if (!plogw->f_console_emu) plogw->ostream->println("band change");
      //upd_display_bandmap();
      bandmap_disp.f_update = 1;  // just request update flag (updated in interval jobs
    }
    if (radio->freq != radio->freq_prev) {
      // operating frequency (dial) moved or band change
      // check cq/sat status to set mode appropriately
      if (verbose & 1) plogw->ostream->println("freq changed");
      if (plogw->sat == 0) {
        if (radio->cq[radio->modetype]) {
          // frequency change in CQ and non-sat detected
          // save previous frequency to memorize as cq frequency
          radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->freq_prev;
          radio->cq[radio->modetype] = LOG_SandP;  // change to s&p
        } else {
          // s&p  and frequency(dial) moved situation
          // if dupe  wipe the qso
          if (radio->dupe) {
            wipe_log_entry();
            radio->dupe = 0;
            upd_display();
          }
        }
        // update bandmap display
        //upd_display_bandmap();
        bandmap_disp.f_update = 1;  // just request update flag (updated in interval jobs

      } else {
        // satellite mode
        // set frequency to uplink down link frequency
        switch (plogw->sat_vfo_mode) {
          case SAT_VFO_SINGLE_A_RX:  //
            if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
              plogw->dn_f = radio->freq*FREQ_UNIT; // satellite related frequency is in Hz unit (not FREQ_UNIT Hz)
            }
            break;
          case SAT_VFO_SINGLE_A_TX:  // received frequency is TX
            if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
              plogw->up_f = radio->freq*FREQ_UNIT;
            }
            break;
          case SAT_VFO_MULTI_TX_0:
            if (radio->rig_idx == 0) {
              // received frequency is TX
              if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
                plogw->up_f = radio->freq*FREQ_UNIT;
              }
            }
            if (radio->rig_idx == 1) {
              // received frequency is RX
              if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
                plogw->dn_f = radio->freq*FREQ_UNIT;
              }
            }
            break;
          case SAT_VFO_MULTI_TX_1:  //
            if (radio->rig_idx == 1) {
              // received frequency is TX
              if (plogw->sat_freq_tracking_mode == SAT_TX_FIX) {
                plogw->up_f = radio->freq*FREQ_UNIT;
              }
            }
            if (radio->rig_idx == 0) {
              // received frequency is RX
              if (plogw->sat_freq_tracking_mode == SAT_RX_FIX) {
                plogw->dn_f = radio->freq*FREQ_UNIT;
              }
            }
            break;
        }
      }
    }
  }
}

void set_mode_nonfil(const char *opmode, struct radio *radio) {
  strcpy(radio->opmode, opmode);

  radio->modetype = modetype_string(radio->opmode);
  if (radio->f_tone_keying && radio->modetype == LOG_MODETYPE_PH ) {
    // tone keying flag set  in PH modes is regarded as CW operattion 24/7/28
    radio->modetype = LOG_MODETYPE_CW;
  } 
  if (radio->modetype != radio->modetype_prev) {
    set_log_rst(radio);  // adjust RST according to the mode
  }
  radio->modetype_prev = radio->modetype;
}

void set_mode(const char *opmode, byte filt, struct radio *radio) {
  // struct radio *radio;
  // radio=radio_selected;

  set_mode_nonfil(opmode, radio);
}

void check_repeat_function() {
  struct radio *radio;
  radio = &radio_list[plogw->repeat_func_radio];
  if (radio->modetype != LOG_MODETYPE_CW) {
    // not CW
    if (plogw->f_repeat_func_stat == 1 && radio->ptt_stat_prev >= 1 && radio->ptt_stat == 0) {
      //
      plogw->f_repeat_func_stat = 2;
      plogw->repeat_func_timer = plogw->repeat_func_timer_set;
    }
  }
  if (verbose & 256) {
    // debug repeat function
    plogw->ostream->print("repeat: stat ");
    plogw->ostream->print(plogw->f_repeat_func_stat);
    plogw->ostream->print(" tmr ");
    plogw->ostream->print(plogw->repeat_func_timer);
    plogw->ostream->print(" radio ");
    plogw->ostream->print(plogw->repeat_func_radio);
    plogw->ostream->print(" ptt ");
    plogw->ostream->print(radio->ptt);
    plogw->ostream->print(" ptt_stat ");
    plogw->ostream->println(radio->ptt_stat);
  }
}

// analyze cmdbuf as Kenwood Cat response
void get_cat_kenwood(struct radio *radio) {

  if (!radio->enabled) return;
  int len;
  len=strlen(radio->cmdbuf);

  int tmp;
  int filt;

  //plogw->ostream->println(radio->cmdbuf);
  if (verbose&512) {
    plogw->ostream->print("get_cat_kenwood() l=");
    plogw->ostream->print(len);
    plogw->ostream->print(":");            
    print_cat_cmdbuf(radio);
  }
  
  if (strncmp(radio->cmdbuf, "IF", 2) == 0) {
    // information
    if (len!=38) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("!IF kenwood len ignored: ");
	plogw->ostream->println(len);
      }
      return;
    }

    if (verbose & 1) plogw->ostream->print("IF received");
    freq = 0;
    //
    for (int i = 0; i < 11-(FREQ_UNIT==10 ? 1 : 0 ); i++) {
	freq *= 10;
	freq += radio->cmdbuf[i + 2] - '0';
    }
    // check frequency range of the TRX
    if (freq <= 1800000UL/FREQ_UNIT || freq >= 1100000000UL) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored");
      }
      return;
    }
    set_frequency(freq, radio);
    // set mode
    switch (radio->cmdbuf[29]) {
      case '1':  // LSB
        sprintf(opmode, "LSB");
        break;
      case '2':  // USB
        sprintf(opmode, "USB");
        break;
      case '3':  // CW
        sprintf(opmode, "CW");
        break;
      case '4':  // FM
        sprintf(opmode, "FM");
        break;
      case '5':  // AM
        sprintf(opmode, "AM");
        break;
      case '6':  // RTTY-LSB
        sprintf(opmode, "RTTY");
        break;
      case '7':  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case '9':  // RTTY-USB
        sprintf(opmode, "RTTY-R");
        break;
      case '8':  // DATA-R
      case 'A':  // DATA-FM
      case 'B':  // FM-NB
        sprintf(opmode, "Other");
        break;
        break;
    }
    filt = 1;
    //  if (radio->rig_idx == 0) {
    radio->filt = filt;
    //  }

    //   if (radio->rig_idx == 0) {

    set_mode(opmode, filt, radio);
    //    }
    // check tx/rx status
    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[28] == '1') {
      // TX
      radio->ptt_stat = 1;
    } else {
      radio->ptt_stat = 0;
    }
    check_repeat_function();

  } else if (strncmp(radio->cmdbuf, "SM", 2) == 0) {
    // read meter
    // could be HEX ?
    // QCX case 5 digit hex number
    // kenwood TS-480 case, could be HEX 4 digit number
    tmp = 0;
    for (int i = 0; i < 5; i++) {
      tmp *= 10;
      tmp += radio->cmdbuf[i + 2] - '0';
    }

    radio->smeter = tmp;
    conv_smeter(radio);
    smeter_postprocess(radio);
    //   }

  } else {
    return;
  }
  //  if (radio->rig_idx == plogw->focused_radio) {
    upd_display();
    //  }
}

void print_cat_cmdbuf(struct radio *radio)
{
  if (!plogw->f_console_emu) {  
    plogw->ostream->print("cmdbuf len:");
    plogw->ostream->print(strlen(radio->cmdbuf));
    plogw->ostream->print(":");      
    plogw->ostream->println(radio->cmdbuf);
  }

}

// analyze cmdbuf as Yaesu Cat response
void get_cat(struct radio *radio) {
  if (!radio->enabled) return;
  int len;
  len=strlen(radio->cmdbuf);
  int tmp;
  int filt;
  if (verbose&512) {
    print_cat_cmdbuf(radio);
  }
  
  if (strncmp(radio->cmdbuf, "IF", 2) == 0) {
    // information
    // check length
    switch (len) {
    default:
      //    if (len!=28) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("!IF len ignored");
	plogw->ostream->println(len);
      }
      return;
      break;
      //    }
    case 28: // FT-991 FTDX10
      freq = 0;
      for (int i = 0; i < 9-(FREQ_UNIT==10 ? 1 : 0 ); i++) { // FREQ_UNIT=10
	freq *= 10;
	freq += radio->cmdbuf[i + 5] - '0';
      }
      break;
    case 27: // FT-3000
      freq = 0;
      for (int i = 0; i < 8-(FREQ_UNIT==10 ? 1 : 0 ); i++) { // FREQ_UNIT=10
	freq *= 10;
	freq += radio->cmdbuf[i + 5] - '0';
      }
      break;
    }
    //            012340123456789
    //print_cat():IF001007012640+000000300000;
    //Freq:7012640 Mode:CW

    // check frequency range of the TRX
    
    if (freq <= 1800000UL/FREQ_UNIT || freq >= 11000000000UL) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored");
      }
      return;
    }

    set_frequency(freq, radio);
    // set mode
    switch (radio->cmdbuf[len-7]) { // was 21
      case '1':  // LSB
        sprintf(opmode, "LSB");
        break;
      case '2':  // USB
        sprintf(opmode, "USB");
        break;
      case '3':  // CW
        sprintf(opmode, "CW");
        break;
      case '4':  // FM
        sprintf(opmode, "FM");
        break;
      case '5':  // AM
        sprintf(opmode, "AM");
        break;
      case '6':  // RTTY-LSB
        sprintf(opmode, "RTTY");
        break;
      case '7':  // CW-R
        sprintf(opmode, "CW-R");
        break;
      case '9':  // RTTY-USB
        sprintf(opmode, "RTTY-R");
        break;
      case '8':  // DATA-R
      case 'A':  // DATA-FM
      case 'B':  // FM-NB
        sprintf(opmode, "Other");
        break;
        break;
    }
    filt = 1;
    //  if (radio->rig_idx == 0) {

    radio->filt = filt;
    //  }
    // if (radio->rig_idx == 0) {

    set_mode(opmode, filt, radio);
    // }
  } else if (strncmp(radio->cmdbuf, "SM", 2) == 0) {
    // read meter
    tmp = 0;
    for (int i = 0; i < 3; i++) {
      tmp *= 10;
      tmp += radio->cmdbuf[i + 3] - '0';
    }

    radio->smeter = tmp;
    conv_smeter(radio);
    smeter_postprocess(radio);
    //  }

  } else if (strncmp(radio->cmdbuf, "TX", 2) == 0) {
    // ptt stat
    // sequence
    // 0 R -> 1 PTT send -> 2 PTT release after TX
    //    if (radio->rig_idx == 0) {

    radio->ptt_stat_prev = radio->ptt_stat;
    if (radio->cmdbuf[2] == '0') {
      //plogw->ostream->print("0");
      if ((radio->ptt_stat == 1) || (radio->ptt_stat == 2)) {
        // previously sending
        radio->ptt_stat = 2;
      } else {
        radio->ptt_stat = 0;  // receiving
      }
    } else {
      radio->ptt_stat = 1;  // transmitting
      //plogw->ostream->print("1");
    }
    check_repeat_function();

  } else if (strncmp(radio->cmdbuf, "RA", 2) == 0) {
    // RF attenuator
    switch (radio->cmdbuf[3]) {
      case '0':  // ATT off
        radio->att = 0;
        break;
      case '1':  // ATT ON 6dB
        radio->att = 6;
        break;
      case '2':  // ATT 12dB
        radio->att = 12;
        break;
      case '3':  // ATT 18dB
        radio->att = 18;
        break;
    }
    if (verbose & 512) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("ATT:");
	plogw->ostream->println(radio->att);
      }
    }

  } else if (strncmp(radio->cmdbuf, "PA", 2) == 0) {
    // PRE-AMP
    radio->preamp = radio->cmdbuf[3] - '0';
    if (verbose & 512) {
      plogw->ostream->print("preamp:");
      plogw->ostream->println(radio->preamp);
    }
  } else if (strncmp(radio->cmdbuf, "ID", 2) == 0) {
    //
    if (radio != NULL) {
      *radio->rig_spec->rig_identification = '\0';
      strncat(radio->rig_spec->rig_identification, radio->cmdbuf + 2, 4);
      if (verbose & 512) {
        plogw->ostream->print("ID:");
        plogw->ostream->println(radio->cmdbuf);
        plogw->ostream->print("ID copied:");
        plogw->ostream->println(radio->rig_spec->rig_identification);
      }
    }
  }

  //  } else {
  //    return ;
  //  }
  if (radio->rig_idx == plogw->focused_radio) {

    upd_display();
  }
}

void conv_smeter(struct radio *radio) {
  if (plogw->show_smeter == 2) {
    if (radio->smeter == 0) {
      radio->smeter = SMETER_MINIMUM_DBM;
    }
    return;  // no conversion
  }
  if (radio->smeter == 0) {
    radio->smeter = SMETER_MINIMUM_DBM;  // no smeter reading -> -200dBm
    return;
  }
  // convert smeter reading into dBm if possible
  switch (radio->rig_spec->rig_type) {
    case 0:  // IC-705
      if (radio->bandid > 7) {
        // 8: 144
        // 144 and up
        if (verbose & 512) plogw->ostream->println("144MHz and Up");
        if (radio->smeter >= 125) {
          radio->smeter = (0.5 * (radio->smeter*1.0) - 141.5) * SMETER_UNIT_DBM; // high map
        } else {
          radio->smeter = (0.2313 * (radio->smeter*1.0) - 108.0) * SMETER_UNIT_DBM; // low map
        }
        // correction ATT and preamp
        switch (radio->preamp) {
          case 0: break;
          case 1: radio->smeter -= 14 * SMETER_UNIT_DBM; break;
        }
      } else {
        // 50 MHz and down
        if (verbose & 512) plogw->ostream->println("50MHz and down");
        if (radio->smeter >= 123) {
          radio->smeter = (0.5108 * (radio->smeter*1.0) - 133.92) * SMETER_UNIT_DBM;
        } else {
          radio->smeter = (0.2072 * (radio->smeter*1.0) - 96.694) * SMETER_UNIT_DBM;
        }
        // correction ATT and preamp
        switch (radio->preamp) {
          case 0: break;
          case 1: radio->smeter -= 10 * SMETER_UNIT_DBM; break;
          case 2: radio->smeter -= 14 * SMETER_UNIT_DBM; break;
        }
        radio->smeter += radio->att * SMETER_UNIT_DBM;
      }
      break;
    case 1:  // IC-9700
      // 144 and up
      if (radio->smeter >= 121) {
        radio->smeter = (0.4934 * (radio->smeter*1.0) - 145.16) * SMETER_UNIT_DBM; // high map
      } else {
        radio->smeter = (0.1855 * (radio->smeter*1.0) - 108.1) * SMETER_UNIT_DBM; // low map
      }
      // correction ATT and preamp
      switch (radio->preamp) {
        case 0: break;
        case 1: radio->smeter -= 10.82 * SMETER_UNIT_DBM; break;
      }
      radio->smeter += radio->att * SMETER_UNIT_DBM;
      break;
    case 2:  // yaesu (measured for FTDX10 and FT991A)
      if (strncmp(radio->rig_spec->rig_identification, "0670", 4) == 0) {
        // FT991A
        if (verbose & 512) {
          plogw->ostream->println("FT991A");
        }
        if (radio->bandid <= 7) {
          // 50MHz and down
          if (verbose & 512) plogw->ostream->println("50MHz and down");
          if (radio->smeter >= 130) {  // =0.5026*T6-128.57
            radio->smeter = (0.5026 * (radio->smeter*1.0) - 128.57) * SMETER_UNIT_DBM;
          } else {  // =0.1819*T6-86.916
            radio->smeter = (0.1819 * (radio->smeter*1.0) - 86.916) * SMETER_UNIT_DBM;
          }
          if (radio->bandid == 7) {
            // 50MHz correction needed // -2.7247 dB
            radio->smeter += -2.7247 * SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 10.9 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 26.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
          break;
        } else {
          // 144MHz and up
          if (verbose & 512) plogw->ostream->println("144MHz and up ");
          if (radio->smeter >= 130) {  //=0.5006*T25-154.55
            radio->smeter = (0.5006 * (radio->smeter*1.0) - 154.55) * SMETER_UNIT_DBM;
          } else {  // =0.1758*T25-112.28
            radio->smeter = (0.1758 * (radio->smeter*1.0) - 112.28) * SMETER_UNIT_DBM;
          }
          // no preamp and ATT
        }
        break;


      } else if (strncmp(radio->rig_spec->rig_identification, "0761", 4) == 0) {
        // FTDX10 0761
        if (verbose & 512) {
          plogw->ostream->println("FTDX10");
        }

        if (radio->bandid > 5) {
          // 28MHz and up
          if (verbose & 512) plogw->ostream->println("28MHz and up");
          if (radio->smeter >= 129) {
            radio->smeter = (0.5031 * (radio->smeter*1.0) - 136.01) * SMETER_UNIT_DBM;
          } else {
            radio->smeter = (0.1655 * (radio->smeter*1.0) - 92.5) * SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 9.8 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 18.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
        } else {
          // 21MHz and down
          if (verbose & 512) plogw->ostream->println("21MHz and down");
          if (radio->smeter >= 126) {
            radio->smeter = (0.5058 * ((float)radio->smeter*1.0) - 128.08) * (float)SMETER_UNIT_DBM;
          } else {
            radio->smeter = (0.1655 * ((float)radio->smeter*1.0) - 85.188) * (float)SMETER_UNIT_DBM;
          }
          // correction ATT and preamp
          switch (radio->preamp) {
            case 0: break;
            case 1: radio->smeter -= 9.8 * SMETER_UNIT_DBM; break;
            case 2: radio->smeter -= 18.0 * SMETER_UNIT_DBM; break;
          }
          radio->smeter += radio->att * SMETER_UNIT_DBM;
        }
        break;
      }
  }
}

void smeter_postprocess(struct radio *radio)
{
  if (radio->smeter != 0 && radio->smeter != SMETER_MINIMUM_DBM) {
    // if smeter value is valid
    if (verbose & 512) {
      plogw->ostream->print("SMETER:");
      plogw->ostream->print(radio->smeter);
      plogw->ostream->print(" Radio:");
      plogw->ostream->println(radio->rig_idx);
    }
    if (!plogw->f_antalt_switched) {
      // sort data on the status of antenna relay
      int relay; relay = plogw->relay[0] + plogw->relay[1] * 2;
      if (relay >= 0 && relay <= 3) {
        radio->smeter_record[relay] = radio->smeter;
        if (plogw->f_rotator_enable) {
          radio->smeter_azimuth[relay] = plogw->rotator_az;
        } else {
          radio->smeter_azimuth[relay] = -1;
        }
        radio->f_smeter_record_ready = 1; // notify new record
      }
      // alternate antenna relay if necessary

    }

  }
  if (radio->smeter_stat == 1) {
    if (radio->smeter > radio->smeter_peak) radio->smeter_peak = radio->smeter;
  }
      if (plogw->f_antalt > 0) {
        plogw->antalt_count++;
        if (plogw->antalt_count >= plogw->f_antalt) {
          // alternate antenna relay1
          alternate_antenna_relay();
          plogw->antalt_count = 0;
        }
      }
  // reset flag
  plogw->f_antalt_switched = 0;
}



// 現在 CI-V バスが単一のリグ占有の前提となっているので、上手くハンドリング
// できないと考えられる。civport_shared[]に共有しているradio のリストを定義する等して、ここで他のradio の受信処理も行うのが適切？

void get_civ(struct radio *radio) {
  int filt;
  int tmp;
  //  u8x8.setFont(u8x8_font_chroma48medium8_r);
  // parse civ sentense and set information accordingly
  // check originated from rig
  if (!((radio->cmdbuf[0] == 0xfe) && (radio->cmdbuf[1] == 0xfe))) {
    //    plogw->ostream->printf("! preamble %02x %02x %02x \n",radio->cmdbuf[0],radio->cmdbuf[1],radio->cmdbuf[2]);

    // ci-v port see what this unit send, so sometimes results in corrupted self port ci-v packet output
    return;
  }
  if (radio->cmdbuf[3] != radio->rig_spec->civaddr) {
    if (radio->cmdbuf[3] == 0xe0) {
      // I myself packet
      return;
    }
    // check rotator
    if (verbose & 2) {
      plogw->ostream->print("!from addr=");
      plogw->ostream->println(radio->cmdbuf[3], HEX);
    }
    if (radio->cmdbuf[3] == 0x91) {
      // rotator
      if (radio->cmdbuf[4] == 0x31) {
        // rotator command
        switch (radio->cmdbuf[5]) {
	case 0:  // query az
	  tmp = bcd2dec(radio->cmdbuf[6]) + 100 * bcd2dec(radio->cmdbuf[7]);
	  if (tmp >= 0 && tmp <= 360) {
	    plogw->rotator_az = tmp;
	    if (verbose & 2) {
	      plogw->ostream->print("Rotator AZ=");
	      plogw->ostream->println(plogw->rotator_az);
	    }
	  }
	  break;
        }
      }
    }
    return;
  }
  //  plogw->ostream->println("get_civ() addr pass");
  radio->f_civ_response_expected = 0;
  radio->civ_response_timer = 0;
  switch (radio->cmdbuf[4]) {
  case 0:  // frequency
  case 3:
    // check size
    if (radio->cmd_ptr != 11) {
      plogw->ostream->print(int(radio->cmd_ptr));
      plogw->ostream->println("! CI-V size Freq");
      break;
    }
    // check postamble (FD)
    if (radio->cmdbuf[10] != 0xfd) {
      plogw->ostream->println("! postamble Freq");
      break;
    }
    //  plogw->ostream->println("get_civ() frequency");
    freq = 0;
    freq = bcd2dec(radio->cmdbuf[9]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[8]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[7]);
    freq = freq * 100 + bcd2dec(radio->cmdbuf[6]);
    freq = freq * (100/FREQ_UNIT) + bcd2dec(radio->cmdbuf[5])/FREQ_UNIT;


    //    Serial.print("Freq received=");
    //    Serial.println(freq);
    // check frequency range of the TRX
    if (freq < 1800000UL/FREQ_UNIT || freq >= 1100000000UL) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print("faulty freq =");
	plogw->ostream->print(freq);
	plogw->ostream->println(" ignored");
      }
      break;
    }
    // check transverter frequency
    radio->transverter_in_use = 0;
    for (int i = 0; i < NMAX_TRANSVERTER; i++) {
      if (radio->rig_spec->transverter_freq[i][0] == 0) break;
      if (!radio->rig_spec->transverter_enable[i]) continue;

      if ((freq >= radio->rig_spec->transverter_freq[i][2]) && (freq <= radio->rig_spec->transverter_freq[i][3])) {
	// transverter frequency
	freq = freq + (radio->rig_spec->transverter_freq[i][0] - radio->rig_spec->transverter_freq[i][2]);
	radio->transverter_in_use = 1;
	break;
      }
    }
    // store to rig information

    //

    set_frequency(freq, radio);

    //      lcd.setCursor(0, 0);
    break;
  case 0x11:  // ATT
    // check size
    if (radio->cmd_ptr != 7) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print(int(radio->cmd_ptr));
	plogw->ostream->println("! CI-V pkt size ATT");
      }
      break;
    }
    // check postamble (FD)
    if (radio->cmdbuf[6] != 0xfd) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->println("! postamble ATT");
      }
      break;
    }
    radio->att = bcd2dec(radio->cmdbuf[5]);
    if (verbose & 512) {
      plogw->ostream->print("ATT:");
      plogw->ostream->println(radio->att);
    }
    break;


  case 0x16:  //
    switch (radio->cmdbuf[5]) {
    case 0x02:  // preamp
      // check size
      if (radio->cmd_ptr != 8) {
	plogw->ostream->print(int(radio->cmd_ptr));
	plogw->ostream->println("! CI-V pkt size PRE");
	break;
      }
      // check postamble (FD)
      if (radio->cmdbuf[7] != 0xfd) {
	plogw->ostream->println("! postamble PRE");
	break;
      }
      radio->preamp = bcd2dec(radio->cmdbuf[6]);
      if (verbose & 512) {
	
	plogw->ostream->print("PREAMP:");
	plogw->ostream->println(radio->preamp);
      }
      break;
    }
    break;
  case 0x19:  // ID query
    if (verbose & 512) {
      plogw->ostream->print("CI-V ID:");
      plogw->ostream->print(radio->cmdbuf[5], HEX);
      plogw->ostream->print(radio->cmdbuf[6], HEX);
      plogw->ostream->println(radio->cmdbuf[7], HEX);
    }
    // check size
    if (radio->cmd_ptr != 8) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->print(int(radio->cmd_ptr));
	plogw->ostream->println("! CI-V pkt size ID");
      }
      break;
    }
    // check postamble (FD)
    if (radio->cmdbuf[7] != 0xfd) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->println("! postamble ID");
      }
      break;
    }

    break;
  case 0x15:  //
    // check subcmd
    switch (radio->cmdbuf[5]) {
    case 0x02:  // s meter
      // check size
      if (radio->cmd_ptr != 9) {
	if (!plogw->f_console_emu) {
	  plogw->ostream->print(int(radio->cmd_ptr));
	  plogw->ostream->println("! CI-V pkt size S");
	}
	break;
      }
      // check postamble (FD)
      if (radio->cmdbuf[8] != 0xfd) {
	if (!plogw->f_console_emu) {	
	  plogw->ostream->println("! postamble S");
	}
	break;
      }

      // read s-meter
      tmp = bcd2dec(radio->cmdbuf[6]);
      tmp = tmp * 100 + bcd2dec(radio->cmdbuf[7]);


      radio->smeter = tmp;
      conv_smeter(radio);
      smeter_postprocess(radio);

      //          radio->smeter = cmdbuf[6] * 256 + cmdbuf[7];  // s-meter reading is BCD not binary  corrected 21/12/30
      break;
    }
    break;
  case 0x24:  // TX stat
    // check size
    if (radio->cmd_ptr != 9) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print(int(radio->cmd_ptr));
	plogw->ostream->println("! CI-V pkt size TX");
      }
      break;
    }
    // check postamble (FD)
    if (radio->cmdbuf[8] != 0xfd) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->println("! postamble TX");
      }
      break;
    }

    if (verbose & 256) {
      plogw->ostream->print("TX");
      plogw->ostream->print((char)(radio->cmdbuf[5] + '0'));
      plogw->ostream->print((char)(radio->cmdbuf[6] + '0'));
      plogw->ostream->print((char)(radio->cmdbuf[7] + '0'));
    }
    if (radio->cmdbuf[6] == 1) {
      radio->ptt_stat_prev = radio->ptt_stat;
      if (radio->cmdbuf[7] == 1) {
	// TX
	radio->ptt_stat = 1;
      }
      if (radio->cmdbuf[7] == 0) {
	// RX
	radio->ptt_stat = 0;
      }
      check_repeat_function();
    }
    break;
  case 1:  // mode
  case 4:
    // check size
    if (radio->cmd_ptr != 8) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print(int(radio->cmd_ptr));
	plogw->ostream->println("! CI-V pkt size MODE");
      }
      break;
    }

    // check postamble (FD)
    if (radio->cmdbuf[7] != 0xfd) {
      if (!plogw->f_console_emu) {      
	plogw->ostream->println("! postamble MODE");
      }
      break;
    }
    switch (radio->cmdbuf[5]) {
    case 0:  // LSB
      sprintf(opmode, "LSB");
      break;
    case 1:  // USB
      sprintf(opmode, "USB");
      break;
    case 2:  // AM
      sprintf(opmode, "AM");
      break;
    case 3:  // CW
      sprintf(opmode, "CW");
      break;
    case 7:  // cw-r
      sprintf(opmode, "CW-R");
      break;
    case 4:  // RTTY
      sprintf(opmode, "RTTY");
      break;
    case 8:  // RTTY
      sprintf(opmode, "RTTY-R");
      break;
    case 5:  // FM
      sprintf(opmode, "FM");
      break;
    default:  // other
      sprintf(opmode, "Other");
      break;
    }
    filt = radio->cmdbuf[6];


    radio->filt = filt;
    if (verbose & 1) {
      plogw->ostream->print("filt:");
      plogw->ostream->println(filt);
    }

    // it is rx mode for satellite
    set_mode(opmode, filt, radio);
    break;
  }
  //
  if (radio->rig_idx == plogw->focused_radio) {
    upd_display();
  }
}

void print_civ(struct radio *radio) {
  if (!(verbose & 1)) return;
  char ostr[32];
  for (int i = 0; i < radio->cmd_ptr; i++) {
    sprintf(ostr, "%02X ", radio->cmdbuf[i]);
    plogw->ostream->print(ostr);
  }
  plogw->ostream->println("");
}

void print_cat(struct radio *radio) {
  if (!(verbose & 1)) return;
  if (!plogw->f_console_emu) {  
    plogw->ostream->print("print_cat():");

    for (int i = 0; i < radio->cmd_ptr; i++) {
      plogw->ostream->print(radio->cmdbuf[i]);
    }
    plogw->ostream->println("");
  }
}



int receive_civport_count=0; // debugging receive_civport

void receive_civport(struct radio *radio) {
  int type;
  type = radio->rig_spec->cat_type;
  if ((type == 1) || (type == 2) || (type == 3)) {
    // not ICOM CI-V
    // receive USB host ftdi port
    if (radio->rig_spec->civport == NULL) return;
    if (radio->rig_idx >=2) return; // software serial ports
    //    if (radio->rig_spec->civport == &Serial3) return; // software serial port may not be read from interrupt     
    receive_cat_data(radio);
    return;
  }
  if (radio->rig_spec->civport == NULL) return; // usb serial may not be read from interrupt
  if (radio->rig_idx >=2) return; // software serial ports
  //  if (radio->rig_spec->civport == &Serial3) return; // software serial port may not be read from interrupt 
  char c;
  int count;
  count=0;
  while (radio->rig_spec->civport->available() && (count<20)) {
    //plogw->ostream->write(SerialBT.read());
    c = radio->rig_spec->civport->read();
    radio->bt_buf[radio->w_ptr] = c;
    radio->w_ptr++;
    radio->w_ptr %= 256;
    count++;
  }
  // record max number of received characters during the civport interrupt for debugging
  if (count>receive_civport_count) receive_civport_count=count;
}

void receive_civport_1() {
  struct radio *radio;
  // check rig #0
  radio = &radio_list[0];
  if (radio->enabled) receive_civport(radio);
  // check rig#1
  if (radio_list[1].rig_spec_idx != radio_list[0].rig_spec_idx) {
    // ignore if same rig is set for the both rig_ctrl[0] and [1]
    // read
    radio = &radio_list[1];
    if (radio->enabled) receive_civport(radio);
  }
  // check rig #2
  if ((radio_list[2].rig_spec_idx != radio_list[0].rig_spec_idx) && (radio_list[2].rig_spec_idx != radio_list[1].rig_spec_idx)) {
    // ignore if same rig is set for the both rig_ctrl[0] and [1]
    // read
    radio = &radio_list[2];
    if (radio->enabled) receive_civport(radio);
  }
}

void clear_civ(struct radio *radio) {
  radio->cmd_ptr = 0;
  radio->cmdbuf[radio->cmd_ptr] = '\0';
}

// antenna switch commands
int antenna_switch_commands(char *cmd)
{
  if (strncmp(cmd, "ANT0", 4) == 0) {
    int tmp, tmp1;
    if (sscanf(cmd + 4, "%d", &tmp1) == 1) {
      RELAY_control(0, tmp1);
      plogw->ostream->print("ANT0 set ");
      plogw->ostream->println(tmp1);
    } else {
      plogw->ostream->println("ANT0/1 state(0/1)");
    }
    return 1;
  }
  if (strncmp(cmd, "ANT1", 4) == 0) {
    int tmp, tmp1;
    if (sscanf(cmd + 4, "%d", &tmp1) == 1) {
      RELAY_control(1, tmp1);
      plogw->ostream->print("ANT0 set ");
      plogw->ostream->println(tmp1);
    } else {
      plogw->ostream->println("ANT0/1 state(0/1)");
    }
    return 1;
  }
  return 0;
}

int antenna_alternate_command(char *s)
{
  if (strncmp(s, "ANTALT", 6) == 0) {
    int tmp1, tmp;
    tmp1 = sscanf(s + 6, "%d", &tmp);
    if (tmp1 == 1 && tmp >= 0) {
      plogw->f_antalt = tmp;
    } else {
      plogw->f_antalt = 1; // default number of signal reception before alternating antenna
    }
    plogw->antalt_count = 0; // reset counter
    plogw->ostream->print("antenna alternate (antalt)=");
    plogw->ostream->println((int)plogw->f_antalt);
    return 1;
  }
  return 0;
}

// signal command to print signal strength, ant selection and azimuth
int signal_command(char *s)
{
  if (strncmp(s, "SIGNAL", 6) == 0) {
    // toggle show signal status periodically
    plogw->f_show_signal = 1 - plogw->f_show_signal;
    plogw->ostream->print("show_signal=");
    plogw->ostream->println((int)plogw->f_show_signal);
    return 1;
  }
  return 0;
}
// rotator command
int rotator_commands(char *s)
{

  struct radio *radio;
  radio = radio_selected;
  if (strncmp(s, "ROT", 3) == 0) {
    int tmp, tmp1;
    // rotator commands
    // check subcmds
    if (strncmp(s + 3, "EN", 2) == 0) {
      // enable/disable rotator
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        // enable/disable rotator query
        plogw->f_rotator_enable = tmp;
        plogw->ostream->print("f_rotator_enable = ");
        plogw->ostream->println(plogw->f_rotator_enable);
      } else {
        plogw->ostream->println("rotator enable en0/1 ?");
      }
    }
    if (strncmp(s + 3, "TYPE", 4) == 0) {
      // rotator type set
      tmp1 = sscanf(s + 7, "%d", &tmp);

      plogw->ostream->print("sent rotator type;");
      plogw->ostream->println(tmp1);
      byte cmds[2];
      cmds[0] = 0x05;  // type
      cmds[1] = (byte)tmp1;
      send_rotator_command_civ(cmds, 2);
    }
    if (strncmp(s + 3, "NORTH", 5) == 0) {
      byte cmds[1];
      cmds[0] = 0x02;  // set north
      send_rotator_command_civ(cmds, 1);
      plogw->ostream->println("rotator set north command");
    }
    if (strncmp(s + 3, "SOUTH", 5) == 0) {
      byte cmds[1];
      cmds[0] = 0x03;  // set south
      send_rotator_command_civ(cmds, 1);
      plogw->ostream->println("rotator set south command");
    }

    if (strncmp(s + 3, "TR", 2) == 0) {
      // enable/disable rotator
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        // enable/disable rotator
        plogw->f_rotator_track = tmp;
        plogw->ostream->print("f_rotator_track = ");
        plogw->ostream->println(plogw->f_rotator_track);
        byte cmds[2];
        cmds[0] = 0x04;  // enable
        cmds[1] = (byte)plogw->f_rotator_track;
        send_rotator_command_civ(cmds, 2);
      } else {
        plogw->ostream->println("rotator tracking tr0/1 ?");
      }
    }
    if (strncmp(s + 3, "AZ", 2) == 0) {
      // set target azimuth
      tmp1 = sscanf(s + 5, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("send rotator target = ");
        plogw->ostream->println(tmp);
        if (tmp >= 0 && tmp <= 360) {
          plogw->rotator_target_az = tmp;
          send_rotator_az_set_civ(plogw->rotator_target_az);
        }
      } else {
        plogw->ostream->println("rotator en ?");
      }
    }
    if (strncmp(s + 3, "STEP", 4) == 0) {
      // sweep rotator slowly until reaching to the target azimuth
      tmp1 = sscanf(s + 7, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("sweep step = ");
		if (tmp>0) {
			plogw->rotator_sweep_step_default =tmp;
		} else {
			plogw->rotator_sweep_step_default = 2;
		}
		plogw->ostream->println(plogw->rotator_sweep_step_default);
	  }
	}
    if (strncmp(s + 3, "SWEEP", 5) == 0) {
      // sweep rotator slowly until reaching to the target azimuth
      tmp1 = sscanf(s + 8, "%d", &tmp);
      if (tmp1 == 1) {
        plogw->ostream->print("sweep target = ");
        plogw->ostream->println(tmp);
        if (tmp >= 0 && tmp <= 360) {
          int az;
          az = plogw->rotator_az;
          if (az > 180) az -= 360;
          if (tmp > 180) {
            tmp -= 360.0;
          }
          if (tmp > az) {
            plogw->rotator_sweep_step = plogw->rotator_sweep_step_default;
          } else {
            plogw->rotator_sweep_step = -plogw->rotator_sweep_step_default;
          }
          if (tmp < 0) tmp += 360;
          plogw->rotator_target_az_sweep = tmp;
          plogw->ostream->print("start rotator sweep at ");
          plogw->ostream->print(plogw->rotator_sweep_step);
          plogw->ostream->print(" deg step to ");
          plogw->ostream->println(plogw->rotator_target_az_sweep);

          plogw->rotator_sweep_timeout = millis()+100; // start sweeping right now
          plogw->f_rotator_sweep = 1; // start sweeping
        }
      } else {
        plogw->f_rotator_sweep = 0;
        plogw->ostream->println("rotator sweep cancelled ?");
      }
    }

    // show rotator information
    sprintf(dp->lcdbuf, "Rotator\nAZ=%d deg\nTarget=%d\nEN=%d TR=%d\n", plogw->rotator_az, plogw->rotator_target_az, plogw->f_rotator_enable, plogw->f_rotator_track);
    plogw->ostream->print(dp->lcdbuf);
    upd_display_info_flash(dp->lcdbuf);
    clear_buf(radio->callsign);
    return 1;
  }
  return 0;
}

// RELAY1, 2 control
void RELAY_control(int relay, int on)
{
  
  switch (relay) {
    case 0: // RELAY 1
      //      mcp.digitalWrite(RELAY1, on);
      plogw->relay[0] = on;
      break;
    case 1: // RELAY 2
      //      mcp.digitalWrite(RELAY2, on);
      plogw->relay[1] = on;
      break;
  }
}

void print_radio_info(int idx_radio)
{
  if (!plogw->f_console_emu) {
    char buf[30];
    sprintf(buf, "Rig%d:%s %c", radio_list[idx_radio].rig_idx,
            radio_list[idx_radio].rig_spec->name,
            radio_list[idx_radio].enabled ? '*' : ' ');
    plogw->ostream->println(buf);
  }

}

void switch_radios(int idx_radio, int radio_cmd)
// radio_cmd -1 switch to circulate
//           0- set specified number
{
  struct radio *radio;
  int tmp_rig_spec_idx;
  radio = &radio_list[idx_radio];
  tmp_rig_spec_idx=radio->rig_spec_idx;    
  int count=0;
  while (1) {
    if (radio_cmd == -1) {
      // + radio
      tmp_rig_spec_idx++;
      if (tmp_rig_spec_idx >= N_RIG) {
	tmp_rig_spec_idx = 0;
      }
    } else if (radio_cmd == -2) {
      // - radio
      tmp_rig_spec_idx--;
      if (tmp_rig_spec_idx == -1) {
	tmp_rig_spec_idx = N_RIG - 1;
      }
    } else {
      if (radio_cmd >= 0 && radio_cmd < N_RIG) {
	tmp_rig_spec_idx = radio_cmd;
      }
    }
    // check conflict
    int ret;
    ret=check_rig_conflict(radio->rig_idx,&rig_spec[tmp_rig_spec_idx]);
    if (ret== -1) {
      radio->rig_spec_idx=tmp_rig_spec_idx;
      select_rig(radio);
      print_radio_info(idx_radio);
      upd_disp_rig_info();
      upd_display();
      return;
    } else {
      sprintf(dp->lcdbuf,"config %s\nconflict %s",rig_spec[tmp_rig_spec_idx].name,
	      radio_list[ret].rig_spec->name);
      upd_display_info_flash(dp->lcdbuf);
      if (radio_cmd >= 0 && radio_cmd < N_RIG) {
	return ; // op not completed
      }
    }
    count++;
    if (count>N_RIG) { //no more rig available
      sprintf(dp->lcdbuf,"no more rig \nwe can switch.");
      upd_display_info_flash(dp->lcdbuf);
      return;
    }
      
  }

}

void alternate_antenna_relay()
{
  plogw->relay[0] = 1 - plogw->relay[0];
  RELAY_control(0, plogw->relay[0]);
}

void config_rig_serialport_param(struct radio *radio)
{
  int txPin,rxPin;
  txPin=16;
  rxPin=17;
  switch(radio->rig_spec->civport_num) {
  case -2: // MANUAL
    radio->rig_spec->civport=NULL; // 念押し
    return;
    break;
  case -1: // USB
    radio->rig_spec->civport=NULL; // 念押し
    return;
    break;
  case 0: // port0 console (should not set)
    radio->rig_spec->civport=NULL;    // 念押し
    return;
    break;
  case 1: // port1 BT
    rxPin=SERIAL1_RX;
    txPin=SERIAL1_TX;
    break;
  case 2: // port2 CIV
    rxPin=SERIAL2_RX;
    txPin=SERIAL2_TX;
    break;
  case 3: // port3 TTL-SER
    rxPin=CATTX;
    txPin=CATRX;
    break;
  }
  int baud;
  baud=radio->rig_spec->civport_baud;
  switch (radio->rig_idx) {
  case 0:// main
    Serial1.begin(baud,SERIAL_8N1,
		  rxPin,
		  txPin,
		  radio->rig_spec->civport_reversed);
    if (!plogw->f_console_emu) {    
        Serial.println("init Serial1");
    }
    break;
  case 1:// sub
    Serial2.begin(baud,SERIAL_8N1,
		  rxPin,
		  txPin,
		  radio->rig_spec->civport_reversed);
      if (!plogw->f_console_emu) {    
	Serial.println("init Serial2");
      }
    break;
  case 2:// subsub
    Serial3.begin(baud,SWSERIAL_8N1,rxPin,txPin,radio->rig_spec->civport_reversed,95,0);
    Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
      if (!plogw->f_console_emu) {    
	Serial.println("init Serial3");
      }
    break;
  }
}

void config_rig_serialport(struct radio *radio) {
  if (radio->rig_spec->civport!=NULL) {
    // deinit serialport
    switch(radio->rig_idx) {
    case 0:
      //      Serial1.end();
      plogw->ostream->println("deinit Serial1");
      radio->rig_spec->civport=NULL;
      break;
    case 1:
      //      Serial2.end();
      plogw->ostream->println("deinit Serial2");
      radio->rig_spec->civport=NULL;      
      break;
    case 2:
      //      Serial3.end();
      plogw->ostream->println("deinit Serial3");
      radio->rig_spec->civport=NULL;      
      break;
    }
  }

  // config serial port with new setting
  config_rig_serialport_param(radio);
  // attach serial ports if civport_num specifies serial
  if (radio->rig_spec->civport_num>=0) {
    switch(radio->rig_idx) {
    case 0: // rig 0 attach Serial1
      radio->rig_spec->civport=&Serial1;
      break;
    case 1: // rig 1 attach Serial2
      radio->rig_spec->civport=&Serial2;
      break;
    case 2: // rig 2 attach Serial3
      radio->rig_spec->civport=&Serial3;
      break;
    }
  } else if (radio->rig_spec->civport_num==-1) {
    // USB
    radio->rig_spec->civport=NULL;
  } else if (radio->rig_spec->civport_num==-2) {
    // MANUAL
    radio->rig_spec->civport=NULL;
  }

}

int check_rig_conflict(int rig_idx,struct rig *rig_spec)
{
  // check rig_idx and rig_spec with current radio 0-2 configurations if conflict of civ ports occurs (cw port conflict is allowed)
  if (rig_spec->civport_num ==-2)  return -1; // manual rig should not conflict
  for (int i =0;i<N_RADIO;i++) {
    if (i==rig_idx) continue;
    if (radio_list[i].rig_spec==NULL) continue;

    if (rig_spec->civport_num == radio_list[i].rig_spec->civport_num) {
      // conflict phys port
      return i;
    }
  }
  return -1; // no conflict
}

// set radio->rig_spec from radio->rig_spec_idx
void select_rig(struct radio *radio) {
  if (!plogw->f_console_emu) {
    plogw->ostream->print("select_rig() spec:");
    plogw->ostream->println(radio->rig_spec_idx);
  }
  // link the rig_spec to the radio structure
  radio->rig_spec = &rig_spec[radio->rig_spec_idx];
  strcpy(radio->rig_name + 2, radio->rig_spec->name);
  set_rig_spec_str_from_spec(radio); // reverse set rig_spec_string from rig_spec

  // set serial port characteristics
  config_rig_serialport(radio);
  
  radio->f_civ_response_expected = 0;
  radio->civ_response_timer = 0;
  if (radio->rig_spec->band_mask != 0x00) {
    radio->band_mask = radio->rig_spec->band_mask;
  }
  if (!plogw->f_console_emu) plogw->ostream->println("select_rig()end");
}

// initialization of KENWOOD cat over software serial 22/05/09 (for QCX mini)
void init_cat_kenwood() {
  //    Serial3.begin(38400, SWSERIAL_8N1, CATRX, CATTX, false, 95, 0); // original worked with hand wired jk1dvplog
  //Serial3.begin(38400, SWSERIAL_8N1, CATTX, CATRX, false, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, original polarity // worked qcx-mini
  Serial3.begin(38400, SWSERIAL_8N1, CATTX, CATRX, true, 95, 0); // tx/rx reversed for esp32-jk1dvplog reba,b board, reversed polarity  
  Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
  //  Serial3.begin(38400, SWSERIAL_8N1, CATRX, CATTX, true, 95, 0); // flip polarity
}

void init_cat_serialport()
{
  // remap to RX 16 TX 4  // BT serial for IC-705
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX, SERIAL1_TX);

  // remap to RX 32 TX 33 // CI-V port for ICOM rigs
  //  Serial2.begin(19200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
  // to support IC-820 set baudrate of CAT port to 9600BPS
  Serial2.begin(9600, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
}


void init_rig() {
  /// setting up rigs
  // clear transverter_setting for all rigs.

  // cwport 0: LED --> IO2 Not connected to keying in revA-C  boards 24/7/28 1: CW_KEY1 --> KEY1 2:CW_KEY2 --> KEY2
  for (int j = 0; j < N_RIG; j++) {
    rig_spec[j].band_mask = 0x0;
    rig_spec[j].civport=NULL;

    
    for (int i = 0; i < NMAX_TRANSVERTER; i++) {
      rig_spec[j].transverter_enable[i] = 0;
      rig_spec[j].transverter_freq[i][0] = 0;
      rig_spec[j].transverter_freq[i][1] = 0;
      rig_spec[j].transverter_freq[i][2] = 0;
      rig_spec[j].transverter_freq[i][3] = 0;
    }
  }
  rig_spec[0].name = "IC-705";
  rig_spec[0].cat_type = 0;  // civ
  rig_spec[0].civaddr = 0xa4;
  //  rig_spec[0].civport = &Serial1;
  rig_spec[0].civport_num=1; // BT
  rig_spec[0].civport_reversed=0; // normal
  rig_spec[0].civport_baud = 115200;    
  rig_spec[0].cwport = 2; // KEY2 reserve KEY1 for manual radio (tone_keying)
  rig_spec[0].rig_type = 0;
  rig_spec[0].pttmethod = 2;
  rig_spec[0].band_mask = 0x1800;

  // 2400 MHz transverter
  rig_spec[0].transverter_freq[0][0] = 2427000000ULL/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][1] = 2429000000ULL/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][2] = 437000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[0][3] = 439000000/FREQ_UNIT;
  // 1200MHz transverter
  rig_spec[0].transverter_freq[1][0] = 1294000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][1] = 1296000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][2] = 144000000/FREQ_UNIT;
  rig_spec[0].transverter_freq[1][3] = 146000000/FREQ_UNIT;

  // 5600 MHz transverter ( use 1200 transverter and 5600 transverter in cascade is not possible
  // 1280 MHz -> 5760 MHz but difficult to produce 1280 IF freq with 1200 MHz transverter (dip sw)

  rig_spec[1].name = "IC-9700";
  rig_spec[1].cat_type = 0;  // civ
  rig_spec[1].civaddr = 0xa2;
  //  rig_spec[1].civport = &Serial2;
  rig_spec[1].civport_num=2; // CIV
  rig_spec[1].civport_reversed=0; // normal
  rig_spec[1].civport_baud = 19200;      
  rig_spec[1].cwport = 2;
  rig_spec[1].rig_type = 1;
  rig_spec[1].pttmethod = 2;
  rig_spec[1].transverter_freq[0][0] = 0;
  rig_spec[1].band_mask = 0x047f;  // ~380


  rig_spec[2].name = "FT-991A";
  rig_spec[2].cat_type = 1;  // cat
  rig_spec[2].civaddr = 0;
  rig_spec[2].civport_num=-1; // USB
  rig_spec[2].civport_reversed=0; // normal
  rig_spec[2].civport_baud = 38400;      
  rig_spec[2].cwport = 2;  // test
  rig_spec[2].rig_type = 2;
  rig_spec[2].pttmethod = 2;
  rig_spec[2].transverter_freq[0][0] = 0;
  rig_spec[2].band_mask = 0x600;

  rig_spec[3].name = "FT-991A-SER";
  rig_spec[3].cat_type = 1;  // cat
  rig_spec[3].civaddr = 0;
  rig_spec[3].civport_num=3; // SER-TTL
  rig_spec[3].civport_reversed=1; // normal
  rig_spec[3].civport_baud = 38400;    
  rig_spec[3].cwport = 1;
  rig_spec[3].rig_type = 2;
  rig_spec[3].pttmethod = 2;
  rig_spec[3].transverter_freq[0][0] = 0;
  rig_spec[2].band_mask = 0x600;

  rig_spec[4].name = "QCX-MINI";
  rig_spec[4].cat_type = 2;  // kenwood cat port Serial3
  // y the transceiver for information
  //entered into the log, typically operating frequency, mode etc.
  // connection :
  /// tip QCX rxd ring QCX txd sleeve gnd
  // 38400 bps 8N1

  // FA7030000; set  VFOA
  // FR 0,1,2 ; VFO mode A,B, Split

  rig_spec[4].civaddr = 0;
  rig_spec[4].civport_num=3; // TTL-SER
  rig_spec[4].civport_reversed=0; // normal
  rig_spec[4].civport_baud = 38400;  
  rig_spec[4].cwport = 1;
  rig_spec[4].rig_type = 3;
  rig_spec[4].pttmethod = 2;
  rig_spec[4].transverter_freq[0][0] = 0;

  // manual rig
  rig_spec[5].name = "MANUAL";
  rig_spec[5].cat_type = 3;  // no cat
  rig_spec[5].civaddr = 0;
  rig_spec[5].civport_num=-2; // MANUAL
  rig_spec[5].civport_reversed=0; // normal
  rig_spec[5].civport_baud =0;      
  rig_spec[5].cwport = 1;
  rig_spec[5].rig_type = 4;
  rig_spec[5].pttmethod = 2;
  rig_spec[5].transverter_freq[0][0] = 0;
  rig_spec[5].band_mask = 0x2000;  // 0x2000 = all band nonzero but zero for all band

  // IC-820
  rig_spec[6].name = "IC-820";
  rig_spec[6].cat_type = 0;  // civ
  rig_spec[6].civaddr = 0x42;
  //  rig_spec[6].civport = &Serial2;
  rig_spec[6].civport_num=2; // CIV
  rig_spec[6].civport_reversed=0; // normal
  rig_spec[6].civport_baud = 9600;      
  rig_spec[6].cwport = 2;
  rig_spec[6].rig_type = 1;
  rig_spec[6].pttmethod = 2;
  rig_spec[6].transverter_freq[0][0] = 0;
  rig_spec[6].band_mask = 0x1e7f;  // 0x2000 = all band nonzero but zero for all band
  
  rig_spec[7].name = "IC-705-SER";
  rig_spec[7].cat_type = 0;  // civ from port 3
  rig_spec[7].civaddr = 0xa4;
  rig_spec[7].civport_num=3; // TTL-SER
  rig_spec[7].civport_reversed=0; // normal
  rig_spec[7].civport_baud = 38400;      
  rig_spec[7].cwport = 1;
  rig_spec[7].rig_type = 0;
  rig_spec[7].pttmethod = 2;
  rig_spec[7].transverter_freq[0][0] = 0;

  rig_spec[8].name = "FTDX10";
  rig_spec[8].cat_type = 1;  // cat
  rig_spec[8].civaddr = 0;
  rig_spec[8].civport_num=3; // TTL-SER
  rig_spec[8].civport_reversed=1; // normal
  rig_spec[8].civport_baud = 38400;      
  rig_spec[8].cwport = 1;  // test
  rig_spec[8].rig_type = 2;
  rig_spec[8].pttmethod = 2;
  rig_spec[8].transverter_freq[0][0] = 0;
  rig_spec[8].band_mask = 0x780;

  rig_spec[9].name = "QMX";
  rig_spec[9].cat_type = 2;  // kenwood cat port USB
  rig_spec[9].civaddr = 0;
  rig_spec[9].civport_num=-1; // USB
  rig_spec[9].civport_reversed=0; // normal
  rig_spec[9].civport_baud = 0;      
  rig_spec[9].cwport = 1;
  rig_spec[9].rig_type = 3;
  rig_spec[9].pttmethod = 2;
  rig_spec[9].transverter_freq[0][0] = 0;

  //
  rig_spec[10].name = "IC-705-USB"; // 705 connected to USB
  rig_spec[10].cat_type = 0;  // civ
  rig_spec[10].civaddr = 0xa4;
  //  rig_spec[10].civport = NULL;
  rig_spec[10].civport_num=-1; // USB
  rig_spec[10].civport_reversed=0; // normal
  rig_spec[10].civport_baud = 115200;      
  rig_spec[10].cwport = 0;
  rig_spec[10].rig_type = 0;
  rig_spec[10].pttmethod = 2;
  rig_spec[10].band_mask = 0x600;
  rig_spec[10].transverter_freq[0][0] = 0;

  rig_spec[11].name = "DJ-G7"; // DJ-G7
  rig_spec[11].cat_type = 3;  // no cat
  rig_spec[11].civaddr = 0;
  rig_spec[11].civport_num=-2; // manual
  rig_spec[11].civport_reversed=0; // normal
  rig_spec[11].civport_baud = 0;      
  rig_spec[11].cwport = 1;
  rig_spec[11].rig_type = 4;
  rig_spec[11].pttmethod = 2;
  rig_spec[11].band_mask = 0x147f; //1 0100 0111 1111
  //                                 1 5214 1522 1731
  //                                 0 6423 4081 4 58
  //                                 1 1110 0111 1111
  rig_spec[11].transverter_freq[0][0] = 5759000000ULL/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][1] = 5761000000ULL/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][2] = 1279000000/FREQ_UNIT;
  rig_spec[11].transverter_freq[0][3] = 1281000000/FREQ_UNIT;
  
  rig_spec[12].name = "KENWOOD-SER";
  rig_spec[12].cat_type = 2;  // kenwood cat port
  rig_spec[12].civaddr = 0;
  rig_spec[12].civport_num=3; // TTL-SER
  rig_spec[12].civport_reversed=1; // reversed
  rig_spec[12].civport_baud = 38400;  
  rig_spec[12].cwport = 1;
  rig_spec[12].rig_type = 3;
  rig_spec[12].pttmethod = 2;
  rig_spec[12].transverter_freq[0][0] = 0;

}

void init_all_radio() {
  init_rig();
  radio_list[0].rig_idx = 0;  
  init_radio(&radio_list[0], "IC-705");  

  radio_list[0].enabled = 1;

  radio_list[1].rig_idx = 1;  
  init_radio(&radio_list[1], "IC-9700");

  radio_list[1].enabled = 1;
  radio_selected = &radio_list[0];

  radio_list[2].rig_idx = 2;  
  init_radio(&radio_list[2], "FT-991A");

  radio_list[2].enabled = 0;
  
  new_log_entry();
}
void set_rig_spec_from_str(struct radio *radio,char *s)
{
  char s1[60];char *p; struct rig *rig_spec;
  int n;
  rig_spec=radio->rig_spec;
  strcpy(s1,s);
  p=strtok(s1,", ");
  while (p!=NULL) {
    if (strncmp(p,"CW:",3)==0) {
      // CW port
      n=atoi(p+3);
      if (n>=0 && n<=3) {
	rig_spec->cwport=n;
      }
    } else if (strncmp(p,"B:",2)==0) {
      // baudrate
      n=atoi(p+2);
      if (n>=1 && n<=115200) {
	rig_spec->civport_baud = n;
      }
    } else if (strncmp(p,"P:",2)==0) {
      // civport num
      n=atoi(p+2);
      if (n>=-2 && n<=3) {
	rig_spec->civport_num = n;
      }
    } else if (strncmp(p,"ADR:",4)==0) {
      // civaddr
      sscanf(p+4,"%x",&n);
      if (n>=0 && n<256) {
	rig_spec->civaddr=n;
      }
    } else if (strncmp(p,"TP:",3)==0) {
      // cat_type and rig_type
      if (strlen(p)>=3+3) {
	if (isdigit(p[3])) {
	  rig_spec->cat_type=p[3]-'0';
	}
	if (isdigit(p[5])) {
	  rig_spec->rig_type=p[5]-'0';
	}
      }
    }
    p=strtok(NULL,", ");
  }
}

void set_rig_spec_str_from_spec(struct radio *radio) // reverse set rig_spec_string from rig_spec
{
  char buf[120],buf1[40];
  *buf='\0';
  sprintf(buf1,"CW:%d,", radio->rig_spec->cwport);  strcat(buf,buf1);
  sprintf(buf1,"TP:%d_%d,", radio->rig_spec->cat_type,radio->rig_spec->rig_type);  strcat(buf,buf1);
  sprintf(buf1,"P:%d,", radio->rig_spec->civport_num);  strcat(buf,buf1);  
  sprintf(buf1,"ADR:%02X,",radio->rig_spec->civaddr);  strcat(buf,buf1);
  if (radio->rig_spec->civport_baud!=0) {
    sprintf(buf1,"B:%d,",radio->rig_spec->civport_baud);strcat(buf,buf1);
  }
  strcpy(radio->rig_spec_str+2,buf);
  if (!plogw->f_console_emu) {  
    plogw->ostream->println(buf);
  }
}

void init_radio(struct radio *radio, const char *rig_name) {

  radio->f_qsl=0;
  radio->smeter_stat = 0;
  radio->smeter = 0;
  radio->smeter_peak = SMETER_MINIMUM_DBM;
  radio->bandid = 0;
  radio->bandid_prev = 0;
  radio->enabled = 0;
  radio->band_mask = 0;  // all band may be switched by default
  radio->band_mask_priority = 0;
  radio->smeter_record[0] = 0;
  radio->smeter_record[1] = 0;
  radio->smeter_record[2] = 0;
  radio->smeter_record[3] = 0;
  radio->smeter_azimuth[0] = -1;
  radio->smeter_azimuth[1] = -1;
  radio->smeter_azimuth[2] = -1;
  radio->smeter_azimuth[3] = -1;
  radio->f_smeter_record_ready = 0;


  radio->bandid_bandmap = 0;

  radio->f_tone_keying = 0;

  radio->qsodata_loaded = 0;

  radio->multi = -1;
  init_buf(radio->callsign, LEN_CALL_WINDOW);
  init_buf(radio->recv_rst, LEN_RST_WINDOW);
  init_buf(radio->sent_rst, LEN_RST_WINDOW);
  init_buf(radio->recv_exch, LEN_EXCH_WINDOW);
  init_buf(radio->remarks, LEN_REMARKS_WINDOW);  //37
  radio->callsign_previously_sent[0] = '\0';
  init_buf(radio->rig_name, LEN_RIG_NAME);
  strcpy(radio->rig_name + 2, rig_name);
  set_rig_from_name(radio);
  
  init_buf(radio->rig_spec_str, LEN_RIG_SPEC_STR);
  set_rig_spec_str_from_spec(radio); // reverse set rig_spec_string from rig_spec

  radio->ptt = 0;  // 0 recv 1 send
  radio->ptt_stat = 0;
  radio->ptr_curr = 0;
  radio->keyer_mode = 0;

  radio->modetype = LOG_MODETYPE_UNDEF;       // not defined
  radio->modetype_prev = LOG_MODETYPE_UNDEF;  // not defined

  for (int i = 0; i < 4; i++) {
    radio->cq[i] = LOG_CQ;  // operation mode cq 1 or s&p 0
  }

  // frequency related
  radio->freq = 0;
  radio->freq_prev = 0;
  radio->freq_target = 0;
  radio->freqchange_timer = 0;
  radio->freqchange_retry = 0;
  radio->f_freqchange_pending = 0;
  radio->transverter_in_use = 0;

  set_log_rst(radio);

  // radio->f_civ_response_expected = 0;
  // radio->civ_response_timer = 0;

  for (int k = 0; k < N_BAND; k++) {

    radio->modetype_bank[k]=0; // clear modetype_bank    
    for (int i = 0; i < 4; i++) {
      radio->cq_bank[k][i]=0; // clear cq_bank      
      for (int j = 0; j < 2; j++) {
        radio->freqbank[k][j][i] = 0;  // clear freqency bank
      }
    }
  }
}


void set_rig_from_name(struct radio *radio) {
  int tmp_rig_spec_idx=0;
  if (strcmp(radio->rig_name + 2, "IC-705") == 0) {
    tmp_rig_spec_idx = 0;
  } else if (strcmp(radio->rig_name + 2, "IC-9700") == 0) {
    tmp_rig_spec_idx = 1;
  } else if (strcmp(radio->rig_name + 2, "FT-991A") == 0) {
    tmp_rig_spec_idx = 2;
  } else if (strcmp(radio->rig_name + 2, "FT-991A-SER") == 0) {
    tmp_rig_spec_idx = 3;
  } else if (strcmp(radio->rig_name + 2, "QCX-MINI") == 0) {
    tmp_rig_spec_idx = 4;  //
  } else if (strcmp(radio->rig_name + 2, "MANUAL") == 0) {
    tmp_rig_spec_idx = 5;  //
  } else if (strcmp(radio->rig_name + 2, "IC-820") == 0) {
    tmp_rig_spec_idx = 6;  //
  } else if (strcmp(radio->rig_name + 2, "IC-705-SER") == 0) {
    tmp_rig_spec_idx = 7;  //
  } else if (strcmp(radio->rig_name + 2, "FTDX10") == 0) {
    tmp_rig_spec_idx = 8;  //
  } else if (strcmp(radio->rig_name + 2, "QMX") == 0) {
    tmp_rig_spec_idx = 9;  //
  } else if (strcmp(radio->rig_name + 2, "IC-705-USB") == 0) {
    tmp_rig_spec_idx = 10;  //
  } else if (strcmp(radio->rig_name + 2, "DJ-G7") == 0) {
    tmp_rig_spec_idx = 11;  //
  } else if (strcmp(radio->rig_name + 2, "KENWOOD-SER") == 0) {
    tmp_rig_spec_idx = 12;  //
  }
  int ret;
  ret=check_rig_conflict(radio->rig_idx,&rig_spec[tmp_rig_spec_idx]);
  if (ret== -1) {
    radio->rig_spec_idx=tmp_rig_spec_idx;
    select_rig(radio);
  } else {
    sprintf(dp->lcdbuf,"config %s\nconflict %s",rig_spec[tmp_rig_spec_idx].name,
	    radio_list[ret].rig_spec->name);
    upd_display_info_flash(dp->lcdbuf);
  }
}

void switch_transverter() {

  // search rig_spec and if current frequency , and
  // if in one of enabled transverter RF frequency, disable it
  // if in one of disabled transverter IF frequency, enable
  sprintf(dp->lcdbuf, "switching XVTR");

  upd_display_info_flash(dp->lcdbuf);

  struct radio *radio;
  radio = radio_selected;

  for (int i = 0; i < NMAX_TRANSVERTER; i++) {
    if (radio->rig_spec->transverter_freq[i][0] == 0) continue;  // skip if not defined
    switch (radio->rig_spec->transverter_enable[i]) {
      case 1:  // currently enabled
        if ((radio->freq >= radio->rig_spec->transverter_freq[i][0]) && (radio->freq <= radio->rig_spec->transverter_freq[i][1])) {


          radio->f_freqchange_pending = 0;

          radio->rig_spec->transverter_enable[i] = 0;
          // disable it
          sprintf(dp->lcdbuf, "XVTR\nRF %d - %d MHz\nIF %d - %d MHz\nDisabled.",
                  radio->rig_spec->transverter_freq[i][0] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][1] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][2] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][3] / (1000000/FREQ_UNIT));
          upd_display_info_flash(dp->lcdbuf);
        }
        break;
      case 0:  // currently disabled
        if ((radio->freq >= radio->rig_spec->transverter_freq[i][2]) && (radio->freq <= radio->rig_spec->transverter_freq[i][3])) {
          radio->f_freqchange_pending = 0;
          radio->rig_spec->transverter_enable[i] = 1;
          // enable it
          sprintf(dp->lcdbuf, "XVTR\nRF %d - %d MHz\nIF %d - %d MHz\nEnabled.",
                  radio->rig_spec->transverter_freq[i][0] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][1] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][2] / (1000000/FREQ_UNIT),
                  radio->rig_spec->transverter_freq[i][3] / (1000000/FREQ_UNIT));
          upd_display_info_flash(dp->lcdbuf);
        }
        break;
    }
  }
}

void signal_process()
{
  struct radio *radio;
  radio = radio_selected;
  if (plogw->f_show_signal) {
    if (radio->f_smeter_record_ready) {
      upd_display_info_signal();
      radio->f_smeter_record_ready = 0;
    }
  }

}

void civ_process() {
  struct radio *radio;

  int nrig;
  if (radio_list[0].rig_spec_idx == radio_list[1].rig_spec_idx) {
    nrig = 1;
  } else {
    nrig = 2;
  }
  for (int i = 0; i < N_RADIO; i++) {
    if (!unique_num_radio(i)) continue;
    radio = &radio_list[i];  // main rig reception
    if (receive_civ(radio)) {
      switch (radio->rig_spec->cat_type) {
        case 0:  // icom
          print_civ(radio);
          get_civ(radio);
          break;
        case 1:  // yaesu
          get_cat(radio);
          print_cat(radio);
          break;
        case 2:  //kenwood
          get_cat_kenwood(radio);
          print_cat(radio);
          break;
      }
      clear_civ(radio);
    }
  }

  radio = radio_selected;
  // check smeter reading trigger
  // S&P and Phone
  if ((!radio->cq[radio->modetype]) && (radio->modetype == LOG_MODETYPE_PH)) {
    if (radio->ptr_curr != 0) {  // if not in callsign window, assume QSO starts
      // number input window
      if (radio->smeter_stat == 0) {
        // start checking smeter_peak
        radio->smeter_stat = 1;
        radio->smeter_peak = SMETER_MINIMUM_DBM;
        if (!plogw->f_console_emu) plogw->ostream->println("start reading smeter_peak");
      }

    } else {
      // callsign window now
      if (radio->smeter_stat != 0) {
        // cancel smeter reading
        radio->smeter_stat = 0;
        radio->smeter_peak = SMETER_MINIMUM_DBM;
        if (!plogw->f_console_emu) plogw->ostream->println("cancel reading smeter_peak");
      }
    }
  }
  if (radio->ptt_stat == 2) {
    radio->ptt_stat = 0;  // force receive ptt why??? 23/1/11
  }
}

int receive_civ(struct radio *radio) {

  if (!radio->enabled) return 0;

  char c;
  while (radio->r_ptr != radio->w_ptr) {
    if (radio->cmd_ptr >= 256) {
      radio->cmd_ptr = 0;
    }
    c = radio->bt_buf[radio->r_ptr];
    radio->r_ptr = (radio->r_ptr + 1) % 256;
    radio->cmdbuf[radio->cmd_ptr++] = c;

    switch (radio->rig_spec->cat_type) {
      case 0:  // icom civ
        if (c == 0xfd) return 1;
        else return 0;
        break;
      case 1:  // yaesu
        if (c == ';') {
	  // term
	  radio->cmdbuf[radio->cmd_ptr]='\0';
	  return 1;
	}
        else return 0;
        break;
      case 2:  // kenwood
        if (c == ';') {
	  // term
	  radio->cmdbuf[radio->cmd_ptr]='\0';	  
	  return 1;
	}
        else return 0;
        break;
    }
  }
  return 0;
}

// check if the radio index i is the radio which did not appear in previos indexes.
int unique_num_radio(int i) {
  if (i == 0) return 1;
  if (i == 1) {
    if (radio_list[0].rig_spec_idx == radio_list[1].rig_spec_idx) {
      return 0;
    } else {
      return 1;
    }
  }
  if (i == 2) {
    if (radio_list[0].rig_spec_idx == radio_list[2].rig_spec_idx) {
      return 0;
    }
    if (radio_list[1].rig_spec_idx == radio_list[2].rig_spec_idx) {
      return 0;
    }
    return 1;
  }
  return 0;
}

// RTTYメッセージ送信前後　などのPTT制御を行う。
void Control_TX_process() {
  struct radio *radio;
  radio = &radio_list[plogw->so2r_tx];
  switch (f_transmission) {
    case 0:  // do nothing
      break;
    case 1:  // force transmission
      set_ptt_rig(radio, 1);
      if (!plogw->f_console_emu) {
        plogw->ostream->println("PTT ON TX=");
        plogw->ostream->println(plogw->so2r_tx);
      }
      if (radio->f_tone_keying) {
	keying(1); // PTT in tone keying by keying contact
      }
      break;
    case 2:  // force stop transmission
      set_ptt_rig(radio, 0);
      if (radio->f_tone_keying) {
	//        ledcWrite(LEDC_CHANNEL_0, 0);  // stop sending tone
	keying(0);
      } else {
        // digitalWrite(LED, 0);
        keying(0);
      }
      if (!plogw->f_console_emu) {
        plogw->ostream->print("PTT OFF TX=");
        plogw->ostream->println(plogw->so2r_tx);
      }

      break;
  }
  f_transmission = 0;
}

void rotator_sweep_process()
{
  if ((plogw->f_rotator_sweep==1) && (plogw->f_rotator_enable==1)) {
    if (plogw->rotator_sweep_timeout < millis()) {
      plogw->rotator_sweep_timeout = millis() + 2000; // interval of the next sweep control set
      // compare current with the target
      // boundary is south (180deg)
      int az, target_az ;
      az = plogw->rotator_az;
      if (az > 180.0) az -= 360.0;
      target_az = plogw->rotator_target_az_sweep;
      if (target_az > 180.0) target_az -= 360.0;
      plogw->ostream->println("rotator_sweep_process");
	  if (plogw->f_rotator_sweep_suspend>0) {
		  plogw->ostream->println("suspending");
		  if (plogw->f_rotator_sweep_suspend>1) {
			  return;
		  } else {
		     plogw->rotator_target_az = az;
			 plogw->f_rotator_sweep_suspend=2;
		  }
	  } else {
      if (plogw->rotator_sweep_step > 0) {
        if (target_az > az) {
          plogw->rotator_target_az = az + plogw->rotator_sweep_step;
        } else {
          // reached target
          plogw->f_rotator_sweep = 0;
          plogw->ostream->println("rotator sweep finished.");
        }
      } else {
        if (target_az < az) {
          plogw->rotator_target_az = az + plogw->rotator_sweep_step;
        } else {
          // reached target
          plogw->f_rotator_sweep = 0;
          plogw->ostream->println("rotator sweep finished.");
        }
      }
	  }
      if (plogw->f_rotator_sweep) {
        if (plogw->rotator_target_az < 0) plogw->rotator_target_az += 360;
        plogw->ostream->print("send rotator target=");
        plogw->ostream->println(plogw->rotator_target_az);
        send_rotator_az_set_civ(plogw->rotator_target_az);
      }
    }
  }
}


/// satellite related ?
void set_sat_opmode(struct radio *radio, char *opmode) {
  if (!radio->enabled) return;
  switch (radio->rig_spec->rig_type) {
    case 0:  // IC-705 VFO sel/unselected to set RX/TX frequency
      break;
    case 1:  // IC-9700 main (TX) and sub (RX) frequencies  normally select sub
      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo
      add_civ_buf((byte)0xd0);  // main (TX)
      send_tail_civ(radio);

      // set mode for uplink
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // set mode
      add_civ_buf((byte)rig_modenum(opmode));
      add_civ_buf((byte)0x01);  // FIL1
      send_tail_civ(radio);

      send_head_civ(radio);
      add_civ_buf((byte)0x07);  // select vfo
      add_civ_buf((byte)0xd1);  // sub (RX)
      send_tail_civ(radio);

      // set mode for downlink
      send_head_civ(radio);
      add_civ_buf((byte)0x06);  // mode
      if ((strcmp(opmode, "CW-R") == 0) || (strcmp(opmode, "CW") == 0)) {
        add_civ_buf((byte)rig_modenum("USB"));

      } else {
        add_civ_buf((byte)rig_modenum(opmode));
      }
      add_civ_buf((byte)0x01);  // FIL1
      send_tail_civ(radio);

      // set opmode, and filt for operation purpose
      set_mode(opmode, 1, radio);

      break;
  }
}

void adjust_frequency(int dfreq) {
  // dfreq is in FREQ_UNIT
  
  // frequency control up and down
  if (!plogw->f_console_emu) plogw->ostream->println("adjust_frequency()");
  if (plogw->sat) {
    // satellite mode manipulate up_f, .. etc
    // satellite frequencies are in Hz (not FREQ_UNIT)
    switch (plogw->sat_freq_tracking_mode) {
      case SAT_RX_FIX:  //RX fixed (tx controll)
        if (plogw->dn_f != 0) {
          plogw->dn_f += dfreq*FREQ_UNIT;
        }
        break;
      case SAT_TX_FIX:  // TX fixed
        if (plogw->up_f != 0) {
          plogw->up_f += dfreq*FREQ_UNIT;
        }

        break;
      case SAT_SAT_FIX:  // satellite freq fixed
        if (plogw->satdn_f != 0) plogw->satdn_f += dfreq*FREQ_UNIT;
        break;
    }
    //    set_sat_freq_calc();   // finally set frequency
  } else {
    set_frequency_rig((int)(radio_selected->freq + dfreq));  //
  }
}

void save_freq_mode_filt(struct radio *radio) {
  radio->bandid = freq2bandid(radio->freq);  // make sure that these are always matching
  if (!plogw->f_console_emu) {
    plogw->ostream->print("save f=");
    plogw->ostream->print(radio->freq);
    plogw->ostream->print(" mode ");
    plogw->ostream->print(radio->opmode);
    plogw->ostream->print(" filt ");
    plogw->ostream->print(radio->filt);
    plogw->ostream->print(" in b ");
    plogw->ostream->print(radio->bandid);
    plogw->ostream->print(" cq ");
    plogw->ostream->print(radio->cq[radio->modetype]);
    plogw->ostream->print(" modetype ");
    plogw->ostream->print(radio->modetype);
    plogw->ostream->println("");
  }
  //  radio->cq_modetype_bank[radio->bandid]=radio->cq[radio->modetype]*4+(radio->modetype&0x3);
  // save cq and modetype
  radio->cq_bank[radio->bandid][radio->modetype]=radio->cq[radio->modetype];
  radio->modetype_bank[radio->bandid]=radio->modetype;
  // then save freq
  radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->freq;
  radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = rig_modenum(radio->opmode);
  radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = radio->filt;
}

void recall_freq_mode_filt(struct radio *radio) {
  int freq;
  // restore cq and modetype first
  radio->modetype=radio->modetype_bank[radio->bandid];
  radio->cq[radio->modetype]=radio->cq_bank[radio->bandid][radio->modetype];
  
  // then recover freq
  freq = radio->freqbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];

  if (!plogw->f_console_emu) {
    plogw->ostream->print("recall f=");
    plogw->ostream->print(freq);

    plogw->ostream->print(" in b ");
    plogw->ostream->print(radio->bandid);
    plogw->ostream->print(" cq ");
    plogw->ostream->print(radio->cq[radio->modetype]);
    plogw->ostream->print(" modetype ");
    plogw->ostream->print(radio->modetype);
  }
  int modenum, filt;

  if (freq == 0) {
    // default not set
    freq = bandid2freq(radio->bandid, radio);
    char *opmode;
    // set default mode and filt in modebank and filtbank
    opmode = default_opmode(radio->bandid, radio->modetype);
    filt = default_filt(opmode);
    strcpy(radio->opmode, opmode);
    modenum = rig_modenum(radio->opmode);
    radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = modenum;
    radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype] = filt;
  }
  //set_frequency(freq, radio);  // this should be performed from civ receive process not to be forced? 23/4/23
  set_frequency_rig(freq);
  //    int modenum, filt;
  modenum = radio->modebank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];
  filt = radio->filtbank[radio->bandid][radio->cq[radio->modetype]][radio->modetype];
  if (!plogw->f_console_emu) {
    plogw->ostream->print(" modenum ");
    plogw->ostream->print(modenum);
    plogw->ostream->print(" filt ");
    plogw->ostream->print(filt);
    plogw->ostream->println("");
  }

  send_mode_set_civ(opmode_string(modenum), filt);
}

int bandid2freq(int bandid, struct radio *radio) {
  // later, obtain previous frequency from the freqbank (with bandid)
  switch (radio->modetype) {
    case 0:  // default
    case 1:  // CW
    case 3:  // DG
      switch (bandid) {
        case 0:  // default
        default:
          return 0;
        case 1:  // 1.9
          return 1801000/FREQ_UNIT;
        case 2:  // 3.5
          return 3510000/FREQ_UNIT;
        case 3:  // 7.0
          return 7010000/FREQ_UNIT;
        case 4:  // 14.0
          return 14050000/FREQ_UNIT;
        case 5:  // 21.
          return 21050000/FREQ_UNIT;
        case 6:  // 28.
          return 28050000/FREQ_UNIT;
        case 7:  // 50.
          return 50050000/FREQ_UNIT;
        case 8:  // 144
          return 144050000/FREQ_UNIT;
        case 9:  // 430
          return 430050000/FREQ_UNIT;
        case 10:  // 1200
          return 1294050000/FREQ_UNIT;
        case 11:  // 2400
          return 2424050000/FREQ_UNIT;
        case 12:  // 5600
          return 5760000000ULL/FREQ_UNIT;
      case 13:  // 10G
          return 10240000000ULL/FREQ_UNIT;
      }
      return 0;
      break;

    case 2:  // PHONE
      switch (bandid) {
        case 0:  // default
        default:
          return 0;
        case 1:  // 1.9
          return 1850000/FREQ_UNIT;
        case 2:  // 3.5
          return 3535000/FREQ_UNIT;
        case 3:  // 7.0
          return 7060000/FREQ_UNIT;
        case 4:  // 14.0
          return 14250000/FREQ_UNIT;
        case 5:  // 21.
          return 21350000/FREQ_UNIT;
        case 6:  // 28.
          return 28600000/FREQ_UNIT;
        case 7:  // 50.
          return 50350000/FREQ_UNIT;
        case 8:  // 144
          return 144250000/FREQ_UNIT;
        case 9:  // 430
          return   433020000/FREQ_UNIT;
        case 10:  // 1200
          return  1295000000/FREQ_UNIT;
        case 11:  // 2400
          return  2427000000/FREQ_UNIT;
        case 12:  // 5600
          return  5760000000ULL/FREQ_UNIT;
        case 13:  // 10000
          return 10240000000ULL/FREQ_UNIT;
      }
      return 0;
      break;
  }
  return 0;
}


char *default_opmode(int bandid, int modetype) {
  // 1    2    3 4  5  6  7  8   9   10   11   12     13 14 15
  // 1.8  3.5  7 14 21 28 50 144 430 1200 2400 5600
  switch (modetype) {
    case LOG_MODETYPE_CW: return "CW"; break;
    case LOG_MODETYPE_PH:
      if (bandid <= 3) {
        return "LSB";
      } else {
        if (bandid <= 7) {
          return "USB";
        } else {
          return "FM";
        }
      }
      break;
    case LOG_MODETYPE_DG:
      return "RTTY";
      break;
    case LOG_MODETYPE_UNDEF:
      return "CW";
      break;
  }
  return "CW";
}

int default_filt(char *opmode) {
  int modenum;
  modenum = rig_modenum(opmode);
  // LSB 0 USB 1 AM 2 CW 3 RTTY 4 FM 5 WFM 6 CW-R 7 RTTY-R 8 DV 0x17
  switch (modenum) {
    case 3:      // CW
    case 7:      // CW-R
      return 2;  // FIL2
      break;
    case 0:     // LSB
    case 1:     // USB
    case 2:     // AM
    case 4:     // RTTY
    case 5:     // FM
    case 6:     // WFM
    case 8:     // RTTY-R
    case 0x17:  // DV
    default:
      return 1;  // FIL1 (wide)
      break;
  }
}
