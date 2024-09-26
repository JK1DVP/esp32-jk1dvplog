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

// zlink  24/6/28 E.Araki
#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "zserver.h"
#include "display.h"
#include "network.h"
#include "AsyncTCP.h"


char zserver_server[40] = "192.168.1.2";
int zserver_port = 23;
char zserver_buf[NCHR_ZSERVER_RINGBUF];
//WiFiClient zserver_client;

AsyncClient *zserver_client = new AsyncClient;

int f_show_zserver=1;
struct zserver zserver;
// idx                       0      1      2    3     4     5    6      7     8     9     10    11      12      13      14     15
const char *zserver_freqcodes[]={"1.9","3.5","7","10","14","18","21","24","28","50","144 ","430 ","1200","2400","5600","10G" };
int zserver_bandid_freqcodes_map[]={0,0,1,2,4,6,8,9,10,11,12,13,14,15};

//コード	モードa
const char *zserver_modecodes[]={"CW","SSB","FM","AM","RTTY","FT4","FT8","OTHER"};
//電力コード	
//コード	電力

const char *zserver_powcodes[]={"1W","2W","5W","10W","20W","25W","50W","100W","200W","500W","1000W" };

const char *zserver_client_commands[]={ "FREQ","QSOIDS","ENDQSOIDS","PROMPTUPDATE","NEWPX","PUTMESSAGE","!","POSTWANTED",
			"DELWANTED","SPOT","SPOT2","SPOT3","BSDATA","SENDSPOT","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			"CONNECTCLUSTER","PUTQSO","DELQSO","EXDELQSO","INSQSOAT","LOCKQSO","UNLOCKQSO","EDITQSOTO",
			"INSQSO","PUTLOGEX","PUTLOG","RENEW","SENDLOG" };


const char *zserver_server_commands[]={"GETQSOIDS","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			       "BSDATA","POSTWANTED","DELWANTED","CONNECTCLUSTER",
			       "GETLOGQSOID","SENDRENEW","DELQSO","EXDELQSO","RENEW",
			       "LOCKQSO","UNLOCKQSO","BAND","OPERATOR","FREQ","SPOT",
			       "SENDSPOT","PUTQSO","PUTLOG","EDITQSOTO","INSQSO",
			       "EDITQSOTO","SENDLOG" };

int opmode2zLogmode(char *opmode)
{
  if ((strcmp(opmode,"CW")==0)||(strcmp(opmode,"CW-R")==0)) return 0;
  if ((strcmp(opmode,"LSB")==0)||(strcmp(opmode,"USB")==0)) return 1;
  if ((strcmp(opmode,"FM")==0)) return 2;
  if ((strcmp(opmode,"AM")==0)) return 3;      
  if ((strcmp(opmode,"RTTY")==0)||(strcmp(opmode,"RTTY-R")==0)) return 4;
  // FT4:5 and FT8:6 not supported
  return 7;
}

int connect_zserver() {
  if (wifi_status == 1) {
    if (!zserver_client->connected()) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("connecting to zserver ");
	plogw->ostream->print(zserver_server);
	plogw->ostream->print(" port:");
	plogw->ostream->println(zserver_port);
      }
      zserver_client->connect(zserver_server, zserver_port);
      return 1;
    } else {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("connect_zserver() : zserver already connected -> ");
	zserver_client->stop();
	plogw->ostream->println("disconnected from zserver.");
      }
      return 0;
    }
  }
  return 0;
}

