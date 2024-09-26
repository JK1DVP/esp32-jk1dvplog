#ifndef FILE_TIMEKEEP_H
#define FILE_TIMEKEEP_H
void timekeep() ;
void init_timekeep();
#include <NTPClient.h>
extern RTC_DS1307 rtcclock;

extern NTPClient timeClient;

#endif
