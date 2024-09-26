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
/// SD memory card
#include "decl.h"
#include "variables.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
//#include "RTClib.h"
#include "timekeep.h"
//#include "SdFat.h"
#include "sd_files.h"
//SdFat32 SD;
 char timestamp[30];
//////////////////////////////////////////
////// SD file related definitions
///
// callhist structure is constructed on each initialization and updated also by qso data
//   exchange is assumed all same for the station
// dupe check structure is constructed on each initialization based on qsologf and run on memory
// bandmap file is constructed for each band (bandmap_bandname.txt)
//  , which contains frequency, callsign, and time  listing also fixed length entry
//

SPIClass SPI2;
enum { sd_miso = 12,
       sd_mosi = 13,
       sd_sck = 14,
       sd_ss = 26
};
// sd card is attached to SPI2(hspi)

void testFileIO(fs::FS &fs, const char *path) {
  File file = fs.open(path);
  static uint8_t buf[512];
  size_t len = 0;
  uint32_t start = millis();
  uint32_t end = start;
  if (file) {
    len = file.size();
    size_t flen = len;
    start = millis();
    while (len) {
      size_t toRead = len;
      if (toRead > 512) {
        toRead = 512;
      }
      file.read(buf, toRead);
      len -= toRead;
    }
    end = millis() - start;
    if (!plogw->f_console_emu) plogw->ostream->printf("%u bytes read for %u ms\n", flen, end);
    file.close();
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file for reading");
  }


  file = fs.open(path, FILE_WRITE);
  if (!file) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file for writing");
    return;
  }

  size_t i;
  start = millis();
  for (i = 0; i < 2048; i++) {
    file.write(buf, 512);
  }
  end = millis() - start;
  if (!plogw->f_console_emu) plogw->ostream->printf("%u bytes written for %u ms\n", 2048 * 512, end);
  file.close();
}

#ifdef notdef
// call back for file timestamps
void dateTime(uint16_t* date, uint16_t* time) {
 DateTime now = rtcclock.now();
 sprintf(timestamp, "%02d:%02d:%02d %2d/%2d/%2d \n", now.hour(),now.minute(),now.second(),now.month(),now.day(),now.year()-2000);
 Serial.println("yy");
 Serial.println(timestamp);
 // return date using FAT_DATE macro to format fields
 *date = FAT_DATE(now.year(), now.month(), now.day());

 // return time using FAT_TIME macro to format fields
 *time = FAT_TIME(now.hour(), now.minute(), now.second());
}
#endif

void init_sd() {

  // set date time callback function
  //  SdFile::dateTimeCallback(dateTime);
 
  SPI2.begin(sd_sck, sd_miso, sd_mosi, sd_ss);
  pinMode(sd_ss, OUTPUT);
  if (!SD.begin(sd_ss, SPI2, 12000000)) {
    plogw->ostream->println("Card Mount Failed");
    return;
  }
  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    plogw->ostream->println("No SD card attached");
    return;
  }

  plogw->ostream->print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    plogw->ostream->println("MMC");
  } else if (cardType == CARD_SD) {
    plogw->ostream->println("SDSC");
  } else if (cardType == CARD_SDHC) {
    plogw->ostream->println("SDHC");
  } else {
    plogw->ostream->println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  plogw->ostream->printf("SD Card Size: %lluMB\n", cardSize);
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  plogw->ostream->printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    plogw->ostream->println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    plogw->ostream->println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      plogw->ostream->print("  DIR : ");
      plogw->ostream->println(file.name());
      if (levels) {
        listDir(fs, file.path(), levels - 1);
      }
    } else {
      plogw->ostream->print("  FILE: ");
      plogw->ostream->print(file.name());
      plogw->ostream->print("  SIZE: ");
      plogw->ostream->println(file.size());
    }
    file = root.openNextFile();
  }
}


