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

#include <hidboot.h>
#include "display.h"

void print_key(MODIFIERKEYS mod, uint8_t key) {
  if (verbose & 1) {
    plogw->ostream->print((mod.bmLeftCtrl == 1) ? "C" : " ");
    plogw->ostream->print((mod.bmLeftShift == 1) ? "S" : " ");
    plogw->ostream->print((mod.bmLeftAlt == 1) ? "A" : " ");
    plogw->ostream->print((mod.bmLeftGUI == 1) ? "G" : " ");
    plogw->ostream->print(" >");
    PrintHex<uint8_t>(key, 0x80);
    plogw->ostream->print("< ");

    plogw->ostream->print((mod.bmRightCtrl == 1) ? "C" : " ");
    plogw->ostream->print((mod.bmRightShift == 1) ? "S" : " ");
    plogw->ostream->print((mod.bmRightAlt == 1) ? "A" : " ");
    plogw->ostream->println((mod.bmRightGUI == 1) ? "G" : " ");
  }
};


