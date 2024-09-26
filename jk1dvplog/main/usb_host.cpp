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
#include "usb_host.h"

#include <cdcftdi.h>  // serial adapter
#include <hidboot.h>
#include <usbhub.h>
#include <BTHID.h>
#include "keyboard.h"
#include "ui.h"
#include "cat.h"
#include "cw_keying.h"
#include "so2r.h"

#ifdef notdef
#include "cdc_ch34x.h"
#endif

// qmx idVendor=0483, idProduct=a34c

//#ifdef notdef
class FTDIAsync : public FTDIAsyncOper {
  public:
    uint8_t OnInit(FTDI *pftdi);
};

uint8_t FTDIAsync::OnInit(FTDI *pftdi) {
  uint8_t rcode = 0;

  rcode = pftdi->SetBaudRate(38400);  // Yaesu CAT baudrate
  //rcode = pftdi->SetBaudRate(115200);  // RN42 default  --> change to 38400

  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetBaudRate"), rcode);
    return rcode;
  }
  rcode = pftdi->SetFlowControl(FTDI_SIO_DISABLE_FLOW_CTRL);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetFlowControl"), rcode);

  return rcode;
}
//#endif

USB Usb;
USBHub Hub(&Usb);  // 使用するハブの数だけ定義しておく
USBHub          Hub2(&Usb);
/// CH340 arduino nano clone SO2R mini
// baudrate 9600
// capital character send
// tx switch send command twice
// switch pattern
// TX1 RX1 0x90 0x90    0000
// TX1 RX2 0x91         0001
// TX1 Stereo 0x92      0010
// TX2 RX1 0x94 0x94    1000
// TX2 RX2 0x95         0101
// TX2 Stereo 0x96 0x96 0110
//
//0x80 so2r close
//0x81 so2r open
//0x82 ptt off
//0x83 ptt on
//0x84 latch off
//0x85 latch on
// winkey command
// 0x02 wpm set wpm
// 0x04 lead tail 10ms set ptt lead/tail
// 0x0a clear buffer
// 0x0b 0/1  key immediate
// 0x0e set winkey mode
// bit 7 disable paddle watchdog
//     6  paddle echoback enable
//    5,4 key mode 00 imabic b 01 iambic a 10 ultimatic 11 bug
//     3 paddle swap
//     2 serial echoback enable
//     1 autospace enable
//     0 ct spacing
// 0x03 weight% weight set key weight 50
// 0x0f load default
// 0x10 ms set 1st extension  (first dit extension for slow TX/RX switching)
// 0x11 msec set key compensation for QSK
// 0x15 winkey status request
// 0x16 buffer pointer commands ???
//0x17 0x50 dit/dah ratio (1:3)
// 0x18 1/0  ptt control
// 1c wpm speed change in buffer
// 0x1f buffered NOP

#ifdef notdef
class CH34XAsyncOper : public CDCAsyncOper {
  public:
    uint8_t OnInit(CH34X *pch34x);
};

uint8_t CH34XAsyncOper::OnInit(CH34X *pch34x) {
  uint8_t rcode;

  LINE_CODING lc;
  lc.dwDTERate = 9600;
  lc.bCharFormat = 0;
  lc.bParityType = 0;
  lc.bDataBits = 8;
  lc.bFlowControl = 0;

  rcode = pch34x->SetLineCoding(&lc);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);

  return rcode;
}
#endif


#ifdef notdef
// IC-705 USB Acm port
#include <cdcacm.h>

class ACMAsyncOper : public CDCAsyncOper {
  public:
    uint8_t OnInit(ACM *pacm);
};

uint8_t ACMAsyncOper::OnInit(ACM *pacm) {
  uint8_t rcode;
  // bit1 RTS=1 bit0 DTR = 1
  rcode = pacm->SetControlLineState(2);
  
  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetControlLineState"), rcode);
    return rcode;
  }

  LINE_CODING lc;
  lc.dwDTERate = 115200;
  lc.bCharFormat = 0;
  lc.bParityType = 0;
  lc.bDataBits = 8;

  rcode = pacm->SetLineCoding(&lc);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);

  return rcode;
}

ACMAsyncOper AsyncOper;
ACM Acm(&Usb, &AsyncOper);


