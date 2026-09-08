/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
// Copyright (c) 2021-2024 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
//#include "SD.h"
//#include "display.h"
#include "callhist.h"
#include "dupechk.h"
#include "callhist_fd.h"
//#include "settings.h"
//#include "so2r.h"


// following is to read callhist_list from file 
int size_callhist_list=500;
// callhist_list is an array of character string and the last string is ""
char **callhist_list=NULL;
char *callhist_list_mem=NULL;
int n_callhist_list=0;
int release_callhist_list_contents()
{
  for (int i=0;i<n_callhist_list;i++) {
    if (callhist_list[i]!=NULL) {
      free(callhist_list[i]);
      callhist_list[i]=NULL;
    }
  }
  n_callhist_list=0;
  return 1;
}


int init_callhist_list()
{

  //  callhist_list=NULL;
  //  callhist_list_mem=NULL;
  n_callhist_list=0;
  // initialize with zero callhist_list
  // free if already allocated something
  if (callhist_list!=NULL) free(callhist_list);    
  if (callhist_list_mem!=NULL) free(callhist_list_mem);
  callhist_list_mem=(char *)malloc(sizeof(char)*1);
  callhist_list=(char **)malloc(sizeof(char *)*(1));
  callhist_list[0]=callhist_list_mem;
  *callhist_list_mem='\0';
  return 1;
  
  /*  if (callhist_list!=NULL) {
    release_callhist_list_contents();
  } else {
    callhist_list = (char **)malloc(sizeof(char **)*size_callhist_list);
    if (callhist_list!=NULL) {
      for (int i=0;i<size_callhist_list;i++) callhist_list[i]=NULL;
    } else {
      return 0;
    }
  }
  n_callhist_list=0;
  return 1;
  */
  
}

char callhist_call_ret[20];
char *callhist_call(const char *callsign)
{
  // return with callsign before / 
  char *s,*s1,*ret;
  int count;  count=0;
  ret=callhist_call_ret;
  *ret='\0';
  s1=ret;
  s=(char *)callsign;
  while (*s && (count<20)) {
    if (*s==' ') {
      *s1='\0';
      break;
    }
    *s1++=*s++;
    count++;
  }
  *s1='\0';
  return ret;
}

char callsign_body_ret[20];
char *callsign_body(const char *callsign)
{
  // return with callsign before / 
  char *s,*s1,*ret;
  int count;  count=0;
  ret=callsign_body_ret;
  *ret='\0';
  s1=ret;
  s=(char *)callsign;
  while (*s && (count<20)) {
    if (*s=='/') {
      *s1='\0';
      break;
    }
    *s1++=*s++;
    count++;
  }
  *s1='\0';
  return ret;
}

char *exch_callhist(const char *callsign)
{
  char *s;
  s=(char *)callsign;
  // search for ' '
  while (*s) {
    if (*s==' ') {
      s++;break;
    }
    s++;
  }
  while (*s) {
    if (*s==' ') s++;
    else break;
  }
  return s;
} 

int count_callhist(const char **callhist_list)
{
  int count;
  count=0;
  if (callhist_list==NULL) return count;
  while (1) {
    if (callhist_list[count]==NULL) break;
    printf("count %d callhist_list %s\n",count,callhist_list[count]);    
    if (*callhist_list[count]!='\0') {
      count++;
    } else {
      break;
    }
  }
  return count;
}

// search callhist_list and set to exch_history
int search_callhist_list_exch(const char **callhist_list,const char *callsign, int match_body,char **exch_history) {
  //  struct radio *radio;
  //  radio = so2r.radio_selected();
  *exch_history=NULL;
  
  const char *callsign1;
  if (match_body) {
    callsign1 = callsign_body(callsign);
  } else {
    callsign1=callsign;
  }
  int i=0;
  while (1) {
    if (*callhist_list[i]=='\0') return -1;
    
    if (dupe_callsign_equal(callhist_call(callhist_list[i]), callsign1)) {
      // exact DUPE/CALLHIST match after portable-suffix normalization
      *exch_history=exch_callhist(callhist_list[i]);
      return i;
    }
    // Normalized calls are not guaranteed to preserve the raw sort order,
    // so continue to the end instead of using the old strcmp() early exit.
    i++;
  }
}
