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
#include "bandmap.h"
#include "cat.h"
#include "display.h"
#include "ui.h"
#include "so2r.h"
#include "qso.h"
#include "zserver.h"
#include "satellite.h"
#include "misc.h"

int interval_process_stat = 0;
void interval_process() {
  struct radio *radio;
  int next_interval;
  static int query_item = 0;
  next_interval = 100;
  if (timeout_interval < millis()) {
    if (verbose & 256) {
      plogw->ostream->print("repeat timer=");
      plogw->ostream->print(plogw->repeat_func_timer);
      plogw->ostream->print("stat ");
      plogw->ostream->println(plogw->f_repeat_func_stat);
    }
    if (bandmap_disp.f_update) {
      upd_display_bandmap();
      bandmap_disp.f_update = 0;
    }

    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      if (!radio->enabled) continue;
      // query item map
      // query_items[]={ 0,1,2,3,4,5, 0,1,2,3,4,6, 0,1,2,3,4,7, 0,1,2,3,4,5
      if (!radio->f_civ_response_expected) {
	if (!plogw->f_show_signal) {
	  // normal 
	  switch (interval_process_stat) {  
	  case 0:
	    send_freq_query_civ(radio);//0
	    break;
	  case 1:
	    send_mode_query_civ(radio);//1
	    break;
	  case 2:
	    send_smeter_query_civ(radio);//2
	    break;
	  case 3:
	    send_ptt_query_civ(radio);//3
	    break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // ATT query or other information query 
	    if (query_item == 0) {
	      send_identification_query_civ(radio);  // 5 
	    } else if (query_item % 2 == 0) {
	      send_att_query_civ(radio);       // 6
	    } else if (query_item % 2 == 1) {
	      send_preamp_query_civ(radio);   //7 
	    }
	    query_item++;
	    if (query_item >= 11) query_item = 0;
			
	    break;
	  }
	} else {
	  // show signal 
	  switch (interval_process_stat) {  
	  case 0:
	  case 1:
	  case 2:
	  case 3:
	    send_smeter_query_civ(radio);//2
	    break;
	  case 4:  // silence period for rotator //4 
	    break;
	  case 5:  // ATT query or other information query 
	    if (query_item == 0) {
	      send_identification_query_civ(radio);  // 5 
	    } else {
	      switch (query_item % 5) {
	      case 0:
		send_att_query_civ(radio); break;       // 6
	      case 1:
		send_preamp_query_civ(radio);break;   //7 
	      case 2:
		send_ptt_query_civ(radio);break;//3
	      case 3:
		send_freq_query_civ(radio);break;//0
	      case 4:
		send_mode_query_civ(radio);break;//1
	      }
	    }
	    query_item++;
	    if (query_item >= 11) query_item = 0;
	    break;
	  }
	}
      }
    }

    if (interval_process_stat == 4) {
      //      rotator_process();
    }

    interval_process_stat++;
    if (interval_process_stat > 5) interval_process_stat = 0;
    timeout_interval = millis() + next_interval;
    //	plogw->ostream->print("PTT:");plogw->ostream->print(plogw->ptt_stat);plogw->ostream->print(" S_stat:");plogw->ostream->println(plogw->smeter_stat);
  }

  // satellite process 500ms
  if (timeout_interval_sat < millis()) {
    sat_process();
    timeout_interval_sat = millis() + 500;
  }

  // second process
  if (timeout_second < millis()) {
    // interval job every second
    //    Serial.print("receive_civport nmax in 1ms interrupt.:");
    //    Serial.println(receive_civport_count);

    receive_civport_count=0;
    timeout_second = millis() + 1000;

    // print_time_measure results and clear counter 
    /*    plogw->ostream->print("usec:");
    char tmp_buf[10];
    for (int i=0;i<15;i++) {
      sprintf(tmp_buf,"%4d ",time_measure_get(i));
      plogw->ostream->print(tmp_buf);
      time_measure_clear(i);
    }
    plogw->ostream->print(" revs=");
    plogw->ostream->print(main_loop_revs);
    main_loop_revs=0;
    plogw->ostream->println("");
    */
  }

  if (timeout_cat < millis()) {

    for (int i = 0; i < N_RADIO; i++) {
      if (!unique_num_radio(i)) continue;
      radio = &radio_list[i];
      receive_cat_data(radio);
    }
    timeout_cat = millis() + 50;
  }


  if (timeout_interval_minute < millis()) {
    // minute processes

    // remove old bandmap entry for all bands
    int i;
    for (i = 0; i < N_BAND; i++) {
      delete_old_entry(i, 20);
    }

    bandmap_disp.f_update = 1;
    timeout_interval_minute = millis() + 60000;

    // frequency notification to zserver
    zserver_freq_notification();
    
    
  }
}

void repeat_func_process() {
  if (plogw->f_repeat_func_stat == 3) {
    plogw->ostream->print("triggered repeat function");
    // trigger function_key function
    if (verbose) plogw->ostream->println("trigger repeat function");
    int focused_radio_save;
    plogw->f_repeat_func_stat = 1;
    SO2R_settx(plogw->repeat_func_radio);
    function_keys(plogw->repeat_func_key, plogw->repeat_func_key_modifier, 0);
    if (verbose & 256) {
      plogw->ostream->print("focused_radio:");
      plogw->ostream->print(plogw->focused_radio);
    }
    //    delay(1);
  }
}
