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
#include "decl.h"
#include "variables.h"
#include "mcp.h"
#include "so2r.h"
#include "cw_keying.h"

void SO2R_settx(int tx) {
  char buf[10];
  if (plogw->so2r_tx != tx) {
    plogw->so2r_tx = tx;

    if (plogw->f_so2rmini) {
      //      int tmp;
      //      tmp = 0x90 + (plogw->so2r_tx) * 4 + ((plogw->so2r_rx) | (plogw->so2r_stereo * 3));
      //      sprintf(buf, "$%02X", tmp);
      //      SO2Rprint(buf);
    } else {
      // MIC switch
      mic_switch(plogw->so2r_tx);
      plogw->ostream->print("mic:");
      plogw->ostream->println(plogw->so2r_tx);
    }
  }
}

void SO2R_setrx(int rx)
// rx 0:RX1 1:RX2 2:RX3
{
  char buf[10];
  if (plogw->so2r_rx != rx) {
    plogw->so2r_rx = rx;
    // SO2R_RX の意味がなくなってきているので、focused_radio 等との統合が必要。
    if (plogw->focused_radio != rx) {
      plogw->focused_radio_prev = plogw->focused_radio;
    }
    plogw->focused_radio = rx;

    if (plogw->f_so2rmini) {
      int tmp;
      if (!plogw->so2r_stereo) {
	//        tmp = 0x90 + (plogw->so2r_tx) * 4 + ((plogw->so2r_rx) | (plogw->so2r_stereo * 3));
	//        sprintf(buf, "$%02X", tmp);
	//        SO2Rprint(buf);
      }
    } else {
      // switch on-board relay to switch RX phone
      phone_switch_management();
    }
  }
}

void phone_switch_management() {
  // STEREO, selected_radio の状況に応じ、phoneを選択する。
  int left, right;
  left = 0;
  right = 0;
  if (!plogw->f_console_emu) {
    plogw->ostream->print("stereo:");
    plogw->ostream->print(plogw->so2r_stereo);
    plogw->ostream->print(" focused_radio:");
    plogw->ostream->print(plogw->focused_radio);
    plogw->ostream->print(" focused_radio_prev:");
    plogw->ostream->print(plogw->focused_radio_prev);
    plogw->ostream->print(" so2r_rx:");
    plogw->ostream->print(plogw->so2r_rx);
  }
  switch (plogw->so2r_stereo) {
    case 1:
      if (plogw->focused_radio == 0 || plogw->focused_radio_prev == 0) {
        left |= 1;
      }
      if (plogw->focused_radio == 2 || plogw->focused_radio_prev == 2) {
        left |= 4;
      }

      if (plogw->focused_radio == 1 || plogw->focused_radio_prev == 1) {
        right |= 2;
      }
      if (plogw->focused_radio == 2 || plogw->focused_radio_prev == 2) {
        right |= 4;
      }

      break;
    case 0:  // not stereo
      switch (plogw->focused_radio) {
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
// monitoring SO2R status and control
void SO2R_process() {
  // switch RX
  if (f_so2r_chgstat_rx != 0) {
    plogw->focused_radio_prev = plogw->focused_radio;
    plogw->focused_radio = f_so2r_chgstat_rx - 1;
    radio_selected = &radio_list[plogw->focused_radio];
    SO2R_setrx(f_so2r_chgstat_rx - 1);
    f_so2r_chgstat_rx = 0;
  }
  // switch TX
  if (f_so2r_chgstat_tx != 0) {
    SO2R_settx(f_so2r_chgstat_tx - 1);
    f_so2r_chgstat_tx = 0;
  }
}


void SO2R_setstereo(int stereo) {
  int tmp;
  char buf[10];
  plogw->so2r_stereo = stereo;
  switch (plogw->radio_mode) {
    case 0:
    case 1:
    case 2:

      if (plogw->f_so2rmini) {
	//        tmp = 0x90 + (plogw->so2r_tx) * 4 + ((plogw->so2r_rx) | (plogw->so2r_stereo * 3));
	//        sprintf(buf, "$%02X", tmp);
	//        SO2Rprint(buf);
      } else {
        phone_switch_management();
      }
      break;
  }
}
