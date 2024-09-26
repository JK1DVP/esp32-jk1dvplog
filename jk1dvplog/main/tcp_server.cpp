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
#include "ui.h"
#include "usb_host.h"
#include "tcp_server.h"

WiFiServer server(23);
WiFiClient serverClients[MAX_SRV_CLIENTS];
int serverClients_status[MAX_SRV_CLIENTS] ;
int timeout_tcpserver = 0;

void tcpserver_process() {
  int ret;
  uint8_t i;
  char c;
  static uint8_t mod_c; // storage of received modifier character

  if (timeout_tcpserver < millis()) {
    timeout_tcpserver = millis() + 1000;
    if (wifi_status == 1) {
      //check if there are any new clients
      if (server.hasClient()) {
        for (i = 0; i < MAX_SRV_CLIENTS; i++) {
          //find free/disconnected spot
          if (!serverClients[i] || !serverClients[i].connected()) {
            if (serverClients[i]) serverClients[i].stop();
            serverClients[i] = server.available();
            if (!serverClients[i]) plogw->ostream->println("available broken");
            plogw->ostream->print("New client: ");
            plogw->ostream->print(i); plogw->ostream->print(' ');
            plogw->ostream->println(serverClients[i].remoteIP());
            break;
          }
        }
        if (i >= MAX_SRV_CLIENTS) {
          //no free/disconnected spot so reject
          server.available().stop();
        }
      }
    }
  }
  //check clients for data
  for (i = 0; i < MAX_SRV_CLIENTS; i++) {
    if (serverClients[i] && serverClients[i].connected()) {
      if (serverClients[i].available()) {
        //get data from the telnet client and push it to the UART

        while (serverClients[i].available()) {
          c = serverClients[i].read();

          if (isprint(c)) {
            plogw->ostream->print(c);
          } else {
            plogw->ostream->print(" "); plogw->ostream->print(c, HEX); plogw->ostream->print(" ");
          }
          // check keycode over tcp
          if (serverClients_status[i] == 0) {
            if (c >= 238) {

              if (c == 239) {
                // key pressed
                serverClients_status[i] = 2;
                break;
              }
              if (c == 238) {
                // key released
                serverClients_status[i] = 3;
                break;
              }
              // check telnet commands
              if (c == 255) {
                // IAC
              }
              serverClients_status[i] = 0;
              continue;
            }
          } else if (serverClients_status[i] == 1) {
            // telnet commands
            continue;
          } else if (serverClients_status[i] == 4) {

            // void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key) {
            // mod : or'ed 0x01 control 0x02 left_shift 0x04 alt 0x08 command 0x10 right_shift ? 0x40 alt 0x80 command
            // key : USB HID scan code for the key
            //KbdRptParser::OnKeyDown((unit8_t mod),(uint8_t c));
            uint8_t mod;
            mod = 0;
            MODIFIERKEYS modkey;
            *((uint8_t *)&modkey) = mod_c;
            uint8_t c_to_ascii = kbd_oemtoascii2(mod,c);
            //on_key_down(modkey, (uint8_t)key, (uint8_t)c);

            on_key_down(modkey, (uint8_t)c, (uint8_t) c_to_ascii);
            // key pressed event
            serverClients_status[i] = 0; // finished processing keydown sequence
            continue;
          } else if (serverClients_status[i] == 5) {
            // key released event
            continue;
          } else if (serverClients_status[i] == 2) {
            // after recieved
            mod_c = c;
            serverClients_status[i] = 4;
            continue;
          } else if (serverClients_status[i] == 3) {
            mod_c = c;
            serverClients_status[i] = 5;
            continue;
          }


          // not special character put into ring line buffer
          write_ringbuf(&(plogw->tcp_ringbuf), c);
          // and check line
          ret = readfrom_ringbuf(&plogw->tcp_ringbuf, plogw->tcp_cmdbuf + plogw->tcp_cmdbuf_ptr, (char)0x0a, (char)0x0d, plogw->tcp_cmdbuf_len - plogw->tcp_cmdbuf_ptr);
          if (ret < 0) {
            // one line read
            plogw->ostream->print("tcp readline:");
            plogw->ostream->println(plogw->tcp_cmdbuf);
            plogw->ostream = &serverClients[i];
            cmd_interp(plogw->tcp_cmdbuf); // pass to command interpreter 
            plogw->ostream = &Serial; // change output stream to serial port 
            *plogw->tcp_cmdbuf = '\0';
            plogw->tcp_cmdbuf_ptr = 0;

          } else {
            if (ret > 0 ) {
              plogw->tcp_cmdbuf_ptr += ret;
            }
            if (plogw->tcp_cmdbuf_ptr == plogw->tcp_cmdbuf_len) {
              plogw->tcp_cmdbuf_ptr = 0;
            }
          }
        }
      }
    }
    else {
      if (serverClients[i]) {
        serverClients[i].stop();
      }
    }
  }

}

void init_tcpserver()
{
  server.begin(); 
  server.setNoDelay(true);
}
  
