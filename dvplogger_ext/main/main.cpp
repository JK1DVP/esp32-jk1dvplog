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


#include "Arduino.h"
#include "esp_heap_caps.h"

#include <Wire.h>
#include "Ticker.h"
#include "keyboard.h"
#include "mux_transport.h"

//////
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

#include <SoftwareSerial.h>
SoftwareSerial Serial3;


//#include "dac_playback.h"
#include "AudioPlayer.h"

// issues
// 25/4/21
// Software serial data receive corruption (loop too slow?)
// occurs very often when BTserial has a traffic

// Bluetooth modem for esp32-jk1dvplog E. Araki 24/8/24
// to link IC-705 and esp32-jk1dvplog by Bluetooth
//
// made from the following demo code SerialToSerialBT.ino
// 
// This example code is in the Public Domain (or CC0 licensed, at your option.)
// By Evandro Copercini - 2018
//
// This example creates a bridge between Serial and Classical Bluetooth (SPP)
// and also demonstrate that SerialBT have the same functionalities of a normal Serial
// Note: Pairing is authenticated automatically by this device

// to suppress Bluetooth debug information menuconfig and go to Bluetooth options to disable log information for Bluetooth 

#include "BluetoothSerial.h"
//#include "dac_playback.h"
#include "main.h"
//#include "decl.h"
#include "dupechk.h"
#include "callhist_remote.h"


Ticker serial_reader;

Stream *console;

String device_name = "ESP32-BT";

// Check if Bluetooth is available
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Check Serial Port Profile
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Port Profile for Bluetooth is not available or not enabled. It is only available for the ESP32 chip.
#endif

BluetoothSerial SerialBT;
uint8_t buf[2048];// serialBT receive buffer
uint8_t buf1[2048]; // Serial receive buffer
uint8_t buf2[2048]; // CAT2 Serial receive buffer
uint8_t buf3[2048]; // CAT3 Serial receive buffer
int count=0,count1=0,count2=0,count3=0;
// cat2 serial setting  (defaults fits FTDX10)
int cat2_baud=38400;
int cat2_reversed=1;

int cat3_baud=38400;
int cat3_reversed=1;
int f_spiram=0;

void read_serial_ports() // read serialports and buffer
{
  // cat2
  char c;
  int counta=0;
  while (Serial2.available() && counta<20) {
    c=Serial2.read();
    buf2[count2++]=c;
    counta++;
    // Serial.print("c:");
    //Serial.println(count);
    
    if ((c==0x0a)||(c==0xfd)||(c==';')) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_CAT2_EXT,MUX_PORT_CAT2_MAIN,buf2,count2);
      } else {
	Serial.write(buf2,count2);	
      }
      count2=0;      
    }
    if (count2>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_CAT2_EXT,MUX_PORT_CAT2_MAIN,buf2,count2);
      } else {
	Serial2.write(buf2,count2);
      }
      count2=0;
    }
  }

  counta=0;
  while (SerialBT.available()&&(counta<20) ) {
    c=SerialBT.read();
    buf[count++]=c;
    counta++;
    // Serial.print("c:");
    //Serial.println(count);
    if ((c==0x0a)||(c==0xfd)) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_BT_SERIAL_EXT,MUX_PORT_BT_SERIAL_MAIN,buf,count);
      } else {
	Serial.write(buf,count);	
      }
      count=0;      
    }
    if (count>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_BT_SERIAL_EXT,MUX_PORT_BT_SERIAL_MAIN,buf,count);
      } else {
	Serial.write(buf,count);
      }
      count=0;
    }
  }
  
}


void cat2_pkt_handler(struct mux_packet *packet)
{
  Serial2.write(packet->buf,packet->idx);
}

void cat3_pkt_handler(struct mux_packet *packet)
{
  Serial3.write(packet->buf,packet->idx);
}

