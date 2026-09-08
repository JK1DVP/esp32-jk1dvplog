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
#include "callhist.h"
#include "callhist_remote.h"
#include "dupechk.h"
#include "mux_transport.h"

#define DVP_STR_HELPER(x) #x
#define DVP_STR(x) DVP_STR_HELPER(x)
// dumb dupe check routine
extern int f_spiram;

struct dupechk *dupechk=NULL;

// Remove only a conservative set of trailing portable suffixes for exact
// DUPE/CALLHIST comparison.  Prefix operations such as F/JA1ABC and
// KH0/JA1ABC are deliberately preserved.
bool normalize_dupe_callsign(const char *src, char *dst, size_t dst_size) {
  if (dst == NULL || dst_size == 0) return false;
  dst[0] = '\0';
  if (src == NULL) return false;
  strlcpy(dst, src, dst_size);

  char *slash = strrchr(dst, '/');
  if (slash == NULL || slash == dst || slash[1] == '\0') return false;
  const char *suffix = slash + 1;
  bool portable = false;
  if (suffix[1] == '\0' && suffix[0] >= '0' && suffix[0] <= '9') portable = true;
  else if (strcasecmp(suffix, "P") == 0 ||
           strcasecmp(suffix, "M") == 0 ||
           strcasecmp(suffix, "MM") == 0 ||
           strcasecmp(suffix, "AM") == 0 ||
           strcasecmp(suffix, "QRP") == 0) portable = true;
  if (!portable) return false;

  *slash = '\0';
  return true;
}

bool dupe_callsign_equal(const char *a, const char *b) {
  char na[LEN_CALLSIGN + 1];
  char nb[LEN_CALLSIGN + 1];
  normalize_dupe_callsign(a, na, sizeof(na));
  normalize_dupe_callsign(b, nb, sizeof(nb));
  return strcasecmp(na, nb) == 0;
}

static volatile bool dupechk_reset_ack = false;
static volatile bool makedupe_done_ack = false;
static bool makedupe_score_received[2] = {false, false};
static unsigned char dupechk_current_mask = 0xff;
#ifdef DVPLOGGER_EXT
static unsigned char makedupe_bulk_mask = 0xff;
static uint16_t makedupe_bulk_worked[2][N_BAND];
static uint32_t makedupe_bulk_rx = 0;
static uint32_t makedupe_bulk_accepted = 0;
static uint32_t makedupe_bulk_duplicate = 0;
static uint32_t makedupe_bulk_malformed = 0;
static uint32_t makedupe_bulk_overflow = 0;
static uint32_t makedupe_bulk_invalid = 0;

#define MAKEDUPE_DUP_SAMPLE_MAX 10
struct makedupe_dup_sample_t {
  char call[LEN_CALLSIGN + 1];
  uint8_t incoming_bandmode;
  uint8_t existing_bandmode;
  uint16_t existing_index;
};
static makedupe_dup_sample_t makedupe_dup_samples[MAKEDUPE_DUP_SAMPLE_MAX];
static uint8_t makedupe_dup_sample_count = 0;
#endif


// Destination for the currently outstanding partial-match query.  Partial
// checking is synchronous, just like the ordinary dupe query, so only one
// request can be active at a time.
static struct check_entry_list *dupechk_partial_entry_list = NULL;

#ifndef VERBOSE_DUPE
#define VERBOSE_DUPE 16384
#endif

#define DUPE_EXACT_CACHE_SIZE 8
struct dupe_exact_cache_entry {
  bool valid;
  char call[LEN_CALLSIGN + 1];
  uint8_t bandmode;
  uint8_t mask;
  bool want_exch;
  bool dupe;
  bool has_exch;
  char exch[LEN_EXCH + 1];
};
static struct dupe_exact_cache_entry dupe_exact_cache[DUPE_EXACT_CACHE_SIZE];
static uint8_t dupe_exact_cache_next = 0;

static void invalidate_dupe_exact_cache() {
  memset(dupe_exact_cache, 0, sizeof(dupe_exact_cache));
  dupe_exact_cache_next = 0;
}

// bandmode calculation 
unsigned char bandmode_param(int bandid,int modetype) {
  return bandid * 4 + modetype;
}

// get current bandmode
unsigned char bandmode(struct radio *radio) {
  //  struct radio *radio;
  //  radio = so2r.radio_selected();
  if (radio->modetype==LOG_MODETYPE_PH && radio->f_tone_keying) {
    return bandmode_param(radio->bandid,LOG_MODETYPE_CW); // TONE KEYING IS REGARDED AS CW QSO IN dupe check 
  } else {
    return bandmode_param(radio->bandid,radio->modetype);
  }
}

