#ifndef FILE_USB_HOST_H
#define FILE_USB_HOST_H
#include <hidboot.h>
#include <BTHID.h>
//USB Usb;
//BTD Btd(USB);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(BTD);
//HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(USB);
class KbdRptParser : public KeyboardReportParser {
    void PrintKey(uint8_t mod, uint8_t key);
  protected:
    void OnControlKeysChanged(uint8_t before, uint8_t after);
    void OnKeyDown(uint8_t mod, uint8_t key);
    void OnKeyUp(uint8_t mod, uint8_t key);
    void OnKeyPressed(uint8_t key);
  public:
    uint8_t OemToAscii2(uint8_t mod, uint8_t key); // added to use

};

//KbdRptParser Prs;
void ACMprocess() ;
void init_usb();
void loop_usb();
  void usb_send_civ_buf() ;
void usb_send_cat_buf(char *cmd) ;
void usb_receive_cat_data(struct radio *radio);
uint8_t kbd_oemtoascii2(uint8_t mod,char c);
#endif
