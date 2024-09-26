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
#include "main.h"
#include "network.h"
#include "timekeep.h"
#include "cluster.h"
#include "display.h"
#include "tcp_server.h"
#include "zserver.h"
#include "AsyncTCP.h"
#include "SD.h"
#include "settings.h"

#include "ESP32FtpServer.h"
FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP32FtpServer.h to see ftp verbose on serial


#include <WiFi.h>  // for WiFi shield

#include <espwmap.h>
#include <ESPmDNS.h>


#include <WiFiUdp.h>
//#include <ESP_Mail_Client.h>

// network 関連の管理について
// setup() で1度、その後loop() で1秒ごとにwifi_enable == True であれば、wifi 接続状態を確認して接続できるようであれば接続する。-> wifi_status = True とする。
// wifi_count が累積5回程度NGなら、wifi_enable = False として、その後は、チェックしない。WIFIコマンドで接続しなおしをすることができる。

// network 利用のプロセスは、それぞれで、wifi をチェックするのでなく、wifi_status を確認してTrue の時だけ実行するようにする。

void init_multiwifi() {
  // read from sd for ssid and password if file is available 

  f=SD.open("/wifiset.txt","r");
  char *ssid,*pass;
  
  if (f) {
    // read from wifiset.txt to add all listed ssid
    while (readline(&f, buf, 0x0d0a, 128) != 0) {
      ssid = strtok(buf," ");
      if (ssid!= NULL) {
	pass= strtok(NULL," ");
	if (pass!= NULL) {
	  // read ssid and pass
	  plogw->ostream->print("setting wifi:");	  	  
	  plogw->ostream->print(ssid);
	  //	  plogw->ostream->print(" ");
	  //	  plogw->ostream->println(pass);	  
	  ESPWMAP.add(ssid,pass);
	}
      }
    }
    f.close();
  } else {
    plogw->ostream->println("fail opening wifi setting file /wifiset.txt");
  }
  
  ESPWMAP.begin();
}

void multiwifi_addap(char *ssid,char *passwd)
{
  // check
  if ((*ssid !='\0') && (*passwd != '\0')) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);    
    ESPWMAP.add(ssid,passwd);
    sprintf(dp->lcdbuf, "addAp:%s",ssid);

    // update wifiset.txt for the entries
    File f1;
    f  = SD.open("/wifiset.new","w");
    
    f1 = SD.open("/wifiset.txt","r");
    if (f1) {
      // read f1 (existing entries) and updat
      while (readline(&f1, buf, 0x0d0a, 128) != 0) {
	//	plogw->ostream->print("f1 read:");
	//	plogw->ostream->println(buf);
	
	if (strncmp(ssid,buf,strlen(ssid))==0) {
	  // this entry hits the current add ap entry
	  plogw->ostream->print(ssid);
	  plogw->ostream->println(" already exists");
	  // --> skip
	} else {
	  // copy the entry to f
	  f.println(buf);
	}
      }
      f1.close();
    }
    // append current entry
    f.print(ssid);
    f.print(" ");
    f.println(passwd);
    f.flush();
    f.close();
    
    // rename after removing old file
    if (SD.remove("/wifiset.txt")) {
      plogw->ostream->println("removed wifiset.txt");
    }
    
    if (SD.rename("/wifiset.new","/wifiset.txt") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("renaming /wifiset.new /wifiset.txt failed");
    } else {
      plogw->ostream->println("success renaming wifiset.new to wifiset.txt");
    }
      
    plogw->ostream->println("wifiset updated");
  } else {
    sprintf(dp->lcdbuf, "Please set\nSSID and Passwd");
  }
  upd_display_info_flash(dp->lcdbuf);  
}

void print_wifiinfo() {
  plogw->ostream->print(WiFi.localIP().toString());
  plogw->ostream->print(" ");
  plogw->ostream->print(WiFi.SSID());
  plogw->ostream->print(" ");
  plogw->ostream->println(WiFi.RSSI());
}

void localip_to_string(char *buf)
{
  WiFi.localIP().toString().toCharArray(buf, 16);
}

void init_network() {
  plogw->ostream->println("init_network()");
  sprintf(dp->lcdbuf, "init_network()\nPlease Wait");
  upd_display_info_flash(dp->lcdbuf);
  
  init_multiwifi() ;
  //  WiFi.begin(ssid, password);
  check_wifi();
  Serial.println("MDNS()");
  MDNS.begin(plogw->hostname+2); // ホスト名 
  timeClient.begin();
  plogw->ostream->println("timeclient started");
  init_cluster_info();
  plogw->ostream->println("inited cluster info");
  
  init_zserver_info();
  plogw->ostream->println("inited zserver info");
  
  for (int i = 0; i < MAX_SRV_CLIENTS; i++) serverClients_status[i] = 0;
  init_tcpserver();
  sprintf(dp->lcdbuf, "init_network()\nFinished");
  upd_display_info_flash(dp->lcdbuf);
  
  print_wifiinfo();
  plogw->ostream->println("tcp server start ");
  plogw->ostream->println("init_network() end");


  ftpSrv.begin("esp32","esp32");    //username, password for ftp.  set ports in ESP32FtpServer.h  (default 21, 50009 for PASV)
  plogw->ostream->println("ftp server start");
  
  
}

void ftp_service_loop()
{
  ftpSrv.handleFTP();        //make sure in loop you call handleFTP()!!   
  
}


int check_wifi() {
  //  Serial.println("check_wifi()");
  if (wifi_enable) {
    if (wifi_status == 0) {
      sprintf(dp->lcdbuf, "check_wifi()\nrun()");
      upd_display_info_flash(dp->lcdbuf);
      
      //      if (wifiMulti.run() == WL_CONNECTED) {
      if (ESPWMAP.handle() == WL_CONNECTED) {      
	sprintf(dp->lcdbuf, "check_wifi()\nConnected.");
	upd_display_info_flash(dp->lcdbuf);

	wifi_count = 0;
	wifi_status = 1;
	return 1;
      } else {
	sprintf(dp->lcdbuf, "check_wifi()\nnot found.cnt=%d",wifi_count);
	upd_display_info_flash(dp->lcdbuf);
	
	wifi_count++;
	if (wifi_count > 10) {
	  Serial.println("check wifi failed > 10... disabling wifi");
	  wifi_enable = 0; wifi_count = 0;
	  wifi_status = 0;
	  cluster.stat = 0;
	  sprintf(dp->lcdbuf, "wifi_enable=%d", wifi_enable);
	  upd_display_info_flash(dp->lcdbuf);
	}
	return 0;
      }
    } else {
      if (ESPWMAP.handle()== WL_CONNECTED) {
	wifi_count = 0;
	wifi_status = 1;
	return 1;
      } else {
	wifi_status = 0;
	return 0;
      }
    }
  } else {
    return 0;
  }
}


/// AsyncTCP comm related routines
int println_tcpserver(void *arg,const char *s)
{
  AsyncClient *client = reinterpret_cast<AsyncClient *>(arg);
  // send s 
  if (client->space() > strlen(s)+2 && client->canSend())    {
    client->add(s, strlen(s));
    client->add("\r\n", 2);    
    client->send();
    return 1;
  } else {
    return 0;
  }
}
