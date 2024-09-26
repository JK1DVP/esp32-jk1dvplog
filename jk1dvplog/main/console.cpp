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
#include "cmd_interp.h"
#include "main.h"
#include "ui.h"
#include "console.h"

// receive command from serial terminal
void console_process() {
  while (plogw->ostream->available() > 0) {
    char c = plogw->ostream->read();
    if (verbose & 32) {
      char buf[20];
      sprintf(buf, "[%02X(%c)]", c, isprint(c) ? c : ' ');
      plogw->ostream->print(buf);
    }


    if (plogw->f_console_emu) {
      emulate_keyboard(c);
      continue;
    }
    //plogw->ostream->print(c);
    if (c == 0x0a) {  // LF end of the line
      // carriage return end of a line
      plogw->cmdbuf[plogw->cmd_ptr] = '\0';
      // send received line to command interpreter
      cmd_interp(plogw->cmdbuf);
      // clear buffer
      plogw->cmd_ptr = 0;
      continue;
    }
    if (c == 0x0d) {
      // ignore CR
      continue;
    }
    if (plogw->cmd_ptr < 128) {
      plogw->cmdbuf[plogw->cmd_ptr] = c;
      plogw->cmd_ptr++;
    } else {
      // buffer overflow and clear
      // plogw->cmd_ptr=0;
    }
  }
}

void print_status_console()
{
  // print current logger status to console to link with PC
  // print logwindow infos
  plogw->ostream->print("# 0 ");
  plogw->ostream->print("focused: ");
  plogw->ostream->print(plogw->focused_radio);
  plogw->ostream->print(" rx: ");
  plogw->ostream->print(plogw->so2r_rx);
  plogw->ostream->print(" tx: ");
  plogw->ostream->print(plogw->so2r_tx);
  plogw->ostream->print(" ");
  print_wifiinfo();

  struct radio *radio;

  char bu[80];
  for (int i = 0; i < N_RADIO; i++) {
    radio = &radio_list[i];
    if (!radio->enabled) continue;
    sprintf(bu, "f: %10d m: %4s s: %4d cq: %1d rig %c %s",
            radio->freq, radio->opmode, radio->smeter/SMETER_UNIT_DBM, radio->cq[radio->modetype],
            radio_list[i].enabled ? '*' : ' ',
            radio_list[i].rig_spec->name
           );
    plogw->ostream->print("# "); plogw->ostream->print(i + 1);
    plogw->ostream->print(" ");
    if (plogw->focused_radio == i) {
      plogw->ostream->print("*");
    } else {
      plogw->ostream->print(" ");
    }
    plogw->ostream->println(bu);
  }

}



int in_keys(char c, const uint8_t *keys, int nkeys) {
  for (int i = 0; i < nkeys; i++) {
    if (keys[i] == c) {
      return i;
    }
  }
  return -1;
}



