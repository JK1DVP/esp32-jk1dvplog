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

#ifndef FILE_USB_HOST_H
#define FILE_USB_HOST_H

// USB host interface MAX3421E defineed by  SS and INT pins by MAX3421E_SS and MAX3421E_INT 
#define MAX3421E_SS P5
#if JK1DVPLOG_HWVER >=2 
#define MAX3421E_INT P25 // enabling PSRAM  moved INT pin moved to GPIO25
#else
#define MAX3421E_INT P17 // original hardware INT pin connected to GPIO17
#endif
#include <hidboot.h>
#include <BTHID.h>
#include "mux_transport.h"



//USB Usb;
//BTD Btd(USB);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(BTD);
//HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(USB);

void receive_pkt_handler_keyboard1_main(struct mux_packet *packet);

// Feed decoded RTTY text into the same display/autofill path used by the
// IC-705 second CDC interface.  Useful for terminal diagnostics as well.
void RTTYDecoderFeedText(const char *text, bool append_newline = false);
void RTTYDecoderResetAutofill();

#define KEYMSG_TYPE_ONCONTROLKEYSCHANGED 1
#define KEYMSG_TYPE_HANDLELOCKINGKEYS 2
#define KEYMSG_TYPE_ONKEYDOWN 3
#define KEYMSG_TYPE_ONKEYUP 4
struct keymsg_t {
  uint8_t type; // keymessage type 1: OnControlKeysChanged(uint8_t , uint8_t)
  // 2: HandleLockingKeys()
  // 3: OnKeyDown()
  // 4: OnKeyUp()
  uint8_t arg1; // 1st arg
  uint8_t arg2; // 2nd arg
  USBHID *hid;
};

#define QUEUE_KEYRPT_LEN 30
class KbdRptParser : public KeyboardReportParser {
  static  uint8_t symKeysUp_us[12];
  static uint8_t symKeysLo_us[12] ;
  static  uint8_t symKeysUp_jp[12] ;
  static   uint8_t symKeysLo_jp[12] ;    
  int kbd_type=0; // 0 us 1 jp106
  void PrintKey(uint8_t mod, uint8_t key);
  uint8_t buf_ext[8]; // external keyboard control keys state (only 0th is used)
  //  protected:
  
  struct keymsg_t msg;

  QueueHandle_t xQueueKeyRpt;

  virtual const uint8_t *getSymKeysUp() {
    if (kbd_type==1) {    
      return symKeysUp_jp;
    } else {
      return symKeysUp_us;
    }
  };

  virtual const uint8_t *getSymKeysLo() {
    if (kbd_type==1) {        
      return symKeysLo_jp;
    } else {
      return symKeysLo_us;      
    }
  };


  void init_keyrpt_queue() ;
  bool send_keyrpt_queue() ;
  uint32_t key_queue_drop_count = 0;
  uint32_t key_queue_drop_last_report_ms = 0;
  
  public:
  void process_keyrpt_queue(const char *profile_name = NULL) ;  
  KbdRptParser() {
    kbdLockingKeys.bLeds = 0;
    init_keyrpt_queue();
    us();
  };
  // here keyboard USB polling is done in separate task and reports to a queue, process_keyrpt_queue() will peek the reports in the queue.
  void Parse(USBHID *hid, bool is_rpt_id, uint8_t len, uint8_t *buf);
  uint8_t HandleLockingKeys(USBHID* hid, uint8_t key);
  void OnControlKeysChanged(uint8_t before, uint8_t after);
  void OnKeyDown(uint8_t mod, uint8_t key);
  void OnKeyUp(uint8_t mod, uint8_t key);
  void OnKeyPressed(uint8_t key);
  void Parse_extKbd(uint8_t hid_code,bool on)   ;
  void init_extKbd();
  void resync_extKbd(const char *reason);
  uint8_t OemToAscii(uint8_t mod, uint8_t key); // added to use  
  uint8_t OemToAscii2(uint8_t mod, uint8_t key); // added to use
  
  void jp() {
    kbd_type = 1;
  }
  void us() {
    kbd_type = 0;
  }

};

//KbdRptParser Prs;
extern KbdRptParser Prs,Prs1;

void USB_desc(); // print usb descriptors
bool usb_audio_capture_active();
bool usb_audio_capture_start(Print *out);
void usb_audio_capture_stop(Print *out);
void usb_audio_capture_status(Print *out);
void usb_audio_capture_free(Print *out);
void usb_audio_capture_diagnose(Print *out);
void usb_audio_capture_set_sof_sync(bool enable, Print *out);
bool usb_audio_capture_sof_sync();
const int16_t *usb_audio_capture_buffer();
size_t usb_audio_capture_samples();
uint32_t usb_audio_capture_sample_rate();
void ACMprocess() ;
bool usb_qmx_cat_ready();
bool usb_cat_ready_for_rig_type(uint8_t cat_type);
// Deferred CDC ACM DTR/RTS keying.  cwport 3=DTR, 4=RTS.
// usb_keying_request() is safe to call from the 1-ms CW ticker callback;
// the actual USB control transfer is performed later by loop_usb().
void usb_keying_request(uint8_t cwport, bool on);
void usb_keying_process();
// USB RTTY scheduler.  The 1-ms CW ticker only enqueues symbol states;
// loop_usb() applies them at measured 45.45-baud symbol boundaries so USB
// control-transfer jitter cannot collapse consecutive Baudot bits.
bool usb_rtty_begin(uint8_t cwport, uint32_t lead_ms, bool invert);
bool usb_rtty_symbol_request(uint8_t cwport, bool mark, uint32_t duration_us);
bool usb_rtty_end_request(uint8_t cwport);
bool usb_rtty_take_tx_done();
void USBRTTYsetInvert(bool invert, Print *out = nullptr);
bool USBRTTYgetInvert();
// True while the USB RTTY scheduler needs sub-symbol polling latency.
bool usb_rtty_fast_service_needed();
void USBRTTYtimingStatus(Print *out = nullptr);
void USBRTTYtimingDump(Print *out = nullptr);
void USBACMstatus(Print *out = nullptr);
bool USBACMselectKeyInterface(uint8_t iface, Print *out = nullptr);
bool USBACMcontrolTest(uint8_t state, Print *out = nullptr);
void CP2105process();
void CP2105status(Stream *out = nullptr);
bool CP2105selectPort(uint8_t port);
bool CP2105setBaud(uint8_t port, uint32_t baudrate);
bool CP2105controlTest(uint8_t port, char line, bool on, Stream *out = nullptr);
bool CP2105flowStatus(uint8_t port, Stream *out = nullptr);
bool CP2105setManualFlow(uint8_t port, Stream *out = nullptr);
void CP2105toggleDebug();
bool CP2105sendRaw(uint8_t port, const char *text);
void init_usb();
void loop_usb();
void usb_send_civ_buf() ;
void usb_send_cat_buf(char *cmd) ;
void usb_receive_cat_data(struct radio *radio);
uint8_t kbd_oemtoascii2(uint8_t mod,char c);



#endif
