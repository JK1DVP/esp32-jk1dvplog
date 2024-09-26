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
#include "SD.h"
#include "decl.h"
#include "variables.h"
#include "settings.h"
#include "contest.h"
#include "cat.h"
#include "cluster.h"
#include "network.h"
#include "zserver.h"

// read one line from file,with line termination 'term'
// maximum line buffer size specified in size
// return with number of chars in the line
// return with negative number ; not complete line read.
int readline(File *f, char *buf, int term, int size) {
  char c;
  int idx;
  idx = 0;
  while (f->available()) {
    if (idx >= size) {
      // no space to store read character
      return -(idx + 1);
    }
    c = f->read();
    switch (c) {
    case 0x0d:
      if (term == 0x0d) {
	buf[idx] = '\0';
	return idx;
      }
      if (term == 0x0d0a) {
	// ignore
	continue;
      }
      break;
    case 0x0a:
      if (term == 0x0d0a || term == 0x0a) {
	buf[idx] = '\0';
	return idx;
      }
      break;
    }
    buf[idx++] = c;
  }
  buf[idx] = '\0';
  return -(idx);
}

int n_settings_dict;
struct dict_item settings_dict[N_SETTINGS_DICT];

void init_settings_dict() {
  n_settings_dict = 0;

  settings_dict[n_settings_dict].name = "my_callsign";
  settings_dict[n_settings_dict].value = (void *)plogw->my_callsign + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "sent_exch";
  settings_dict[n_settings_dict].value = (void *)plogw->sent_exch + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_1";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[0] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_2";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[1] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_3";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[2] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_4";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[3] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_5";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[4] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_6";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[5] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cw_msg_7";
  settings_dict[n_settings_dict].value = (void *)plogw->cw_msg[6] + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "dupechk_mask";
  settings_dict[n_settings_dict].value = (void *)&plogw->mask;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "contest_id";
  settings_dict[n_settings_dict].value = (void *)&plogw->contest_id;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "bandmap_mask";
  settings_dict[n_settings_dict].value = (void *)&bandmap_mask;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "multi_type";
  settings_dict[n_settings_dict].value = (void *)&plogw->multi_type;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;


  settings_dict[n_settings_dict].name = "voice_memory_enable";
  settings_dict[n_settings_dict].value = (void *)&plogw->voice_memory_enable;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster_name";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "cluster_cmd";
  settings_dict[n_settings_dict].value = (void *)plogw->cluster_cmd + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "email_addr";
  settings_dict[n_settings_dict].value = (void *)plogw->email_addr + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  //  settings_dict[n_settings_dict].name = "wifi_ssid";
  //  settings_dict[n_settings_dict].value = (void *)plogw->wifi_ssid + 2;
  //  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  //  n_settings_dict++;

  //  settings_dict[n_settings_dict].name = "wifi_passwd";
  //  settings_dict[n_settings_dict].value = (void *)plogw->wifi_passwd + 2;
  //  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  //  n_settings_dict++;
  
  settings_dict[n_settings_dict].name = "rig_name_1";
  settings_dict[n_settings_dict].value = (void *)radio_list[0].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rig_name_2";
  settings_dict[n_settings_dict].value = (void *)radio_list[1].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "rig_name_3";
  settings_dict[n_settings_dict].value = (void *)radio_list[2].rig_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "radios_enabled";
  settings_dict[n_settings_dict].value = (void *)&plogw->radios_enabled;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;


  settings_dict[n_settings_dict].name = "radio_mode";
  settings_dict[n_settings_dict].value = (void *)&plogw->radio_mode;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "callhistfn";
  settings_dict[n_settings_dict].value = (void *)callhistfn;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "power_code";
  settings_dict[n_settings_dict].value = (void *)&plogw->power_code + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "zserver_name";
  settings_dict[n_settings_dict].value = (void *)&plogw->zserver_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "my_name";
  settings_dict[n_settings_dict].value = (void *)&plogw->my_name + 2;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_CHARARRAY;
  n_settings_dict++;

  settings_dict[n_settings_dict].name = "";
  settings_dict[n_settings_dict].value = NULL;
  settings_dict[n_settings_dict].value_type = DICT_VALUE_TYPE_INT;
  n_settings_dict++;
}


void clear_string(char *s)
{
  char *p;
  p=s;
  while (*p!='\0') {
    *p='\0';
    p++;
  }
}

// assign value from read line according to the dictionary
int assign_settings(char *line, struct dict_item *dict) {
  char *p, *arg, *value;
  p = strtok(line, " ");
  if (p == NULL) {
    return 0;
  }
  arg = p;
  p = strtok(NULL, "");
  if (p == NULL) return 0;
  value = p;

  while ((*dict).value != NULL) {
    if (strcmp((*dict).name, arg) == 0) {
      switch ((*dict).value_type) {
      case DICT_VALUE_TYPE_INT:
	*(int *)((*dict).value) = atoi(value);
	break;
      case DICT_VALUE_TYPE_CHARARRAY:
	// clear variable string with '\0'
	clear_string((char *)((*dict).value));
	*(char *)((*dict).value)='\0';
	strcat((char *)((*dict).value), value);
	break;
      }
      return 1;
    }
    dict++;
  }
  return 0;
}

