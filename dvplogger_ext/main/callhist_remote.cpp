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
#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "settings.h"
#include "callhist_remote.h"
#include "dupechk.h"
#include "mux_transport.h"
#ifdef DVPLOGGER_EXT
struct remote_callhist_entry { char call[LEN_CALLSIGN+1]; char exch[LEN_EXCH+1]; };
static remote_callhist_entry *rch = NULL;
static int rch_capacity = 0, rch_count = 0;
static int rch_last_seq = 0;
static size_t rch_bytes = 0;

static void send_callhist_ack(int seq, bool ok) {
  char b[64];
  snprintf(b, sizeof(b), "chack:%d|%d|%d", seq, rch_count, ok ? 1 : 0);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
}

void process_callhist_reset_subcpu(const char *s) {
  int n = atoi(s);
  free(rch);
  rch = NULL;
  rch_count = 0;
  rch_capacity = 0;
  rch_last_seq = 0;
  rch_bytes = 0;
  if (n > 0 && n <= 5000) {
    rch = (remote_callhist_entry *)calloc(n, sizeof(remote_callhist_entry));
    if (rch) rch_capacity = n;
  }
}

void process_callhist_entry_subcpu(char *s) {
  char *p = strchr(s, '|');
  if (!p) return;
  *p++ = '\0';
  int seq = atoi(s);

  /* ACK may have been lost.  Re-ACK the most recently accepted entry
     without inserting it twice. */
  if (seq == rch_last_seq) {
    send_callhist_ack(seq, true);
    return;
  }

  /* Stop at a missing/out-of-order entry.  Main CPU will retry it. */
  if (seq != rch_last_seq + 1) {
    send_callhist_ack(seq, false);
    return;
  }

  char *exch = strchr(p, '|');
  if (!exch) {
    send_callhist_ack(seq, false);
    return;
  }
  *exch++ = '\0';

  if (!rch || rch_count >= rch_capacity ||
      strlen(p) > LEN_CALLSIGN || strlen(exch) > LEN_EXCH) {
    send_callhist_ack(seq, false);
    return;
  }

  strncpy(rch[rch_count].call, p, LEN_CALLSIGN);
  rch[rch_count].call[LEN_CALLSIGN] = '\0';
  strncpy(rch[rch_count].exch, exch, LEN_EXCH);
  rch[rch_count].exch[LEN_EXCH] = '\0';
  rch_bytes += strlen(p) + strlen(exch) + 2;
  rch_count++;
  rch_last_seq = seq;
  send_callhist_ack(seq, true);
}

void process_callhist_end_subcpu() {
  char b[64];
  snprintf(b, sizeof(b), "chdone:%d|%u", rch_count, (unsigned)rch_bytes);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
}

bool search_callhist_subcpu_local(const char *call,char *exch,size_t n) {
  for(int i=0;i<rch_count;i++) if(dupe_callsign_equal(rch[i].call,call)) {
    if(n){strncpy(exch,rch[i].exch,n-1);exch[n-1]='\0';} return true;
  }
  return false;
}
int append_callhist_partial_subcpu(const char *call, struct check_entry_list *list,int maxe) {
  int added=0;
  for(int i=0;i<rch_count && list->nentry<maxe;i++) {
    if(!strstr(rch[i].call,call)) continue;
    bool dup=false; for(int j=0;j<list->nentry;j++) if(!strcmp(list->entryl[j].callsign,rch[i].call)){dup=true;break;}
    if(dup) continue;
    check_entry *e=&list->entryl[list->nentry++]; memset(e,0,sizeof(*e));
    strncpy(e->callsign,rch[i].call,sizeof(e->callsign)-1);
    strncpy(e->exch,rch[i].exch,sizeof(e->exch)-1);
    e->flag=CHECK_ENTRY_FLAG_CALLHIST_LIST;
    if(strlen(call)==strlen(rch[i].call)) e->flag|=CHECK_ENTRY_FLAG_EXACT_MATCH;
    added++;
  }
  return added;
}
int get_callhist_subcpu_count(){return rch_count;} size_t get_callhist_subcpu_bytes(){return rch_bytes;}
bool get_callhist_subcpu_entry(int i,const char **c,const char **e){if(i<0||i>=rch_count)return false;*c=rch[i].call;*e=rch[i].exch;return true;}
#else
#include "SD.h"
static volatile bool ch_done = false;
static int ch_count = 0;
static size_t ch_bytes = 0;
static volatile bool ch_ack_received = false;
static int ch_ack_seq = 0;
static int ch_ack_count = 0;
static int ch_ack_ok = 0;

