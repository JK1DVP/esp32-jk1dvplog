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
#include "ui.h"
#include "console.h"
#include "i2c_guard.h"

// ToDo CW sending with shift only works for alphabetical key: Shift+0-9 should map to symbols(!@#$%^&*{}) (japanese keyboard mapping?) 
//      document relocated maps Alt-i ... etc
//      document that Shift left/right nolonger maps in manual

// receive key information from M5 CardKB (i2c address 0x5f) and handle received key info to send to on_key_down() like emulate_keyboard through emulate_usbkeyboard(c)


struct cardkey_table_tag {
  unsigned char cardkey_chr;
  unsigned char key;
  unsigned char mod;
} cardkey_table[] PROGMEM = {
  // pgup pgdn が割り当て必要
  //              cardkey_chr    key     mod     leftctrl8/shift4/alt2/gui1   right ctrl8/shift4/alt2/gui1
  { 0x7f,0x4c,0},  // Shift+BS=Del   Del
  { 0x80,0x22 ,8}, // Fn+ESC         5       Ctrl   (SO1R/Sat/SO2R switch)
  { 0x81,0x3a ,0}, // Fn+1           F1
  { 0x82,0x3b ,0}, // Fn+2           F2
  { 0x83,0x3c ,0}, // Fn+3           F3
  { 0x84,0x3d ,0}, // Fn+4           F4
  { 0x85,0x3e ,0}, // Fn+5           F5
  { 0x86,0x3f ,0}, // Fn+6           F6
  { 0x87,0x40 ,0}, // Fn+7           F7
  { 0x88,'i'-'a'+0x04 ,2}, // Fn+8           i        Alt    (temporally enable/disable rig)
  { 0x89,0x2d ,2}, // Fn+9           -        Alt    (set spectrum scope)
  { 0x8A,0x2d ,8}, // Fn+0           -        Ctrl   (edit QSO)
  { 0x8B,0x4c ,2}, // Fn+BS          DEL      Alt    (show information)
  { 0x8C,0x2b ,4}, // Fn+TAB         TAB      Shift
  { 0x8D,'q'-'a'+0x04 ,2}, // Fn+Q           Q        Alt     (CQ /S&P)
  { 0x8E,'w'-'a'+0x04 ,2}, // Fn+W           W        Alt     (swipe QSO)
  { 0x8F,'e'-'a'+0x04  ,8}, // Fn+E           E        Ctrl    (Recv Exch)
  { 0x90,'r'-'a'+0x04  ,8}, // Fn+R           R        Ctrl  (Remarks)
  { 0x91,'t'-'a'+0x04  ,2}, // Fn+T           T        Alt   (tuning)
  { 0x92,'y'-'a'+0x04  ,8}, // Fn+Y           y        Ctrl  (show multi list)
  { 0x93,'u'-'a'+0x04  ,8}, // Fn+U           u        Ctrl (show QSO/multi)
  { 0x94,'c'-'a'+0x04  ,8}, // Fn+I           c        Ctrl  (Call sign)
  { 0x95,'o'-'a'+0x04  ,8}, // Fn+O           o        Ctrl (last QSO show)
  { 0x96,'p'-'a'+0x04  ,8}, // Fn+P           p        Ctrl (previous QSO show)

  //  { 0x98,0x50 ,4}, // Fn+Left        Left      Alt (bandmap change)
  
  { 0x99,0x4b ,0}, // Fn+Up          PGUP         CW WPM UP
  
  { 0x9A,'a'-'a'+0x04  ,8}, // Fn+A           a         Ctrl  (his RST)
  { 0x9B,'s'-'a'+0x04  ,8}, // Fn+S           s         Ctrl  (received RST)
  { 0x9C,'d'-'a'+0x04  ,2}, // Fn+D           d         Alt (bandmap entry deletion)
  { 0x9D,'f'-'a'+0x04  ,8}, // Fn+F           f         Ctrl multi search from Remarks string --> no response?
  { 0x9E,'g'-'a'+0x04  ,2}, // Fn+G           g         Alt (tone keying)
  { 0x9F,'g'-'a'+0x04  ,8}, // Fn+H           g         Ctrl (dx entity search)

  { 0xA0,'j'-'a'+0x04  ,8}, // Fn+J           j         Ctrl (show current QSO view)
  { 0xA1,'k'-'a'+0x04  ,8}, // Fn+K           k         Ctrl (keyer mode toggle)
  { 0xA2,'n'-'a'+0x04  ,2}, // Fn+L           n         Alt  (bandmap register)

  { 0xA3,0x28 ,4}, // Fn+Enter       Enter   Shift (conf QSO without TU)

  { 0xA4,0x4e ,0}, // Fn+Down        PGDN     (cw pwm down)
  //  { 0xA5,0x4f ,4}, // Fn+Right       Right    Alt (bandmap change)   reflect to manual

  { 0xA6,'z'-'a'+0x04  ,2}, // Fn+Z           z        Alt (CW<=>PHONE)
  { 0xA7,'x'-'a'+0x04  ,2}, // Fn+X           x        Alt (transverter on/off)
  { 0xA8,'c'-'a'+0x04  ,2}, // Fn+C           c        Alt (CW message edit)
  { 0xA9,'v'-'a'+0x04  ,8}, // Fn+V           v        Ctrl (voice memory control)
  { 0xAA,'b'-'a'+0x04  ,2}, // Fn+B           b        Alt (bandmap sort/satellite beacon)
  { 0xAB,'n'-'a'+0x04  ,8}, // Fn+N           n        Ctrl  (next QSO show)
  { 0xAC,'m'-'a'+0x04  ,2}, // Fn+M            m       Alt (mode change)
  { 0xAD,0x36 ,2}, // Fn+,            ,       Alt (band change)
  { 0xAE,0x37 ,2}, // Fn+.            .       Alt (band change)
  { 0xAF,0x2c ,2}, // Fn+Space        Space   Alt (pick bandmap)

  // { 0xB4,0x50 ,0}, // Left            Left 
  //  { 0xB5,0x52 ,2}, // Up              Up      Alt (up bandmap)
  //  { 0xB6,0x51 ,2}, // Down            Down    Alt (down bandmap)
  //  { 0xB7,0x4f ,0}, // Right           Right
  { 0   ,0,0} // end of table
};