//#ifdef notdef
void ACMprocess() {
  // IC-705 operation
    uint8_t rcode;  
  if (Acm.isReady()) {

    // Acm.SndData は送った後にポインタの中身をクリアするので
    // 別途バッファを用意する。
    char usbSndBuff[40];
    strcpy(usbSndBuff, "");

    //rcode = Acm.SndData(39, usbSndBuff);
    // 一度にまとめて送っても反映されないので1文字ずつ送る。
    // サンプルプログラムと同じ。
    //        plogw->ostream->print("wxSend: ");
    for (uint8_t i = 0; i < strlen(usbSndBuff); i++) {
      //             plogw->ostream->print(wxStr[i], HEX);
      //             plogw->ostream->print(' ');
      //             delay(10);
      rcode = Acm.SndData(1, (uint8_t *)&usbSndBuff[i]);
      if (rcode) {
        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
        plogw->ostream->println("SndData error");
      }
    }  //for
    //        plogw->ostream->println();

    /* reading from usb device */
    /* buffer size must be greater or equal to max.packet size */
    /* it it set to 64 (largest possible max.packet size) here, can be tuned down
      for particular endpoint */
    // 受け取り側はサンプルプログラムのまま。

    uint8_t buf[64];
    uint16_t rcvd = 64;
    rcode = Acm.RcvData(&rcvd, buf);
    if (rcode && rcode != hrNAK)
      ErrorMessage<uint8_t>(PSTR("Ret"), rcode);

    if (rcvd) {  //more than zero bytes received
      plogw->ostream->print("ACMrcvd:");
      for (uint16_t i = 0; i < rcvd; i++) {
        plogw->ostream->print((char)buf[i]);  //printing on the screen
      }
    }
  }
}

#endif

#ifdef notdef
CH34XAsyncOper CH34XAsyncOper;
CH34X Ch34x(&Usb, &CH34XAsyncOper);
#endif
/* on connecting IC-705 
ACM Init
Addr:0A
Num of Conf:01
Acm EPxtract adr:83
Acm   -> index:00000003
Endpoint descriptor:
Length:         07
Type:           05
Address:        83
Attributes:     03
MaxPktSize:     0010
Poll Intrv:     10
Acm EPxtract adr:86
Acm   -> index:00000003
Endpoint descriptor:
Length:         07
Type:           05
Address:        86           --> epaddr 6
Attributes:     03
MaxPktSize:     0010
Poll Intrv:     10
Acm EPxtract adr:01
Acm   -> index:00000002
Endpoint descriptor:
Length:         07
Type:           05
Address:        01
Attributes:     02
MaxPktSize:     0040
Poll Intrv:     00
Acm EPxtract adr:82
Acm   -> index:00000001
Endpoint descriptor:
Length:         07
Type:           05
Address:        82             --> epAddr 2
Attributes:     02
MaxPktSize:     0040
Poll Intrv:     00
Acm EPxtract adr:04
Acm   -> index:00000002
Endpoint descriptor:
Length:         07
Type:           05
Address:        04              --> epaddr 4
Attributes:     02
MaxPktSize:     0040
Poll Intrv:     00
Acm EPxtract adr:85
Acm   -> index:00000001
Endpoint descriptor:
Length:         07
Type:           05
Address:        85              --> epAddr 5 
Attributes:     02
MaxPktSize:     0040
Poll Intrv:     00
ACM NumEP:07
ConfNum:01
ACM configured
*/
BTD Btd(&Usb);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(&Btd);
BTHID bthid(&Btd, PAIR, "0000");
//#ifdef notdef
FTDIAsync FtdiAsync;
FTDI Ftdi(&Usb, &FtdiAsync);
//#endif

void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t *)&beforeMod) = before;
  MODIFIERKEYS afterMod;
  *((uint8_t *)&afterMod) = after;

  // check Right Shift if straight key mode
  struct radio *radio;

  radio = &radio_list[plogw->focused_radio];
  if (plogw->f_straightkey) {
    if (beforeMod.bmRightShift == 0 && afterMod.bmRightShift == 1) {
      if (plogw->so2r_tx != plogw->focused_radio) {
	keying(0);
	SO2R_settx(plogw->focused_radio);
      }
      switch (radio->modetype) {
        case LOG_MODETYPE_CW:
	  keying(1);
          if (verbose) plogw->ostream->print("keyon ");
          break;
      case LOG_MODETYPE_PH:  // not sure what this is trying to do on the phone
          radio->ptt = 1;
	  set_ptt_rig(radio, radio->ptt);
          break;
      }
    } else {
      if (beforeMod.bmRightShift == 1 && afterMod.bmRightShift == 0) {
        if (plogw->so2r_tx != plogw->focused_radio) {
	  keying(0);
	  SO2R_settx(plogw->focused_radio);
        }
        switch (radio->modetype) {
          case LOG_MODETYPE_CW:
	    keying(0);
            if (verbose) plogw->ostream->print("keyoff ");
            break;
          case LOG_MODETYPE_PH: // not sure what this is trying to do on the phone
            radio->ptt = 0;
	    set_ptt_rig(radio, radio->ptt);
            break;
        }
      }
    }
  }
}

