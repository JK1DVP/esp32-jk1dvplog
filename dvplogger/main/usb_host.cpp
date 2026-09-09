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
#include "usb_host.h"
#include "usb_cat_transport.h"
#include "uac_pcm2901.h"

#include <cdcftdi.h>  // serial adapter
#include <cp2105.h>
#include <hidboot.h>
#include <usbhub.h>
#include <BTHID.h>
#include "keyboard.h"
#include "ui.h"
#include "cat.h"
#include "cw_keying.h"
#include "display.h"
#include "so2r.h"
#include "mux_transport.h"
#include "pgmstrings_usbhost.h"

#ifdef notdef
#include "cdc_ch34x.h"
#endif

USB Usb;
PCMAudioCapture PcmAudio(&Usb);
USBHub Hub(&Usb);  // 使用するハブの数だけ定義しておく
USBHub Hub2(&Usb);

void print_hex(int v, int num_places);
void printintfdescr( uint8_t* descr_ptr );
void printconfdescr( uint8_t* descr_ptr );
void printepdescr( uint8_t* descr_ptr );
void printunkdescr( uint8_t* descr_ptr );
uint8_t getconfdescr( uint8_t addr, uint8_t conf );
void printProgStr(const char* str);


// ---------------------------------------------------------------------------
// RTTY decoded-text display and conservative callsign autofill
// ---------------------------------------------------------------------------
static constexpr uint8_t RTTY_RX_ROWS = 5;
static constexpr uint8_t RTTY_RX_COLS = 20;
static char rtty_rx_line[RTTY_RX_ROWS][RTTY_RX_COLS + 1] = {{0}};
static uint8_t rtty_rx_col = 0;
static bool rtty_rx_last_cr = false;
static char rtty_rx_token[LEN_CALLSIGN + 1] = "";
static uint8_t rtty_rx_token_len = 0;

// Independent terminal log buffer.  The OLED display wraps every 20 columns,
// but the serial log should preserve the decoder's original CR/LF line so it
// is useful for contest logging and post-mortem debugging.
static constexpr size_t RTTY_TERM_LINE_MAX = 160;
static char rtty_term_line[RTTY_TERM_LINE_MAX + 1] = "";
static size_t rtty_term_line_len = 0;
static bool rtty_term_last_cr = false;

struct RttyCallCandidate {
  char call[LEN_CALLSIGN + 1];
  uint32_t when_ms;
};
static constexpr uint8_t RTTY_CALL_HISTORY = 8;
static RttyCallCandidate rtty_call_history[RTTY_CALL_HISTORY] = {};
static uint8_t rtty_call_history_count = 0;
static char rtty_last_autofill[N_RADIO][LEN_CALLSIGN + 1] = {{0}};

static void rtty_decoder_display_snapshot() {
  char text[RTTY_RX_ROWS * (RTTY_RX_COLS + 1) + 1];
  size_t pos = 0;
  for (uint8_t row = 0; row < RTTY_RX_ROWS; ++row) {
    const size_t n = strnlen(rtty_rx_line[row], RTTY_RX_COLS);
    if (pos + n + 2 > sizeof(text)) break;
    memcpy(text + pos, rtty_rx_line[row], n);
    pos += n;
    if (row + 1 < RTTY_RX_ROWS) text[pos++] = '\n';
  }
  text[pos] = '\0';
  upd_display_rtty_decoder(text);
}

static void rtty_decoder_newline() {
  for (uint8_t row = 0; row + 1 < RTTY_RX_ROWS; ++row)
    memcpy(rtty_rx_line[row], rtty_rx_line[row + 1], RTTY_RX_COLS + 1);
  memset(rtty_rx_line[RTTY_RX_ROWS - 1], 0, RTTY_RX_COLS + 1);
  rtty_rx_col = 0;
}

static void rtty_terminal_log_flush() {
  if (rtty_term_line_len == 0 || plogw == NULL || plogw->ostream == NULL) return;
  rtty_term_line[rtty_term_line_len] = '\0';
  plogw->ostream->printf("RTTY RX: %s\r\n", rtty_term_line);
  rtty_term_line_len = 0;
  rtty_term_line[0] = '\0';
}

static void rtty_terminal_log_feed(uint8_t c) {
  if (c == '\r') {
    rtty_terminal_log_flush();
    rtty_term_last_cr = true;
    return;
  }
  if (c == '\n') {
    if (!rtty_term_last_cr) rtty_terminal_log_flush();
    rtty_term_last_cr = false;
    return;
  }
  rtty_term_last_cr = false;

  if (c == 0x08 || c == 0x7f) {
    if (rtty_term_line_len > 0) rtty_term_line[--rtty_term_line_len] = '\0';
    return;
  }
  if (c == '\t') c = ' ';
  if (c < 0x20 || c > 0x7e) return;

  // Do not lose a very long decoder line.  Emit a continued chunk rather than
  // silently truncating it; normal contest exchanges are far shorter than this.
  if (rtty_term_line_len >= RTTY_TERM_LINE_MAX) rtty_terminal_log_flush();
  rtty_term_line[rtty_term_line_len++] = (char)c;
  rtty_term_line[rtty_term_line_len] = '\0';
}

static bool rtty_callsign_candidate(const char *s) {
  if (s == NULL) return false;
  const size_t n = strlen(s);
  if (n < 4 || n > LEN_CALLSIGN) return false;
  int alpha = 0, digit = 0, slash = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') alpha++;
    else if (c >= '0' && c <= '9') digit++;
    else if (c == '/') slash++;
    else return false;
  }
  if (alpha < 2 || digit < 1 || slash > 1) return false;
  if (s[0] == '/' || s[n - 1] == '/') return false;
  // Common RTTY report/control tokens are not callsigns.
  if (strcmp(s, "5NN") == 0 || strcmp(s, "599") == 0) return false;
  if (plogw != NULL && plogw->my_callsign[1] != 0 &&
      strcasecmp(s, plogw->my_callsign + 2) == 0) return false;
  return true;
}

static int rtty_call_distance(const char *a, const char *b) {
  if (strlen(a) != strlen(b)) return 99;
  int d = 0;
  for (size_t i = 0; a[i]; ++i) {
    if (a[i] != b[i] && ++d > 2) return d;
  }
  return d;
}

static void rtty_try_autofill_callsign(const char *new_call) {
  if (!rtty_callsign_candidate(new_call)) return;
  if (plogw != NULL && plogw->ostream != NULL)
    plogw->ostream->printf("RTTY CALL candidate=%s\r\n", new_call);

  // Newest candidate is the cluster anchor.  This naturally forgets a
  // previous QSO as soon as a sufficiently different recent callsign arrives.
  if (rtty_call_history_count < RTTY_CALL_HISTORY) rtty_call_history_count++;
  for (int i = rtty_call_history_count - 1; i > 0; --i)
    rtty_call_history[i] = rtty_call_history[i - 1];
  strlcpy(rtty_call_history[0].call, new_call, sizeof(rtty_call_history[0].call));
  rtty_call_history[0].when_ms = millis();

  const size_t n = strlen(new_call);
  const uint32_t now = millis();
  int cluster[RTTY_CALL_HISTORY];
  int nc = 0;
  for (uint8_t i = 0; i < rtty_call_history_count; ++i) {
    if ((uint32_t)(now - rtty_call_history[i].when_ms) > 15000U) continue;
    if (rtty_call_distance(new_call, rtty_call_history[i].call) <= 2)
      cluster[nc++] = i;
  }
  // One decode is too easy to get wrong.  Wait for a repeat/similar decode.
  if (nc < 2) {
    plogw->ostream->printf("RTTY CALL consensus=WAIT samples=%d\r\n", nc);
    return;
  }

  char consensus[LEN_CALLSIGN + 1];
  for (size_t pos = 0; pos < n; ++pos) {
    int counts[37] = {0}; // A-Z,0-9,/
    for (int j = 0; j < nc; ++j) {
      char c = rtty_call_history[cluster[j]].call[pos];
      int k = (c >= 'A' && c <= 'Z') ? c - 'A' :
              (c >= '0' && c <= '9') ? 26 + c - '0' :
              (c == '/') ? 36 : -1;
      if (k >= 0) counts[k]++;
    }
    int best = -1, bestn = 0;
    for (int k = 0; k < 37; ++k) {
      if (counts[k] > bestn) { bestn = counts[k]; best = k; }
    }
    if (bestn * 2 <= nc) consensus[pos] = '-';
    else if (best < 26) consensus[pos] = (char)('A' + best);
    else if (best < 36) consensus[pos] = (char)('0' + best - 26);
    else consensus[pos] = '/';
  }
  consensus[n] = '\0';
  plogw->ostream->printf("RTTY CALL consensus=%s samples=%d%s\r\n",
                         consensus, nc,
                         strchr(consensus, '-') ? " PARTIAL" : "");

  struct radio *radio = so2r.radio_selected();
  if (radio == NULL) return;
  const int ridx = (int)(radio - radio_list);
  if (ridx < 0 || ridx >= N_RADIO) return;

  // Continue refining only an empty field or a value that this decoder put
  // there.  Any operator edit immediately takes ownership and stops autofill.
  const char *current = radio->callsign + 2;
  if (*current != '\0' && strcmp(current, rtty_last_autofill[ridx]) != 0) {
    rtty_last_autofill[ridx][0] = '\0';
    return;
  }
  if (strcmp(current, consensus) == 0) return;

  strlcpy(rtty_last_autofill[ridx], consensus, sizeof(rtty_last_autofill[ridx]));
  set_callsign_and_request_dupe(radio, consensus, true);
  plogw->ostream->printf("RTTY CALL autofill=%s samples=%d%s\n",
                         consensus, nc,
                         strchr(consensus, '-') ? " PARTIAL-only" : "");
}

static void rtty_decoder_finish_token() {
  if (rtty_rx_token_len == 0) return;
  rtty_rx_token[rtty_rx_token_len] = '\0';
  rtty_try_autofill_callsign(rtty_rx_token);
  rtty_rx_token_len = 0;
  rtty_rx_token[0] = '\0';
}

static void rtty_decoder_feed_byte(uint8_t c) {
  rtty_terminal_log_feed(c);

  if (c == '\r') {
    rtty_decoder_finish_token();
    rtty_decoder_newline();
    rtty_rx_last_cr = true;
    return;
  }
  if (c == '\n') {
    rtty_decoder_finish_token();
    if (!rtty_rx_last_cr) rtty_decoder_newline();
    rtty_rx_last_cr = false;
    return;
  }
  rtty_rx_last_cr = false;

  if (c == 0x08 || c == 0x7f) {
    if (rtty_rx_col > 0) {
      --rtty_rx_col;
      rtty_rx_line[RTTY_RX_ROWS - 1][rtty_rx_col] = '\0';
    }
    if (rtty_rx_token_len > 0) rtty_rx_token[--rtty_rx_token_len] = '\0';
    return;
  }

  if (c == '\t') c = ' ';
  if (c < 0x20 || c > 0x7e) return;
  if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');

  if (rtty_rx_col >= RTTY_RX_COLS) rtty_decoder_newline();
  rtty_rx_line[RTTY_RX_ROWS - 1][rtty_rx_col++] = (char)c;
  rtty_rx_line[RTTY_RX_ROWS - 1][rtty_rx_col] = '\0';

  // Callsign token chars.  Everything else terminates a token.  '-' is not
  // accepted from the decoder itself; it is reserved for our uncertainty
  // marker generated by consensus.
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/') {
    if (rtty_rx_token_len < LEN_CALLSIGN)
      rtty_rx_token[rtty_rx_token_len++] = (char)c;
  } else {
    rtty_decoder_finish_token();
  }
}

void RTTYDecoderFeedText(const char *text, bool append_newline) {
  if (text == NULL) return;
  while (*text) rtty_decoder_feed_byte((uint8_t)*text++);
  if (append_newline) rtty_decoder_feed_byte('\n');
  // A terminal injection often ends without whitespace.  Treat end-of-call as
  // a token boundary without changing the visual line unless requested.
  rtty_decoder_finish_token();
  rtty_decoder_display_snapshot();
}

void RTTYDecoderResetAutofill() {
  rtty_call_history_count = 0;
  rtty_rx_token_len = 0;
  rtty_rx_token[0] = '\0';
  rtty_term_line_len = 0;
  rtty_term_line[0] = '\0';
  rtty_term_last_cr = false;
  memset(rtty_last_autofill, 0, sizeof(rtty_last_autofill));
}

void PrintAllAddresses(UsbDevice *pdev)
{
  UsbDeviceAddress adr;
  adr.devAddress = pdev->address.devAddress;
  Serial.print("\r\nAddr:");
  Serial.print(adr.devAddress, HEX);
  Serial.print("(");
  Serial.print(adr.bmHub, HEX);
  Serial.print(".");
  Serial.print(adr.bmParent, HEX);
  Serial.print(".");
  Serial.print(adr.bmAddress, HEX);
  Serial.println(")");
}

void PrintAddress(uint8_t addr)
{
  UsbDeviceAddress adr;
  adr.devAddress = addr;
  Serial.print("\r\nADDR:\t");
  Serial.println(adr.devAddress, HEX);
  Serial.print("DEV:\t");
  Serial.println(adr.bmAddress, HEX);
  Serial.print("PRNT:\t");
  Serial.println(adr.bmParent, HEX);
  Serial.print("HUB:\t");
  Serial.println(adr.bmHub, HEX);
}
uint8_t getdevdescr( uint8_t addr, uint8_t &num_conf );


void PrintDescriptors(uint8_t addr)
{
  uint8_t rcode = 0;
  uint8_t num_conf = 0;

  rcode = getdevdescr( (uint8_t)addr, num_conf );
  if ( rcode )
  {
    printProgStr(Gen_Error_str);
    print_hex( rcode, 8 );
  }
  Serial.print("\r\n");

  for (int i = 0; i < num_conf; i++)
  {
    rcode = getconfdescr( addr, i );                 // get configuration descriptor
    if ( rcode )
    {
      printProgStr(Gen_Error_str);
      print_hex(rcode, 8);
    }
    Serial.println("\r\n");
  }
}

