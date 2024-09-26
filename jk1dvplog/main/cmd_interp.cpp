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
#include "callhist.h"
#include "qso.h"
#include "dupechk.h"
#include "ui.h"
#include "log.h"
#include "multi_process.h"
#include "misc.h"
#include "cat.h"
#include "cluster.h"
#include "contest.h"
#include "cluster.h"
#include "settings.h"
#include "bandmap.h"
#include "main.h"
#include "sd_files.h"
#include "SD.h"
#include "mcp.h"
#include "satellite.h"
#include "console.h"

int cmd_interp_state = 0;
// command interpreter
// callhist_set
// dumpqsolog
//

char *strtoupper(char *s) {
  char *p;
  p = s;
  while (*p) {
    *p = toupper(*p);
    p++;
  }
  return s;
}

void cmd_interp(char *cmd) {
  int tmp, tmp1;
  switch (cmd_interp_state) {
    case 0:  // command line
      plogw->ostream->print("cmd:");
      plogw->ostream->println(cmd);

      if (strcmp("loadsat", cmd) == 0) {
	load_satinfo();
        break;
      }
      if (rotator_commands(cmd)) break;
      if (antenna_switch_commands(cmd)) {
		  break;
	  }
      if (signal_command(cmd)) break;
      if (antenna_alternate_command(cmd)) break;
      if (strcmp("savesat", cmd) == 0) {
	save_satinfo();
        break;
      }
      if (strcmp(cmd, "emu") == 0) {
        // enter into console terminal emulation mode
        plogw->f_console_emu = 1;
	// send clear screen command
	char buf[40];	
	for (int i=0;i<20;i++) {
	  sprintf(buf,"\033[%d;1H\033[K",i);
	  plogw->ostream->print(buf);
	}
        break;
      }
      if (strncmp(cmd, "send", 4) == 0) {
	//        SO2Rprint(cmd + 5);
        break;
      }
      if (strncmp(cmd, "verbose", 7) == 0) {
        tmp1 = sscanf(cmd + 7, "%d", &tmp);
        if (tmp1 == 1) {
          verbose = tmp;
        } else {
          if (verbose > 0) {
            verbose = 0;
          } else {
            verbose = 1;
          }
        }
        break;
      }
      if (strncmp(cmd, "nextaos", 7) == 0) {
	start_calc_nextaos();
        break;
      }
      if (strcmp(cmd, "satellite") == 0) {
        f_sat_updated = 0;  // reset flag
	allocate_sat();
	getTLE();
        break;
      }

      if (strncmp(cmd, "gpio", 4) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 4, "%d %d", &tmp, &tmp1) == 2) {
	  write_mcp_gpio(tmp,tmp1);

          plogw->ostream->print("write mcp gpio port ");
          plogw->ostream->print(tmp);
          plogw->ostream->print(" value ");
          plogw->ostream->println(tmp1);
          for (int i = 0; i < 15; i++) {
            plogw->ostream->print(i);
            plogw->ostream->print(" ");
            plogw->ostream->println(read_mcp_gpio(i));
          }
        } else {
          plogw->ostream->println("gpio mcp param error");
        }
        break;
      }



      if (strcmp(cmd, "listdir") == 0) {
        listDir(SD, "/", 0);
        break;
      }
      if (strcmp(cmd, "help") == 0) {
        // show help of commands
        plogw->ostream->println("Available Commands:");
        plogw->ostream->println("emu (entering terminal emulation EXITEMU to end)/verbose[num]\nhelp (show this)\nloadsat/nextaos/satellite\n\n// QSO log commands\nnewqsolog (start new log QSO.TXT)\nmakedupe (dupe/multi check from QSO.TXT)\ndumpqso[num](dump raw qso file [num] if read backup files\nreadqso (read log QSO.txt in ctestwin txt imporable format)\nlistdir (show directory content)\n\n// CLUSTER commands\nDx de (push cluster information)\n\n// RADIO remote control commands\nsetstninfo{Callsign} (set target to work station information)\nstatus (get status of the radios)\nswitch_radio {radio#}/enable_radio {radio#}\nfocus {radio#} (change focused radio)\n\n// SETTINGS commands\nsave[name] (save settings)\nload[name]\nsettings (show settings information)\nassign{variable_name} {value} (assign settings variables)\npost_assign (perform post assignment tasks)\n\n// CALLHISTORY commands\ncallhist_set [callhist_filename] (start setting callhist information,input lines of callsign exchange,end will end setting.\ncallhist_open [callhist_fn] (set callhist filename and open)\ncallhist_search (start callhist search, input callsign to search, end will end search session.)\n\n// MAINTENANCE commands\ngpio[port] [value](write mcp gpio port)\nreset_settings/restart_jk1dvplog\n");

        break;
      }
      if (strncmp("callhist_set", cmd, 12) == 0) {
        plogw->ostream->println("callhist_set command");
        if (cmd[12] == ' ') {
          set_callhistfn(cmd + 13);
        } else {
          set_callhistfn("");
        }
        cmd_interp_state = 1;
        break;
      }
      if (strncmp(cmd, "DX de", 5) == 0) {
        // cluster info push from serial
        get_info_cluster(cmd);
        break;
      }
      if (strncmp("dumpqso", cmd, 7) == 0) {
        plogw->ostream->println("dumpqso command");
        plogw->ostream->println(cmd + 8);
	if (strlen(cmd)>7) {
	  dump_qso_bak(cmd + 8);
	} else {
	  plogw->ostream->println("dumping current qso log.");	  
	  dump_qso_log();
	}
        break;
      }
      if (strncmp("showqso",cmd,7)==0) {
        plogw->ostream->println("showqso command");
	// get qso data:  showqso sernr 
	break;
      }

      
      if (strncmp("readqso", cmd, 7) == 0) {
        plogw->ostream->println("readqso command");
        read_qso_log(READQSO_PRINT);
        break;
      }
      if (strncmp("mailqso", cmd, 7) == 0) {
        plogw->ostream->println("mailsqso command");
	//        mail_qso_log();
        break;
      }

      if (strncmp("status", cmd, 7) == 0) {
        // get status of the radios
        print_status_console();
        break;
      }

      if (strncmp("setstninfo", cmd, 10) == 0) {
        // set target to work station information
        set_target_station_info(cmd + 11);
        break;
      }

      if (strncmp("load", cmd, 4) == 0) {
        // load setting
        load_settings(cmd + 4);
        break;
      }
      if (strncmp("assign", cmd, 6) == 0) {
        // assign variables similarly to load/save file.
        if (!plogw->f_console_emu) {
          plogw->ostream->print("assign:");
          plogw->ostream->print(cmd + 7);
          plogw->ostream->println(":");
        }
        assign_settings(cmd + 7, settings_dict);
        break;
      }
      if (strcmp("post_assign", cmd) == 0) {
        // setting after assigning variables
        radio_list[0].enabled = plogw->radios_enabled & 1;
        radio_list[1].enabled = (plogw->radios_enabled >> 1) & 1;
        radio_list[2].enabled = (plogw->radios_enabled >> 2) & 1;
        set_rig_from_name(&radio_list[0]);
        set_rig_from_name(&radio_list[1]);
        set_rig_from_name(&radio_list[2]);
        set_contest_id();
        set_cluster();
        break;
      }
      if (strcmp("settings", cmd) == 0) {
        dump_settings(plogw->ostream, settings_dict);
        break;
      }
      if (strncmp("save", cmd, 4) == 0) {
        // release other settings  including sat
        release_memory();
        save_settings(cmd + 4);
        if (!plogw->f_console_emu) plogw->ostream->println("save");
        break;
      }

      if (strncmp("switch_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          switch_radios(tmp, -1);
        } else {
          plogw->ostream->println("switch_radio radio#");
        }
        break;
      }
      if (strncmp("enable_radio", cmd, 12) == 0) {
        int tmp, tmp1;
        if (sscanf(cmd + 12, "%d", &tmp) == 1) {
          enable_radios(tmp, -1);
        } else {
          plogw->ostream->println("enable_radio radio#");
        }
        break;
      }
      if (strncmp("makedupe", cmd, 8) == 0 ) {
        init_score();
	//        init_multi();
	clear_multi_worked();
        init_dupechk();
        read_qso_log(READQSO_MAKEDUPE);
        break;
      }
      if (strncmp("focus", cmd, 5) == 0) {
        int new_focus;
        if (sscanf(cmd + 5, "%d", &new_focus) == 1) {
          change_focused_radio(new_focus);
        }
        break;
      }

      if (strcmp(cmd, "newqsolog") == 0) {
        // create new QSO log
        create_new_qso_log();
        break;
      }

      if (strcmp(cmd,"reset_settings")==0) {
	// remove files
	SD.remove("/settings.txt");
	SD.remove("/ch.txt");
	SD.remove("/wifiset.txt");	
        plogw->ostream->println("reset_settings by removing files settings.txt ch.txt wifiset.txt");
	break;
      }
      if (strcmp(cmd,"restart_jk1dvplog")==0) {
        plogw->ostream->println("restarting jk1dvplog by esp32 reset...");
	delay(1000);
	ESP.restart();
	break;
      }

      if (strncmp("callhist_open", cmd, 13) == 0) {
        if (cmd[13] == ' ') {
          set_callhistfn(cmd + 14);
        } else {
          set_callhistfn("");
        }
        open_callhist();
        break;
      }
      if (strcmp("callhist_search", cmd) == 0) {
        plogw->ostream->println("callhist_search command");
        cmd_interp_state = 2;
        break;
      }
      // other commands follow
      plogw->ostream->println("???");
      break;
    case 1:  // after callhist_set commsnd
      if (strcmp("end", cmd) == 0) {
        plogw->ostream->println("callhist_set end");
	close_callhistf();
	//        callhistf.close();
        callhistf_stat = 0;
        // open callhist
        open_callhist();
        cmd_interp_state = 0;
      } else {
        write_callhist(strtoupper(cmd));
      }
      break;
    case 2:  // call history search
      if (strcmp("end", cmd) == 0) {
        plogw->ostream->println("callhist_search end");
        cmd_interp_state = 0;
        break;
      }
      search_callhist(strtoupper(cmd));
      break;
  }
}
