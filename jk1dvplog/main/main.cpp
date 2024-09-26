/* 
   jk1dvplog main.cpp 24/6/30 E. Araki
*/

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

// 24/7/29
// python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 summary
//  python3 ~/esp/esp-idf/components/esptool_py/esptool/espefuse.py -p /dev/ttyUSB1 set_flash_voltage 3.3V
// to disable selecting flash voltage from pin stage SPI interface

// memo cq sp frequency gets equal if quickly alt-q's pressed
// in band changing operation cq/s and p, and phone/cw should be remembered for each band
// radio->cq_modetype_bank  does that but not implemented anything 24/07/30
// save/recall routine would take care of that
// implemented above but cq/sp switch doesnot recall old frequency (may be saved overwriting)

#include "Arduino.h"

#include "hardware.h"
#include "decl.h"
#include "variables.h"
#include "multi.h"
#include <Wire.h>
#include "SPIFFS.h"

#include "sd_files.h"
#include "usb_host.h"
#include "cw_keying.h"
#include "display.h"
#include "cat.h"
#include "log.h"
#include "qso.h"
#include "cluster.h"
#include "bandmap.h"
#include "multi_process.h"
#include "ui.h"
#include "so2r.h"
#include "processes.h"
#include "misc.h"
#include "settings.h"
#include "edit_buf.h"
#include "mcp.h"
#include "console.h"
#include "tcp_server.h"
#include "timekeep.h"
#include "network.h"
#include "zserver.h"
#include "satellite.h"
//#include "btserial.h" // not enough memory


//////
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

void setup()
{
  init_logwindow();

  digitalWrite(LED, 0);
  digitalWrite(CW_KEY1, 0);
  digitalWrite(CW_KEY2, 0);
  pinMode(LED, OUTPUT);
  pinMode(CW_KEY1, OUTPUT);
  pinMode(CW_KEY2, OUTPUT);

  radio_list[0].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[1].bt_buf = (char *)malloc(sizeof(char) * 256);
  radio_list[2].bt_buf = (char *)malloc(sizeof(char) * 256);

  init_cat_serialport();
  init_cat_kenwood() ;
  
  pinMode(21, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);

  Wire.begin();
  //  Wire.setBufferSize(256);
  init_mcp_port();

  init_sat();

  init_qso();
  init_bandmap();
  init_info_display();

  //  set_arrl_multi();
  //  multi_list.multi = NULL;
  init_multi(NULL,-1,-1);
  
  init_all_radio();
  init_settings_dict();

  init_display();
  init_usb();

  plogw->ostream->print("rig rigspec check");

  attach_interrupt_civ();

  init_cw_keying();
  init_sd();

  load_settings("settings");
  
  init_network();
  init_timekeep();
  timeout_rtc = millis() + 1000;
  upd_display();

  Serial.println("check 0");
  phone_switch_management();
  open_qsolog();

  strcpy(plogw->grid_locator_set, plogw->grid_locator + 2);
  set_location_gl_calc(plogw->grid_locator_set);
  print_memory();

  //  btserial_init();
  plan13_test();
}


void loop() {
  //  btserial_process();
  ftp_service_loop();
  time_measure_start(0);  Control_TX_process();  time_measure_stop(0);  
  time_measure_start(1);    timekeep();  time_measure_stop(1);  
  time_measure_start(2);    if (plogw->radio_mode == 2) SO2R_process();  time_measure_stop(2);  
  time_measure_start(3);   civ_process();time_measure_stop(3);
  //  ACMprocess();
  time_measure_start(4);   
  if (wifi_timeout < millis()) {
    wifi_timeout = millis() + 2000;
    check_wifi();
  }
  time_measure_stop(4);  
  //  Serial.println(digitalRead(15));
  time_measure_start(5);   interval_process(); time_measure_stop(5);  
  time_measure_start(6);   signal_process();  time_measure_stop(6);  
  time_measure_start(7);   rotator_sweep_process();  time_measure_stop(7);  

  time_measure_start(8);   repeat_func_process();  time_measure_stop(8);  

  if (plogw->sat) {
    sat_find_nextaos_sequence();
  }
  time_measure_start(9);   cluster_process();  time_measure_stop(9);  
  zserver_process();
  time_measure_start(10);   display_cwbuf();  time_measure_stop(10);  
  time_measure_start(11);   tcpserver_process();   time_measure_stop(11);  
  time_measure_start(12);   console_process();   time_measure_stop(12);  
  time_measure_start(13);   loop_usb();   time_measure_stop(13);
  main_loop_revs++;
  delay(1);
}


extern "C" void app_main(void)
{
  initArduino();
  Serial.begin(115200);
  while (!Serial) ;

  setup();

  while (1) {
    loop();
  }
    
}