void PrintAllDescriptors(UsbDevice *pdev)
{
  Serial.println("\r\n");
  print_hex(pdev->address.devAddress, 8);
  Serial.println("\r\n--");
  PrintDescriptors( pdev->address.devAddress );
}

uint8_t getdevdescr( uint8_t addr, uint8_t &num_conf )
{
  USB_DEVICE_DESCRIPTOR buf;
  uint8_t rcode;
  rcode = Usb.getDevDescr( addr, 0, 0x12, ( uint8_t *)&buf );
  if ( rcode ) {
    return ( rcode );
  }
  printProgStr(Dev_Header_str);
  printProgStr(Dev_Length_str);
  print_hex( buf.bLength, 8 );
  printProgStr(Dev_Type_str);
  print_hex( buf.bDescriptorType, 8 );
  printProgStr(Dev_Version_str);
  print_hex( buf.bcdUSB, 16 );
  printProgStr(Dev_Class_str);
  print_hex( buf.bDeviceClass, 8 );
  printProgStr(Dev_Subclass_str);
  print_hex( buf.bDeviceSubClass, 8 );
  printProgStr(Dev_Protocol_str);
  print_hex( buf.bDeviceProtocol, 8 );
  printProgStr(Dev_Pktsize_str);
  print_hex( buf.bMaxPacketSize0, 8 );
  printProgStr(Dev_Vendor_str);
  print_hex( buf.idVendor, 16 );
  printProgStr(Dev_Product_str);
  print_hex( buf.idProduct, 16 );
  printProgStr(Dev_Revision_str);
  print_hex( buf.bcdDevice, 16 );
  printProgStr(Dev_Mfg_str);
  print_hex( buf.iManufacturer, 8 );
  printProgStr(Dev_Prod_str);
  print_hex( buf.iProduct, 8 );
  printProgStr(Dev_Serial_str);
  print_hex( buf.iSerialNumber, 8 );
  printProgStr(Dev_Nconf_str);
  print_hex( buf.bNumConfigurations, 8 );
  num_conf = buf.bNumConfigurations;
  return ( 0 );
}

void printhubdescr(uint8_t *descrptr, uint8_t addr)
{
  HubDescriptor  *pHub = (HubDescriptor*) descrptr;
  uint8_t        len = *((uint8_t*)descrptr);

  printProgStr(PSTR("\r\n\r\nHub Descriptor:\r\n"));
  printProgStr(PSTR("bDescLength:\t\t"));
  Serial.println(pHub->bDescLength, HEX);

  printProgStr(PSTR("bDescriptorType:\t"));
  Serial.println(pHub->bDescriptorType, HEX);

  printProgStr(PSTR("bNbrPorts:\t\t"));
  Serial.println(pHub->bNbrPorts, HEX);

  printProgStr(PSTR("LogPwrSwitchMode:\t"));
  Serial.println(pHub->LogPwrSwitchMode, BIN);

  printProgStr(PSTR("CompoundDevice:\t\t"));
  Serial.println(pHub->CompoundDevice, BIN);

  printProgStr(PSTR("OverCurrentProtectMode:\t"));
  Serial.println(pHub->OverCurrentProtectMode, BIN);

  printProgStr(PSTR("TTThinkTime:\t\t"));
  Serial.println(pHub->TTThinkTime, BIN);

  printProgStr(PSTR("PortIndicatorsSupported:"));
  Serial.println(pHub->PortIndicatorsSupported, BIN);

  printProgStr(PSTR("Reserved:\t\t"));
  Serial.println(pHub->Reserved, HEX);

  printProgStr(PSTR("bPwrOn2PwrGood:\t\t"));
  Serial.println(pHub->bPwrOn2PwrGood, HEX);

  printProgStr(PSTR("bHubContrCurrent:\t"));
  Serial.println(pHub->bHubContrCurrent, HEX);

  for (uint8_t i = 7; i < len; i++)
    print_hex(descrptr[i], 8);

  //for (uint8_t i=1; i<=pHub->bNbrPorts; i++)
  //    PrintHubPortStatus(&Usb, addr, i, 1);
}

uint8_t getconfdescr( uint8_t addr, uint8_t conf )
{
  uint8_t buf[ BUFSIZE ];
  uint8_t* buf_ptr = buf;
  uint8_t rcode;
  uint8_t descr_length;
  uint8_t descr_type;
  uint16_t total_length;
  rcode = Usb.getConfDescr( addr, 0, 4, conf, buf );  //get total length
  LOBYTE( total_length ) = buf[ 2 ];
  HIBYTE( total_length ) = buf[ 3 ];
  if ( total_length > 256 ) {   //check if total length is larger than buffer
    printProgStr(Conf_Trunc_str);
    total_length = 256;
  }
  rcode = Usb.getConfDescr( addr, 0, total_length, conf, buf ); //get the whole descriptor
  while ( buf_ptr < buf + total_length ) { //parsing descriptors
    descr_length = *( buf_ptr );
    descr_type = *( buf_ptr + 1 );
    switch ( descr_type ) {
      case ( USB_DESCRIPTOR_CONFIGURATION ):
        printconfdescr( buf_ptr );
        break;
      case ( USB_DESCRIPTOR_INTERFACE ):
        printintfdescr( buf_ptr );
        break;
      case ( USB_DESCRIPTOR_ENDPOINT ):
        printepdescr( buf_ptr );
        break;
      case 0x29:
        printhubdescr( buf_ptr, addr );
        break;
      default:
        printunkdescr( buf_ptr );
        break;
    }//switch( descr_type
    buf_ptr = ( buf_ptr + descr_length );    //advance buffer pointer
  }//while( buf_ptr <=...
  return ( rcode );
}
// copyright, Peter H Anderson, Baltimore, MD, Nov, '07
// source: http://www.phanderson.com/arduino/arduino_display.html
void print_hex(int v, int num_places)
{
  int mask = 0, n, num_nibbles, digit;

  for (n = 1; n <= num_places; n++) {
    mask = (mask << 1) | 0x0001;
  }
  v = v & mask; // truncate v to specified number of places

  num_nibbles = num_places / 4;
  if ((num_places % 4) != 0) {
    ++num_nibbles;
  }
  do {
    digit = ((v >> (num_nibbles - 1) * 4)) & 0x0f;
    Serial.print(digit, HEX);
  }
  while (--num_nibbles);
}
void printconfdescr( uint8_t* descr_ptr )
{
  USB_CONFIGURATION_DESCRIPTOR* conf_ptr = ( USB_CONFIGURATION_DESCRIPTOR* )descr_ptr;
  printProgStr(Conf_Header_str);
  printProgStr(Conf_Totlen_str);
  print_hex( conf_ptr->wTotalLength, 16 );
  printProgStr(Conf_Nint_str);
  print_hex( conf_ptr->bNumInterfaces, 8 );
  printProgStr(Conf_Value_str);
  print_hex( conf_ptr->bConfigurationValue, 8 );
  printProgStr(Conf_String_str);
  print_hex( conf_ptr->iConfiguration, 8 );
  printProgStr(Conf_Attr_str);
  print_hex( conf_ptr->bmAttributes, 8 );
  printProgStr(Conf_Pwr_str);
  print_hex( conf_ptr->bMaxPower, 8 );
  return;
}
void printintfdescr( uint8_t* descr_ptr )
{
  USB_INTERFACE_DESCRIPTOR* intf_ptr = ( USB_INTERFACE_DESCRIPTOR* )descr_ptr;
  printProgStr(Int_Header_str);
  printProgStr(Int_Number_str);
  print_hex( intf_ptr->bInterfaceNumber, 8 );
  printProgStr(Int_Alt_str);
  print_hex( intf_ptr->bAlternateSetting, 8 );
  printProgStr(Int_Endpoints_str);
  print_hex( intf_ptr->bNumEndpoints, 8 );
  printProgStr(Int_Class_str);
  print_hex( intf_ptr->bInterfaceClass, 8 );
  printProgStr(Int_Subclass_str);
  print_hex( intf_ptr->bInterfaceSubClass, 8 );
  printProgStr(Int_Protocol_str);
  print_hex( intf_ptr->bInterfaceProtocol, 8 );
  printProgStr(Int_String_str);
  print_hex( intf_ptr->iInterface, 8 );
  return;
}
void printepdescr( uint8_t* descr_ptr )
{
  USB_ENDPOINT_DESCRIPTOR* ep_ptr = ( USB_ENDPOINT_DESCRIPTOR* )descr_ptr;
  printProgStr(End_Header_str);
  printProgStr(End_Address_str);
  print_hex( ep_ptr->bEndpointAddress, 8 );
  printProgStr(End_Attr_str);
  print_hex( ep_ptr->bmAttributes, 8 );
  printProgStr(End_Pktsize_str);
  print_hex( ep_ptr->wMaxPacketSize, 16 );
  printProgStr(End_Interval_str);
  print_hex( ep_ptr->bInterval, 8 );

  return;
}
void printunkdescr( uint8_t* descr_ptr )
{
  uint8_t length = *descr_ptr;
  uint8_t i;
  printProgStr(Unk_Header_str);
  printProgStr(Unk_Length_str);
  print_hex( *descr_ptr, 8 );
  printProgStr(Unk_Type_str);
  print_hex( *(descr_ptr + 1 ), 8 );
  printProgStr(Unk_Contents_str);
  descr_ptr += 2;
  for ( i = 0; i < length; i++ ) {
    print_hex( *descr_ptr, 8 );
    descr_ptr++;
  }
}


void printProgStr(const char* str)
{
  char c;
  if (!str) return;
  while ((c = pgm_read_byte(str++)))
    Serial.print(c);
}
// these are from USB_desc.ino

void USB_desc()
{
    if ( Usb.getUsbTaskState() == USB_STATE_RUNNING )  {
      Usb.ForEachUsbDevice(&PrintAllDescriptors);
      Usb.ForEachUsbDevice(&PrintAllAddresses);
    }

}


// qmx idVendor=0483, idProduct=a34c

#ifdef notdef
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
#endif

//using MyMax = 
//USB<MyMax> Usb;


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


//#ifdef notdef
// IC-705 USB Acm port
#include <cdcacm.h>

class ACMAsyncOper : public CDCAsyncOper {
  public:
    uint8_t OnInit(ACM *pacm);
};

static constexpr uint16_t QMX_USB_VID = 0x0483;
static constexpr uint16_t QMX_USB_PID = 0xA34C;
static constexpr uint16_t ATS_MINI_USB_VID = 0x303A;
static constexpr uint16_t ATS_MINI_USB_PID = 0x1001;

// CDC ACM line-state keying is deferred out of the 1-ms CW ticker.  A USB
// control transfer must never be issued directly from interrupt_cw_send().
struct UsbKeyingEvent {
  uint8_t cwport;
  uint8_t on;
};
static constexpr uint8_t USB_KEYING_QUEUE_LEN = 32;
static volatile UsbKeyingEvent usb_keying_queue[USB_KEYING_QUEUE_LEN];
static volatile uint8_t usb_keying_head = 0;
static volatile uint8_t usb_keying_tail = 0;
static volatile uint32_t usb_keying_drops = 0;
static uint8_t usb_acm_line_state = 0; // bit0 DTR, bit1 RTS
// CDC class requests use wIndex=interface.  IC-705 is composite, so keep
// the keying control-interface selectable until its USB(A) interface is
// identified from descriptors/diagnostic probing.
static uint8_t usb_acm_key_iface = 0;
extern ACM Acm;

// RTTY over USB DTR/RTS needs much tighter timing than CW.  Do not issue
// SET_CONTROL_LINE_STATE from the 1-ms ticker.  Instead queue complete
// Baudot symbol cells here and let loop_usb() start each cell only after the
// previous cell has actually occupied its requested duration.
struct UsbRttyEvent {
  uint8_t kind;       // 0=symbol, 1=end-of-message
  uint8_t cwport;     // 3=DTR, 4=RTS
  uint8_t mark;       // logical MARK state for kind=0
  uint32_t duration_us;
};
static constexpr uint8_t USB_RTTY_QUEUE_LEN = 64;
static volatile UsbRttyEvent usb_rtty_queue[USB_RTTY_QUEUE_LEN];
static volatile uint8_t usb_rtty_head = 0;
static volatile uint8_t usb_rtty_tail = 0;
static volatile uint32_t usb_rtty_drops = 0;
static volatile bool usb_rtty_tx_done = false;
static bool usb_rtty_active = false;
static uint32_t usb_rtty_due_us = 0;
static uint32_t usb_rtty_last_boundary_us = 0;
static uint32_t usb_rtty_last_duration_us = 0;
static uint8_t usb_rtty_last_mark = 1;
// Logical RTTY MARK/SPACE to physical CDC control-line polarity.
// false: logical MARK=asserted (DTR/RTS=1), true: logical MARK=deasserted.
// Default true is required for correct Baudot decoding with the tested
// IC-705 USB(A) DTR RTTY keying setup.  This changes only the logical
// MARK/SPACE-to-control-line polarity; it does not change the RF frequency pair.
static volatile bool usb_rtty_invert = true;
static bool usb_rtty_active_invert = true;

// DTR/RTS transport is selected automatically: CDC ACM for Icom/generic
// radios, CP2105 Standard COM (port 1) for Yaesu USB CAT radios.
static bool usb_control_line_ready();
static uint8_t usb_control_line_apply(uint8_t new_state);
static uint8_t usb_control_line_state();

