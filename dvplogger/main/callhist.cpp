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
#include "SD.h"
#include "display.h"
#include "callhist.h"
#include "dupechk.h"
#include "callhist_fd.h"
#include "callhist_mem.h"
#include "settings.h"
#include "so2r.h"

// to be able to construct callhist_list from file data,
// callhist_list is an array of malloced pointer array points to a big single concatenated array "callsign exch\0"
// two pass process to first investigate the file content and decide number of entry and size of memory then read contents allocating memory by malloc (psram?)



//char *last_item = "";

static size_t callhist_memory_bytes = 0;
static bool callhist_loaded = false;
static bool callhist_in_psram = false;

int read_callhist_list(char *fn)
{
  //  if (!init_callhist_list()) return 0;
  //  FILE *inf;
  
  File inf;
  inf=SD.open (fn,FILE_READ);
  if (!inf) {
    printf("failed to open %s for callhist_list\n",fn);
    return 0;
  }
  char buf[128];
  char *ptmp,*pcallhist_list;
  int memsize=0;
  n_callhist_list=0;
  console->print("counting callhist file ");
  console->print(fn);
  while (readline(&inf,buf,0x0d0a,128)!=0) {
    //    console->printf("read :%s:\n",buf);
    //    callhist_list[n_callhist_list]=strdup(buf);
    // check contents (do this later)
    n_callhist_list++;
    memsize+=strlen(buf)+1;
  }
  inf.close();
  console->print("n_callhist_list ");  console->print(n_callhist_list);
  console->print(" memsize ");  console->println (memsize);  
  // allcate memory


  //#ifndef PSRAM_EXISTS
  if (!f_spiram) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)-(sizeof(char)*memsize+10+sizeof(char*)*(n_callhist_list+3)) < 60000 ) {
      console->printf("not enough heap memory %ud\n",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      sprintf(dp->lcdbuf, "no enough mem\n%ud\n",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      upd_display_info_flash(dp->lcdbuf);
      n_callhist_list=0;
      return n_callhist_list;    
    }

  //#endif
  }
    
  if (callhist_list!=NULL) free(callhist_list);    
  if (callhist_list_mem!=NULL) free(callhist_list_mem);
  
  //#ifdef PSRAM_EXISTS
  if (f_spiram) {
    callhist_list_mem = (char *)heap_caps_malloc(sizeof(char) * memsize+10, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    callhist_list=(char **)heap_caps_malloc(sizeof(char *)*(n_callhist_list+3),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    //#else
  } else {
    callhist_list_mem=(char *)malloc(sizeof(char)*memsize+10);
    callhist_list=(char **)malloc(sizeof(char *)*(n_callhist_list+3));
    //#endif
  }

  if (callhist_list_mem == NULL || callhist_list == NULL) {
    console->println("failed to allocate Call History memory");
    if (callhist_list_mem != NULL) free(callhist_list_mem);
    if (callhist_list != NULL) free(callhist_list);
    callhist_list_mem = NULL;
    callhist_list = NULL;
    n_callhist_list = 0;
    callhist_memory_bytes = 0;
    callhist_loaded = false;
    callhist_in_psram = false;
    return 0;
  }

  callhist_memory_bytes = (size_t)memsize + 10 +
                          sizeof(char *) * (n_callhist_list + 3);
  callhist_in_psram = f_spiram != 0;
  // reopen and construct callhist_list and callhist_list_mem
  inf=SD.open (fn,FILE_READ);
  if (!inf) {
    printf("failed to open %s for callhist_list\n",fn);
    free(callhist_list_mem);
    free(callhist_list);
    callhist_list_mem = NULL;
    callhist_list = NULL;
    n_callhist_list = 0;
    callhist_memory_bytes = 0;
    callhist_loaded = false;
    callhist_in_psram = false;
    return 0;
  }
  ptmp=callhist_list_mem;
  //  pcallhist_list=callhist_list[0];
  int n=0;
  while (readline(&inf,buf,0x0d0a,128)!=0) {
    //    console->printf("readA :%s:",buf);    console->print((int) ptmp);console->print(" n ");console->print(n);
    callhist_list[n]=ptmp;
    strcpy(ptmp,buf);
    ptmp+=strlen(buf)+1;
    //    console->printf(" stored, callhist_list[]:%s:\n",callhist_list[n]);
    n++;
  }
  inf.close();
  //  *callhist_list++=ptmp;
  callhist_list[n]=ptmp;
  strcpy(ptmp,""); // terminate
  callhist_loaded = true;

  return n_callhist_list;
}


void print_callhist_list(const char **callhist_list,int n)
{
  for (int i=0;i<n;i++) {
    if (*callhist_list[i]=='\0') break; // end of the list by nul callsign
    console->printf("%d",i);
    console->print(" ");
    console->println(callhist_list[i]);    
  }
}
#ifdef notdef
int search_callhist_list(const char **callhist_list,const char *callsign, int match_body) {
  struct radio *radio;
  radio = so2r.radio_selected();
  const char *callsign1;
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
#endif
///// following is OLD callhist system which consults SD memory file each time look up


File callhistf;          // call history file
int callhistf_stat = 0;  // 0 not open 1 open for reading 2 open for writing
int nmaxcallhist = 40;
int ncallhist;
struct callhist *callhist=NULL;

struct callhist callhist_work;  // stored call hist exchange here when matched

void copy_tail_character(char *dest, char *s) {
  *dest = '\0';
  strncat(dest, s + strlen(s) - 1, 1);
}


void init_callhist() {
  // When Call History lives on the SUBCPU, the MAIN-side table is unused.
  // Avoid allocating it in both LOWMEM and normal modes.
  if (callhist_at == 1) {
    if (callhist != NULL) {
      free(callhist);
      callhist = NULL;
    }
    ncallhist = 0;
    if (f_low_memory_mode) {
      plogw->ostream->println("LOWMEM: MAIN Call History buffer disabled");
    } else {
      plogw->ostream->println("Call History on SUBCPU: MAIN buffer disabled");
    }
    return;
  }
  //  callhist = (struct callhist *)malloc(sizeof(struct callhist) * nmaxcallhist);
  if (callhist!=NULL) {
    free(callhist);
    printf("freed callhist\r\n");
  }
  //  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)>sizeof(struct callhist) * nmaxcallhist ) {
  //#ifdef PSRAM_EXISTS
  if (f_spiram) {
    callhist = (struct callhist *)heap_caps_malloc(sizeof(struct callhist) * nmaxcallhist, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); 
    printf("callhist allocated by heap_caps_malloc() SPIRAM\r\n");
    //#else //  } else {
  } else { 
    callhist = (struct callhist *)malloc(sizeof(struct callhist) * nmaxcallhist);
    printf("callhist allocated by malloc()\r\n");     
    //#endif //  }
  }
  
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
  radio = so2r.radio_selected();
  
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
  if (callsign == NULL || getexch == NULL || callhist == NULL) return 0;
  if (strlen(callsign) <= 3 || !callhistf) return 0;

  unsigned long usec = micros();
  char base[LEN_CALLSIGN + 1];
  normalize_dupe_callsign(callsign, base, sizeof(base));
  const char base_tail = base[0] ? base[strlen(base) - 1] : '\0';

  // Call History is grouped by the last character to conserve RAM.  A base
  // call can match a stored portable form whose last character is a digit,
  // P (/P,/QRP) or M (/M,/MM,/AM), so scan those candidate groups too.
  for (int i = 0; i < ncallhist; i++) {
    char tailbuf[10];
    copy_tail_character(tailbuf, callhist[i].u.entry.callsign);
    const char tail = tailbuf[0];
    const bool portable_tail = (tail >= '0' && tail <= '9') ||
                               tail == 'P' || tail == 'p' ||
                               tail == 'M' || tail == 'm';
    if (tail != base_tail && !portable_tail) continue;

    if (callhist[i].nstations != 1) {
      if (!callhistf.seek(callhist[i].pos)) {
        if (!plogw->f_console_emu)
          plogw->ostream->println("file seek failed in search_callhist()");
        continue;
      }
    }

    for (int n = 0; n < callhist[i].nstations; n++) {
      if (callhist[i].nstations == 1) {
        memcpy(callhist_work.u.buffer, callhist[i].u.buffer,
               sizeof(callhist_work.u.buffer));
      } else {
        int ret = callhistf.read(callhist_work.u.buffer,
                                 sizeof(callhist_work.u.buffer));
        if (ret != sizeof(callhist_work.u.buffer)) break;
      }

      if (!dupe_callsign_equal(callhist_work.u.entry.callsign, callsign))
        continue;

      if (!plogw->f_console_emu) {
        plogw->ostream->print(micros() - usec);
        plogw->ostream->print(" usec matched:");
        plogw->ostream->print(callsign);
        plogw->ostream->print(" exch:");
        plogw->ostream->println(callhist_work.u.entry.exch);
      }
      strcpy(getexch, callhist_work.u.entry.exch);
      return 1;
    }
  }

  if (!plogw->f_console_emu) {
    plogw->ostream->print(micros() - usec);
    plogw->ostream->println(" usec not found");
  }
  return 0;
}

void release_callhist() {
  free(callhist);
  callhist = NULL;
}
void set_callhistfn(char *fn) {
  if (fn == NULL || *fn == '\0') return;

  // CALLHIST/ selects the default Call History file.
  if (strcmp(fn, "/") == 0) {
    strcpy(callhistfn, "/CALLHIST.TXT");
  } else {
    const char *name = fn;
    if (*name == '/') name++;

    size_t len = strlen(name);
    bool has_pck = len >= 4 && strcasecmp(name + len - 4, ".pck") == 0;
    snprintf(callhistfn, sizeof(callhistfn), has_pck ? "/%s" : "/%s.PCK", name);

    // FAT filenames are case-insensitive; keep the displayed/stored 8.3 name
    // consistently upper-case to avoid confusing mixed-case names.
    for (char *p = callhistfn; *p != '\0'; ++p) {
      if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
    }
  }

  plogw->ostream->print("callhistfn=");
  plogw->ostream->println(callhistfn);
}

void show_callhist_status() {
  const char *status;
  if (!plogw->enable_callhist) {
    status = "Disabled";
  } else if (!callhist_loaded) {
    status = "Not loaded";
  } else {
    status = "Enabled";
  }

  snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
           "Call History\n"
           "File %s\n"
           "%d entries %uKB\n"
           "%s %s",
           callhistfn, n_callhist_list,
           (unsigned int)((callhist_memory_bytes + 1023) / 1024),
           callhist_in_psram ? "PSRAM" : "Internal", status);
  upd_display_info_flash(dp->lcdbuf);

  plogw->ostream->println("Call History Status");
  plogw->ostream->print("File      : ");
  plogw->ostream->println(callhistfn);
  plogw->ostream->print("Entries   : ");
  plogw->ostream->println(n_callhist_list);
  plogw->ostream->print("Memory    : ");
  plogw->ostream->print((unsigned int)callhist_memory_bytes);
  plogw->ostream->println(" bytes");
  plogw->ostream->print("RAM       : ");
  plogw->ostream->println(callhist_in_psram ? "PSRAM" : "Internal");
  plogw->ostream->print("Status    : ");
  plogw->ostream->println(status);
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