// Send a dupe query to the subcpu and wait for the matching response.
// want_exch requests an exchange from any previous QSO with the same callsign.
static bool query_dupechk_subcpu(const char *call, byte bandmode, byte mask,
                                 bool want_exch) {
  char buf[80];

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = millis() + 500;
  dupechk->dupechk_dupe = 0;
  dupechk->dupechk_getexch = 0;
  dupechk->dupechk_exch[0] = '\0';

  snprintf(buf, sizeof(buf), "dupec%u|%.*s|%u|%u|%u",
           (unsigned int)dupechk->dupechk_query_id,
           LEN_CALLSIGN, call,
           (unsigned int)bandmode, (unsigned int)mask,
           want_exch ? 1U : 0U);
  if (verbose & 4) console->println(buf);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));

  while (dupechk->dupechk_status == 1) {
    if (f_mux_transport) mux_transport.recv_pkt();
    task_dupechk();
    delay(1);
  }
  return dupechk->dupechk_dupe != 0;
}

// Send a partial callsign query to the subcpu and wait for the matching
// response.  The subcpu returns only QSO-history entries; callhist_list is
// still searched locally by dupe_partial_check().
int query_dupechk_partial_subcpu(const char *call, byte bandmode, byte mask,
                                 struct check_entry_list *entry_list) {
  char buf[80];

  if (entry_list == NULL) return 0;

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = millis() + 500;
  dupechk_partial_entry_list = entry_list;

  snprintf(buf, sizeof(buf), "dupep%u|%.*s|%u|%u|%u",
           (unsigned int)dupechk->dupechk_query_id,
           LEN_CALLSIGN, call,
           (unsigned int)bandmode, (unsigned int)mask,
           (unsigned int)min(entry_list->nmax_entry, 10));
  if (verbose & 4) console->println(buf);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));

  while (dupechk->dupechk_status == 1) {
    if (f_mux_transport) mux_transport.recv_pkt();
    task_dupechk();
    delay(1);
  }

  dupechk_partial_entry_list = NULL;
  return entry_list->nentry;
}

// Process "id|call|bandmode|mask|max_entries" on the subcpu.
// Response format:
//   dupepr:id|count|dupe_count|CALL,EXCH,BANDMODE,FLAGS|...
void process_dupechk_partial_query_subcpu(char *s) {
  unsigned int query_id, bandmode, mask, max_entries;
  char call[LEN_CALLSIGN + 1];
  char response[240];
  size_t used;
  int count = 0;
  int ndupe = 0;
  uint32_t qso_start_us, qso_us, hist_start_us, hist_us = 0;
  unsigned int qso_scanned = 0, hist_scanned = 0;
  char matched_qso[10][LEN_CALLSIGN + 1];
  int nmatched_qso = 0;

  if (sscanf(s, "%u|%" DVP_STR(LEN_CALLSIGN) "[^|]|%u|%u|%u",
             &query_id, call, &bandmode, &mask, &max_entries) != 5) {
    snprintf(response, sizeof(response), "dupepr:0|0|0");
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)response, strlen(response));
    return;
  }

  {
    char ack[32];
    snprintf(ack, sizeof(ack), "dupeack:%u A", query_id);
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)ack, strlen(ack));
  }

  if (max_entries > 10) max_entries = 10;
  used = snprintf(response, sizeof(response), "dupepr:%u|0|0", query_id);

  qso_start_us = micros();
  for (int i = 0; i < dupechk->ncallsign && count < (int)max_entries; i++) {
    qso_scanned++;
    const bool exact_match = dupe_callsign_equal(dupechk->callsign[i], call);
    if (strstr(dupechk->callsign[i], call) == NULL && !exact_match) continue;
    if (nmatched_qso < 10) {
      strncpy(matched_qso[nmatched_qso], dupechk->callsign[i], LEN_CALLSIGN);
      matched_qso[nmatched_qso][LEN_CALLSIGN] = '\0';
      nmatched_qso++;
    }

    int flags = CHECK_ENTRY_FLAG_DUPECHECK_LIST;
    if (exact_match) {
      flags |= CHECK_ENTRY_FLAG_EXACT_MATCH;
      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
        flags |= CHECK_ENTRY_FLAG_DUPE;
        ndupe++;
      }
    }

    char item[64];
    int item_len = snprintf(item, sizeof(item), "|%s,%s,%u,%d",
                            dupechk->callsign[i],
                            dupechk->exch[i][0] ? dupechk->exch[i] : "-",
                            (unsigned int)dupechk->bandmode[i], flags);
    if (item_len <= 0 || used + (size_t)item_len >= sizeof(response)) break;
    memcpy(response + used, item, item_len);
    used += item_len;
    response[used] = '\0';
    count++;
  }
  qso_us = (uint32_t)(micros() - qso_start_us);