struct UsbRttyTimingSample {
  uint32_t dt_us;
  uint32_t expected_us;
  uint8_t mark;
  uint8_t rcode;
};
static constexpr uint8_t USB_RTTY_TIMING_LEN = 64;
static UsbRttyTimingSample usb_rtty_timing[USB_RTTY_TIMING_LEN];
static uint8_t usb_rtty_timing_w = 0;
static uint8_t usb_rtty_timing_n = 0;
static uint32_t usb_rtty_timing_min = 0xffffffffUL;
static uint32_t usb_rtty_timing_max = 0;
static uint32_t usb_rtty_timing_maxerr = 0;

static bool usb_time_reached(uint32_t now, uint32_t due)
{
  return (int32_t)(now - due) >= 0;
}

static void usb_rtty_timing_reset()
{
  usb_rtty_timing_w = 0;
  usb_rtty_timing_n = 0;
  usb_rtty_timing_min = 0xffffffffUL;
  usb_rtty_timing_max = 0;
  usb_rtty_timing_maxerr = 0;
  usb_rtty_last_boundary_us = 0;
  usb_rtty_last_duration_us = 0;
  usb_rtty_last_mark = 1;
}

static void usb_rtty_record_boundary(uint32_t now, uint8_t next_mark, uint8_t rcode)
{
  if (usb_rtty_last_boundary_us != 0) {
    const uint32_t dt = now - usb_rtty_last_boundary_us;
    const uint32_t expected = usb_rtty_last_duration_us;
    const uint32_t err = (dt > expected) ? (dt - expected) : (expected - dt);
    UsbRttyTimingSample &sm = usb_rtty_timing[usb_rtty_timing_w];
    sm.dt_us = dt;
    sm.expected_us = expected;
    sm.mark = usb_rtty_last_mark;
    sm.rcode = rcode;
    usb_rtty_timing_w = static_cast<uint8_t>((usb_rtty_timing_w + 1) % USB_RTTY_TIMING_LEN);
    if (usb_rtty_timing_n < USB_RTTY_TIMING_LEN) ++usb_rtty_timing_n;
    if (dt < usb_rtty_timing_min) usb_rtty_timing_min = dt;
    if (dt > usb_rtty_timing_max) usb_rtty_timing_max = dt;
    if (err > usb_rtty_timing_maxerr) usb_rtty_timing_maxerr = err;
  }
  usb_rtty_last_boundary_us = now;
  usb_rtty_last_mark = next_mark ? 1 : 0;
}

static bool usb_rtty_enqueue(uint8_t kind, uint8_t cwport, bool mark, uint32_t duration_us)
{
  if (cwport != 3 && cwport != 4) return false;
  const uint8_t head = usb_rtty_head;
  const uint8_t next = static_cast<uint8_t>((head + 1) % USB_RTTY_QUEUE_LEN);
  if (next == usb_rtty_tail) {
    ++usb_rtty_drops;
    return false;
  }
  usb_rtty_queue[head].kind = kind;
  usb_rtty_queue[head].cwport = cwport;
  usb_rtty_queue[head].mark = mark ? 1 : 0;
  usb_rtty_queue[head].duration_us = duration_us;
  usb_rtty_head = next;
  return true;
}

bool usb_rtty_begin(uint8_t cwport, uint32_t lead_ms, bool invert)
{
  if (cwport != 3 && cwport != 4) return false;
  // A new STX starts a fresh RTTY timing epoch.
  usb_rtty_tail = usb_rtty_head;
  usb_rtty_active = false;
  usb_rtty_tx_done = false;
  usb_rtty_timing_reset();
  usb_rtty_active_invert = invert;
  return usb_rtty_enqueue(0, cwport, true, lead_ms * 1000UL);
}

bool usb_rtty_symbol_request(uint8_t cwport, bool mark, uint32_t duration_us)
{
  if (duration_us == 0) duration_us = 1;
  return usb_rtty_enqueue(0, cwport, mark, duration_us);
}

bool usb_rtty_end_request(uint8_t cwport)
{
  return usb_rtty_enqueue(1, cwport, true, 0);
}

bool usb_rtty_take_tx_done()
{
  if (!usb_rtty_tx_done) return false;
  usb_rtty_tx_done = false;
  return true;
}

void USBRTTYsetInvert(bool invert, Print *out)
{
  usb_rtty_invert = invert;
  if (!out) out = console;
  if (out) out->printf("USB RTTY invert=%d (logical MARK -> line %s)\n",
                       usb_rtty_invert ? 1 : 0,
                       usb_rtty_invert ? "deasserted" : "asserted");
}

bool USBRTTYgetInvert()
{
  return usb_rtty_invert;
}

bool usb_rtty_fast_service_needed()
{
  // Include queued-but-not-yet-started lead/ETX cells as well as an active
  // cell so the USB loop changes to the 1-ms cadence before the first edge.
  return usb_rtty_active || (usb_rtty_head != usb_rtty_tail);
}

static void usb_rtty_process()
{
  if (!usb_control_line_ready()) return;

  if (usb_rtty_tail == usb_rtty_head) return;
  uint32_t now = micros();
  const bool had_active_cell = usb_rtty_active;
  const uint32_t scheduled_boundary_us = usb_rtty_due_us;
  if (had_active_cell && !usb_time_reached(now, scheduled_boundary_us)) return;

  const uint8_t tail = usb_rtty_tail;
  const UsbRttyEvent ev = {
    usb_rtty_queue[tail].kind,
    usb_rtty_queue[tail].cwport,
    usb_rtty_queue[tail].mark,
    usb_rtty_queue[tail].duration_us
  };
  usb_rtty_tail = static_cast<uint8_t>((tail + 1) % USB_RTTY_QUEUE_LEN);

  if (ev.kind == 1) {
    usb_rtty_active = false;
    usb_rtty_tx_done = true;
    return;
  }

  const uint8_t old_state = usb_control_line_state();
  uint8_t new_state = old_state;
  const uint8_t mask = (ev.cwport == 3) ? 0x01 : 0x02;
  // ev.mark is always the logical Baudot state used by diagnostics.  Apply
  // optional inversion only at the final CDC DTR/RTS mapping so Baudot
  // generation, timing logs, and the physical-line polarity stay separable.
  const bool line_asserted = (ev.mark != 0) ^ usb_rtty_active_invert;
  if (line_asserted) new_state |= mask;
  else               new_state &= static_cast<uint8_t>(~mask);

  uint8_t rcode = 0;
  if (new_state != old_state)
    rcode = usb_control_line_apply(new_state);

  // Timestamp after the control transfer: this is closest to the instant
  // at which the rig has accepted the new DTR/RTS state.
  now = micros();
  usb_rtty_record_boundary(now, ev.mark, rcode);
  usb_rtty_last_duration_us = ev.duration_us;

  // Keep a phase-locked 45.45-baud timeline instead of adding the USB-loop
  // wake-up latency to every cell.  With 1-ms polling this gives adjacent
  // cells a small +/- jitter while preserving the long-term 22.000-ms rate.
  // If the task was delayed badly, resynchronise rather than emitting a very
  // short catch-up cell.
  if (had_active_cell) {
    const int32_t late_us = (int32_t)(now - scheduled_boundary_us);
    if (late_us >= 0 && late_us <= 4000)
      usb_rtty_due_us = scheduled_boundary_us + ev.duration_us;
    else
      usb_rtty_due_us = now + ev.duration_us;
  } else {
    usb_rtty_due_us = now + ev.duration_us;
  }
  usb_rtty_active = true;
}

void USBRTTYtimingStatus(Print *out)
{
  if (!out) out = console;
  const uint8_t qdepth = (usb_rtty_head >= usb_rtty_tail)
      ? (usb_rtty_head - usb_rtty_tail)
      : (USB_RTTY_QUEUE_LEN - usb_rtty_tail + usb_rtty_head);

  uint32_t total_all = 0;
  uint32_t min_all = 0xffffffffUL;
  uint32_t max_all = 0;
  uint32_t maxerr_all = 0;
  uint32_t total_22 = 0, min_22 = 0xffffffffUL, max_22 = 0, late24_22 = 0, late30_22 = 0;
  uint32_t total_11 = 0, min_11 = 0xffffffffUL, max_11 = 0;
  uint16_t n22 = 0, n11 = 0;

  const uint8_t n = usb_rtty_timing_n;
  uint8_t idx = static_cast<uint8_t>((usb_rtty_timing_w + USB_RTTY_TIMING_LEN - n) % USB_RTTY_TIMING_LEN);
  for (uint8_t i = 0; i < n; ++i) {
    const UsbRttyTimingSample &sm = usb_rtty_timing[idx];
    const uint32_t err = (sm.dt_us > sm.expected_us)
        ? (sm.dt_us - sm.expected_us) : (sm.expected_us - sm.dt_us);
    total_all += sm.dt_us;
    if (sm.dt_us < min_all) min_all = sm.dt_us;
    if (sm.dt_us > max_all) max_all = sm.dt_us;
    if (err > maxerr_all) maxerr_all = err;

    if (sm.expected_us == 22000UL) {
      ++n22; total_22 += sm.dt_us;
      if (sm.dt_us < min_22) min_22 = sm.dt_us;
      if (sm.dt_us > max_22) max_22 = sm.dt_us;
      if (sm.dt_us > 24000UL) ++late24_22;
      if (sm.dt_us > 30000UL) ++late30_22;
    } else if (sm.expected_us == 11000UL) {
      ++n11; total_11 += sm.dt_us;
      if (sm.dt_us < min_11) min_11 = sm.dt_us;
      if (sm.dt_us > max_11) max_11 = sm.dt_us;
    }
    idx = static_cast<uint8_t>((idx + 1) % USB_RTTY_TIMING_LEN);
  }

  out->printf("USB RTTY active=%d fast=%d invert=%d q=%u drops=%lu samples=%u",
              usb_rtty_active ? 1 : 0, usb_rtty_fast_service_needed() ? 1 : 0,
              usb_rtty_invert ? 1 : 0, qdepth, (unsigned long)usb_rtty_drops, n);
  if (n) {
    out->printf(" all[min=%lu max=%lu avg=%lu maxerr=%lu]",
                (unsigned long)min_all, (unsigned long)max_all,
                (unsigned long)(total_all / n), (unsigned long)maxerr_all);
  }
  out->println();

  if (n22) {
    out->printf("  22ms n=%u min=%lu max=%lu avg=%lu >24ms=%lu >30ms=%lu\n",
                n22, (unsigned long)min_22, (unsigned long)max_22,
                (unsigned long)(total_22 / n22),
                (unsigned long)late24_22, (unsigned long)late30_22);
  }
  if (n11) {
    out->printf("  11ms n=%u min=%lu max=%lu avg=%lu\n",
                n11, (unsigned long)min_11, (unsigned long)max_11,
                (unsigned long)(total_11 / n11));
  }
}

void USBRTTYtimingDump(Print *out)
{
  if (!out) out = console;
  USBRTTYtimingStatus(out);
  const uint8_t n = usb_rtty_timing_n;
  uint8_t idx = static_cast<uint8_t>((usb_rtty_timing_w + USB_RTTY_TIMING_LEN - n) % USB_RTTY_TIMING_LEN);
  for (uint8_t i = 0; i < n; ++i) {
    const UsbRttyTimingSample &sm = usb_rtty_timing[idx];
    out->printf("RTTY TIMING %u dt=%lu expected=%lu err=%ld state=%c rcode=0x%02X\n",
                i, (unsigned long)sm.dt_us, (unsigned long)sm.expected_us,
                (long)sm.dt_us - (long)sm.expected_us, sm.mark ? 'M' : 'S', sm.rcode);
    idx = static_cast<uint8_t>((idx + 1) % USB_RTTY_TIMING_LEN);
  }
}

static void usb_keying_queue_clear()
{
  usb_keying_tail = usb_keying_head;
}

void usb_keying_request(uint8_t cwport, bool on)
{
  if (cwport != 3 && cwport != 4) return;

  const uint8_t head = usb_keying_head;
  const uint8_t next = static_cast<uint8_t>((head + 1) % USB_KEYING_QUEUE_LEN);
  if (next == usb_keying_tail) {
    ++usb_keying_drops;
    return;
  }

  usb_keying_queue[head].cwport = cwport;
  usb_keying_queue[head].on = on ? 1 : 0;
  usb_keying_head = next;
}