void serialbt_pkt_handler(struct mux_packet *packet)
{
  SerialBT.write((uint8_t *)packet->buf,packet->idx);
}

// cw keying related rountine
void cwbuf_control(char *cmd)
{
  // if KEY31 .. does not work ok implement interrupt based cw keying for key3
}

void key_control(char *cmd)
{
  switch(cmd[0]) {
  case '3': // key 3 port
    switch(cmd[1]) {
    case '1':// on
      digitalWrite(KEY3,1);
      break;
    case '0':// off
      digitalWrite(KEY3,0);
      break;
    }
    break;
  }
}

void control_pkt_handler(struct mux_packet *packet)
{
  int ret;
  char buf[256];
  // check content
  if (strncmp(packet->buf,"chping",6)==0) {
    const char *pong = "chpong";
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)pong, strlen(pong));
    return;
  }
  if (strncmp(packet->buf, "kbdiag:", 7) == 0) {
    bool enable = false;
    if (strncmp(packet->buf + 7, "on", 2) == 0) {
      enable = true;
      set_ch9350_diag_enabled(true);
    } else if (strncmp(packet->buf + 7, "off", 3) == 0) {
      enable = false;
      set_ch9350_diag_enabled(false);
    } else {
      enable = ch9350_diag_enabled();
    }

    snprintf(buf, sizeof(buf), "kbdiagack:%s", enable ? "on" : "off");
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)buf, strlen(buf));
    return;
  }

  if (strncmp(packet->buf,"memstat",7)==0) {
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    snprintf(buf, sizeof(buf), "memstat:%u|%u|%u|%u|%u|%u|%u",
             (unsigned int)free8,
             (unsigned int)min8,
             (unsigned int)largest8,
             (unsigned int)free_internal,
             (unsigned int)free_spiram,
	     (unsigned int)get_dupechk_nmaxqso(),
	     (unsigned int)get_dupechk_ncallsign()	     
	     );
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)buf, strlen(buf));
    return;
  }
  if (strncmp(packet->buf,"go_no_mux",9)==0) {
    f_mux_transport=0;
    return;
  }
  if (strncmp(packet->buf,"play",4)==0) {
    // playback command
    strncpy(buf,packet->buf+4,packet->idx-4);
    buf[packet->idx-4]='\0';
    //    play_sound(packet->buf+4);
    play_sound(buf);
    return;
  }
  if (strncmp(packet->buf,"key",3)==0) {
    // cw key on/off
    key_control(packet->buf+3);
    return;
  }
    
  if (strncmp(packet->buf,"cwbuf",5)==0) {
    // control cw/phone sending buffer
    cwbuf_control(packet->buf+5);
    return;
  }
  if (strncmp(packet->buf,"chreset",7)==0) {
    process_callhist_reset_subcpu(packet->buf + 7);
    return;
  }
  // "chend" also begins with "che", so handle the longer command first.
  if (strncmp(packet->buf,"chend",5)==0) {
    process_callhist_end_subcpu();
    return;
  }
  if (strncmp(packet->buf,"che",3)==0) {
    size_t n = min((size_t)(packet->idx - 3), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 3, n); buf[n] = '\0';
    process_callhist_entry_subcpu(buf);
    return;
  }
  if (packet->idx >= 8 &&
      memcmp(packet->buf, "dupemask", 8) == 0) {
    // MUX payloads are length-delimited, not NUL-terminated.  Parse only
    // bytes belonging to this packet; otherwise stale trailing digits can
    // change e.g. "252" into "2520".
    char mask_buf[16];
    size_t n = min((size_t)(packet->idx - 8), sizeof(mask_buf) - 1);
    memcpy(mask_buf, packet->buf + 8, n);
    mask_buf[n] = '\0';
    int mask = atoi(mask_buf);
    if (mask >= 0 && mask <= 255)
      set_dupechk_mask_subcpu((unsigned char)mask);
    return;
  }
  if (packet->idx >= 13 &&
      memcmp(packet->buf, "dupebulkbegin", 13) == 0) {
    // Same bounded parsing rule as dupemask.  With the old direct atoi(),
    // an intended decimal mask "252" followed by stale '0' became 2520;
    // casting that to uint8_t produced 216 (0xD8), exactly the observed bug.
    char mask_buf[16];
    size_t n = min((size_t)(packet->idx - 13), sizeof(mask_buf) - 1);
    memcpy(mask_buf, packet->buf + 13, n);
    mask_buf[n] = '\0';
    int mask = atoi(mask_buf);
    if (mask >= 0 && mask <= 255)
      begin_makedupe_bulk_subcpu((unsigned char)mask);
    return;
  }
  if (strncmp(packet->buf,"dupebulkend",11)==0) {
    finish_makedupe_bulk_subcpu();
    return;
  }
  if (strncmp(packet->buf,"dupeb",5)==0) {
    size_t n = min((size_t)(packet->idx - 5), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 5, n);
    buf[n] = '\0';
    entry_makedupe_bulk_subcpu(buf);
    return;
  }
  if (strncmp(packet->buf,"dupereset",9)==0) {
    // reset_dupechk_subcpu_database() does not return until the old DB has
    // been freed, the new DB allocated/cleared, and ncallsign is zero.
    // Only then send the completion ACK; MAIN will not start dupebulkbegin
    // before receiving this packet.
    const int ncallsign = reset_dupechk_subcpu_database();
    char response[32];
    snprintf(response, sizeof(response), "dupereset:done:%d", ncallsign);
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)response, strlen(response));
    return;
  }
  if (strncmp(packet->buf,"dupee",5)==0) { // dupe entry
    size_t n = min((size_t)(packet->idx - 5), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 5, n);
    buf[n] = '\0';
    entry_dupechk_subcpu(buf);
    return;
  }
  if (strncmp(packet->buf,"dupep",5)==0) { // partial callsign check
    size_t n = min((size_t)(packet->idx - 5), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 5, n);
    buf[n] = '\0';
    process_dupechk_partial_query_subcpu(buf);
    return;
  }
  if (strncmp(packet->buf,"dupec",5)==0) { // dupe check
    size_t n = min((size_t)(packet->idx - 5), sizeof(buf) - 1);
    memcpy(buf, packet->buf + 5, n);
    buf[n] = '\0';
    process_dupechk_query_subcpu(buf);
    return;
  }
  // setting cat2 parameter
  if (strncmp(packet->buf,"cat2_param",10)==0) {
    cat2_reversed=0;
    ret=sscanf(packet->buf+10,"%d %d",&cat2_baud,&cat2_reversed) ;
    // set serial
    Serial2.begin(cat2_baud,SERIAL_8N1,
		  27,
		  15,
		  cat2_reversed);
    return;
  }
  if (strncmp(packet->buf,"cat3_param",10)==0) {
    cat3_reversed=0;
    ret=sscanf(packet->buf+10,"%d %d",&cat3_baud,&cat3_reversed) ;
    // set serial
    //    Serial3.begin(cat3_baud,SERIAL_8N1,
    //		  27,
    //		  15,
    //		  cat3_reversed);
#define CATRX 35 
#define CATTX 33 
    Serial3.begin(cat3_baud,SWSERIAL_8N1,CATRX,CATTX,cat3_reversed,95,0);
    Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
    return;
  }


  
  
}