#ifdef DVPLOGGER_EXT
  // Fill remaining slots from the microSD Call History stored on the subcpu.
  // Any QSO callsign matching this partial string was already collected above;
  // compare against that small set instead of rescanning the whole QSO database
  // for every Call History candidate.
  hist_start_us = micros();
  for (int ci = 0; ci < get_callhist_subcpu_count() && count < (int)max_entries; ci++) {
    hist_scanned++;
    const char *hc = NULL, *he = NULL;
    if (!get_callhist_subcpu_entry(ci, &hc, &he)) continue;
    const bool exact_match = dupe_callsign_equal(hc, call);
    if (strstr(hc, call) == NULL && !exact_match) continue;
    bool already = false;
    for (int j = 0; j < nmatched_qso; j++) {
      if (strcmp(matched_qso[j], hc) == 0) { already = true; break; }
    }
    if (already) continue;
    int flags = CHECK_ENTRY_FLAG_CALLHIST_LIST;
    if (exact_match) flags |= CHECK_ENTRY_FLAG_EXACT_MATCH;
    char item[64];
    int item_len = snprintf(item, sizeof(item), "|%s,%s,0,%d", hc, he && *he ? he : "-", flags);
    if (item_len <= 0 || used + (size_t)item_len >= sizeof(response)) break;
    memcpy(response + used, item, item_len);
    used += item_len; response[used] = '\0'; count++;
  }
  hist_us = (uint32_t)(micros() - hist_start_us);
#endif

  // Fill in count and dupe_count, and put timing metadata before the
  // variable-length result entries so MAIN can parse it without ambiguity.
  char final_response[240];
  const char *tail = strchr(response + 7, '|');
  if (tail != NULL) tail = strchr(tail + 1, '|');
  if (tail != NULL) tail = strchr(tail + 1, '|');
  if (tail == NULL) tail = "";
  snprintf(final_response, sizeof(final_response),
           "dupepr:%u|%d|%d|T=%lu,%u,%lu,%u%s",
           query_id, count, ndupe, (unsigned long)(qso_us + hist_us),
           qso_scanned, (unsigned long)hist_us, hist_scanned, tail);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)final_response, strlen(final_response));
}

// Decode a partial-match response on the main CPU.
void process_dupechk_partial_response_maincpu(char *s) {
  char *saveptr = NULL;
  char *tok;
  unsigned int query_id;
  int count, ndupe;

  tok = strtok_r(s, "|", &saveptr);
  if (tok == NULL) return;
  query_id = strtoul(tok, NULL, 10);
  tok = strtok_r(NULL, "|", &saveptr);
  if (tok == NULL) return;
  count = atoi(tok);
  tok = strtok_r(NULL, "|", &saveptr);
  if (tok == NULL) return;
  ndupe = atoi(tok);

  if (dupechk->dupechk_status != 1 ||
      query_id != dupechk->dupechk_query_id ||
      dupechk_partial_entry_list == NULL) {
    if (verbose & 4) console->println("ignored stale dupepr response");
    return;
  }

  struct check_entry_list *entry_list = dupechk_partial_entry_list;
  entry_list->nentry = 0;
  entry_list->dupe = ndupe;

  while (entry_list->nentry < count && entry_list->nentry < 10 &&
         (tok = strtok_r(NULL, "|", &saveptr)) != NULL) {
    struct check_entry *entry = &entry_list->entryl[entry_list->nentry];
    char *item_save = NULL;
    char *callsign = strtok_r(tok, ",", &item_save);
    char *exch = strtok_r(NULL, ",", &item_save);
    char *bm = strtok_r(NULL, ",", &item_save);
    char *flags = strtok_r(NULL, ",", &item_save);
    if (callsign == NULL || exch == NULL || bm == NULL || flags == NULL) continue;

    strncpy(entry->callsign, callsign, sizeof(entry->callsign) - 1);
    entry->callsign[sizeof(entry->callsign) - 1] = '\0';
    if (strcmp(exch, "-") == 0) {
      entry->exch[0] = '\0';
    } else {
      strncpy(entry->exch, exch, sizeof(entry->exch) - 1);
      entry->exch[sizeof(entry->exch) - 1] = '\0';
    }
    entry->bandmode = (unsigned char)strtoul(bm, NULL, 10);
    entry->flag = atoi(flags);
    entry_list->nentry++;
  }

  dupechk->dupechk_status = 0;
}