void KbdRptParser::OnKeyUp(uint8_t mod, uint8_t key) {
  // check capslock for another modifier
  if (key == 0x39) {
    f_capslock = 0;
  }
}

void KbdRptParser::OnKeyPressed(uint8_t key) {
  if (verbose & 1) plogw->ostream->print((char)key);
};

void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key) {

  MODIFIERKEYS modkey;
  *((uint8_t *)&modkey) = mod;
  uint8_t c = KbdRptParser::OemToAscii(mod, key);


  on_key_down(modkey, key, c);
}

uint8_t KbdRptParser::OemToAscii2(uint8_t mod, uint8_t key) {

  MODIFIERKEYS modkey;
  *((uint8_t *)&modkey) = mod;
  uint8_t c = KbdRptParser::OemToAscii(mod, key);
  return c;
}

void KbdRptParser::PrintKey(uint8_t m, uint8_t key) {
  MODIFIERKEYS mod;
  *((uint8_t *)&mod) = m;
  print_key(mod, key);
}


HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(&Usb);
KbdRptParser Prs;

void init_usb() {
  if (Usb.Init() == -1)
    plogw->ostream->println("OSC did not start.");
  delay(500);
  HidKeyboard.SetReportParser(0, &Prs);



  
  bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
  //bthid.SetReportParser(MOUSE_PARSER_ID, &mousePrs);


  // If "Boot Protocol Mode" does not work, then try "Report Protocol Mode"
  // If that does not work either, then uncomment PRINTREPORT in BTHID.cpp to see the raw report
  bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);  // Boot Protocol Mode
  //  bthid.setProtocolMode(HID_RPT_PROTOCOL); // Report Protocol Mode

  plogw->ostream->print(F("\r\nHID Bluetooth Library Started"));
}

void loop_usb() {
    Usb.Task();
}

// just a wrapper 
uint8_t kbd_oemtoascii2(uint8_t mod,char c)
{
  return Prs.OemToAscii2(mod, c);
}
void usb_send_civ_buf() {
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      rcode = Ftdi.SndData(civ_buf_idx, (uint8_t *)civ_buf);
      //      rcode = Acm.SndData(civ_buf_idx, (uint8_t *)civ_buf);      
      if (verbose & 1) {
        plogw->ostream->print("send civ cmd:");
        for (int i = 0; i < civ_buf_idx; i++) {
          plogw->ostream->print((civ_buf[i]), HEX);
          plogw->ostream->print(" ");
        }
        plogw->ostream->println("");
      }

      if (rcode)
        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
    }
}

void usb_send_cat_buf(char *cmd) {
    // send to USB host serial adapter
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      //char strbuf[] = "IF;";

      rcode = Ftdi.SndData(strlen(cmd), (uint8_t *)cmd);
      //      rcode = Acm.SndData(strlen(cmd), (uint8_t *)cmd);      
      if (verbose & 1) {
        plogw->ostream->print("send cat cmd:");
        plogw->ostream->println(cmd);
        //	plogw->ostream->print("r:");plogw->ostream->print(r_ptr);
        //	plogw->ostream->print("w:");plogw->ostream->println(w_ptr);
        //	plogw->ostream->print("cmdbuf:");plogw->ostream->print(cmdbuf);plogw->ostream->print(":");plogw->ostream->println(cmd_ptr);
      }

      if (rcode)
        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
    }
}


void usb_receive_cat_data(struct radio *radio) {
    while (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      uint8_t buf[64];

      for (uint8_t i = 0; i < 64; i++)
        buf[i] = 0;

      uint16_t rcvd = 64;
      rcode = Ftdi.RcvData(&rcvd, buf);
      //      rcode = Acm.RcvData(&rcvd, buf);      

      if (rcode && rcode != hrNAK) {
	//        ErrorMessage<uint8_t>(PSTR("Ret"), rcode);
        return;
      }
      if (verbose & 1) {
	plogw->ostream->print("Ftdi: rcode=");
	//	plogw->ostream->print("Acm: rcode=");	
        plogw->ostream->println(rcode);
      }

      // The device reserves the first two bytes of data
      //   to contain the current values of the modem and line status registers.

      if (rcvd > 2) {
        if (verbose & 1) {
          plogw->ostream->print("received from USB serial : ");
        }
        for (uint8_t i = 2; i < rcvd; i++) {
          if (verbose & 1) {
            plogw->ostream->print(buf[i], HEX);
            plogw->ostream->print(" ");
          }
          radio->bt_buf[radio->w_ptr] = buf[i];
          radio->w_ptr++;
          radio->w_ptr %= 256;
        }
        if (verbose & 1) {
          plogw->ostream->println("");
        }
        //            plogw->ostream->println((char*)(buf+2));
      }
    }
}