void setup() {

  Serial1.setRxBufferSize(512);
  Serial1.begin(115200, SERIAL_8N1, 16, 17); // for ch9350
  
  // Serial.begin(115200);
  //  Serial.begin(38400);
  //  SerialBT.setRxBufferSize(512);  
  SerialBT.begin(device_name);  //Bluetooth device name
  //SerialBT.deleteAllBondedDevices(); // Uncomment this to delete paired devices; Must be called after begin
  Serial.printf("The device with name \"%s\" is started.\nNow you can pair it with Bluetooth!\n", device_name.c_str());

  console=&Serial;
  
  count=0;count1=0;


  Serial2.setRxBufferSize(512);
  // serial.begin(rx,tx)
  // serial3_tx IO15  seirla3_rx IO27
  Serial2.begin(cat2_baud, SERIAL_8N1, 27,15,cat2_reversed);
  // CAT2  // need to have ways to reconfigure the setting of CAT2 by packet command to controller
  //    Serial1.begin(baud,SERIAL_8N1,
  //		  rxPin,
  //		  txPin,
  //		  radio->rig_spec->civport_reversed);

  Serial3.begin(cat3_baud,SWSERIAL_8N1,CATRX,CATTX,cat3_reversed,95,0);
  Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal

  init_dupechk_subcpu();
  
  mux_transport=Mux_transport();
  mux_transport.mux_stream=&Serial;
  mux_transport.set_port_handler(MUX_PORT_EXT_BRD_CTRL,control_pkt_handler);
  mux_transport.set_port_handler(MUX_PORT_CAT2_EXT,cat2_pkt_handler);
  mux_transport.set_port_handler(MUX_PORT_CAT3_EXT,cat3_pkt_handler);  
  mux_transport.set_port_handler(MUX_PORT_BT_SERIAL_EXT,serialbt_pkt_handler);
		
  init_keyboard()  ;

  serial_reader.attach_ms(1,read_serial_ports); // scan and read serial ports and store in ring buffers
}