// only check dupe no call history retrieval
bool dupe_check_nocallhist(char *call, byte bandmode, byte mask) {
  if (dupechk->dupechk_at == 1) {
    return query_dupechk_subcpu(call, bandmode, mask, false);
  }

  int ret = 0;
  for (int i = 0; i < dupechk->ncallsign; i++) {
    if ((dupechk->bandmode[i] & mask) == (bandmode & mask) &&
        dupe_callsign_equal(dupechk->callsign[i], call)) {
      ret = 1;
      break;
    }
  }
  return ret != 0;
}

// Process "id|call|bandmode|mask|want_exch" on the subcpu.
void process_dupechk_query_subcpu(char *s) {
  unsigned int query_id, bandmode, mask, want_exch;
  char callsign[LEN_CALLSIGN + 1];
  char response[96];
  char exch[LEN_EXCH + 1] = "";
  int dupe = 0;
  int has_exch = 0;

  if (sscanf(s, "%u|%" DVP_STR(LEN_CALLSIGN) "[^|]|%u|%u|%u",
             &query_id, callsign, &bandmode, &mask, &want_exch) != 5) {
    snprintf(response, sizeof(response), "duper:0 0 0 -");
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)response, strlen(response));
    return;
  }

  {
    char ack[32];
    snprintf(ack, sizeof(ack), "dupeack:%u E", query_id);
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)ack, strlen(ack));
  }

  uint32_t search_start_us = micros();
  uint32_t search_us;
  unsigned int qso_scanned = 0;
  unsigned int hist_scanned = 0;
  bool cache_hit = false;

#ifdef DVPLOGGER_EXT
  for (int ci = 0; ci < DUPE_EXACT_CACHE_SIZE; ci++) {
    struct dupe_exact_cache_entry *ce = &dupe_exact_cache[ci];
    if (!ce->valid || ce->bandmode != bandmode || ce->mask != mask ||
        ce->want_exch != (want_exch != 0) || strcmp(ce->call, callsign) != 0)
      continue;
    dupe = ce->dupe;
    has_exch = ce->has_exch;
    if (has_exch) {
      strncpy(exch, ce->exch, LEN_EXCH);
      exch[LEN_EXCH] = '\0';
    }
    cache_hit = true;
    break;
  }
#endif

  if (!cache_hit) {
    // Search backwards so that the newest exchange is returned. Cluster and
    // bandmap checks use this exact-match path and avoid the partial/history scan.
    for (int i = dupechk->ncallsign - 1; i >= 0; i--) {
      qso_scanned++;
      if (!dupe_callsign_equal(dupechk->callsign[i], callsign)) continue;
      if (want_exch && !has_exch && dupechk->exch[i][0] != '\0') {
        strncpy(exch, dupechk->exch[i], LEN_EXCH);
        exch[LEN_EXCH] = '\0';
        has_exch = 1;
      }
      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) dupe = 1;
      if (dupe && (!want_exch || has_exch)) break;
    }

#ifdef DVPLOGGER_EXT
    if (want_exch && !has_exch) {
      hist_scanned = get_callhist_subcpu_count();
      if (search_callhist_subcpu_local(callsign, exch, sizeof(exch))) has_exch = 1;
    }
    struct dupe_exact_cache_entry *ce = &dupe_exact_cache[dupe_exact_cache_next];
    memset(ce, 0, sizeof(*ce));
    ce->valid = true;
    strncpy(ce->call, callsign, LEN_CALLSIGN);
    ce->call[LEN_CALLSIGN] = '\0';
    ce->bandmode = bandmode;
    ce->mask = mask;
    ce->want_exch = want_exch != 0;
    ce->dupe = dupe != 0;
    ce->has_exch = has_exch != 0;
    if (has_exch) {
      strncpy(ce->exch, exch, LEN_EXCH);
      ce->exch[LEN_EXCH] = '\0';
    }
    dupe_exact_cache_next = (dupe_exact_cache_next + 1) % DUPE_EXACT_CACHE_SIZE;
#endif
  }
  search_us = (uint32_t)(micros() - search_start_us);
  snprintf(response, sizeof(response), "duper:%u %d %d %s %lu %u %u %u",
           query_id, dupe, has_exch, has_exch ? exch : "-",
           (unsigned long)search_us, qso_scanned, hist_scanned, cache_hit ? 1U : 0U);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)response, strlen(response));
}