uint8_t ACMAsyncOper::OnInit(ACM *pacm) {
  uint8_t rcode;
  const bool is_qmx = pacm->IsDevice(QMX_USB_VID, QMX_USB_PID);
  const bool is_ats_mini = pacm->IsDevice(ATS_MINI_USB_VID, ATS_MINI_USB_PID);
  const usb_cat_profile_t &profile = usb_cat_profile(
    is_qmx ? USB_CAT_BACKEND_ACM_QMX : USB_CAT_BACKEND_ACM_GENERIC);

  /*
   * QMX exposes a standard CDC data interface, but its CAT port does not
   * require UART line coding.  Some reconnects fail on SET_LINE_CODING,
   * and DTR/RTS may be assigned to PTT/CW keying.  Skip both requests for
   * QMX and start with a clean CAT queue so stale ICOM CI-V frames cannot
   * be sent to it after enumeration.
   */
  if (is_qmx) {
    UBaseType_t cleared = 0;
    if (xQueueCATUSBTx != NULL) {
      cleared = uxQueueMessagesWaiting(xQueueCATUSBTx);
      xQueueReset(xQueueCATUSBTx);
    }

    /*
     * Keep the Aug-4 QMX CAT path unchanged, but explicitly start the
     * virtual modem-control outputs inactive.  QMX firmware may use DTR
     * as the CW key; if the host/device retained DTR asserted, the rig
     * remains keyed continuously even though CAT data itself works.
     *
     * Do not use SetLineCoding() here.  Also do not fail/release the ACM
     * device if SET_CONTROL_LINE_STATE is rejected: CAT is the priority.
     */
    usb_acm_key_iface = pacm->GetControlIface();
    /*
     * QMX: DTR=1 keys TX, so the safe idle state on connect/reconnect is
     * DTR=0, RTS=0.  Keep the Aug-4 CAT data path unchanged.
     */
    const uint8_t qmx_idle_state = 0x00; // DTR=0, RTS=0
    const uint8_t qmx_ctl_rcode =
      pacm->SetControlLineStateOnInterface(usb_acm_key_iface, qmx_idle_state);
    usb_acm_line_state = qmx_idle_state;
    usb_keying_queue_clear();

    console->printf(
      "USB ACM connected: QMX CAT control_if=%u idle DTR=0 RTS=0 rcode=0x%02X\n",
      usb_acm_key_iface, qmx_ctl_rcode);
    if (verbose & VERBOSE_USB) {
      console->printf(
        "USB ACM detail: VID=%04X PID=%04X CDC control skipped queue cleared=%u\n",
        pacm->GetVid(), pacm->GetPid(), (unsigned int)cleared);
    }
    return 0;
  }

  if (is_ats_mini) {
    // ATS Mini Ad hoc uses TinyUSB CDC data endpoints directly.  Avoid
    // SET_CONTROL_LINE_STATE / SET_LINE_CODING: they are unnecessary for the
    // protocol and can disturb/restart some ESP32-S3 TinyUSB builds.
    const UBaseType_t cleared = usb_cat_reset_tx_queue();
    usb_cat_set_backend(USB_CAT_BACKEND_ACM_GENERIC);
    console->printf("USB ACM connected: ATS-MINI VID=%04X PID=%04X\n",
                    pacm->GetVid(), pacm->GetPid());
    if (verbose & VERBOSE_USB)
      console->printf("ATS-MINI CDC control skipped queue cleared=%u\n",
                      (unsigned int)cleared);
    return 0;
  }

  // Use the first CDC ACM pair as the default keying port.  On IC-705
  // this is USB(A): control IF 0 / data IF 1.
  usb_acm_key_iface = pacm->GetControlIface();
  console->printf("USB ACM interfaces: control=%u data=%u key_if=%u\n",
                  pacm->GetControlIface(), pacm->GetDataIface(),
                  usb_acm_key_iface);

  // Start ICOM/generic ACM with both DTR and RTS inactive.  This is
  // important when either line is assigned to CW/RTTY keying in the rig.
  rcode = pacm->SetControlLineStateOnInterface(usb_acm_key_iface, 0);
  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetControlLineState"), rcode);
    return rcode;
  }
  usb_acm_line_state = 0;
  usb_keying_queue_clear();

  LINE_CODING lc;
  lc.dwDTERate = 115200;
  lc.bCharFormat = 0;
  lc.bParityType = 0;
  lc.bDataBits = 8;

  rcode = pacm->SetLineCoding(&lc);
  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);
    return rcode;
  }

  usb_cat_set_backend(profile.backend);
  console->printf("USB ACM connected: VID=%04X PID=%04X\n",
                  pacm->GetVid(), pacm->GetPid());
  return 0;
}

ACMAsyncOper AsyncOper;
ACM Acm(&Usb, &AsyncOper);
CP2105 Cp2105(&Usb);
static uint8_t cp2105_cat_port = 0;
// Yaesu dual-port CP2105: Enhanced COM (port 0) is CAT, Standard COM
// (port 1) carries PTT/CW/FSK control.  Keep its modem-line state separate
// from the CDC ACM state used by Icom.
static uint8_t cp2105_key_line_state = 0;
static constexpr uint8_t CP2105_YAESU_KEY_PORT = 1;

static bool yaesu_cp2105_keying_ready()
{
  if (!Cp2105.isReady() || Cp2105.isSinglePort() ||
      !Cp2105.portReady(CP2105_YAESU_KEY_PORT)) return false;
  for (int i = 0; i < N_RADIO; ++i) {
    if (!radio_list[i].enabled || radio_list[i].rig_spec == NULL) continue;
    struct rig *r = radio_list[i].rig_spec;
    if (r->civport_num != -1) continue;
    if (r->cat_type == CAT_TYPE_YAESU_NEW ||
        r->cat_type == CAT_TYPE_YAESU_OLD ||
        r->cat_type == CAT_TYPE_YAESU_FT817) return true;
  }
  return false;
}

static bool usb_control_line_ready()
{
  if (yaesu_cp2105_keying_ready()) return true;

  // QMX uses standard CDC SET_CONTROL_LINE_STATE DTR for straight-key CW
  // on recent firmware.  It is intentionally allowed here; QMX OnInit()
  // merely avoids asserting the line during enumeration.
  return Acm.isReady() &&
         !Acm.IsDevice(ATS_MINI_USB_VID, ATS_MINI_USB_PID);
}

static uint8_t usb_control_line_state()
{
  return yaesu_cp2105_keying_ready() ? cp2105_key_line_state
                                     : usb_acm_line_state;
}

bool CP2105controlTest(uint8_t port, char line, bool on, Stream *out)
{
  if (!out) out = console;

  if (!Cp2105.isReady()) {
    out->println("CP2105 not ready");
    return false;
  }
  if (port >= CP2105::PORTS || !Cp2105.portReady(port)) {
    out->printf("CP2105 port%u not ready\n", port);
    return false;
  }

  uint8_t rcode = 0xff;
  if (line == 'D' || line == 'd') {
    rcode = Cp2105.SetDTR(port, on);
    out->printf("CP2105 direct port=%u if=%u DTR=%u rcode=0x%02X\n",
                port, Cp2105.interfaceNumber(port), on ? 1 : 0, rcode);
  } else if (line == 'R' || line == 'r') {
    rcode = Cp2105.SetRTS(port, on);
    out->printf("CP2105 direct port=%u if=%u RTS=%u rcode=0x%02X\n",
                port, Cp2105.interfaceNumber(port), on ? 1 : 0, rcode);
  } else {
    out->println("CP2105 direct: line must be D or R");
    return false;
  }

  return rcode == 0;
}

static uint32_t cp2105_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void cp2105_put_le32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void cp2105_print_flow(uint8_t port, const uint8_t flow[16], Stream *out)
{
  const uint32_t hs = cp2105_le32(flow + 0);
  const uint32_t repl = cp2105_le32(flow + 4);
  const uint32_t xon = cp2105_le32(flow + 8);
  const uint32_t xoff = cp2105_le32(flow + 12);
  out->printf("CP2105 flow port=%u if=%u raw=",
              port, Cp2105.interfaceNumber(port));
  for (int i = 0; i < 16; ++i) out->printf("%02X", flow[i]);
  out->println();
  out->printf(" handshake=0x%08lX replace=0x%08lX xon=%lu xoff=%lu\n",
              (unsigned long)hs, (unsigned long)repl,
              (unsigned long)xon, (unsigned long)xoff);
  out->printf(" DTRmode=%lu RTSmode=%lu CTS_HS=%u DSR_HS=%u DCD_HS=%u DSRsens=%u\n",
              (unsigned long)(hs & 0x03),
              (unsigned long)((repl >> 6) & 0x03),
              (hs & 0x08) ? 1 : 0, (hs & 0x10) ? 1 : 0,
              (hs & 0x20) ? 1 : 0, (hs & 0x40) ? 1 : 0);
}

bool CP2105flowStatus(uint8_t port, Stream *out)
{
  if (!out) out = console;
  if (!Cp2105.isReady() || port >= CP2105::PORTS || !Cp2105.portReady(port)) {
    out->printf("CP2105 port%u not ready\n", port);
    return false;
  }
  uint8_t flow[16] = {0};
  const uint8_t rcode = Cp2105.GetFlow(port, flow);
  if (rcode != 0) {
    out->printf("CP2105 GET_FLOW port=%u rcode=0x%02X\n", port, rcode);
    return false;
  }
  cp2105_print_flow(port, flow, out);
  return true;
}

bool CP2105setManualFlow(uint8_t port, Stream *out)
{
  if (!out) out = console;
  if (!Cp2105.isReady() || port >= CP2105::PORTS || !Cp2105.portReady(port)) {
    out->printf("CP2105 port%u not ready\n", port);
    return false;
  }

  uint8_t flow[16] = {0};
  uint8_t rcode = Cp2105.GetFlow(port, flow);
  if (rcode != 0) {
    out->printf("CP2105 GET_FLOW port=%u rcode=0x%02X\n", port, rcode);
    return false;
  }

  out->println("CP2105 flow before:");
  cp2105_print_flow(port, flow, out);

  uint32_t hs = cp2105_le32(flow + 0);
  uint32_t repl = cp2105_le32(flow + 4);

  // Match Linux cp210x with CRTSCTS disabled: software-controlled DTR/RTS,
  // and no CTS/DSR/DCD hardware handshake or DSR sensitivity.
  hs = (hs & ~0x0000007BUL) | 0x00000001UL;  // DTR ACTIVE/manual
  repl = (repl & ~0x000000C0UL) | 0x00000040UL; // RTS ACTIVE/manual
  cp2105_put_le32(flow + 0, hs);
  cp2105_put_le32(flow + 4, repl);

  rcode = Cp2105.SetFlow(port, flow);
  if (rcode != 0) {
    out->printf("CP2105 SET_FLOW port=%u rcode=0x%02X\n", port, rcode);
    return false;
  }

  uint8_t verify[16] = {0};
  rcode = Cp2105.GetFlow(port, verify);
  if (rcode != 0) {
    out->printf("CP2105 GET_FLOW verify port=%u rcode=0x%02X\n", port, rcode);
    return false;
  }
  out->println("CP2105 flow after:");
  cp2105_print_flow(port, verify, out);

  uint8_t r1 = Cp2105.SetDTR(port, false);
  uint8_t r2 = Cp2105.SetRTS(port, false);
  out->printf("CP2105 manual line reset DTR_rcode=0x%02X RTS_rcode=0x%02X\n", r1, r2);
  if (port == CP2105_YAESU_KEY_PORT && r1 == 0 && r2 == 0)
    cp2105_key_line_state = 0;
  return r1 == 0 && r2 == 0;
}

static uint8_t usb_control_line_apply(uint8_t new_state)
{
  new_state &= 0x03;
  if (yaesu_cp2105_keying_ready()) {
    /*
     * Match Windows VCP SETDTR/CLRDTR and SETRTS/CLRRTS semantics:
     * only the modem-control output that changed gets a SET_MHS write mask.
     * On Yaesu Standard COM the other output may be PTT or CW/FSK keying.
     */
    const uint8_t changed = (cp2105_key_line_state ^ new_state) & 0x03;
    uint8_t rcode = 0;

    if (changed & 0x01) {
      rcode = Cp2105.SetDTR(CP2105_YAESU_KEY_PORT,
                            (new_state & 0x01) != 0);
      if (rcode != 0) return rcode;
      cp2105_key_line_state =
        (cp2105_key_line_state & static_cast<uint8_t>(~0x01)) |
        (new_state & 0x01);
    }

    if (changed & 0x02) {
      rcode = Cp2105.SetRTS(CP2105_YAESU_KEY_PORT,
                            (new_state & 0x02) != 0);
      if (rcode != 0) return rcode;
      cp2105_key_line_state =
        (cp2105_key_line_state & static_cast<uint8_t>(~0x02)) |
        (new_state & 0x02);
    }

    return 0;
  }
  if (!Acm.isReady()) return 0xff;
  const uint8_t rcode =
    Acm.SetControlLineStateOnInterface(usb_acm_key_iface, new_state);
  if (rcode == 0) usb_acm_line_state = new_state;
  return rcode;
}

void USBACMstatus(Print *out)
{
  if (!out) out = console;
  out->printf("USB ACM ready=%d addr=%u VID=%04X PID=%04X\n",
              Acm.isReady() ? 1 : 0, Acm.GetAddress(),
              Acm.GetVid(), Acm.GetPid());
  out->printf(" stored control_if=%u data_if=%u key_if=%u line_state=0x%02X\n",
              Acm.GetControlIface(), Acm.GetDataIface(),
              usb_acm_key_iface, usb_acm_line_state);
  if (yaesu_cp2105_keying_ready())
    out->printf(" Yaesu CP2105 key port=%u line_state=0x%02X (DTR/RTS)\n",
                CP2105_YAESU_KEY_PORT, cp2105_key_line_state);
  out->printf(" EP data IN=%u OUT=%u second IN=%u second=%d\n",
              Acm.GetDataInEp(), Acm.GetDataOutEp(),
              Acm.GetSecondDataInEp(), Acm.HasSecondDataIn() ? 1 : 0);
  out->println(" line state: bit0=DTR bit1=RTS");
}

bool USBACMselectKeyInterface(uint8_t iface, Print *out)
{
  if (!out) out = console;
  if (iface > 15) {
    out->println("USB ACM interface must be 0..15");
    return false;
  }
  usb_acm_key_iface = iface;
  out->printf("USB ACM key/control interface=%u\n", usb_acm_key_iface);
  return true;
}

bool USBACMcontrolTest(uint8_t state, Print *out)
{
  if (!out) out = console;
  state &= 0x03;
  if (!Acm.isReady()) {
    out->println("USB ACM not ready");
    return false;
  }
  const uint8_t rcode =
      Acm.SetControlLineStateOnInterface(usb_acm_key_iface, state);
  out->printf("USB ACM SET_CONTROL_LINE_STATE if=%u state=0x%02X "
              "DTR=%u RTS=%u rcode=0x%02X\n",
              usb_acm_key_iface, state,
              (state & 0x01) ? 1 : 0, (state & 0x02) ? 1 : 0, rcode);
  if (rcode == 0) {
    usb_acm_line_state = state;
    usb_keying_queue_clear();
    return true;
  }
  return false;
}