void process_callhist_control_response_main(const char *b) {
  if (!strncmp(b, "chdone:", 7)) {
    unsigned n = 0, sz = 0;
    if (sscanf(b + 7, "%u|%u", &n, &sz) == 2) {
      ch_count = n;
      ch_bytes = sz;
      ch_done = true;
    }
    return;
  }

  if (!strncmp(b, "chack:", 6)) {
    int seq = 0, count = 0, ok = 0;
    if (sscanf(b + 6, "%d|%d|%d", &seq, &count, &ok) == 3) {
      ch_ack_seq = seq;
      ch_ack_count = count;
      ch_ack_ok = ok;
      ch_ack_received = true;
    }
  }
}

static bool send_callhist_entry_with_ack(int seq, const char *packet) {
  const int max_retries = 3;
  const uint32_t ack_timeout_ms = 250;

  for (int attempt = 0; attempt < max_retries; attempt++) {
    ch_ack_received = false;
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                           (unsigned char *)packet, strlen(packet));

    uint32_t deadline = millis() + ack_timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
      if (f_mux_transport) mux_transport.recv_pkt();
      if (ch_ack_received) {
        if (ch_ack_seq == seq) {
          return ch_ack_ok != 0 && ch_ack_count == seq;
        }
        /* Ignore a stale ACK and continue waiting for this sequence. */
        ch_ack_received = false;
      }
      delay(1);
    }
  }

  return false;
}

bool load_callhist_subcpu(const char *fn) {
  File f = SD.open(fn, FILE_READ);
  if (!f) {
    console->printf("callhist: cannot open %s\n", fn);
    return false;
  }

  char line[128];
  int count = 0;
  while (readline(&f, line, 0x0d0a, sizeof(line)) != 0)
    if (line[0]) count++;
  f.close();

  char b[160];
  snprintf(b, sizeof(b), "chreset%d", count);
  ch_done = false;
  ch_ack_received = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)b, strlen(b));
  delay(10);

  f = SD.open(fn, FILE_READ);
  if (!f) return false;

  int sent = 0;
  while (readline(&f, line, 0x0d0a, sizeof(line)) != 0) {
    char *p = line;
    while (*p == ' ') p++;
    char *sp = strchr(p, ' ');
    if (!sp) continue;
    *sp++ = '\0';
    while (*sp == ' ') sp++;
    if (!*p || !*sp) continue;

    int seq = sent + 1;
    snprintf(b, sizeof(b), "che%d|%.*s|%.*s",
             seq, LEN_CALLSIGN, p, LEN_EXCH, sp);
    if (!send_callhist_entry_with_ack(seq, b)) {
      console->printf("callhist transfer failed at seq=%d sent=%d\n",
                      seq, sent);
      f.close();
      clear_callhist_subcpu_main();
      return false;
    }
    sent = seq;
  }
  f.close();

  ch_done = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"chend", 5);
  uint32_t deadline = millis() + 3000;
  while (!ch_done && (int32_t)(millis() - deadline) < 0) {
    if (f_mux_transport) mux_transport.recv_pkt();
    delay(1);
  }

  console->printf(
      "subcpu callhist: received=%d sent=%d bytes=%u done=%d\n",
      ch_count, sent, (unsigned)ch_bytes, ch_done ? 1 : 0);

  return ch_done && ch_count == sent;
}

void clear_callhist_subcpu_main(){const char*b="chreset0";mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL,MUX_PORT_EXT_BRD_CTRL,(unsigned char*)b,strlen(b));}
void process_callhist_reset_subcpu(const char*){} void process_callhist_entry_subcpu(char*){} void process_callhist_end_subcpu(){}
bool search_callhist_subcpu_local(const char*,char*,size_t){return false;} int append_callhist_partial_subcpu(const char*,check_entry_list*,int){return 0;}
int get_callhist_subcpu_count(){return ch_count;} size_t get_callhist_subcpu_bytes(){return ch_bytes;}
bool get_callhist_subcpu_entry(int,const char**,const char**){return false;}
#endif
