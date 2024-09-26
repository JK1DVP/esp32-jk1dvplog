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
#include "display.h"
#include "RTClib.h"
//#include <DS3231.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include "misc.h"

WiFiUDP ntpUDP;

// You can specify the time server pool and the offset (in seconds, can be
// changed later with setTimeOffset() ). Additionaly you can specify the
// update interval (in milliseconds, can be changed using setUpdateInterval() ).
NTPClient timeClient(ntpUDP, "ntp.nict.jp", 32400, 60000);

//DS3231 rtcclock;
RTC_DS1307 rtcclock;
DateTime myRTC;
//RTClib myRTC;

//static const uint8_t LED = 2;

void init_timekeep()
{
  rtcclock.begin();
}

void timekeep() {
  //  DateTime rtctime_bak;
  if (timeout_rtc <= millis()) {
    char datestr[30];
    //    rtctime_bak=rtctime;
    //    Serial.print("rtctime:");
    //    Serial.println(rtctime_bak.year());
    //    Serial.println(rtctime_bak.month());
    //    Serial.println(rtctime_bak.day());    
    //    rtctime = myRTC.now();
    rtctime = rtcclock.now();    
    //    if (rtctime.year()<2000||rtctime.year()>2100||rtctime.month()>12||rtctime.day()>31) {
    //      rtctime=rtctime_bak;
    //    }
      
    //getTLE();
    // ntp update
    uint32_t dt;
    //    if (WiFi.status() == WL_CONNECTED) {
    if (wifi_enable && (wifi_status == 1)) {
      timeClient.update();
      if (timeClient.isTimeSet()) {
        //plogw->ostream->println("timeClient is TimeSet");
        ntptime = DateTime(timeClient.getEpochTime());
        if (rtctime.unixtime() > ntptime.unixtime()) {
          dt = rtctime.unixtime() - ntptime.unixtime();
        } else {
          dt = ntptime.unixtime() - rtctime.unixtime();
        }
        if (dt >= 2) {
          if (!plogw->f_console_emu) {
            plogw->ostream->print("dt=");
            plogw->ostream->println(dt);
          }

          rtcadj_count++;
          if (!plogw->f_console_emu) {
            plogw->ostream->print("rtcadj_count=");
            plogw->ostream->println(rtcadj_count);
          }
          if (rtcadj_count >= 10) {
            // set rtc from ntptime
	    //            rtcclock.setEpoch(ntptime.unixtime(), 0);
	    rtcclock.adjust(ntptime);
            if (!plogw->f_console_emu) plogw->ostream->println("RTC reset by NTP");
	    //            rtctime = myRTC.now();
	    rtctime = rtcclock.now();	    
            rtcadj_count = 0;
          }
        } else {
          rtcadj_count = 0;
        }
      } else {
        if (!plogw->f_console_emu) plogw->ostream->println("time is not set");
      }
    }
    time_measure_start(14);
    sprintf(plogw->tm, "%02d/%02d/%02d-%02d:%02d:%02d", rtctime.year() % 100, rtctime.month(), rtctime.day(),
            rtctime.hour(), rtctime.minute(), rtctime.second());
    upd_display_tm();
    right_display_sendBuffer();
    if (f_show_clock == 2) {

      sprintf(datestr, "%04d/%02d/%02d-%02d:%02d:%02d",
              rtctime.year(), rtctime.month(), rtctime.day(),
              rtctime.hour(), rtctime.minute(), rtctime.second());

      if (!plogw->f_console_emu) {
        plogw->ostream->print("RTC:");
        //plogw->ostream->print(rtctime.unixtime()); plogw->ostream->print(" ");
        plogw->ostream->print(datestr);
      }
      if (timeClient.isTimeSet()) {
        if (!plogw->f_console_emu) {
          plogw->ostream->print(" ");
          sprintf(datestr, "%04d/%02d/%02d %02d:%02d:%02d",
                  ntptime.year(), ntptime.month(), ntptime.day(),
                  ntptime.hour(), ntptime.minute(), ntptime.second());
          plogw->ostream->print("NTP:");
          //plogw->ostream->print(ntptime.unixtime()); plogw->ostream->print(" ");
          plogw->ostream->println(datestr);
        }
      } else {
        if (!plogw->f_console_emu) plogw->ostream->println("");
      }
    }
    time_measure_stop(14);
    timeout_rtc = millis() + 1000;
    //    Serial.print(timeout_rtc);
  }
}