// dump the dictionary values according to dictionary into given file f
int dump_settings(Stream *f, struct dict_item *dict) {
  if (!plogw->f_console_emu) plogw->ostream->println("dump_settings");
  while ((*dict).value != NULL) {

    f->print((*dict).name);
    f->print(" ");
    switch ((*dict).value_type) {
    case DICT_VALUE_TYPE_INT:
      f->println(*(int *)((*dict).value));
      break;
    case DICT_VALUE_TYPE_CHARARRAY:
      f->println((char *)((*dict).value));
      break;
    }
    dict++;  // to the next value
  }
  return 0;
}

int load_settings(char *fn) {
  char fnbuf[30];
  plogw->ostream->print("load settings from:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/settings");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);
  // f = SPIFFS.open(fnbuf, FILE_READ);
  f = SD.open(fnbuf, FILE_READ);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file settings.txt for reading");
    return 0;
  }
  while (readline(&f, buf, 0x0d0a, 128) != 0) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("line:");
      plogw->ostream->print(buf);
      plogw->ostream->println(":");
    }
    assign_settings(buf, settings_dict);
  }
  f.close();

  // post process

  radio_list[0].enabled = plogw->radios_enabled & 1;
  radio_list[1].enabled = (plogw->radios_enabled >> 1) & 1;
  radio_list[2].enabled = (plogw->radios_enabled >> 2) & 1;
  set_rig_from_name(&radio_list[0]);
  set_rig_from_name(&radio_list[1]);
  set_rig_from_name(&radio_list[2]);

  //  multiwifi_addap(plogw->wifi_ssid+2,plogw->wifi_passwd+2);  // do not try to remember wifi settings here but wifiset.txt
  set_contest_id();
  set_cluster();
  reconnect_zserver();
  
  return 1;
}

int save_settings(char *fn) {
  char fnbuf[30];
  plogw->ostream->print("save settings to:");
  if (*fn == '\0') {
    strcpy(fnbuf, "/settings");
  } else {
    strcpy(fnbuf, "/");
    strcat(fnbuf, fn);
  }
  strcat(fnbuf, ".txt");
  plogw->ostream->println(fnbuf);

  //  f = SPIFFS.open(settingsfn, FILE_WRITE);
  //  f = SPIFFS.open(fnbuf, FILE_WRITE);
  f = SD.open(fnbuf, FILE_WRITE);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file settings.txt for writing");
    return 0;
  }


  // before saving set variables
  // plogw->radios_enabled

  plogw->radios_enabled = radio_list[0].enabled | (radio_list[1].enabled << 1) | (radio_list[2].enabled << 2);

  if (!plogw->f_console_emu) plogw->ostream->println("dump_settings");
  int i;
  i = 0;
  while (settings_dict[i].value != NULL) {
    Serial.print(i);
    Serial.print(":");
    if (*settings_dict[i].name=='\0') break;
    Serial.print(settings_dict[i].name);
    Serial.println(" ");
    //    Serial.print(settings_dict[i].value);    
    if (!plogw->f_console_emu) {
      plogw->ostream->print(settings_dict[i].name);
      plogw->ostream->print(" ");
    }
    f.print(settings_dict[i].name);
    f.print(" ");
    switch (settings_dict[i].value_type) {
    case DICT_VALUE_TYPE_INT:
      if (!plogw->f_console_emu) plogw->ostream->println(*(int *)(settings_dict[i].value));
      f.println(*(int *)(settings_dict[i].value));
      break;
    case DICT_VALUE_TYPE_CHARARRAY:
      if (!plogw->f_console_emu) plogw->ostream->println((char *)(settings_dict[i].value));
      f.println((char *)(settings_dict[i].value));
      break;
    }
    i++;  // to the next value
  }


  //dump_settings(&f, settings_dict);

  // output options
  /* char cluster_name[LEN_HOST_NAME+3];
     char my_callsign[LEN_CALL_WINDOW + 3];
     char recv_exch[LEN_EXCH_WINDOW + 3];
     char jcc[LEN_EXCH_WINDOW + 3];
     char rig_name[LEN_RIG_NAME + 3];
     char cw_msg[N_CWMSG][LEN_CWMSG_WINDOW + 3];


     // contest type related
     int mask ; // dupe check mask PH/CW 0xff dupe in CW and PH 0xff-3 dupe in operating mode
     int contest_id; // contest id number

     char grid_locator[LEN_GL + 3]; // current location  grid locator  ... editable

     int  sat_freq_mode; // 0... rig freq indipendent of sat  1.. receive frequency fixed transmit freq control  2.. transmission freq fixed receive freq control
     char sat_name[LEN_SATNAME_WINDOW + 3];  //

     // rig type 0 IC-705 TX/TRX (RX kenwood USB connected?) 1 IC-9700 (main tx /sub rx)
     int rig_type;
  */
  //   f.print("my_callsign ");   f.println(plogw->my_callsign+2);
  //   f.print("recv_exch ");   f.println(plogw->recv_exch+2);

  f.close();
  if (!plogw->f_console_emu) plogw->ostream->println("saving finished.");
  return 1;
}