void emulate_keyboard(char c) {
  static int f_esc = 0;
  static int f_funcnum = 0;
  const uint8_t numKeys[10] PROGMEM = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' };
  const uint8_t symKeysUp[12] PROGMEM = { '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?' };
  const uint8_t symKeysLo[12] PROGMEM = { '-', '=', '[', ']', '\\', ' ', ';', '\'', '`', ',', '.', '/' };
  //    const uint8_t padKeys[5] PROGMEM = {'/', '*', '-', '+', '\r'};

  //void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key)
  uint8_t key;
  int ret;




  static MODIFIERKEYS modkey;


  // UP 1B 5B 41
  // DN 1B 5B 42
  // RIGHT 1B 5B 43
  // LEFT 1B 5B 44

  // HOME 1B 5B 31 7E
  // F1 1B 5B 31 31 7E
  // F2 1B 5B 31 32 7E
  // F5          35
  // F6          37
  // F8          39

  // F9       32 30
  // F10      32 31
  // F11         33
  // F12         34

  // END 1B 5B 34 7E
  // PGUP 1B 5B 35 7E
  // PGDN 1B 5B 36 7E

  // by sending meta Alt -> 1B (ESC) +key
  // Alt-A 1B 61
  // Alt-B 1B 7A
  // ...
  modkey.bmLeftShift = 0;
  modkey.bmRightShift = 0;
  modkey.bmLeftCtrl = 0;
  modkey.bmRightCtrl = 0;
  modkey.bmLeftAlt = 0;
  modkey.bmRightAlt = 0;
  key = 0;
  // check esc sequence
  switch (f_esc) {
    case 0:  // no esc
      break;
    case 1:  // esc pressed 1b
      if (c == 0x5b) {
        // sequence
        f_esc = 2;
        return;
      }
      if (c == 0x1b) {
        // double ESC --> ESC
        key = 0x29;
        c = 0;
        break;
      }
      modkey.bmLeftAlt = 1;
      break;
    case 3:  // 1b 5b 31
      if (c == 0x7e) {
        // HOME
        key = 0x4a;
        c = 0;
        break;
      }
      if (c >= 0x30 && c <= 0x34) {
        f_funcnum = c;
        f_esc = 8;  // 1b5b313x
        return;
      }
      break;
    case 8:  // 1b5b313x
      if (c == 0x7e) {
        // end of sequence
        if (f_funcnum >= 0x31 && f_funcnum <= 0x35) {
          key = f_funcnum + 0x3a - 0x31;
          c = 0;
        }
        if (f_funcnum >= 0x37 && f_funcnum <= 0x39) {
          key = f_funcnum + 0x3f - 0x31;
          c = 0;
        }
      }
      break;

    case 4:  // 1b 5b 32
      if (c >= 0x31 && c <= 0x39) {
        f_funcnum = c;  //save
        f_esc = 9;      // 1b5b323x
        return;
      }
      break;
    case 9:  // 1b5b323x
      if (c == 0x7e) {
        // end of sequence
        key = f_funcnum + 0x42 - 0x30;
        c = 0;
      }
      break;
    case 5:  // 1b 5b 34
      if (c == 0x7e) {
        // END
        key = 0x4d;
        c = 0;
      }
      break;
    case 6:  // 1b 5b 35
      if (c == 0x7e) {
        // PGUP
        key = 0x4b;
        c = 0;
      }
      break;
    case 7:  // 1b 5b 36
      if (c == 0x7e) {
        // PGDN
        key = 0x4e;
        c = 0;
      }
      break;

    case 2:  // 0x1b 0x5b+ ?
      if (c == 0x31) {
        f_esc = 3;  // 1b 5b 31
        return;
      }
      if (c == 0x32) {
        f_esc = 4;  // 1b 5b 32
        return;
      }
      if (c == 0x34) {  // 1b 5b 34
        f_esc = 5;
        return;
      }
      if (c == 0x35) {  // 1b 5b 35
        f_esc = 6;
        return;
      }
      if (c == 0x36) {  // 1b 5b 36
        f_esc = 7;
        return;
      }
      if (c == 0x41) {  // UP
        key = 0x52;
        c = 0;
        break;
      }
      if (c == 0x42) {  // DN
        key = 0x51;
        c = 0;
        break;
      }
      if (c == 0x44) {  // LEFT
        key = 0x50;
        c = 0;
        break;
      }
      if (c == 0x43) {  // RIGHT
        key = 0x4F;
        c = 0;
        break;
      }
      if (c == 0x5a) {  // Shift + TAB
        key = 0x2b;
        c = 0;
        modkey.bmLeftShift = 1;
        break;
      }
  }
  if (key == 0) {
    switch (c) {
      case ' ': key = 0x2c; break;
      case 0x0d:
        key = 0x28;
        c = 0;
        break;
      case 0x08:
        key = 0x2a;
        c = 0;
        break;  // BS
      case 0x09:
        key = 0x2b;
        c = 0;
        break;  // TAB
      case 0x7f:
        key = 0x4c;
        c = 0;
        break;    // DEL
      case 0x1b:  // ESC
        if (f_esc == 0) {
          f_esc = 1;
          return;
        }
        break;
      default:
        // ctrl keys
        if (c >= 0x01 && c <= 0x1f) {
          c = c - 0x01 + 'A';
          modkey.bmLeftCtrl = 1;
          if (c >= 'A' && c <= 'Z') key = c - 'A' + 0x04;
        } else {
          if (c >= 'a' && c <= 'z') {
            key = c - 'a' + 0x04;
          } else {
            if (c >= 'A' && c <= 'Z') {
              // cap characters
              key = c - 'A' + 0x04;
              // shift up
              modkey.bmLeftShift = 1;
            } else {
              if (c >= '0' && c <= '9') {
                // numbers
                key = c - '0' + 0x1e;
              } else {
                if ((ret = in_keys(c, numKeys, 10)) != -1) {
                  key = 0x1e + ret;
                  // shift up
                  modkey.bmLeftShift = 1;

                } else {
                  if ((ret = in_keys(c, symKeysUp, 12)) != -1) {
                    key = 0x2d + ret;
                    // shift up
                    modkey.bmLeftShift = 1;
                  } else {
                    if ((ret = in_keys(c, symKeysLo, 12)) != -1) {
                      key = 0x2d + ret;
                      // shift down
                    }
                  }
                }
              }
            }
          }
        }
    }
  }
  f_esc = 0;

  // print key information
  if (verbose & 4) {
    char buf[10];
    sprintf(buf, " $%02x", key);
    plogw->ostream->print("key=");
    plogw->ostream->print(buf);
    plogw->ostream->print(" ctrl=");
    plogw->ostream->print(modkey.bmLeftCtrl);
    plogw->ostream->print(" shift=");
    plogw->ostream->print(modkey.bmLeftShift);
    plogw->ostream->print(" alt=");
    plogw->ostream->println(modkey.bmLeftAlt);
  }
  on_key_down(modkey, (uint8_t)key, (uint8_t)c);
}