// dupe check and check and fill call history
//  callhist_check : true if check callhistory and obtain exchange to fill
bool dupe_check(struct radio *radio,char *call, byte bandmode, byte mask, bool callhist_check) 
{
  int ret;
  //  struct radio *radio;
  char *getexch;
  bool f_getexch=0; // flag if exchange is obtained
  bool f_callhist = 0;
  

  if ((verbose&4) &&(dupechk->dupechk_at!=2)) {
    console->print("dupe_check() radio=");console->print(radio->rig_idx);
    console->print(" bandmode=");console->println(bandmode,HEX);
  }
  
  getexch=radio->recv_exch + 2;

  if (strlen(getexch) == 0) {
    // not yet filled in the my exchange, search previous qso and fill it.
    f_callhist = 1;
  }
  
  if (dupechk->dupechk_at == 1) {
    ret = query_dupechk_subcpu(call, bandmode, mask, f_callhist);
    if (dupechk->dupechk_getexch && f_callhist) {
      strncpy(getexch, dupechk->dupechk_exch, LEN_EXCH);
      getexch[LEN_EXCH] = '\0';
      f_getexch = 1;
      f_callhist = 0;
    }
    // The large QSO history lives on the subcpu, but the ordinary
    // call-history table remains on the main CPU.
    if (!ret && f_callhist && callhist_check && plogw->enable_callhist) {
      if (search_callhist_getexch(call, getexch)) f_getexch = 1;
    }
  } else {
    ret=dupe_check_get_callhist(call, bandmode, mask, callhist_check,getexch,&f_getexch,&f_callhist);
  }

  if (f_getexch) {
    //    strcpy(radio->recv_exch + 2, getexch);	   // here already copied
    // also need to locate cursor to the end of the string
    radio->recv_exch[1] = strlen(radio->recv_exch + 2);
  }
  return ret;
}

// f_callhist : flag if callhist check necessary
// f_getexch : flag if exchange is obtained from history
bool dupe_check_get_callhist(char *call, byte bandmode, byte mask, bool callhist_check,char *getexch,bool *f_getexch,bool *f_callhist) {
  int i;
  bool ret,ret1;

  // check all qso
  ret = 0;
  for (i = 0; i < dupechk->ncallsign; i++) {
    if ((ret == 0) || (*f_callhist == 1)) {

      if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) {
        // current band and mode
        if (dupe_callsign_equal(dupechk->callsign[i], call)) {
          // dupe
          if (verbose & 1) {
            plogw->ostream->println("dupe");
          }
          ret = 1;
        }
      } else if (*f_callhist) {
        // other band and mode
        if (dupe_callsign_equal(dupechk->callsign[i], call)) {
          // hit !

          strcpy(getexch, dupechk->exch[i]);	  
          *f_callhist = 0;  // no longer need to search for history
	  *f_getexch=1;
        }
      }
    } else {
      break;
    }
  }
  if ((ret != 1) && (*f_callhist == 1) && (callhist_check)) {
    // not dupe and not yet worked in other band
    // if enabled search for the history
    if (plogw->enable_callhist) {
      if (search_callhist_getexch(call,getexch)) {
	*f_getexch=1;
      }
    }
  }
  return ret;  // not dupe
}


void entry_makedupe_subcpu_data(const char *callsign, const char *recv_exch, unsigned char bandmode) {
  char buf[80];
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  snprintf(buf, sizeof(buf), "dupeb%.*s|%.*s|%u",
           LEN_CALLSIGN, callsign, LEN_EXCH, recv_exch,
           (unsigned int)bandmode);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));
}

void begin_makedupe_subcpu(unsigned char mask) {
  char buf[32];
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  makedupe_done_ack = false;
  makedupe_score_received[0] = false;
  makedupe_score_received[1] = false;
  snprintf(buf, sizeof(buf), "dupebulkbegin%u", (unsigned int)mask);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));
}

void process_makedupe_score_maincpu(char *s, int group) {
  if (group < 0 || group > 1) return;
  char *save = NULL;
  char *p = strtok_r(s, ",", &save);
  for (int bandid = 1; bandid < N_BAND && p != NULL; bandid++) {
    score.worked[group][bandid - 1] = atoi(p);
    p = strtok_r(NULL, ",", &save);
  }
  makedupe_score_received[group] = true;
  if (makedupe_score_received[0] && makedupe_score_received[1])
    makedupe_done_ack = true;
}

void finish_makedupe_subcpu() {
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  makedupe_done_ack = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"dupebulkend", strlen("dupebulkend"));
  uint32_t timeout = millis() + 2000;
  while (!makedupe_done_ack && (int32_t)(millis() - timeout) < 0) {
    if (f_mux_transport) mux_transport.recv_pkt();
    delay(1);
  }
  if (!makedupe_done_ack) console->println("finish_makedupe_subcpu() timeout");
}

void entry_dupechk_data(const char *callsign, const char *recv_exch, unsigned char bandmode) {
  char buf[80];

  if (dupechk->dupechk_at == 1) {
    snprintf(buf, sizeof(buf), "dupee%.*s|%.*s|%u",
             LEN_CALLSIGN, callsign, LEN_EXCH, recv_exch,
             (unsigned int)bandmode);
    if (verbose & 4) console->println(buf);
    mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                           (unsigned char *)buf, strlen(buf));
  } else {
    entry_dupechk_call_exch_bandmode((char *)callsign, (char *)recv_exch, bandmode);
  }
}