void emulate_keyboard_cardkey(char c) {
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
  // conversion from c to key
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
    key = 0x29;
    c = 0;
    break;
  case 0xb4: // left
    key= 0x50;
    c = 0;
    break;
  case 0xb7: // right
    key = 0x4f;
    c = 0 ;
    break;
  case 0xb5: // up 
    key = 0x52;
    c =0;
    modkey.bmLeftAlt = 1;
    break;
  case 0xb6: // down
    key = 0x51;  c =0;    modkey.bmLeftAlt = 1;   break;
  case 0x98: // Fn +Left
    key=0x50; c=0;    modkey.bmLeftAlt = 1;    break;
  case 0xa5: // Fn +Right
    key=0x4f; c=0;    modkey.bmLeftAlt = 1;    break;
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
		} else {
		  // other keys table lookup
		  struct cardkey_table_tag *cardkey;
		  cardkey=cardkey_table;
		  while (cardkey->cardkey_chr != 0) {
		    if (c==cardkey->cardkey_chr) {
		      // found
		      key=cardkey->key;
		      switch(cardkey->mod) {
		      case 0:   break;
		      case 2:   modkey.bmLeftAlt = 1;   break;
		      case 4:   modkey.bmLeftShift = 1;    break;
		      case 8:   modkey.bmLeftCtrl = 1;   break;
		      }
		      break;
		    }
		    cardkey++;
		  }
		}
	      }
	    }
	  }
	}
      }
    }
  }


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
  if ((verbose & 16) && ((uint8_t)key == 0x36 || (uint8_t)key == 0x37)) {
    Serial.printf("KBDLOW t=%lu src=CARDKEY hid=0x%02X mod=0x%02X on=1\n",
                  (unsigned long)millis(), (unsigned int)(uint8_t)key,
                  (unsigned int)(*((uint8_t *)&modkey)));
  }
  on_key_down(modkey, (uint8_t)key, (uint8_t)c);
  plogw->ostream->flush();
  
}
  
void cardkey_process()
{
  if (!i2c_bus_lock("cardkey", 0)) return;
  uint32_t i2c_t0 = micros();
  Wire.requestFrom(0x5F, 1);
  while(Wire.available())	  {
    char c = Wire.read(); // receive a byte as characterif
    if (c != 0) {
      emulate_keyboard_cardkey(c);
      if (verbose & 4) {
	console->print("cardkb:");
	if (isprint(c)) {
	  console->print(c);
	  console->print(":");		
	} else {
	  console->print(":");
	}
	console->println(c, HEX);
      }

    }
  }
  i2c_bus_unlock("cardkey");
  i2c_diag_io("cardkey", micros() - i2c_t0);
}
  
void init_cardkey()
{
  // check if cardkey 0x5f present
  bool present = false;
  if (i2c_bus_lock("cardkey_init", pdMS_TO_TICKS(20))) {
    uint32_t i2c_t0 = micros();
    Wire.beginTransmission(0x5f);
    present = (Wire.endTransmission() == 0);
    i2c_bus_unlock("cardkey_init");
    i2c_diag_io("cardkey_init", micros() - i2c_t0);
  }
  if (present) {
    f_cardkey_present=1;
    console->println("cardkey present!");    
  } else {
    f_cardkey_present=0;
    console->println("cardkey NOT present!");    
  }
  //  f_cardkey_present=1;

}
