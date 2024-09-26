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
#include "SD.h"
#include "display.h"
#include "callhist.h"
#include "callhist_fd.h"


#ifdef notdef
// following is to read callhist_list from file 
int size_callhist_list=500;
// callhist_list is an array of character string and the last string is ""
char **callhist_list=NULL;
int n_callhist_list;

int release_callhist_list_contents()
{
  for (int i=0;i<n_callhist_list;i++) {
    if (callhist_list[i]!=NULL) {
      free(callhist_list[i]);
      callhist_list[i]=NULL;
    }
  }
  n_callhist_list=0;
}


int init_callhist_list()
{
  if (callhist_list!=NULL) {
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
  
}

char *last_item = "";

int read_callhist_list(char *fn)
{
  if (!init_callhist_list()) return 0;
  FILE *inf;
  inf=fopen (fn,"r");
  if (inf == NULL) {
    printf("failed to open %s for callhist_list\n",fn);
    return 0;
  }
  char buf[128];
  char **tmp;
  n_callhist_list=0;
  while (readline(inf,buf,0x0d0a,128)!=0) {
    printf("read :%s:\n",buf);
    callhist_list[n_callhist_list]=strdup(buf);
    n_callhist_list++;
    if(n_callhist_list>=size_callhist_list) {
      tmp=(char **)realloc(callhist_list,sizeof(char *)*(size_callhist_list*2));
      if (tmp != NULL) {
	callhist_list=tmp;
	size_callhist_list*=2;
      } else {
	// allocation fail
	break;
      }
    }
  };
  fclose(inf);
  callhist_list[n_callhist_list]=last_item;
  // last item add
  
  return n_callhist_list;
}

#endif

void print_callhist_list(const char **callhist_list,int n)
{
  for (int i=0;i<n;i++) {
    if (*callhist_list[i]=='\0') break; // end of the list by nul callsign
    printf("%d",i);
    printf(" ");
    printf(callhist_list[i]);    
  }
}
//const char *exch_callhist(const char *callsign_exch)
//{
//  char *s;
//  s=(char *)(callsign_exch);
//  while (*s) {
//    if (*s++==' ') return s;
//  }
//  return s;
//}

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
int search_callhist_list_exch(const char **callhist_list,char *callsign, int match_body,char **exch_history) {
  struct radio *radio;
  radio = radio_selected;
  *exch_history=NULL;
  
  char *callsign1;
  if (match_body) {
    callsign1 = callsign_body(callsign);
  } else {
    callsign1=callsign;
  }
  int i=0;
  int ret;
  while (1) {
    if (*callhist_list[i]=='\0') return -1;
    
    ret=strcmp(callhist_call(callhist_list[i]),callsign1);
    if (ret==0) {
      // match ! 
      *exch_history=exch_callhist(callhist_list[i]);
      return i;
    }
    if (ret>0) {  // no longer match 
      return -1;
    }
    i++;
  }
}

int search_callhist_list(const char **callhist_list,char *callsign, int match_body) {
  struct radio *radio;
  radio = radio_selected;
  char *callsign1;
  if (match_body) {
    callsign1 = callsign_body(callsign);
  } else {
    callsign1=callsign;
  }
  int i=0;
  int ret;
  while (1) {
    if (*callhist_list[i]=='\0') return -1;
    
    ret=strcmp(callhist_call(callhist_list[i]),callsign1);
    if (ret==0) {
      // match ! 

      // copy to current working
      strcpy(radio->recv_exch + 2, exch_callhist(callhist_list[i]));
      // and cursor to the last of the exchange string
      radio->recv_exch[1] = strlen(radio->recv_exch + 2);

      return i;
    }
    if (ret>0) {  // no longer match 
      return -1;
    }
    i++;
  }
}



File callhistf;          // call history file
int callhistf_stat = 0;  // 0 not open 1 open for reading 2 open for writing
int nmaxcallhist = 40;
int ncallhist;
struct callhist *callhist;

struct callhist callhist_work;  // stored call hist exchange here when matched

void copy_tail_character(char *dest, char *s) {
  *dest = '\0';
  strncat(dest, s + strlen(s) - 1, 1);
}


void init_callhist() {
  callhist = (struct callhist *)malloc(sizeof(struct callhist) * nmaxcallhist);
  ncallhist = 0;
  plogw->ostream->println("init_callhist()");
}

void close_callhistf()
{
  callhistf.close();
}
void close_callhist() {
  if (callhistf_stat == 2) return;
  if (callhistf_stat == 1) {
    callhistf.close();
    if (!plogw->f_console_emu) plogw->ostream->println("callhist file closed (opened read)");
    callhistf_stat = 0;
    ncallhist = 0;
  }
}

void open_callhist() {

  char buf[128], buf1[10];
  if (callhist == NULL) return;
  if (callhistf_stat == 2) {
    // currently opened for writing
    if (!plogw->f_console_emu) plogw->ostream->println("currently callhistf is opened for writing");
    return;
  }
  if (callhistf_stat == 0) {
    callhistf = SD.open(callhistfn, "r");
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("callhistf already open");
    return;  // already open
  }
  if (!callhistf) {
    //
    if (!plogw->f_console_emu) plogw->ostream->println("failed to open call history");
    sprintf(dp->lcdbuf, "%s\nfail open.\n", callhistfn);
    upd_display_info_flash(dp->lcdbuf);

    return;
  } else {
    if (!plogw->f_console_emu) plogw->ostream->println("callhistf opened");
    ncallhist = 0;
    callhistf_stat = 1;
    sprintf(dp->lcdbuf, "%s\ncallhist open.\n", callhistfn);
    upd_display_info_flash(dp->lcdbuf);
  }

  // read through the file and record pointer of the first callsign of each prefix. in call history structure
  int ret;
  char prefix[4];
  *prefix = '\0';  // initially no prefix read
  while (1) {
    int pos = callhistf.position();
    ret = callhistf.read(callhist_work.u.buffer, sizeof(callhist_work.u.buffer));
    if (ret != sizeof(callhist_work.u.buffer)) {
      if (!plogw->f_console_emu) plogw->ostream->println("not read callhist_entry");
      break;
    } else {
      //// check callsign for 3 characters with previous one
      // if (strncmp(callhist_work.u.entry.callsign, prefix, 3) != 0) {
      copy_tail_character(buf1, callhist_work.u.entry.callsign);
      if (strncmp(buf1, prefix, 1) != 0) {
        // new prefix, record this entry to callhist[ncallhist]
        *prefix = '\0';
        //	        strncat(prefix, callhist_work.u.entry.callsign, 3);
        copy_tail_character(prefix, callhist_work.u.entry.callsign);

        ncallhist++;

        memcpy(callhist[ncallhist - 1].u.buffer, callhist_work.u.buffer, sizeof(callhist_work.u.buffer));
        callhist[ncallhist - 1].pos = pos;
        callhist[ncallhist - 1].nstations = 1;

        if (!plogw->f_console_emu) {
          plogw->ostream->print("\nNew prefix:");
          plogw->ostream->print(prefix);
          plogw->ostream->print(", call=");
          plogw->ostream->print(callhist[ncallhist - 1].u.entry.callsign);
          plogw->ostream->print(",pos:");
          plogw->ostream->println(pos);
        }
        sprintf(dp->lcdbuf, "%s\n%s\n%s\npos=%d", callhistfn, prefix, callhist[ncallhist - 1].u.entry.callsign, pos);
        upd_display_info_flash(dp->lcdbuf);

        // next entry
        if (ncallhist == nmaxcallhist) {
          // reached allocated max limit
          if (!plogw->f_console_emu) plogw->ostream->println("reached max unmber of callhist");
          break;
        }
      } else {
        if (!plogw->f_console_emu) plogw->ostream->print("*");
        callhist[ncallhist - 1].nstations++;
      }
    }
  }
}


// search call history and fill in present op window (recv_exch)
int search_callhist (char *callsign) {
  struct radio *radio;
  radio = radio_selected;
  
  int ret;
  ret=search_callhist_getexch(callsign,radio->recv_exch + 2);
  // and cursor to the last of the exchange string
  if (ret==1) {
    radio->recv_exch[1] = strlen(radio->recv_exch + 2);
  }
  return ret;
}

// search callhist and if found obtain exchange to the getexch
int search_callhist_getexch (char *callsign,char *getexch) {
  int len;
  char prefix[4];
  len = strlen(callsign);
  if (callhist == NULL) return 0;
  int ret;
  int len_callhist = CALLHIST_CALLEXCH_SIZE;  // size of call history in the file

  unsigned long usec;
  char buf[10];
  usec = micros();
  if (len > 3) {
    if (!callhistf) return 0;  // not found
    *prefix = '\0';
    //    strncat(prefix, callsign, 3);
    copy_tail_character(prefix, callsign);

    for (int i = 0; i < ncallhist; i++) {
      //// use prefix
      //if (strncmp(prefix, callhist[i].u.entry.callsign, 3) == 0) {
      //// use tail character (to conserve memory)
      copy_tail_character(buf, callhist[i].u.entry.callsign);
      if (strncmp(prefix, buf, 1) == 0) {
        // seek to the position of  prefix
        if (callhist[i].nstations != 1) {
          if (!callhistf.seek(callhist[i].pos)) {
            if (!plogw->f_console_emu) plogw->ostream->println("file seek failed in search_callhist()");
            return 0;
          }
        }
        // search start from here
        while (1) {
          if (callhist[i].nstations == 1) {
            // just copy from
            memcpy(callhist_work.u.buffer, callhist[i].u.buffer, sizeof(callhist_work.u.buffer));
          } else {
            ret = callhistf.read(callhist_work.u.buffer, sizeof(callhist_work.u.buffer));
            if (ret != sizeof(callhist_work.u.buffer)) break;
          }
          // check callsign
          if (strcmp(callhist_work.u.entry.callsign, callsign) == 0) {
            // matched!
            if (!plogw->f_console_emu) {
              plogw->ostream->print(micros() - usec);
              plogw->ostream->print(" usec ");
              plogw->ostream->print("matched:");
              plogw->ostream->print(callsign);
              plogw->ostream->print(" exch:");
              plogw->ostream->println(callhist_work.u.entry.exch);
            }
            // copy to current working

            strcpy(getexch, callhist_work.u.entry.exch);	    


            return 1;  // found
          } else {
            if (callhist[i].nstations == 1) {
              if (!plogw->f_console_emu) {
                plogw->ostream->print(micros() - usec);
                plogw->ostream->println(" usec 3 ");
              }
              return 0;
            }
            //// check callsign for 3 characters with previous one
            //  if (strncmp(callhist_work.u.entry.callsign, prefix, 3) != 0) {
            copy_tail_character(buf, callhist_work.u.entry.callsign);
            if (strncmp(buf, prefix, 1) != 0) {
              // reached of the end of the prefix
              if (!plogw->f_console_emu) {
                plogw->ostream->print(micros() - usec);
                plogw->ostream->println(" usec 2 ");
              }
              return 0;  // not found
            }
          }
        }
      }
    }
  }
  if (!plogw->f_console_emu) {
    plogw->ostream->print(micros() - usec);
    plogw->ostream->println(" usec 1");
  }
  return 0;  // not found
}

void release_callhist() {
  free(callhist);
  callhist = NULL;
}
void set_callhistfn(char *fn) {
  if (*fn == '\0') {
    strcpy(callhistfn, "/ch");
  } else {
    strcpy(callhistfn, "/ch");
    strcat(callhistfn, fn);
  }
  strcat(callhistfn, ".txt");
  plogw->ostream->print("callhistfn=");
  plogw->ostream->println(callhistfn);
}

void write_callhist(char *s) {
  int ret;
  // append to file
  if (callhistf_stat == 1) {
    callhistf.close();
    plogw->ostream->println("callhist file closed (opened read)");
    callhistf_stat = 0;
  }
  if (callhistf_stat == 0) {
    // new open

    callhistf = SD.open(callhistfn, "w");
    if (!callhistf) {
      plogw->ostream->println("callhistf failed to open for writing");
    } else {
      plogw->ostream->println("callhistf opened for writing");
      callhistf_stat = 2;
    }
  }
  if (callhistf_stat == 2) {
    // write to the file
    char *s1;
    s1 = strtok(s, " ");
    if (s1 != NULL) {
      strcpy(callhist_work.u.entry.callsign, s1);
    } else {
      plogw->ostream->println("invalid callhist format? 1");
      return;
    }
    s1 = strtok(NULL, " ");  // s1 points to freq
    if (s1 == NULL) {
      plogw->ostream->println("invalid callhist format? 2");
      return;
    }
    strcpy(callhist_work.u.entry.exch, s1);
    //callhistf.println(s);
    ret = callhistf.write(callhist_work.u.buffer, sizeof(callhist_work.u.buffer));
    plogw->ostream->print("callhistf written ");
    plogw->ostream->print(ret);
    plogw->ostream->print("bytes:");
    plogw->ostream->print(callhist_work.u.entry.callsign);
    plogw->ostream->print(",");
    plogw->ostream->println(callhist_work.u.entry.exch);
  }
}

