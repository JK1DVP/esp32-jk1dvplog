#ifndef FILE_CTY_CHK_H
#define FILE_CTY_CHK_H
#include "cty_chk.h"
#include "cty.h"
int get_entity_info(char *callsign,char *entity, char *entity_desc,
		    int *cqzone, int *ituzone, char *continent,
		    char *lat, char *lon, char *tz);
void show_entity_info(char *callsign) ;
#endif