void usb_keying_process()
{
  if (usb_keying_tail == usb_keying_head) return;

  // Icom/generic radios use CDC ACM.  Yaesu dual-port CP2105 radios use
  // Standard COM (port 1) for DTR/RTS TX control while CAT stays on port 0.
  if (!usb_control_line_ready()) {
    usb_keying_queue_clear();
    return;
  }

  const uint8_t tail = usb_keying_tail;
  const UsbKeyingEvent ev = {
    usb_keying_queue[tail].cwport, usb_keying_queue[tail].on
  };
  usb_keying_tail = static_cast<uint8_t>((tail + 1) % USB_KEYING_QUEUE_LEN);

  const uint8_t old_state = usb_control_line_state();
  uint8_t new_state = old_state;
  const uint8_t mask = (ev.cwport == 3) ? 0x01 : 0x02;
  if (ev.on) new_state |= mask;
  else       new_state &= static_cast<uint8_t>(~mask);

  if (new_state == old_state) return;

  const uint8_t rcode = usb_control_line_apply(new_state);
  if (rcode == 0) {
    if (verbose & VERBOSE_USB) {
      console->printf("USB KEY if=%u %s=%u state=0x%02X drops=%lu\n",
                      usb_acm_key_iface,
                      ev.cwport == 3 ? "DTR" : "RTS", ev.on,
                      usb_acm_line_state,
                      (unsigned long)usb_keying_drops);
    }
  } else if (verbose & VERBOSE_USB) {
    console->printf("USB KEY control error rcode=0x%02X\n", rcode);
  }
}

bool usb_qmx_cat_ready() {
  return usb_cat_ready_for_rig_type(CAT_TYPE_QMX);
}

bool usb_cat_ready_for_rig_type(uint8_t cat_type) {
  switch (cat_type) {
  case CAT_TYPE_QMX:
    return Acm.isReady() && Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  case CAT_TYPE_ATS_MINI:
    // ATS Mini is standard ESP32-S3 CDC ACM. The selected rig profile
    // identifies the protocol, so do not depend on a fixed VID/PID.
    return Acm.isReady() && !Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_YAESU_FT817:
    // Yaesu USB CAT will use the selected CP2105 port.  The profile is
    // already common; only descriptor-based port selection remains.
    return Cp2105.isReady() && Cp2105.portReady(cp2105_cat_port);
  default:
    return Acm.isReady() ||
           (Cp2105.isReady() && Cp2105.portReady(cp2105_cat_port));
  }
}

static bool cp2105_debug = false;

static void dump_cp2105_data(const char *direction,
                             uint8_t port,
                             const uint8_t *data,
                             uint16_t len)
{
    console->printf("CP2105 %s port%u len=%u HEX:",
                    direction, port, len);

    for (uint16_t i = 0; i < len; i++) {
        console->printf(" %02X", data[i]);
    }

    console->print(" ASCII:\"");

    for (uint16_t i = 0; i < len; i++) {
        char c = static_cast<char>(data[i]);
        if (c >= 0x20 && c <= 0x7e)
            console->print(c);
        else
            console->print('.');
    }

    console->println("\"");
}

bool CP2105selectPort(uint8_t port) {
  if (port >= CP2105::PORTS) return false;
  cp2105_cat_port = port;
  console->printf("CP2105 CAT port=%u\n", cp2105_cat_port);
  return true;
}

bool CP2105setBaud(uint8_t port, uint32_t baudrate) {
  if (!Cp2105.isReady() || port >= CP2105::PORTS || baudrate == 0) return false;
  uint8_t rcode = Cp2105.ConfigurePort(port, baudrate);
  console->printf("CP2105 port %u baud %lu rcode=0x%02x\n",
                  port, (unsigned long)baudrate, rcode);
  return rcode == 0;
}

void CP2105status(Stream *out) {
  if (!out) out = console;
  out->printf("CP210x ready=%d addr=0x%02x VID=%04X PID=%04X single=%d CATport=%u\n",
                  Cp2105.isReady() ? 1 : 0,
                  Cp2105.GetAddress(), Cp2105.GetVid(), Cp2105.GetPid(),
                  Cp2105.isSinglePort() ? 1 : 0, cp2105_cat_port);
  for (uint8_t port = 0; port < CP2105::PORTS; port++) {
    out->printf(" port%u ready=%d if=%u IN=%u/%u OUT=%u/%u baud=%lu%s\n",
                    port, Cp2105.portReady(port) ? 1 : 0,
                    Cp2105.interfaceNumber(port),
                    Cp2105.inEndpoint(port), Cp2105.inMaxPacket(port),
                    Cp2105.outEndpoint(port), Cp2105.outMaxPacket(port),
                    (unsigned long)Cp2105.baudRate(port),
                    port == cp2105_cat_port ? " CAT" : "");
  }
}

static bool cp210x_active_usb_rig(uint8_t *cat_type, uint32_t *baud)
{
  for (int i = 0; i < N_RADIO; ++i) {
    if (!radio_list[i].enabled || radio_list[i].rig_spec == NULL) continue;
    struct rig *r = radio_list[i].rig_spec;
    if (r->civport_num != -1) continue;
    if (r->cat_type == CAT_TYPE_YAESU_NEW ||
        r->cat_type == CAT_TYPE_YAESU_OLD ||
        r->cat_type == CAT_TYPE_YAESU_FT817 ||
        r->cat_type == CAT_TYPE_KENWOOD) {
      if (cat_type) *cat_type = r->cat_type;
      if (baud) *baud = r->civport_baud;
      return true;
    }
  }
  return false;
}

static bool cp210x_prepare_selected_rig()
{
  uint8_t cat_type = 0;
  uint32_t baud = 0;
  if (!Cp2105.isReady() || !cp210x_active_usb_rig(&cat_type, &baud)) return false;

  // CP2105: Yaesu CAT is the Enhanced COM port (port 0).
  // CP2102: TS-590G has one UART, also represented as port 0.
  cp2105_cat_port = 0;
  if (Cp2105.isSinglePort()) {
    if (cat_type != CAT_TYPE_KENWOOD) return false;
  } else {
    if (cat_type != CAT_TYPE_YAESU_NEW &&
        cat_type != CAT_TYPE_YAESU_OLD &&
        cat_type != CAT_TYPE_YAESU_FT817) return false;
  }

  if (baud != 0 && Cp2105.baudRate(cp2105_cat_port) != baud) {
    uint8_t rcode = Cp2105.ConfigurePort(cp2105_cat_port, baud);
    console->printf("CP210x USB CAT configure port=%u baud=%lu rcode=0x%02X\n",
                    cp2105_cat_port, (unsigned long)baud, rcode);
    if (rcode) return false;
  }
  return Cp2105.portReady(cp2105_cat_port);
}

void CP2105process() {
  if (!cp210x_prepare_selected_rig()) return;
  usb_cat_set_backend(USB_CAT_BACKEND_CP2105);

  struct catmsg_t catmsg;
  while (usb_cat_dequeue(&catmsg)) {

      if (cp2105_debug) {
	dump_cp2105_data("TX",
			 cp2105_cat_port,
			 reinterpret_cast<uint8_t *>(catmsg.buf),
			 catmsg.size);
      }

      uint8_t rcode =
	Cp2105.SndData(cp2105_cat_port,
		       catmsg.size,
		       reinterpret_cast<uint8_t *>(catmsg.buf));

      if (rcode && rcode != hrNAK) {
	console->printf("CP2105 TX error port%u rcode=0x%02X\n",
			cp2105_cat_port, rcode);
	// break; //?
      }
 
      /*      uint8_t rcode = Cp2105.SndData(cp2105_cat_port, catmsg.size,
                                    reinterpret_cast<uint8_t *>(catmsg.buf));
      if (rcode && rcode != hrNAK) {
        ErrorMessage<uint8_t>(PSTR("CP2105 SndData"), rcode);
        break;
      }
      */
  }

  uint8_t buf[64];
  uint16_t rcvd = Cp2105.inMaxPacket(cp2105_cat_port);
  if (rcvd == 0 || rcvd > sizeof(buf)) rcvd = sizeof(buf);
  
  uint8_t rcode = Cp2105.RcvData(cp2105_cat_port, &rcvd, buf);

  if (rcode && rcode != hrNAK) {
    console->printf("CP2105 RX error port%u rcode=0x%02X\n",
                    cp2105_cat_port, rcode);
    return;
  }

  if (rcvd) {
    if (cp2105_debug) {
      dump_cp2105_data("RX",
		       cp2105_cat_port,
		       buf,
		       rcvd);
    }

    usb_cat_deliver_rx(buf, rcvd);
  }
 
  /*  if (rcode && rcode != hrNAK) {
    ErrorMessage<uint8_t>(PSTR("CP2105 RcvData"), rcode);
    return;
  }
  if (rcvd) {
    catmsg.size = min((uint16_t)sizeof(catmsg.buf), rcvd);
    memcpy(catmsg.buf, buf, catmsg.size);
    xQueueSend(xQueueCATUSBRx, &catmsg, 0);
    if (verbose & 1) {
      console->printf("CP2105 port%u received %u bytes\n",
                      cp2105_cat_port, catmsg.size);
    }
  }
  */
}



void CP2105toggleDebug()
{
    cp2105_debug = !cp2105_debug;
    console->printf("CP2105 debug=%d\n",
                    cp2105_debug ? 1 : 0);
}
bool CP2105sendRaw(uint8_t port, const char *text)
{
    if (!text ||
        !Cp2105.isReady() ||
        !Cp2105.portReady(port)) {
        return false;
    }

    uint16_t len = strlen(text);

    dump_cp2105_data(
        "RAW-TX",
        port,
        reinterpret_cast<const uint8_t *>(text),
        len);

    uint8_t rcode =
        Cp2105.SndData(
            port,
            len,
            reinterpret_cast<uint8_t *>(
                const_cast<char *>(text)));

    console->printf("CP2105 raw port%u rcode=0x%02X\n",
                    port, rcode);

    return rcode == 0;
}

static bool ats_mini_monitor_started = false;
static uint32_t ats_mini_monitor_retry_ms = 0;

static bool ats_mini_usb_rig_active()
{
  for (int i = 0; i < N_RADIO; ++i) {
    if (!radio_list[i].enabled || radio_list[i].rig_spec == NULL) continue;
    if (radio_list[i].rig_spec->civport_num == -1 &&
        radio_list[i].rig_spec->cat_type == CAT_TYPE_ATS_MINI)
      return true;
  }
  return false;
}

static void ats_mini_start_monitor_if_needed()
{
  if (!Acm.isReady() || !Acm.IsDevice(0x303A, 0x1001) ||
      !ats_mini_usb_rig_active() || ats_mini_monitor_started)
    return;

  const uint32_t now = millis();
  if ((int32_t)(now - ats_mini_monitor_retry_ms) < 0) return;

  // Send 't' directly and consider the monitor started ONLY after the USB OUT
  // transfer succeeds. hrNAK means "try again later", not failure.
  uint8_t cmd = 't';
  const uint8_t rcode = Acm.SndData(1, &cmd);
  if (rcode == 0) {
    ats_mini_monitor_started = true;
    usb_cat_set_backend(USB_CAT_BACKEND_ACM_GENERIC);
    console->println("ATS-MINI USB: status monitor enabled");
    return;
  }

  if (rcode == hrNAK) {
    ats_mini_monitor_retry_ms = now + 100;
    return;
  }

  ats_mini_monitor_retry_ms = now + 500;
  if (verbose & VERBOSE_USB)
    console->printf("ATS-MINI monitor start rcode=0x%02X; retrying\\n", rcode);
}

