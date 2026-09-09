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

#include <Arduino.h>
#include <stdarg.h>
#include <ch9350if.h>
#include <ch9350if_hidkeys.h>
#include "mux_transport.h"

#ifndef DVP_CH9350_KEY_DIAG
#define DVP_CH9350_KEY_DIAG 1
#endif

static bool g_ch9350_diag_enabled = false; // default OFF for normal operation

void set_ch9350_diag_enabled(bool enabled)
{
  g_ch9350_diag_enabled = enabled;
}

bool ch9350_diag_enabled()
{
  return g_ch9350_diag_enabled;
}

static void ch9350_diag_mux(const char *fmt, ...)
{
#if DVP_CH9350_KEY_DIAG
  if (!g_ch9350_diag_enabled || !f_mux_transport) return;
  char body[180];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  char pkt[196];
  snprintf(pkt, sizeof(pkt), "kbdiag:%s", body);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)pkt, strlen(pkt));
#endif
}

static bool ch9350_diag_key(uint8_t hid_code)
{
  return hid_code == 0x36 || hid_code == 0x37 || // band down/up
         hid_code == 0x10 ||                     // M (Alt-M)
         hid_code == 0xe0 || hid_code == 0xe1 ||
         hid_code == 0xe2 || hid_code == 0xe3 ||
         hid_code == 0xe4 || hid_code == 0xe5 ||
         hid_code == 0xe6 || hid_code == 0xe7;
}


class _ch9350 : public ch9350if {
  uint8_t led_state = 0;
public:
  _ch9350(uint8_t rst) : ch9350if(rst) {}
  void key_event(uint8_t hid_code, bool on) {
#if DVP_CH9350_KEY_DIAG
    if (g_ch9350_diag_enabled && ch9350_diag_key(hid_code)) {
      Serial.printf("CH9350_EVT t=%lu hid=0x%02X on=%u mux=%u\n",
                    (unsigned long)millis(), (unsigned int)hid_code,
                    on ? 1U : 0U, f_mux_transport ? 1U : 0U);
      ch9350_diag_mux("EVT t=%lu hid=0x%02X on=%u",
                       (unsigned long)millis(), (unsigned int)hid_code,
                       on ? 1U : 0U);
    }
#endif
    if (f_mux_transport) {
      // Include a monotonically increasing sequence number so that the main
      // board can detect a lost key transition.  Losing only an Alt/Ctrl/Shift
      // release otherwise leaves that modifier latched until reset.
      static uint8_t key_event_seq = 0;
      unsigned char tmp_buf[3];
      tmp_buf[0] = hid_code;
      tmp_buf[1] = on;
      tmp_buf[2] = key_event_seq++;
#if DVP_CH9350_KEY_DIAG
      if (g_ch9350_diag_enabled && ch9350_diag_key(hid_code)) {
        Serial.printf("CH9350_TX  t=%lu seq=%u hid=0x%02X on=%u\n",
                      (unsigned long)millis(), (unsigned int)tmp_buf[2],
                      (unsigned int)hid_code, on ? 1U : 0U);
        ch9350_diag_mux("TX t=%lu seq=%u hid=0x%02X on=%u",
                         (unsigned long)millis(), (unsigned int)tmp_buf[2],
                         (unsigned int)hid_code, on ? 1U : 0U);
      }
#endif
      mux_transport.send_pkt(MUX_PORT_USB_KEYBOARD1_EXT,
                             MUX_PORT_USB_KEYBOARD1_MAIN, tmp_buf, 3);
    } else {
      Serial.print(" : ");
      dump_byte(hid_code);
      Serial.print(on ? " ON  " : " OFF ");
      Serial.println(get_hid_keyname(hid_code));
      if (on) {
	uint8_t led = get_led_state();
	switch(hid_code) {
	case HID_CAPS: // caps
	  //        led ^= HID_LED_CAPSLOCK;
	  break;
	case HID_KEYPAD_NUMLOCK: // numlock
	  //        led ^= HID_LED_NUMLOCK;
	  break;
	case HID_SCRLOCK: // numlock
	  led ^= HID_LED_SCRLOCK;
	  break;
	}
	set_led_state(led);
      }
    }
  }
  void dataframe(uint8_t* data, uint8_t data_length) {
#if DVP_CH9350_KEY_DIAG
    if (g_ch9350_diag_enabled) {
    Serial.printf("CH9350_RAW t=%lu len=%u", (unsigned long)millis(),
                  (unsigned int)data_length);
    bool interesting = false;
    for (uint8_t i = 0; i < data_length; i++) {
      Serial.printf(" %02X", (unsigned int)data[i]);
      const uint8_t b = data[i];
      if (b == 0x36 || b == 0x37 || b == 0x10 ||
          (b >= 0xe0 && b <= 0xe7)) interesting = true;
    }
    Serial.println();

    if (interesting && f_mux_transport) {
      char raw[150];
      int n = snprintf(raw, sizeof(raw), "RAW t=%lu len=%u",
                       (unsigned long)millis(), (unsigned int)data_length);
      for (uint8_t i = 0; i < data_length && n < (int)sizeof(raw) - 4; i++) {
        n += snprintf(raw + n, sizeof(raw) - n, " %02X",
                      (unsigned int)data[i]);
      }
      ch9350_diag_mux("%s", raw);
    }
    }
#endif
    if (!f_mux_transport) {
      for(uint8_t i = 0; i < data_length; i++)
	dump_byte(data[i]);
    }
  }
};

//_ch9350 ch9350(CH_RST_PORT);
_ch9350 ch9350(0xff); // without reset port connection

void init_keyboard()
{
  ch9350.ch9350_setup();
}

void loop_keyboard()
{
  ch9350.ch9350_loop();
    
  //  ch9350.set_led_state(HID_LED_CAPSLOCK);
  //    delay(10);
  //    ch9350.set_led_state(0);
  //    delay(10);    
}
// 57 ab 12 00 00 00 00 ff 80 00 20 stop sending
// 57 ab 82 a3
// 57 ab 83/88 len label key_values serial checksum

