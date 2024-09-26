#ifndef FILE_SD_FILES_H
#define FILE_SD_FILES_H
//#include "SdFat.h"
//extern SdFat32 SD;

void init_sd();
void listDir(fs::FS &fs, const char *dirname, uint8_t levels) ;

#endif