void ACMprocess() {
  /*
   * One-second heartbeat for the USB CAT path.  Print while QMX is attached
   * or while the TX queue contains data, so a stalled consumer is visible
   * without flooding normal non-CAT operation.
   */
  static uint32_t last_cat_heartbeat = 0;
  const bool qmx_attached = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  const UBaseType_t tx_waiting = usb_cat_tx_waiting();
  const uint32_t now = millis();
  if (!Acm.isReady() && !Cp2105.isReady()) {
    usb_cat_set_backend(USB_CAT_BACKEND_NONE);
    ats_mini_monitor_started = false;
    ats_mini_monitor_retry_ms = 0;
  }
  if ((verbose & VERBOSE_USB) &&
      (qmx_attached || tx_waiting != 0) &&
      now - last_cat_heartbeat >= 1000) {
    last_cat_heartbeat = now;
    console->printf(
      "USB CAT heartbeat state=0x%02X vbus=0x%02X "
      "ACMready=%d QMX=%d CP2105ready=%d waiting=%u free=%u\n",
      Usb.getUsbTaskState(), Usb.getVbusState(),
      Acm.isReady() ? 1 : 0, qmx_attached ? 1 : 0,
      Cp2105.isReady() ? 1 : 0,
      (unsigned int)tx_waiting,
      (unsigned int)usb_cat_tx_free());
  }

  if (Cp2105.isReady() && cp210x_prepare_selected_rig()) {
    if ((verbose & VERBOSE_USB) && (qmx_attached || tx_waiting != 0)) {
      static uint32_t last_cp2105_redirect_report = 0;
      if (now - last_cp2105_redirect_report >= 1000) {
        last_cp2105_redirect_report = now;
        console->printf(
          "USB CAT consumer redirected to CP2105 addr=0x%02X waiting=%u\n",
          Cp2105.GetAddress(), (unsigned int)tx_waiting);
      }
    }
    CP2105process();
    return;
  }
  // IC-705,IC-905 operation
  uint8_t rcode;
  int ret;
  struct catmsg_t catmsg;

  if ((verbose & VERBOSE_USB) && !Acm.isReady() && tx_waiting != 0) {
    static uint32_t last_not_ready_report = 0;
    if (now - last_not_ready_report >= 1000) {
      last_not_ready_report = now;
      console->printf(
        "USB CAT TX blocked: ACM not ready state=0x%02X waiting=%u free=%u\n",
        Usb.getUsbTaskState(), (unsigned int)tx_waiting,
        (unsigned int)usb_cat_tx_free());
    }
  }
  
  static bool qmx_ready_prev = false;
  const bool qmx_ready_now = Acm.isReady() && qmx_attached;
  if (qmx_ready_now && !qmx_ready_prev) {
    if (verbose & VERBOSE_USB) console->printf(
      "QMX CDC endpoints: BulkOUT=%02X BulkIN=%02X BulkIN2=%02X second=%d\n",
      Acm.GetDataOutEp(), Acm.GetDataInEp(), Acm.GetSecondDataInEp(),
      Acm.HasSecondDataIn() ? 1 : 0);

    // Fresh QMX ACM session forced IF query temporarily disabled.
    // Let the normal periodic CAT query sequence start communication.
#if 0
    if (xQueueCATUSBTx) {
      xQueueReset(xQueueCATUSBTx);
      struct catmsg_t startup = {};
      memcpy(startup.buf, "IF;", 3);
      startup.size = 3;
      const BaseType_t qret = xQueueSend(xQueueCATUSBTx, &startup, 0);
      if (verbose & VERBOSE_USB) console->printf(
        "QMX CAT startup enqueue ret=%d waiting=%u free=%u cmd=IF;\n",
        qret,
        (unsigned int)uxQueueMessagesWaiting(xQueueCATUSBTx),
        (unsigned int)uxQueueSpacesAvailable(xQueueCATUSBTx));
    }
#endif
  }
  qmx_ready_prev = qmx_ready_now;

  /*
   * QMX CAT rollback path (Aug-4 behavior).
   * Keep QMX away from the newer generic USB CAT backend/activation logic:
   * direct TX queue -> Acm.SndData(), direct Acm.RcvData() -> RX queue.
   */
  if (qmx_ready_now) {
    struct catmsg_t qmx_msg;

    while (uxQueueMessagesWaiting(xQueueCATUSBTx)) {
      if (xQueueReceive(xQueueCATUSBTx, &qmx_msg, 0) == pdTRUE) {
        if (verbose & VERBOSE_USB) {
          console->printf("QMX USB CAT TX len=%d ascii=\"", qmx_msg.size);
          for (int i = 0; i < qmx_msg.size; ++i) {
            const uint8_t c = qmx_msg.buf[i];
            console->print((c >= 0x20 && c <= 0x7e) ? (char)c : '.');
          }
          console->println("\"");
        }

        rcode = Acm.SndData(qmx_msg.size, (uint8_t *)qmx_msg.buf);
        if (rcode) {
          ErrorMessage<uint8_t>(PSTR("SndData CATUSBTx"), rcode);
          plogw->ostream->println("SndData CATUSBTx error");
        }
      }
    }

    uint8_t qmx_buf[64];
    uint16_t qmx_rcvd = sizeof(qmx_buf);
    rcode = Acm.RcvData(&qmx_rcvd, qmx_buf);

    if (rcode && rcode != hrNAK &&
        (rcode != hrJERR || (verbose & VERBOSE_USB)))
      ErrorMessage<uint8_t>(PSTR("Ret"), rcode);

    if (qmx_rcvd) {
      struct catmsg_t qmx_rx = {};
      qmx_rx.size = min((uint16_t)sizeof(qmx_rx.buf), qmx_rcvd);
      memcpy(qmx_rx.buf, qmx_buf, qmx_rx.size);
      const BaseType_t qret = xQueueSend(xQueueCATUSBRx, &qmx_rx, 0);

      if (verbose & VERBOSE_USB) {
        console->printf("QMX USB CAT RX len=%u qret=%d ascii=\"",
                        (unsigned int)qmx_rx.size, (int)qret);
        for (int i = 0; i < qmx_rx.size; ++i) {
          const uint8_t c = qmx_rx.buf[i];
          console->print((c >= 0x20 && c <= 0x7e) ? (char)c : '.');
        }
        console->println("\"");
      }
    }

    // QMX has only one CDC data interface; never poll RcvData1().
    return;
  }

  if (Acm.isReady()) {
    ats_mini_start_monitor_if_needed();

    // check queue and forward to USB
    while (usb_cat_dequeue(&catmsg)) {
      ret = pdTRUE;
	const bool is_qmx = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
        usb_cat_set_backend(is_qmx ? USB_CAT_BACKEND_ACM_QMX
                                   : USB_CAT_BACKEND_ACM_GENERIC);
        usb_cat_dump("TX", catmsg.buf, catmsg.size);
	rcode = Acm.SndData(catmsg.size, (uint8_t *)catmsg.buf);
        const bool is_ats_mini = Acm.IsDevice(0x303A, 0x1001);

        if (is_qmx) {
          console->printf("QMX CAT TX rcode=0x%02X len=%d data=",
                          rcode, catmsg.size);
          for (int i = 0; i < catmsg.size; ++i) {
            const uint8_t c = static_cast<uint8_t>(catmsg.buf[i]);
            if (c >= 0x20 && c <= 0x7E)
              console->print((char)c);
            else
              console->printf("\\x%02X", c);
          }
          console->println();
        } else if (verbose & VERBOSE_USB) {
	  console->printf("%sUSB CAT TX complete rcode=0x%02X len=%d\n",
	                  is_ats_mini ? "ATS-MINI " : "",
                          rcode, catmsg.size);
	}

        if ((is_qmx || is_ats_mini) && rcode == hrNAK) {
          /*
           * Both QMX and ATS-MINI can briefly NAK just after enumeration.
           * Preserve command order instead of losing the first startup query.
           */
          const bool requeued = usb_cat_requeue_front(&catmsg);
          if (is_qmx) {
            console->printf("QMX CAT TX NAK requeue=%d waiting=%u\n",
                            requeued ? 1 : 0,
                            (unsigned int)usb_cat_tx_waiting());
          } else if (!requeued && (verbose & VERBOSE_USB)) {
            console->println("ATS-MINI TX NAK: failed to requeue command");
          }
          break;
        }

	if (rcode) {
	  ErrorMessage<uint8_t>(PSTR("SndData CATUSBTx"), rcode);
	  plogw->ostream->println("SndData CATUSBTx error");
	}

        /*
         * QMX is request/response oriented.  Do not drain the whole CAT TX
         * queue before polling Bulk IN; send one command, then immediately
         * give the receive path a chance to collect the reply.
         */
        if (is_qmx)
          break;
    }
	
	
    if (1==0) {
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
    }
    //        plogw->ostream->println();

    /* reading from usb device */
    /* buffer size must be greater or equal to max.packet size */
    /* it it set to 64 (largest possible max.packet size) here, can be tuned down
       for particular endpoint */
    // 受け取り側はサンプルプログラムのまま。

    uint8_t buf[64];
    uint16_t rcvd = 64;
    int ret;
    rcode = Acm.RcvData(&rcvd, buf);
    if (rcode && rcode != hrNAK &&
        (rcode != hrJERR || (verbose & VERBOSE_USB)))
      ErrorMessage<uint8_t>(PSTR("Ret"), rcode);

    struct catmsg_t catmsg;
    if (rcvd) {  //more than zero bytes received
      const bool is_qmx = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
      usb_cat_set_backend(is_qmx ? USB_CAT_BACKEND_ACM_QMX
                                 : USB_CAT_BACKEND_ACM_GENERIC);

      if (is_qmx) {
        console->printf("QMX CAT RX len=%u data=", (unsigned int)rcvd);
        for (uint16_t i = 0; i < rcvd; ++i) {
          const uint8_t c = buf[i];
          if (c >= 0x20 && c <= 0x7E)
            console->print((char)c);
          else
            console->printf("\\x%02X", c);
        }
        console->println();
      }

      usb_cat_dump("RX", buf, rcvd);
      ret = usb_cat_deliver_rx(buf, rcvd) ? pdTRUE : pdFALSE;
      if (verbose & VERBOSE_USB) plogw->ostream->printf(
        "CATUSBRx queuesend ret=%d size=%u\r\n", ret,
        (unsigned int)rcvd);
    }

    // QMX has only one CDC data interface.  Polling RcvData1() when no
    // second Bulk-IN endpoint exists can enter a long MAX3421E NAK loop and
    // starve IDLE0, triggering the task watchdog.
    if (Acm.HasSecondDataIn()) {
      rcvd=64;
      rcode = Acm.RcvData1(&rcvd, buf);
      if (rcode && rcode != hrNAK &&
          (rcode != hrJERR || (verbose & VERBOSE_USB)))
        ErrorMessage<uint8_t>(PSTR("Ret1"), rcode);

      if (rcvd) {
        // IC-705 second CDC data interface: feed both the decoder display and
        // conservative callsign extraction path.
        for (uint16_t i = 0; i < rcvd; ++i) rtty_decoder_feed_byte(buf[i]);
        rtty_decoder_display_snapshot();

        if (verbose & VERBOSE_USB) {
          plogw->ostream->print("ACMrcvd1:");
          for (uint16_t i = 0; i < rcvd; i++) plogw->ostream->print((char)buf[i]);
          plogw->ostream->print("\r\n");
        }
      }
    }
  }
}

//#endif

#ifdef notdef
CH34XAsyncOper CH34XAsyncOper;
CH34X Ch34x(&Usb, &CH34XAsyncOper);
#endif
BTD Btd(&Usb);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(&Btd);
BTHID bthid(&Btd, PAIR, "0000");
#ifdef notdef
FTDIAsync FtdiAsync;
FTDI Ftdi(&Usb, &FtdiAsync);
#endif

void receive_pkt_handler_keyboard1_main(struct mux_packet *packet)
{
  // New extension firmware appends an 8-bit event sequence number.  Continue
  // accepting the legacy two-byte packet during mixed-version updates.
  static bool seq_valid = false;
  static uint8_t expected_seq = 0;

  if (packet->idx >= 3) {
    const uint8_t received_seq = (uint8_t)packet->buf[2];

    if (seq_valid) {
      const uint8_t previous_seq = (uint8_t)(expected_seq - 1);

      if (received_seq == previous_seq) {
        // Exact retransmission/duplicate.  Do NOT resync and, importantly,
        // do NOT pass the same key transition to Parse_extKbd() twice.
        if (verbose & 16) {
          Serial.printf(
              "KBD EXT duplicate seq=%u expected=%u hid=0x%02X on=%u; ignored\n",
              (unsigned int)received_seq, (unsigned int)expected_seq,
              packet->idx >= 1 ? (unsigned int)(uint8_t)packet->buf[0] : 0U,
              packet->idx >= 2 && packet->buf[1] ? 1U : 0U);
        }
        return;
      }

      if (received_seq != expected_seq) {
        // Genuine gap/out-of-order event: parser state may no longer match
        // the extension keyboard, so resync before accepting this event.
        Serial.printf("KBD EXT sequence gap expected=%u received=%u; resync\n",
                      (unsigned int)expected_seq,
                      (unsigned int)received_seq);
        Prs1.resync_extKbd("sequence gap");
      }
    }

    expected_seq = (uint8_t)(received_seq + 1);
    seq_valid = true;
  }

  if (packet->idx >= 2) {
    Prs1.Parse_extKbd((uint8_t)packet->buf[0], packet->buf[1] != 0);
  }
}

void KbdRptParser::init_extKbd()
{
    for (uint8_t i = 1; i < 8; i++) {
      prevState.bInfo[i]=1;
    }
    prevState.bInfo[0]=0;
}

  
void KbdRptParser::resync_extKbd(const char *reason)
{
  const uint8_t old_mod = prevState.bInfo[0];

  // Release modifier-driven functions (notably Right-Shift PTT/keying) before
  // clearing the parser state.  Normal keys do not have persistent actions.
  if (old_mod != 0) {
    OnControlKeysChanged(old_mod, 0);
  }

  for (uint8_t i = 1; i < 8; i++) {
    prevState.bInfo[i] = 1;
  }
  prevState.bInfo[0] = 0;
  buf_ext[0] = 0;
  f_capslock = 0;

  Serial.printf("KBD EXT resync t=%lu reason=%s old_mod=0x%02X\n",
                (unsigned long)millis(),
                reason ? reason : "unknown", (unsigned int)old_mod);
}