void sync_dupechk_mask_subcpu(unsigned char mask) {
  char cmd[24];
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  snprintf(cmd, sizeof(cmd), "dupemask%u", (unsigned int)mask);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)cmd, strlen(cmd));
}

void set_dupechk_mask_subcpu(unsigned char mask) {
  dupechk_current_mask = mask;
}

int get_dupechk_nmaxqso()
{
  if (dupechk == NULL) return 0;
  return dupechk->nmaxqso;
}

int get_dupechk_ncallsign()
{
  if (dupechk == NULL) return 0;
  return dupechk->ncallsign;
}
unsigned char get_dupechk_mask_subcpu() {
  return dupechk_current_mask;
}

void notify_dupechk_subcpu_reset() {
  dupechk_reset_ack = true;
}

bool reset_dupechk_subcpu() {
  if (dupechk == NULL || dupechk->dupechk_at != 1) return false;

  dupechk_reset_ack = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"dupereset", strlen("dupereset"));

  uint32_t timeout = millis() + 500;
  while (!dupechk_reset_ack && (int32_t)(millis() - timeout) < 0) {
    if (f_mux_transport) mux_transport.recv_pkt();
    delay(1);
  }

  if (!dupechk_reset_ack) {
    console->println("reset_dupechk_subcpu() timeout");
    return false;
  }
  return true;
}

void entry_dupechk_call_exch_bandmode(char *callsign,char *recv_exch,unsigned char bandmode) {
  // entry current qso into dupecheck
  if (dupechk->ncallsign < dupechk->nmaxqso) {
    //
    int i;
    i = dupechk->ncallsign;
    strcpy(dupechk->callsign[i], callsign);
    strcpy(dupechk->exch[i], recv_exch );
    dupechk->bandmode[i] = bandmode;
    dupechk->ncallsign++;
    invalidate_dupe_exact_cache();
  }
}

#ifdef DVPLOGGER_EXT
void begin_makedupe_bulk_subcpu(unsigned char mask) {
  /* Rebuild the SUBCPU DUPE database from the QSO log from the beginning. */
  if (dupechk != NULL) {
    dupechk->ncallsign = 0;
    dupechk->dupechk_status = 0;
    dupechk->dupechk_timeout = 0;
  }
  invalidate_dupe_exact_cache();

  dupechk_current_mask = mask;
  makedupe_bulk_mask = dupechk_current_mask;
  memset(makedupe_bulk_worked, 0, sizeof(makedupe_bulk_worked));
  makedupe_bulk_rx = 0;
  makedupe_bulk_accepted = 0;
  makedupe_bulk_duplicate = 0;
  makedupe_bulk_malformed = 0;
  makedupe_bulk_overflow = 0;
  makedupe_bulk_invalid = 0;
  makedupe_dup_sample_count = 0;

  char begin_diag[96];
  snprintf(begin_diag, sizeof(begin_diag),
           "dupebulkbeginack:%u,%d,%d",
           (unsigned int)makedupe_bulk_mask,
           dupechk ? dupechk->ncallsign : -1,
           dupechk ? dupechk->nmaxqso : -1);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)begin_diag, strlen(begin_diag));
}