// received data  from zserver
void handleData_zserver(void *arg, AsyncClient *client, void *data, size_t len)
{
  if (verbose & 16) {
    plogw->ostream->print("Z:");
    plogw->ostream->write((uint8_t *)data, len);
  }
  if (zserver.stat == 4) {
    zserver.timeout_alive = millis() + 120000; // not used but timeout counter 
    char c;
    int ret;
    for (int i=0;i<len;i++) {
      c=(char)((uint8_t *)data)[i];
      
      write_ringbuf(&zserver.ringbuf, c);

      ret = readfrom_ringbuf(&zserver.ringbuf, zserver.cmdbuf + zserver.cmdbuf_ptr, (char)0x0d, (char)0x0a, zserver.cmdbuf_len - zserver.cmdbuf_ptr);
      if (ret < 0) {
	// one line read
	if (!plogw->f_console_emu) {    	
	  Serial.print("Z readline:");
	  Serial.println(zserver.cmdbuf);
	}
	// check commands
	if (strncmp(zserver.cmdbuf,"#ZLOG# PUTMESSAGE",17)==0) {
	  //	    sprintf(dp->lcdbuf,"ZserverMSG:\n%s",zserver.cmdbuf);
	  int pos=17;int len;
	  len=strlen(zserver.cmdbuf);
	  *dp->lcdbuf='\0';
	  strcat(dp->lcdbuf,"ZserverMSG:\n");
	  if (len>=21*5) len=21*5;
	  while (pos<len) {
	    strncat(dp->lcdbuf,zserver.cmdbuf+pos,21);
	    strcat(dp->lcdbuf,"\n");
	    pos+=21;
	  }
	  upd_display_info_flash(dp->lcdbuf);
	}
	  
	*zserver.cmdbuf = '\0';
	zserver.cmdbuf_ptr = 0;
      } else {
	if (ret > 0) {
	  zserver.cmdbuf_ptr += ret;
	  //plogw->ostream->println(ret);
	  if (zserver.cmdbuf_ptr == zserver.cmdbuf_len) {
	    // reached end of cmdbuf
	    if (f_show_zserver >= 0) {
	      if (!plogw->f_console_emu) {
		plogw->ostream->print("OVERFLOW CMDBUF:");
		plogw->ostream->println(zserver.cmdbuf);
	      }
	    }
	    zserver.cmdbuf_ptr = 0;
	  }
	}
      }
    }
  }
}

  
void onDisconnect_zserver(void *arg, AsyncClient *client)
{
  //  zserver.stat = 0;
  //  zserver.timeout = millis() + 2000;
  if (!plogw->f_console_emu) {
    plogw->ostream->println("onDisconnect_zserver():disconnected from zserver.");
  }
}

void onConnect_zserver(void *arg, AsyncClient *client)
{
  if (!plogw->f_console_emu) {
    plogw->ostream->print("connected to zserver ");
    plogw->ostream->print(zserver_server);
    plogw->ostream->print(" port:");
    plogw->ostream->println(zserver_port);
  }

  sprintf(dp->lcdbuf, "ZSERVER\nConnected\n%s\nPort %d", zserver_server, zserver_port);
  upd_display_info_flash(dp->lcdbuf);

  zserver.stat = 1;
  zserver.timeout = millis() + 2000;
  zserver.timeout_alive = millis() + 120000;
}


void init_zserver_info() {
  zserver.ringbuf.buf = zserver_buf;
  zserver.ringbuf.len = NCHR_ZSERVER_RINGBUF;
  zserver.ringbuf.wptr = 0;
  zserver.ringbuf.rptr = 0;
  zserver.timeout = 0;
  zserver.stat = 0;
  zserver.timeout_count=0;
  zserver.cmdbuf_ptr = 0;
  zserver.cmdbuf_len = NCHR_ZSERVER_CMD;
  memset(zserver.cmdbuf, '\0', NCHR_ZSERVER_CMD + 1);

  // define handler
  zserver_client->onData(handleData_zserver, zserver_client);
  zserver_client->onConnect(onConnect_zserver, zserver_client);
  zserver_client->onDisconnect(onDisconnect_zserver, zserver_client);  
  
}