void KbdRptParser::Parse_extKbd(uint8_t hid_code,bool on) 
{
  if ((verbose & 16) && (hid_code == 0x36 || hid_code == 0x37)) {
    bool already_pressed = false;
    int slot = -1;
    for (uint8_t i = 2; i < 8; i++) {
      if (prevState.bInfo[i] == hid_code) {
        already_pressed = true;
        slot = i;
        break;
      }
    }
    Serial.printf(
        "EXT_RAW t=%lu hid=0x%02X on=%d mod=0x%02X pressed=%d slot=%d "
        "keys=%02X,%02X,%02X,%02X,%02X,%02X\n",
        (unsigned long)millis(), (unsigned int)hid_code, on ? 1 : 0,
        (unsigned int)prevState.bInfo[0],
        already_pressed ? 1 : 0, slot,
        (unsigned int)prevState.bInfo[2], (unsigned int)prevState.bInfo[3],
        (unsigned int)prevState.bInfo[4], (unsigned int)prevState.bInfo[5],
        (unsigned int)prevState.bInfo[6], (unsigned int)prevState.bInfo[7]);
  }
  /*
   * The extension-board keyboard reports each key transition separately.
   * Treating CapsLock as Ctrl later through f_capslock is racy for chords:
   * the CapsLock key event can be queued/released independently of Shift+key.
   * Convert CapsLock to the real Left-Ctrl HID modifier here, before modifier
   * state and key-down events are generated.  This makes Caps+Shift+2 behave
   * exactly like Ctrl+Shift+2 and also keeps the existing Ctrl shortcuts.
   */
  if (hid_code == UHS_HID_BOOT_KEY_CAPS_LOCK) {
    hid_code = 0xe0;  // Left Ctrl HID usage
  }

  // receive hid_code and on to process OnControlKeysChanged(prevState, curState)
  // OnKeyDown(), OnKeyUp() updating prevState.bInfo[i] ...

  // check control keys to modify control keys state
  buf_ext[0]=prevState.bInfo[0x00];
  uint8_t bmask;
  bmask=0;
  switch (hid_code) {
  case 0xe0: // L_Ctrl
    bmask=0x1;    break;
  case 0xe1: // L_Shift
    bmask=0x2;    break;    
  case 0xe2: // L_Alt
    bmask=0x4;    break;    
  case 0xe3: // L_Gui
    bmask=0x8;    break;    
  case 0xe4: // R_Ctrl
    bmask=0x10;    break;        
  case 0xe5: // R_Shift
    bmask=0x20;    break;        
  case 0xe6: // R_Alt
    bmask=0x40;    break;        
  case 0xe7: // R_Gui
    bmask=0x80;    break;
  }
  if (bmask!=0) {
    if (on) {
      buf_ext[0]|=bmask;
    } else {
      buf_ext[0]&=~bmask;
    }
  }

  if (prevState.bInfo[0x00] != buf_ext[0x00]) {
    //                OnControlKeysChanged(prevState.bInfo[0x00], buf[0x00]);
    // send to keymsg
    msg.arg1=prevState.bInfo[0x00];
    msg.arg2=buf_ext[0x00];
    msg.type=KEYMSG_TYPE_ONCONTROLKEYSCHANGED;
    send_keyrpt_queue();
  }

  // HID usages 0xE0..0xE7 are modifier keys, not ordinary scan codes.
  // The old code fell through here and also inserted Alt/Ctrl/Shift into
  // prevState.bInfo[2..7], producing states such as keys=E2,37,01,...
  // and making the external-keyboard pressed list inconsistent.
  if (bmask != 0) {
    prevState.bInfo[0] = buf_ext[0];
    return;
  }

  bool found=false;  
  for (uint8_t i = 2; i < 8; i++) {
    //      Serial.print("prevState0:");
    //      Serial.print(prevState.bInfo[i]);
    //      Serial.println(":");
    
    // search hid_code in prevState.binfo[]
    if (prevState.bInfo[i] == hid_code) {
      // found in previous key scan codes
      found=true;
      if (on) {
	// keep pressed state
      } else {
	prevState.bInfo[i]=1; // delete entry	
	
	msg.arg1=buf_ext[0];
	msg.arg2=hid_code;
	msg.type=KEYMSG_TYPE_ONKEYUP;
	send_keyrpt_queue();
      }
      break;
    }
  }
  if (!found) {
    // add to key list
    found=false;
    for (uint8_t i = 2; i < 8; i++) {
      //      Serial.print("prevState:");
      //      Serial.print(prevState.bInfo[i]);
      //      Serial.println(":");
      if (prevState.bInfo[i]==1) {
	found=true;
	// empty entry found
	if (on) {
	  // update directly prevState 
	  prevState.bInfo[i]=hid_code;
	  //	  Serial.print("prevState2:");
	  //	  Serial.print(i);
	  //	  Serial.print(":");
	  //	  Serial.println(prevState.bInfo[i]);
	  
	  // handle locking keys 
	  msg.hid=(USBHID *)buf_ext[0];
	  msg.arg2=hid_code;
	  msg.type=KEYMSG_TYPE_HANDLELOCKINGKEYS;
	  send_keyrpt_queue();

	  // onkeydown
	  msg.arg1=buf_ext[0];
	  msg.arg2=hid_code;
	  msg.type=KEYMSG_TYPE_ONKEYDOWN;
	  send_keyrpt_queue();

	}
	break;
      }
    }
  }
  
  prevState.bInfo[0]=buf_ext[0];

// other keys  ... 
//  case 0x53: // NumLock
//  case 0x39: // CapsLock
//  case 0x47: // ScrLock
  
}

void KbdRptParser::Parse(USBHID *hid, bool is_rpt_id __attribute__((unused)), uint8_t len __attribute__((unused)), uint8_t *buf) {
        // On error - return
        if (buf[2] == 1)
                return;

        //KBDINFO       *pki = (KBDINFO*)buf;

        // provide event for changed control key state
        if (prevState.bInfo[0x00] != buf[0x00]) {
	  //                OnControlKeysChanged(prevState.bInfo[0x00], buf[0x00]);
	  // send to keymsg
	  msg.arg1=prevState.bInfo[0x00];
	  msg.arg2=buf[0x00];
	  msg.type=KEYMSG_TYPE_ONCONTROLKEYSCHANGED;
	  send_keyrpt_queue();
        }

	//	Serial.print("HID:");	
	//        for (uint8_t i = 2; i < 8; i++) {
	//	  Serial.print(buf[i],HEX);
	//	}
	//	Serial.println("");	
        for (uint8_t i = 2; i < 8; i++) {
                bool down = false;
                bool up = false;
		
                for (uint8_t j = 2; j < 8; j++) {
                        if (buf[i] == prevState.bInfo[j] && buf[i] != 1)
                                down = true;
                        if (buf[j] == prevState.bInfo[i] && prevState.bInfo[i] != 1)
                                up = true;
                }
                if (!down) {
		  //                        HandleLockingKeys(hid, buf[i]);
		  msg.hid=hid;      // hid is USBHID pointer, why?
		  msg.arg2=buf[i];
		  msg.type=KEYMSG_TYPE_HANDLELOCKINGKEYS;
		  send_keyrpt_queue();
			
		  //		Serial.print("down i=");Serial.print(i);Serial.print("buf=");
		  //			Serial.println(buf[i],HEX);
		  //                        OnKeyDown(*buf, buf[i]);
		  msg.arg1=*buf; // OnKeyDown(mod, key) 
		  msg.arg2=buf[i];
		  msg.type=KEYMSG_TYPE_ONKEYDOWN;
		  send_keyrpt_queue();
		  
                }
                if (!up) {
		  //                        OnKeyUp(prevState.bInfo[0], prevState.bInfo[i]);
		  msg.arg1=prevState.bInfo[0];
		  msg.arg2=prevState.bInfo[i];
		  msg.type=KEYMSG_TYPE_ONKEYUP;
		  send_keyrpt_queue();
		}
		
        }
        for (uint8_t i = 0; i < 8; i++)
                prevState.bInfo[i] = buf[i];
};



//const uint8_t KbdRptParser::numKeys[10] PROGMEM = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
uint8_t KbdRptParser::symKeysUp_us[12] PROGMEM = {'_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?'};
uint8_t KbdRptParser::symKeysLo_us[12] PROGMEM = {'-', '=', '[', ']', '\\', ' ', ';', '\'', '`', ',', '.', '/'};

// jp106 use the following
uint8_t KbdRptParser::symKeysUp_jp[12] PROGMEM = {'=', '~', '`', '{', '|', '}', '+', '*', ' ', '<', '>', '?'};
uint8_t KbdRptParser::symKeysLo_jp[12] PROGMEM = {'-', '^', '@', '[', '\\', ']', ';', ':', ' ', ',', '.', '/'};





//const uint8_t KbdRptParser::padKeys[5] PROGMEM = {'/', '*', '-', '+', '\r'};


void KbdRptParser::init_keyrpt_queue() {
    xQueueKeyRpt = xQueueCreate(QUEUE_KEYRPT_LEN, sizeof(struct keymsg_t));
}

bool KbdRptParser::send_keyrpt_queue(){
  const BaseType_t ret = xQueueSend(xQueueKeyRpt, &msg, 0);
  if (ret != pdTRUE) {
    key_queue_drop_count++;
    const uint32_t now_ms = millis();
    if (key_queue_drop_last_report_ms == 0 ||
        (uint32_t)(now_ms - key_queue_drop_last_report_ms) >= 1000U) {
      key_queue_drop_last_report_ms = now_ms;
      Serial.printf("KBD queue full drops=%lu type=%u arg1=0x%02X arg2=0x%02X depth=%u\n",
                    (unsigned long)key_queue_drop_count,
                    (unsigned int)msg.type, (unsigned int)msg.arg1,
                    (unsigned int)msg.arg2,
                    (unsigned int)uxQueueMessagesWaiting(xQueueKeyRpt));
    }
    return false;
  }
  return true;
}

void KbdRptParser::process_keyrpt_queue(const char *profile_name) {
  struct key_profile_stats_t {
    uint32_t window_start_ms;
    uint32_t calls;
    uint32_t messages;
    uint32_t max_total_us;
    uint32_t max_waiting_us;
    uint32_t max_receive_us;
    uint32_t max_control_us;
    uint32_t max_locking_us;
    uint32_t max_keydown_us;
    uint32_t max_keyup_us;
    uint32_t slow_total;
    uint32_t slow_handler;
    UBaseType_t max_depth;
  };

  // process_keyrpt_queue() is called for the main and external keyboard.
  // Keep independent statistics without adding state to KbdRptParser.
  static key_profile_stats_t stats[2] = {};
  const int profile_index =
      (profile_name != NULL && profile_name[0] == 'e') ? 1 : 0;
  key_profile_stats_t &st = stats[profile_index];
  const char *name = (profile_name != NULL) ? profile_name : "unknown";
  const bool perf_verbose = (verbose & VERBOSE_PERF) != 0;
  const uint32_t slow_threshold_us = 5000;
  const uint32_t total_start_us = micros();

  st.calls++;

  uint32_t t0 = micros();
  UBaseType_t waiting = uxQueueMessagesWaiting(xQueueKeyRpt);
  uint32_t dt = (uint32_t)(micros() - t0);
  if (dt > st.max_waiting_us) st.max_waiting_us = dt;
  if (waiting > st.max_depth) st.max_depth = waiting;

  struct keymsg_t msg;
  BaseType_t ret;

  while (waiting > 0) {
    t0 = micros();
    ret = xQueueReceive(xQueueKeyRpt, &msg, 0);
    dt = (uint32_t)(micros() - t0);
    if (dt > st.max_receive_us) st.max_receive_us = dt;

    if (ret == pdTRUE) {
      st.messages++;
      const uint32_t handler_start_us = micros();
      const char *handler_name = "unknown";

      switch (msg.type) {
      case KEYMSG_TYPE_ONCONTROLKEYSCHANGED:
        handler_name = "control";
        OnControlKeysChanged(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_control_us) st.max_control_us = dt;
        break;

      case KEYMSG_TYPE_HANDLELOCKINGKEYS:
        handler_name = "locking";
        HandleLockingKeys(msg.hid, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_locking_us) st.max_locking_us = dt;
        break;

      case KEYMSG_TYPE_ONKEYDOWN:
        handler_name = "keydown";
        if ((verbose & 16) &&
            (msg.arg2 == 0x10 || msg.arg2 == 0x36 || msg.arg2 == 0x37)) {
          Serial.printf("KBDLOW t=%lu src=%s hid=0x%02X mod=0x%02X on=1 depth=%u\n",
                        (unsigned long)millis(), name,
                        (unsigned int)msg.arg2, (unsigned int)msg.arg1,
                        (unsigned int)uxQueueMessagesWaiting(xQueueKeyRpt));
        }
        OnKeyDown(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_keydown_us) st.max_keydown_us = dt;
        break;

      case KEYMSG_TYPE_ONKEYUP:
        handler_name = "keyup";
        OnKeyUp(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_keyup_us) st.max_keyup_us = dt;
        break;

      default:
        dt = (uint32_t)(micros() - handler_start_us);
        break;
      }

      if (dt >= slow_threshold_us) {
        st.slow_handler++;
        if (perf_verbose) {
          Serial.printf(
              "KEY PROFILE SLOW keyboard=%s stage=%s dt=%luus type=%u "
              "arg1=0x%02X arg2=0x%02X depth=%u core=%d\n",
              name, handler_name, (unsigned long)dt,
              (unsigned int)msg.type, (unsigned int)msg.arg1,
              (unsigned int)msg.arg2, (unsigned int)waiting,
              xPortGetCoreID());
        }
      }
    }

    t0 = micros();
    waiting = uxQueueMessagesWaiting(xQueueKeyRpt);
    dt = (uint32_t)(micros() - t0);
    if (dt > st.max_waiting_us) st.max_waiting_us = dt;
    if (waiting > st.max_depth) st.max_depth = waiting;
  }

  const uint32_t total_us = (uint32_t)(micros() - total_start_us);
  if (total_us > st.max_total_us) st.max_total_us = total_us;
  if (total_us >= slow_threshold_us) {
    st.slow_total++;
    if (perf_verbose) {
      Serial.printf(
          "KEY PROFILE SLOW keyboard=%s stage=total dt=%luus messages=%lu "
          "max_depth=%u core=%d\n",
          name, (unsigned long)total_us, (unsigned long)st.messages,
          (unsigned int)st.max_depth, xPortGetCoreID());
    }
  }

  const uint32_t now_ms = millis();
  if (st.window_start_ms == 0) st.window_start_ms = now_ms;
  if (perf_verbose && (uint32_t)(now_ms - st.window_start_ms) >= 1000U) {
    Serial.printf(
        "KEY PROFILE summary keyboard=%s calls=%lu messages=%lu depth=%u "
        "total=%lu waiting=%lu receive=%lu control=%lu locking=%lu "
        "keydown=%lu keyup=%lu slow_total=%lu slow_handler=%lu\n",
        name, (unsigned long)st.calls, (unsigned long)st.messages,
        (unsigned int)st.max_depth, (unsigned long)st.max_total_us,
        (unsigned long)st.max_waiting_us, (unsigned long)st.max_receive_us,
        (unsigned long)st.max_control_us, (unsigned long)st.max_locking_us,
        (unsigned long)st.max_keydown_us, (unsigned long)st.max_keyup_us,
        (unsigned long)st.slow_total, (unsigned long)st.slow_handler);

    key_profile_stats_t cleared = {};
    cleared.window_start_ms = now_ms;
    st = cleared;
  }
}