void entry_makedupe_bulk_subcpu(char *s) {
  makedupe_bulk_rx++;

  char callsign[LEN_CALLSIGN + 1];
  char recv_exch[LEN_EXCH + 1];
  char *sep1 = strchr(s, '|');
  char *sep2 = sep1 ? strchr(sep1 + 1, '|') : NULL;
  int bandmode;

  if (!sep1 || !sep2) {
    makedupe_bulk_malformed++;
    return;
  }
  *sep1 = '\0';
  *sep2 = '\0';
  bandmode = atoi(sep2 + 1);

  if (strlen(s) > LEN_CALLSIGN || strlen(sep1 + 1) > LEN_EXCH) {
    makedupe_bulk_malformed++;
    return;
  }
  if (bandmode < 0 || bandmode > 255) {
    makedupe_bulk_invalid++;
    return;
  }

  int bandid = bandmode / 4;
  int modetype = bandmode % 4;
  if (bandid < 1 || bandid >= N_BAND || modetype < 0 || modetype > 3) {
    makedupe_bulk_invalid++;
    return;
  }

  strncpy(callsign, s, LEN_CALLSIGN);
  callsign[LEN_CALLSIGN] = '\0';
  strncpy(recv_exch, sep1 + 1, LEN_EXCH);
  recv_exch[LEN_EXCH] = '\0';

  for (int i = 0; i < dupechk->ncallsign; i++) {
    if (((dupechk->bandmode[i] & makedupe_bulk_mask) ==
         (((unsigned char)bandmode) & makedupe_bulk_mask)) &&
        dupe_callsign_equal(dupechk->callsign[i], callsign)) {
      makedupe_bulk_duplicate++;
      if (makedupe_dup_sample_count < MAKEDUPE_DUP_SAMPLE_MAX) {
        makedupe_dup_sample_t *sample =
            &makedupe_dup_samples[makedupe_dup_sample_count++];
        strncpy(sample->call, callsign, LEN_CALLSIGN);
        sample->call[LEN_CALLSIGN] = '\0';
        sample->incoming_bandmode = (uint8_t)bandmode;
        sample->existing_bandmode = dupechk->bandmode[i];
        sample->existing_index = (uint16_t)i;
      }
      return;
    }
  }

  if (dupechk->ncallsign >= dupechk->nmaxqso) {
    makedupe_bulk_overflow++;
    return;
  }

  entry_dupechk_call_exch_bandmode(callsign, recv_exch,
                                   (unsigned char)bandmode);
  makedupe_bulk_accepted++;

  char accepted[48];
  snprintf(accepted, sizeof(accepted), "dupebulka:%u|%.*s",
           (unsigned int)bandmode, LEN_EXCH, recv_exch);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)accepted, strlen(accepted));

  makedupe_bulk_worked[modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;
}

void finish_makedupe_bulk_subcpu() {
  char diag[128];
  snprintf(diag, sizeof(diag),
           "dupebulkdiag:%lu,%lu,%lu,%lu,%lu,%lu",
           (unsigned long)makedupe_bulk_rx,
           (unsigned long)makedupe_bulk_accepted,
           (unsigned long)makedupe_bulk_duplicate,
           (unsigned long)makedupe_bulk_malformed,
           (unsigned long)makedupe_bulk_overflow,
           (unsigned long)makedupe_bulk_invalid);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)diag, strlen(diag));

  // Send duplicate samples only after the bulk scan has completed so the
  // diagnostic traffic does not perturb the timing of the rebuild itself.
  for (uint8_t i = 0; i < makedupe_dup_sample_count; i++) {
    char sample_buf[96];
    const makedupe_dup_sample_t *sample = &makedupe_dup_samples[i];
    snprintf(sample_buf, sizeof(sample_buf),
             "dupebulkdup:%u|%s|%u|%u|%u",
             (unsigned int)i,
             sample->call,
             (unsigned int)sample->incoming_bandmode,
             (unsigned int)sample->existing_bandmode,
             (unsigned int)sample->existing_index);
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)sample_buf, strlen(sample_buf));
  }

  for (int group = 0; group < 2; group++) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "dupebulk%d:", group);
    for (int bandid = 1; bandid < N_BAND && n < (int)sizeof(buf) - 8; bandid++) {
      n += snprintf(buf + n, sizeof(buf) - n, "%s%u",
                    bandid == 1 ? "" : ",",
                    (unsigned int)makedupe_bulk_worked[group][bandid - 1]);
    }
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)buf, strlen(buf));
  }
}

#endif

void entry_dupechk_subcpu(char *s)
{
  char callsign[LEN_CALLSIGN + 1];
  char recv_exch[LEN_EXCH + 1];
  char buf[100];
  char *sep1 = strchr(s, '|');
  char *sep2 = sep1 ? strchr(sep1 + 1, '|') : NULL;
  int bandmode;
  int ret = 0;

  if (sep1 && sep2) {
    *sep1 = '\0';
    *sep2 = '\0';
    bandmode = atoi(sep2 + 1);
    if (strlen(s) <= LEN_CALLSIGN && strlen(sep1 + 1) <= LEN_EXCH &&
        bandmode >= 0 && bandmode <= 255) {
      strncpy(callsign, s, LEN_CALLSIGN);
      callsign[LEN_CALLSIGN] = '\0';
      strncpy(recv_exch, sep1 + 1, LEN_EXCH);
      recv_exch[LEN_EXCH] = '\0';
      entry_dupechk_call_exch_bandmode(callsign, recv_exch,
                                       (unsigned char)bandmode);
      ret = 1;
    }
  }
  snprintf(buf, sizeof(buf), "duped:%d %d %d", ret,
           dupechk->ncallsign, dupechk->nmaxqso);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));
}