int timeout_play=0;

// SUBCPU loop/MUX latency profiler. Results are returned to MAIN over the
// control MUX port, never printed directly into the MUX UART.
struct subcpu_profile_t {
  uint32_t loops;
  uint32_t max_loop_us;
  uint32_t max_keyboard_us;
  uint32_t max_serial3_us;
  uint32_t max_mux_us;
  uint32_t max_mux_gap_us;
  uint32_t serial3_bytes;
  uint32_t slow_keyboard;
  uint32_t slow_serial3;
  uint32_t slow_loop;
};

static subcpu_profile_t subprof = {};
static uint32_t subprof_last_report_ms = 0;
static uint32_t subprof_last_mux_us = 0;

static inline void subprof_update_max(uint32_t &dst, uint32_t value)
{
  if (value > dst) dst = value;
}

// Keep MAIN<->SUBCPU request latency low even while keyboard/CAT work is busy.
static inline void service_mux_transport()
{
  const uint32_t now = micros();
  if (subprof_last_mux_us != 0) {
    subprof_update_max(subprof.max_mux_gap_us, now - subprof_last_mux_us);
  }
  subprof_last_mux_us = now;

  const uint32_t started = micros();
  if (f_mux_transport) {
    mux_transport.recv_pkt();
  }
  subprof_update_max(subprof.max_mux_us, micros() - started);
}

static void report_subcpu_profile()
{
  const uint32_t now_ms = millis();
  // Keep diagnostics, but reduce routine traffic on the KBD/DUPE MUX.
  if ((uint32_t)(now_ms - subprof_last_report_ms) < 5000) return;
  subprof_last_report_ms = now_ms;

  if (f_mux_transport) {
    char msg[220];
    snprintf(msg, sizeof(msg),
             "subprof:loops=%u loop=%u keyboard=%u serial3=%u mux=%u "
             "muxgap=%u bytes3=%u slowk=%u slows3=%u slowloop=%u",
             (unsigned int)subprof.loops,
             (unsigned int)subprof.max_loop_us,
             (unsigned int)subprof.max_keyboard_us,
             (unsigned int)subprof.max_serial3_us,
             (unsigned int)subprof.max_mux_us,
             (unsigned int)subprof.max_mux_gap_us,
             (unsigned int)subprof.serial3_bytes,
             (unsigned int)subprof.slow_keyboard,
             (unsigned int)subprof.slow_serial3,
             (unsigned int)subprof.slow_loop);
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)msg, strlen(msg));
  }

  subprof = {};
  subprof_last_mux_us = micros();
}