uint8_t KbdRptParser::OemToAscii(uint8_t mod, uint8_t key) {
        uint8_t shift = (mod & 0x22);

        // [a-z]
        if (VALUE_WITHIN(key, 0x04, 0x1d)) {
                // Upper case letters
                if ((kbdLockingKeys.kbdLeds.bmCapsLock == 0 && shift) ||
                        (kbdLockingKeys.kbdLeds.bmCapsLock == 1 && shift == 0))
                        return (key - 4 + 'A');

                        // Lower case letters
                else
                        return (key - 4 + 'a');
        }// Numbers
        else if (VALUE_WITHIN(key, 0x1e, 0x27)) {
                if (shift)
                        return ((uint8_t)pgm_read_byte(&getNumKeys()[key - 0x1e]));
                else
                        return ((key == UHS_HID_BOOT_KEY_ZERO) ? '0' : key - 0x1e + '1');
        }// Keypad Numbers
        else if(VALUE_WITHIN(key, 0x59, 0x61)) {
	  if(kbdLockingKeys.kbdLeds.bmNumLock == 1)    return (key - 0x59 + '1');
        } else if(VALUE_WITHIN(key, 0x2d, 0x38)) {

	  return ((shift) ? (uint8_t)pgm_read_byte(&getSymKeysUp()[key - 0x2d]) : (uint8_t)pgm_read_byte(&getSymKeysLo()[key - 0x2d]));
	    
	}
        else if(VALUE_WITHIN(key, 0x54, 0x58)) {
                return (uint8_t)pgm_read_byte(&getPadKeys()[key - 0x54]);
	}
        else {
                switch(key) {
                        case UHS_HID_BOOT_KEY_SPACE: return (0x20);
                        case UHS_HID_BOOT_KEY_ENTER: return ('\r'); // Carriage return (0x0D)
                        case UHS_HID_BOOT_KEY_ZERO2: return ((kbdLockingKeys.kbdLeds.bmNumLock == 1) ? '0': 0);
                        case UHS_HID_BOOT_KEY_PERIOD: return ((kbdLockingKeys.kbdLeds.bmNumLock == 1) ? '.': 0);
                }
        }
        return ( 0);
}



uint8_t KbdRptParser::HandleLockingKeys(USBHID* hid, uint8_t key) {
  uint8_t old_keys = kbdLockingKeys.bLeds;

  switch(key) {
  case UHS_HID_BOOT_KEY_NUM_LOCK:
    kbdLockingKeys.kbdLeds.bmNumLock = ~kbdLockingKeys.kbdLeds.bmNumLock;
    break;
  case UHS_HID_BOOT_KEY_CAPS_LOCK:
    // no caps lock function
    //                                kbdLockingKeys.kbdLeds.bmCapsLock = ~kbdLockingKeys.kbdLeds.bmCapsLock;
    break;
  case UHS_HID_BOOT_KEY_SCROLL_LOCK:
    kbdLockingKeys.kbdLeds.bmScrollLock = ~kbdLockingKeys.kbdLeds.bmScrollLock;
    break;
  }
  
  if(old_keys != kbdLockingKeys.bLeds && hid) {
    uint8_t lockLeds = kbdLockingKeys.bLeds;
    return (hid->SetReport(0, 0/*hid->GetIface()*/, 2, 0, 1, &lockLeds));
  }
  
  return 0;
};


void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t *)&beforeMod) = before;
  MODIFIERKEYS afterMod;
  *((uint8_t *)&afterMod) = after;

  // check Right Shift if straight key mode
  struct radio *radio;

  if (verbose&4) {
    console->println("OnControlKeysChanged()");
  }
  radio = &radio_list[so2r.focused_radio()];
  if (plogw->f_straightkey) {
    if (beforeMod.bmRightShift == 0 && afterMod.bmRightShift == 1) {
      if (so2r.tx() != so2r.focused_radio()) {
	keying(0);
	so2r.set_tx(so2r.focused_radio());
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
        if (so2r.tx() != so2r.focused_radio()) {
	  keying(0);
	  // go back to tx target to focused ratio
	  so2r.set_tx(so2r.focused_radio());
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
  } else {
    if (plogw->f_toggle_ptt_mode) {
      // toggle ptt of the currently focused radio
      if (beforeMod.bmRightShift == 0 && afterMod.bmRightShift == 1) {
	// pressed rightshift
	console->println("rightshift pressed");	
	if (so2r.tx() != so2r.focused_radio()) {
	  keying(0);
	  so2r.set_tx(so2r.focused_radio());
	}
	switch (radio->modetype) {
        case LOG_MODETYPE_CW:
	  // do nothing
          break;
	case LOG_MODETYPE_PH:  
          radio->ptt = 1- radio->ptt; // toggle
	  set_ptt_rig(radio, radio->ptt);
	  console->println("-> set_ptt_rig()");	
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

  // send the request to queue (plan)

  
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
KbdRptParser Prs,Prs1;

bool usb_audio_capture_active() { return PcmAudio.capturing(); }
bool usb_audio_capture_start(Print *out) { return PcmAudio.start(out); }
void usb_audio_capture_stop(Print *out) { PcmAudio.stop(out); }
void usb_audio_capture_status(Print *out) { PcmAudio.status(out); }
void usb_audio_capture_free(Print *out) { PcmAudio.freeBuffer(out); }
void usb_audio_capture_diagnose(Print *out) { PcmAudio.diagnose(out); }
void usb_audio_capture_set_sof_sync(bool enable, Print *out) { PcmAudio.setSofSync(enable, out); }
bool usb_audio_capture_sof_sync() { return PcmAudio.sofSync(); }
const int16_t *usb_audio_capture_buffer() { return PcmAudio.buffer(); }
size_t usb_audio_capture_samples() { return PcmAudio.samples(); }
uint32_t usb_audio_capture_sample_rate() { return PCMAudioCapture::kSampleRate; }

void init_usb()
{
    Prs1.init_extKbd();

    /*
     * Power-cycle an already-connected USB device.
     * This emulates unplugging and reconnecting it.
     */
    Usb.vbusPower(vbus_off);
    delay(500);

    Usb.vbusPower(vbus_on);
    delay(500);

    int8_t ret = Usb.Init(1500);

    Serial.printf(
		  "Usb.Init(1500) returned %d, vbus=%02x\n",
		  ret,
		  Usb.getVbusState()
		  );

    if (ret == -1) {
      plogw->ostream->println("OSC did not start.");
    }
 
    //    int8_t ret = Usb.Init();
    //    Serial.printf("Usb.Init() returned %d\n", ret);
    //    if (ret == -1) {
    //        plogw->ostream->println("OSC did not start.");
    //    }

    HidKeyboard.SetReportParser(0, &Prs);

    bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
    bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);

    plogw->ostream->print(
        F("\r\nHID Bluetooth Library Started")
    );
}
void init_usb_bak() {

  // external keyboard on the extension board handler 
  Prs1.init_extKbd();

  // Wait before sampling the USB bus.


  int ret;
  if ((ret=Usb.Init()) == -1) 
    console->printf("Usb.Init() returned %d\n", ret);
  
  if (ret == -1)  
    plogw->ostream->println("OSC did not start.");
  
  HidKeyboard.SetReportParser(0, &Prs);
  
  bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
  //  bthid.SetReportParser(MOUSE_PARSER_ID, &mousePrs);


  // If "Boot Protocol Mode" does not work, then try "Report Protocol Mode"
  // If that does not work either, then uncomment PRINTREPORT in BTHID.cpp to see the raw report
  bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);  // Boot Protocol Mode
  //  bthid.setProtocolMode(HID_RPT_PROTOCOL); // Report Protocol Mode

  plogw->ostream->print(F("\r\nHID Bluetooth Library Started"));

}
void loop_usb()
{
    static uint8_t previous_state = 0xff;
    static uint8_t previous_vbus = 0xff;
    static uint32_t last_report = 0;

    Usb.Task();
    usb_rtty_process();
    usb_keying_process();

    uint8_t state = Usb.getUsbTaskState();
    uint8_t vbus = Usb.getVbusState();

    if ((verbose & VERBOSE_USB) &&
        (state != previous_state || vbus != previous_vbus)) {
        Serial.printf(
            "USB: state 0x%02x -> 0x%02x, "
            "vbus 0x%02x -> 0x%02x, ACM=%d\n",
            previous_state,
            state,
            previous_vbus,
            vbus,
            Acm.isReady()
        );

    }

    previous_state = state;
    previous_vbus = vbus;

    /*
    if (millis() - last_report >= 1000) {
        last_report = millis();

        Serial.printf(
            "USB heartbeat: state=0x%02x vbus=0x%02x ACM=%d\n",
            state,
            vbus,
            Acm.isReady()
        );
    }
    */
}

void loop_usb_bak1()
{
    static uint8_t previous_state = 0xff;
    static uint32_t last_report = 0;
    static uint32_t task_count = 0;

    Usb.Task();
    task_count++;

    uint8_t state = Usb.getUsbTaskState();

    if (state != previous_state) {
        Serial.printf(
            "USB state changed: 0x%02x -> 0x%02x, ACM ready=%d\n",
            previous_state,
            state,
            Acm.isReady()
        );
        previous_state = state;
    }

    /*
     * Print periodically even when the state does not change.
     * This also confirms that usb_loop_task() is alive.
     */
    /*
    if (millis() - last_report >= 1000) {
        last_report = millis();

        Serial.printf(
            "USB heartbeat: count=%lu state=0x%02x ACM=%d\n",
            (unsigned long)task_count,
            state,
            Acm.isReady()
        );
    }
    */
}

void loop_usb_bak() {
    Usb.Task();
}

// just a wrapper 
uint8_t kbd_oemtoascii2(uint8_t mod,char c)
{
  return Prs.OemToAscii2(mod, c);
}

void usb_send_civ_buf() {
  return; // return doing nothing 
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      rcode=0;
      //      rcode = Ftdi.SndData(civ_buf_idx, (uint8_t *)civ_buf);
      rcode = Acm.SndData(civ_buf_idx, (uint8_t *)civ_buf);
      if (verbose & 1) {
        plogw->ostream->print("send civ cmd:");
        for (int i = 0; i < civ_buf_idx; i++) {
          plogw->ostream->print((civ_buf[i]), HEX);
          plogw->ostream->print(" ");
        }
        plogw->ostream->println("");
      }

      if (rcode) {
	//        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
      }
    }
}

void usb_send_cat_buf(char *cmd) {
  return;
    // send to USB host serial adapter
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      //char strbuf[] = "IF;";
      rcode=0;
      //      rcode = Ftdi.SndData(strlen(cmd), (uint8_t *)cmd);
      //      rcode = Acm.SndData(strlen(cmd), (uint8_t *)cmd);      
      if (verbose & 1) {
        plogw->ostream->print("send cat cmd:");
        plogw->ostream->println(cmd);
        //	plogw->ostream->print("r:");plogw->ostream->print(r_ptr);
        //	plogw->ostream->print("w:");plogw->ostream->println(w_ptr);
        //	plogw->ostream->print("cmdbuf:");plogw->ostream->print(cmdbuf);plogw->ostream->print(":");plogw->ostream->println(cmd_ptr);
      }

      if (rcode) {
	//        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
      }
    }
}


void usb_receive_cat_data(struct radio *radio) {
  if (radio == NULL || radio->rig_spec == NULL) return;
  if (radio->rig_spec->civport_num != -1) return;
  if (xQueueCATUSBRx == NULL) return;

  struct catmsg_t catmsg;
  int copied = 0;
  int dropped = 0;

  // USB bulk packets can split a CAT response at any byte boundary.  Copy
  // every received chunk into the existing per-radio CAT ring buffer; the
  // normal CAT parser will join the chunks and recognize the ';' terminator.
  while (xQueueReceive(xQueueCATUSBRx, &catmsg, 0) == pdTRUE) {
    if (verbose & VERBOSE_USB) {
      static uint32_t bind_ascii_rx_seq = 0;
      ++bind_ascii_rx_seq;
      console->printf(
        "[USBBIND] RX_CONSUME seq=%lu path=ASCII radio=%d spec=%d name=%s "
        "cat_type=%d civport=%d size=%d data=",
        (unsigned long)bind_ascii_rx_seq, radio->rig_idx, radio->rig_spec_idx,
        radio->rig_spec->name ? radio->rig_spec->name : "(null)",
        radio->rig_spec->cat_type, radio->rig_spec->civport_num, catmsg.size);
      const int bind_dump_n = catmsg.size < 16 ? catmsg.size : 16;
      for (int bi = 0; bi < bind_dump_n; ++bi) {
        const uint8_t c = (uint8_t)catmsg.buf[bi];
        if (c >= 0x20 && c <= 0x7e) console->print((char)c);
        else console->printf("\\x%02X", c);
      }
      if (catmsg.size > bind_dump_n) console->print("...");
      console->println();
    }

    int size = catmsg.size;
    if (size < 0) size = 0;
    if (size > (int)sizeof(catmsg.buf)) size = sizeof(catmsg.buf);

    for (int i = 0; i < size; i++) {
      const int next = (radio->w_ptr + 1) % 256;
      if (next == radio->r_ptr) {
        // Keep the already buffered partial command intact.  Drop the rest of
        // this USB chunk and wait for the parser to make room.
        dropped += size - i;
        break;
      }
      radio->bt_buf[radio->w_ptr] = catmsg.buf[i];
      radio->w_ptr = next;
      copied++;
    }
  }

  if ((verbose & VERBOSE_USB) && (copied > 0 || dropped > 0)) {
    console->printf(
        "USB CAT RX bridge rig=%d copied=%d dropped=%d r=%d w=%d\n",
        radio->rig_idx, copied, dropped, radio->r_ptr, radio->w_ptr);
  }
}
// key input from usb running in separate task 24/10/29 