void entry_dupechk(struct radio *radio) {

  if ((verbose &4)&&(dupechk->dupechk_at!=2)) {
    console->print("entry_dupechk(): radio=");console->print(radio->rig_idx);
    console->print("bandmode=");console->println((int)bandmode(radio),HEX);
  }
  char buf[80];
  switch(dupechk->dupechk_at) {
  case 0:
  case 1:
    entry_dupechk_data(radio->callsign + 2, radio->recv_exch + 2, bandmode(radio));
    break;
  case 2:
    // I am subcpu should not happen
    break;
  }
}

// initialize score statistics
void init_score() {
  for (int i = 0; i < N_BAND; i++) {
    score.worked[0][i] = 0;
    score.worked[1][i] = 0;
    score.nmulti[i] = 0;
    plogw->seqnr_band[i]=0;
  }
}

void init_dupechk_maincpu()
{
  init_dupechk(1,1);
  if (plogw != NULL) sync_dupechk_mask_subcpu(plogw->mask);
}

void init_dupechk_subcpu()
{
  invalidate_dupe_exact_cache();
  init_dupechk(NMAXQSO_SUBCPU,2);
}

int reset_dupechk_subcpu_database()
{
  const int before = (dupechk != NULL) ? dupechk->ncallsign : -1;
  const uint32_t started = millis();

  // A rebuild only needs to make old entries unreachable.  Every DUPE scan
  // is bounded by ncallsign, so do not free/reallocate/clear 1300 entries on
  // every MAKEDUPE.  Keeping the allocated arrays in place also avoids heap
  // churn and makes repeated resets deterministic.
  if (dupechk == NULL) {
    init_dupechk_subcpu();
  } else {
    dupechk->ncallsign = 0;
    dupechk->dupechk_status = 0;
    dupechk->dupechk_timeout = 0;
    dupechk->dupechk_dupe = 0;
    dupechk->dupechk_getexch = 0;
    dupechk->dupechk_exch[0] = '\0';
    invalidate_dupe_exact_cache();
  }

  const int after = (dupechk != NULL) ? dupechk->ncallsign : -1;
  console->printf("DUPE RESET logical clear before=%d after=%d elapsed=%lu ms\n",
                  before, after, (unsigned long)(millis() - started));
  return after;
}

void task_dupechk()
{
  // main loop task to check timeout
  if (dupechk->dupechk_status == 1) {
    // now querying
    if ((int32_t)(millis() - dupechk->dupechk_timeout) >= 0) {
      // timeout reached
      console->println("task_dupechk() timeout");
      dupechk->dupechk_status=0; // reset
      dupechk->dupechk_dupe=0; // communication failure is not a real dupe
    }
  }
}

void init_dupechk(int nmaxqso,int dupechk_at) {
  // allocate memory for dupechk pointer
  if (dupechk!=NULL) {
    // free contents
    free(dupechk->callsign);
    free(dupechk->exch);
    free(dupechk->bandmode);        
    free(dupechk) ;
    dupechk=NULL;
  }

  if (dupechk_at!=2)  {
    console->print("init_dupechk() nmaxqso=");
    console->print(nmaxqso);
    console->print(" dupechk_at ");
    console->println(dupechk_at);
  }
  dupechk =(struct dupechk *) malloc(sizeof(struct dupechk));

  if (f_spiram) {
    size_t psram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    printf("PSRAM size: %d bytes\r\n", psram_size);
    dupechk->callsign = (char (*)[LEN_CALLSIGN+1]) heap_caps_malloc(nmaxqso * (LEN_CALLSIGN+1),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dupechk->exch=(char (*)[LEN_EXCH+1]) heap_caps_malloc((LEN_EXCH+1)*nmaxqso, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dupechk->bandmode=(byte *) heap_caps_malloc(nmaxqso, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  
    //#else
  } else {
    dupechk->callsign = (char (*)[LEN_CALLSIGN+1]) malloc(nmaxqso * (LEN_CALLSIGN+1));
    dupechk->exch=(char (*)[LEN_EXCH+1]) malloc((LEN_EXCH+1)*nmaxqso);
    dupechk->bandmode=(byte *) malloc(nmaxqso);
    printf("dupechk allocated by malloc\n");
  }
  //#endif
  dupechk->nmaxqso=nmaxqso;
  dupechk->dupechk_at = dupechk_at;
  dupechk->dupechk_status=0;
  dupechk->dupechk_timeout=0;
  dupechk->dupechk_query_id=0;
  dupechk->dupechk_getexch=0;
  dupechk->dupechk_exch[0]='\0';
  
  for (int i = 0; i < nmaxqso; i++) {
    strcpy(dupechk->callsign[i], "");
    strcpy(dupechk->exch[i], "");    
    dupechk->bandmode[i] = 0;
  }
  dupechk->ncallsign = 0;
}
