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

// search cty, zones, continent for given callsign E. Araki
// 24/8/30
#include <Arduino.h>
#include "cty.h"
#include "decl.h"
#include "display.h"
#include "variables.h"
#include "stdio.h"

// binary search
// Binary Search in C

//#include <stdio.h>

int binarySearch_prefix(const struct prefix_list *prefix_list, char *callsign, int low, int high) {
  // Repeat until the pointers low and high meet each other
  const char *p;
  int cmp;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    p=prefix_list[mid].prefix;
    cmp=strcmp(callsign,p);
    //    Serial.print(low);
    //    Serial.print("-");
    //    Serial.print(high);
    //    Serial.print("-mid-");
    //    Serial.print("cmp");
    //    Serial.println(p);
    if (cmp==0)  {
      //      Serial.println("match");
      return mid;
    }
    if (cmp > 0)
      low = mid + 1;
    else
      high = mid - 1;
  }
  //  Serial.print("not match");
  return -1;
}

int sequential_partial_Search_prefix(const struct prefix_list *prefix_list, char *callsign, int low, int high) {
  const char *p;
  int cmp;
  for (int i=low;i<=high;i++) {
    p=prefix_list[i].prefix;
    cmp=strncmp(callsign,p,strlen(p));
    //    Serial.print(i);
    //    Serial.print(":cmp:");
    //    Serial.println(p);
    if (cmp==0) {
      //      Serial.println("match");      
      return i;
    }
  }
  //  Serial.print("not match");  
  return -1;
}
    
  

int get_prefix_index(char *callsign)
{
  int idx_char;
  if (*callsign>='0' && *callsign<='9') {
    idx_char = *callsign - '0';
  } else {
    if (*callsign>='A' && *callsign <= 'Z') {
      idx_char = *callsign - 'A'+10;
    } else {
      idx_char = -1;
    }
  }
  return idx_char;
}

const struct prefix_list *search_cty(char *callsign)
{
  // firstly search exact match
  int idx;

  idx=get_prefix_index(callsign);
  if (idx==-1) {
    //    Serial.println("faulty callsign");
    return NULL; // faulty callsign
  }
  int idx1,idx2;
  idx1=prefix_exact_match_list_idx[idx][0];
  idx2=prefix_exact_match_list_idx[idx][1];
  int ret;  
  if (idx1!=-1) {
    // need to check exact match prefix list
    //    Serial.println("exact check callsign");        
    ret=binarySearch_prefix(prefix_exact_match_list, callsign, idx1, idx2);

    if (ret!=-1) { // found match
      return prefix_exact_match_list+ret;
    }
  }
  // yet not found so 
  idx1=prefix_fwd_match_list_idx[idx][0];
  idx2=prefix_fwd_match_list_idx[idx][1];
  if (idx1!=-1) {
    // need to check fwd match prefix list
    //    Serial.println("fwd match check callsign");            
    ret=sequential_partial_Search_prefix(prefix_fwd_match_list, callsign, idx1, idx2);
    if (ret!=-1) { // found match
      return prefix_fwd_match_list+ret;
    }
  } else {
    return NULL; // not found in fwd match list 
  }
  return NULL;
}

int get_entity_info(char *callsign,char *entity, char *entity_desc,
		    char *cqzone, char *ituzone, char *continent,
		    char *lat, char *lon, char *tz)
{
  //int idx;
  const struct prefix_list *prefix_list_searched;
  prefix_list_searched=search_cty(callsign);
  if (prefix_list_searched==NULL) return 0; // not found entity
  // copy results to entity ...
  int entity_idx;
  entity_idx=prefix_list_searched->entity_idx;
  strcpy(entity,entity_infos[entity_idx].entity);
  strcpy(entity_desc,entity_infos[entity_idx].entity_desc);
  strcpy(cqzone,entity_infos[entity_idx].cqzone);
  strcpy(ituzone,entity_infos[entity_idx].ituzone);
  strcpy(continent,entity_infos[entity_idx].continent);
  strcpy(lat,entity_infos[entity_idx].lat);
  strcpy(lon,entity_infos[entity_idx].lon);
  strcpy(tz,entity_infos[entity_idx].tz);
  const char *ovrride_info;
  if (prefix_list_searched->ovrride_info_idx!=-1) {
    ovrride_info=cty_ovrride_info[prefix_list_searched->ovrride_info_idx];
    //    char bufp[100];
    //    sprintf(bufp,"ovrride_info: cq %s itu %s cont %s lat %s lon %s tz %s : info %s->\n",
    //	    cqzone,ituzone,continent,lat,lon,tz,ovrride_info);
    //    Serial.print(bufp);
    
    // modify information according to the ovrride_info
    //    Serial.print("mod entity info:");
    //    Serial.println(ovrride_info);
    //(#)はOverride CQ Zone
    //[#]はOverride ITU Zone
    //{#}はOverride Continent
    //<#/#>はOverride latitude/longitude
    //~はOverride UTCOffset
    char *p;
    char buf[20];char *pbuf;
    pbuf=buf;
    *buf='\0';
    int stat;
    stat=0;
    p=(char *) ovrride_info;
    while (*p) {
      switch (*p) {
      case '(': stat=1; break; // cq zone
      case ')': stat=0; *pbuf='\0';strcpy(cqzone,buf);pbuf=buf;break;
      case '[': stat=2; break; // itu zone
      case ']': stat=0; *pbuf='\0';strcpy(ituzone,buf);pbuf=buf;break;
      case '{': stat=3; break; // continent
      case '}': stat=0; *pbuf='\0';strcpy(continent,buf);pbuf=buf;break;
      case '<': stat=4; break; // lat
      case '/': stat=5; *pbuf='\0';strcpy(lat,buf);pbuf=buf;break; // lon
      case '>': stat=0; *pbuf='\0';strcpy(lon,buf);pbuf=buf;break;
      case '~': if (stat==6) {
	  stat=0;*pbuf='\0';strcpy(tz,buf);pbuf=buf;break; // tz
	} else {
	  stat=6;
	}
	break;
      default: // append char
	*pbuf++=*p;
	break;
      }
      p++;
    }
    //    sprintf(bufp,"      : cq %s itu %s cont %s lat %s lon %s tz %s \n",
    //	    cqzone,ituzone,continent,lat,lon,tz);
    //    Serial.print(bufp);
    
  }
  return 1;
}

void show_entity_info(char *callsign) {
  char entity[10];
  char entity_desc[40];
  char cqzone[4];
  char ituzone[4];
  char continent[4];
  char lat[10];
  char lon[10];
  char tz[10];

  int ret;
  ret=get_entity_info(callsign  ,entity, entity_desc,
		    cqzone, ituzone, continent,
		      lat, lon, tz);
  
  if (ret) {
    // print to lcd
    if (lat[0]=='-') {
      lat[0]=' ';strcat(lat,"S");
    } else {
      strcat(lat,"N");
    }
    if (lon[0]=='-') {
      lon[0]=' ';strcat(lon,"E");
    } else {
      strcat(lon,"W");
    }
    sprintf(dp->lcdbuf,"%-8s %s\n%s\nCQ:%2s ITU:%2s %2s\n%s,%s\nTZ=%s\n",callsign,entity,entity_desc,cqzone,ituzone,continent,lat,lon,tz);

  } else {
    sprintf(dp->lcdbuf,"%s\nEntity Not found\n",callsign);
  }
  upd_display_info_flash(dp->lcdbuf);
}