void zserver_process() {
  //  plogw->ostream->println(zserver.stat);
  int ret;
  struct radio *radio;
  radio = radio_selected;

  switch (zserver.stat) {
  case 0:  // not logged in
    
    if (wifi_status == 0 ) {
      zserver.stat = 10;
      zserver.timeout = millis() + 1000;
      break;
    } else {
      if (!zserver_client->connected()) {
	if (zserver.timeout_count>=5) {
	  sprintf(dp->lcdbuf, "ZSERVER\nNo Try\n%s\nPort %d", zserver_server, zserver_port);
	  plogw->ostream->println(dp->lcdbuf);
	  upd_display_info_flash(dp->lcdbuf);

	  zserver.stat=11; // not try to connect to zserver unless instructed
	  zserver.timeout_count=0;
	} else {
	  connect_zserver(); // try connecting
	  if (!plogw->f_console_emu) {    
	    plogw->ostream->print("Zserver connection tried. count=");
	    plogw->ostream->println(zserver.timeout_count);
	  }
	  // retry after 10 seconds
	  zserver.stat = 10;
	  zserver.timeout_count++;
	  zserver.timeout = millis() + 10000;
	}
      } else {
	// already connected but status not updated !?
	if (!plogw->f_console_emu) {    	
	  plogw->ostream->println("zserver already connected but status not updated !?");
	}
	zserver.stat = 0;
      }
    }
    break;
  case 10:
    if (zserver.timeout < millis()) {
      zserver.stat = 0;  // try again
    }
    break;
  case 11:  // after forced disconnection, not try to connect
    break;
  case 1:  // connected and wait for a while to send band
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# BAND %d",zserver_bandid_freqcodes_map[radio->bandid]);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 2;
      zserver.timeout = millis() + 500;
    }
    break;
  case 2:// op name
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# OPERATOR %s",plogw->my_name+2);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 3;
      zserver.timeout = millis() + 500;
    }
    break;
  case 3://pc name
    if (zserver.timeout < millis()) {
      sprintf(zserver.cmdbuf,"#ZLOG# PCNAME %s",plogw->hostname+2);
      println_tcpserver(zserver_client,zserver.cmdbuf);
      if (!plogw->f_console_emu) {
	plogw->ostream->print(zserver.cmdbuf);
	plogw->ostream->println("... sent to zserver");
      }
      zserver.stat = 4;
      zserver.timeout = millis() + 500;
    }
    break;      
  case 4: //
    /// zserver is connected and initialized when 
    if (!zserver_client->connected()) {
      if (verbose & 16) plogw->ostream->println("disconnecting from zserver .");
      zserver_client->stop();
      zserver.stat = 0;
    } 
    break;
  case 5: // merging log with zserver
    // protocol:
    // 1. send BEGINMERGE and wait for BEGINMERGE-OK or BEGINMERGE-NG
    // 2. send GETQSOIDS and read QSOIDS until ENDQSOIDS received.
    // 3. check each QSOID and if local is newer EDITQSOTO to renew server's entry
    //    if QSO not in zserver PUTLOG
    //    if local is older, send GETLOGQSOID and receive PUTLOGEX to replace new QSO data.
    // 4. if received RENEW, renew and display the local database
    // 5. to finish GETLOGQSOID, send SENDRENEW to request zserver display
    // 6. to finish merging, send ENDMERGE to unlock zserver
    //
    // to download log from zserver
    // 1. send SENDLOG to request all log data to zserver
    // 2. receive PUTLOG for all QSO data
    // 3. if received RENEW renew our display.(end of download log from zserver)
    break;
    
  } 
}

void zserver_send(char *buf)
{
  if (zserver.stat==4) {
    // if stat is connected to zserver
    println_tcpserver(zserver_client,buf);
  } else {
    if (!plogw->f_console_emu) {    
      plogw->ostream->println("zserver not linked.");
    }
  }
}

void reconnect_zserver()
{
  // stop zserver if any
  zserver_client->stop();
  zserver.stat = 0;
  Serial.println("reconnect_zserver()");
  // set new server name from plogw->zserver_name
  if (strlen(plogw->zserver_name+2)!=0) {
    strcpy(zserver_server,plogw->zserver_name+2); // set new name
    sprintf(dp->lcdbuf,"set new zserver name:\n%s",zserver_server);
  } else {
    zserver.stat=11; // stop connecting to zserver
    sprintf(dp->lcdbuf,"stop conn to zserver");
  }
  upd_display_info_flash(dp->lcdbuf);      
  
}

void zserver_freq_notification()
{
  struct radio *radio;
  int zlog_ifreq;const char *zlog_freqcode,*zlog_mode;
  radio=radio_selected;
  
  zlog_ifreq=zserver_bandid_freqcodes_map[radio->bandid];
  zlog_freqcode=zserver_freqcodes[zlog_ifreq];
  zlog_mode=zserver_modecodes[opmode2zLogmode(radio->opmode)];
  
  sprintf(buf,"#ZLOG# FREQ %-3d%-5s%-11.1f%-5s%-3s%-10s%s",
	  zlog_ifreq, // band
	  zlog_freqcode, // bandstr
	  (radio->freq/1000.0)*FREQ_UNIT, // freq
	  zlog_mode, // modestr
	  (radio->cq[radio->modetype]==LOG_CQ) ? "CQ":"SP", // cq sp
	  plogw->tm+9, // timestr
	  plogw->hostname+2 //  pc name
	  );
  zserver_send(buf);
}