void loop() {
  char c;
  const uint32_t loop_started = micros();
  subprof.loops++;

  service_mux_transport();
  const uint32_t keyboard_started = micros();
  loop_keyboard();
  const uint32_t keyboard_us = micros() - keyboard_started;
  subprof_update_max(subprof.max_keyboard_us, keyboard_us);
  if (keyboard_us >= 5000) subprof.slow_keyboard++;
  service_mux_transport();
  /*    while (Serial1.available()) {
      c=Serial1.read();
      Serial.println(c,HEX);
    }
  */
  /*
  char buf[100];
  if (timeout_play < millis()) {
    if (!player.isPlaying()) {
      player.play_string("jk1edc ");
      ESP_LOGI("MAIN", "pushed FIFO jk1edc ");      
    } else {
      if (player.get_string_fifo(buf,100)) {
	ESP_LOGI("MAIN", "FIFO empty, playback finished %s",buf);
      } 
    }
    timeout_play=millis()+100;
  }
  */
  // cat3
  const uint32_t serial3_started = micros();
  uint32_t serial3_bytes_this_loop = 0;
  while (Serial3.available() ) {
    c=Serial3.read();
    serial3_bytes_this_loop++;
    buf3[count3++]=c;
    // Serial.print("c:");
    //Serial.println(count);

    if ((c==0x0a)||(c==0xfd)||(c==';')) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_CAT3_EXT,MUX_PORT_CAT3_MAIN,buf3,count3);
      } else {
	Serial.write(buf3,count3);	
      }
      count3=0;      
    }
    if (count3>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_CAT3_EXT,MUX_PORT_CAT3_MAIN,buf3,count3);
      } else {
	Serial3.write(buf3,count3);
      }
      count3=0;
    }
  }
  const uint32_t serial3_us = micros() - serial3_started;
  subprof.serial3_bytes += serial3_bytes_this_loop;
  subprof_update_max(subprof.max_serial3_us, serial3_us);
  if (serial3_us >= 5000) subprof.slow_serial3++;
  service_mux_transport();
  

  if (f_mux_transport) {
    service_mux_transport();
  } else {
    while (Serial.available()) {
      c=Serial.read();
      buf1[count1++]=c;
      //      Serial.print("c1:");
      //      Serial.println(c);
      //      Serial.print(c); // loop back test
      if ((c==0x0a)||(c==0xfd)) {
	// check command
	buf1[count1]='\0';
	//	Serial.print("received cmd:");
	//	Serial.println((char *)buf1);
	if (strcmp((char *)buf1,"go_mux\r\n")==0) {
	  // go to mux_transport
	  Serial.println("go_mux accepted");
	  f_mux_transport=1;
	} else {
	  SerialBT.write(buf1,count1);
	}
	count1=0;
      }
      if (count1>=2000) {
	SerialBT.write(buf1,count1);
	count1=0;
      }
    }
  }
  const uint32_t loop_us = micros() - loop_started;
  subprof_update_max(subprof.max_loop_us, loop_us);
  if (loop_us >= 10000) subprof.slow_loop++;
  report_subcpu_profile();

  delay(1);
  //  Serial.print("*");
}

extern "C" void app_main_test1(void);
extern "C" void app_main_test(void) ;


extern "C" void app_main(void)
{
  esp_log_level_set("*", ESP_LOG_NONE);
  initArduino();
  Serial.setRxBufferSize(512);
  //  Serial.begin(115200);
  //  Serial.begin(750000);
  Serial.begin(3000000);    
  while (!Serial) ;

  setup();
  //  app_main_test1() ;
  //  app_main_test();
  init_AudioPlayer();

  //dac_playback_test();
  while (1) {
    loop();
  }
    
}
