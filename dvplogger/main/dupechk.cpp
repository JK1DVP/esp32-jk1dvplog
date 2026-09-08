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
#include "callhist_mem.h"
#include "callhist_remote.h"
#include "dupechk.h"
#include "mux_transport.h"
#include "ui.h"
#include "log.h"
#include "multi_process.h"

static uint32_t makedupe_sent_count = 0;
static uint32_t makedupe_accepted_count = 0;

bool dupechk_remote_query_pending() {
  return dupechk != NULL && dupechk->dupechk_at == 1 &&
         dupechk->dupechk_status == 1;
}

void note_makedupe_accepted_maincpu() {
  makedupe_accepted_count++;
}
#include "display.h"

#ifndef VERBOSE_DUPE
#define VERBOSE_DUPE 16384
#endif
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

bool dupe_callsign_partial_match(const char *candidate, const char *pattern) {
  if (candidate == NULL || pattern == NULL || *pattern == '\0') return false;

  // RTTY autofill uses '-' as a single unknown character.  Keep this
  // deliberately different from the ordinary substring partial check: an
  // uncertain callsign represents the whole callsign and therefore must have
  // the same length.
  if (strchr(pattern, '-') != NULL) {
    const size_t nc = strlen(candidate);
    const size_t np = strlen(pattern);
    if (nc != np) return false;
    for (size_t i = 0; i < np; ++i) {
      if (pattern[i] == '-') continue;
      if (toupper((unsigned char)candidate[i]) !=
          toupper((unsigned char)pattern[i])) return false;
    }
    return true;
  }

  // Historical behaviour for normal operator partial entry.
  return strstr(candidate, pattern) != NULL || dupe_callsign_equal(candidate, pattern);
}

static volatile bool dupechk_reset_ack = false;
static volatile bool makedupe_done_ack = false;
static bool makedupe_score_received[2] = {false, false};
static unsigned char dupechk_current_mask = 0xff;
#ifdef DVPLOGGER_EXT
static unsigned char makedupe_bulk_mask = 0xff;
static uint16_t makedupe_bulk_worked[2][N_BAND];
#endif


// Destination for the currently outstanding partial-match query.  Partial
// checking is synchronous, just like the ordinary dupe query, so only one
// request can be active at a time.
static struct check_entry_list *dupechk_partial_entry_list = NULL;

// Asynchronous operator-entry query.  Only one subcpu request is active at a
// time, matching the existing transport protocol.  The callsign snapshot
// prevents a late response from changing a newly edited entry.
static struct radio *dupechk_async_radio = NULL;
static char dupechk_async_call[LEN_CALLSIGN + 1] = "";
static unsigned char dupechk_async_bandmode = 0;
static unsigned char dupechk_async_mask = 0xff;
static bool dupechk_async_active = false;
static bool dupechk_async_result_valid = false;
static bool dupechk_async_pending_sp_send = false;

// Protect the main loop from repeated 500 ms stalls when the extension CPU
// stops answering dupe queries.  After two consecutive timeouts, suppress
// new queries for a short period and then allow one probe automatically.
static uint8_t dupechk_timeout_streak = 0;
static uint32_t dupechk_breaker_until = 0;
static bool dupechk_remote_unavailable = false;
static uint32_t dupechk_last_lcd_warning = 0;

// Only an operator-entry DUPE query may put a full-screen warning on the LCD.
// Background checks (bandmap/Web/QSO rebuild) still report a timeout on the
// console, but must not disturb the entry display.
static bool dupechk_query_is_operator_entry = false;
static bool dupechk_exact_response_confirmed = false;
// Cluster exact checks are background work.  They must never overwrite an
// operator query or trip/recover the operator-facing circuit breaker.
static bool dupechk_cluster_query_active = false;
static uint32_t dupechk_cluster_backoff_until = 0;
static bool dupechk_background_exact_active = false;
static unsigned int dupechk_background_exact_query_id = 0;
static void dupechk_remote_query_succeeded();
// Timing data for the single outstanding MAIN->SUBCPU dupe query.
static uint32_t dupechk_query_create_us = 0;
static uint32_t dupechk_query_tx_done_us = 0;
static uint32_t dupechk_query_ack_us = 0;

bool dupechk_remote_ack_received() {
  return dupechk_query_ack_us != 0;
}
static uint32_t dupechk_query_rx_us = 0;
static char dupechk_query_call[LEN_CALLSIGN + 1] = "";
static char dupechk_query_kind = '-'; // E=exact, P=partial, A=async partial

static void dupechk_note_query(char kind, const char *call) {
  dupechk_query_kind = kind;
  strncpy(dupechk_query_call, call ? call : "", LEN_CALLSIGN);
  dupechk_query_call[LEN_CALLSIGN] = '\0';
  dupechk_query_create_us = micros();
  dupechk_query_tx_done_us = 0;
  dupechk_query_ack_us = 0;
  dupechk_query_rx_us = 0;
}

void dupechk_note_main_ack(unsigned int query_id) {
#ifndef DVPLOGGER_EXT
  if (dupechk_query_create_us == 0 || dupechk_query_ack_us != 0) return;
  if (dupechk == NULL || query_id != dupechk->dupechk_query_id) return;
  dupechk_query_ack_us = micros();
#endif
}

void dupechk_note_main_rx() {
#ifndef DVPLOGGER_EXT
  if (dupechk_query_create_us != 0 && dupechk_query_rx_us == 0)
    dupechk_query_rx_us = micros();
#endif
}

void dupechk_note_exact_response_success(unsigned int query_id) {
#ifndef DVPLOGGER_EXT
  if (dupechk != NULL && dupechk->dupechk_status == 1 &&
      query_id == dupechk->dupechk_query_id) {
    dupechk_exact_response_confirmed = true;
    if (!dupechk_cluster_query_active) dupechk_remote_query_succeeded();
  }
#endif
}

static void dupechk_send_query_packet(unsigned char *buf, size_t len) {
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         buf, len);
  // send_pkt() writes synchronously to mux_stream; this is the first instant
  // after the complete framed packet has been handed to the UART stream.
  dupechk_query_tx_done_us = micros();
}

void dupechk_log_timing(const char *phase, unsigned int query_id,
                               uint32_t sub_search_us, unsigned int qso_scanned,
                               unsigned int hist_scanned, bool cache_hit) {
#ifndef DVPLOGGER_EXT
  const uint32_t commit_us = micros();
  const uint32_t total_us = dupechk_query_create_us
      ? (uint32_t)(commit_us - dupechk_query_create_us) : 0;
  const uint32_t create_to_tx_us =
      (dupechk_query_create_us && dupechk_query_tx_done_us)
      ? (uint32_t)(dupechk_query_tx_done_us - dupechk_query_create_us) : 0;
  const uint32_t tx_to_ack_us =
      (dupechk_query_tx_done_us && dupechk_query_ack_us)
      ? (uint32_t)(dupechk_query_ack_us - dupechk_query_tx_done_us) : 0;
  const uint32_t ack_to_rx_us =
      (dupechk_query_ack_us && dupechk_query_rx_us)
      ? (uint32_t)(dupechk_query_rx_us - dupechk_query_ack_us) : 0;
  const uint32_t tx_to_rx_us =
      (dupechk_query_tx_done_us && dupechk_query_rx_us)
      ? (uint32_t)(dupechk_query_rx_us - dupechk_query_tx_done_us) : 0;
  const uint32_t rx_to_commit_us = dupechk_query_rx_us
      ? (uint32_t)(commit_us - dupechk_query_rx_us) : 0;
  const uint32_t transport_us = total_us > sub_search_us
      ? total_us - sub_search_us : 0;

  // Detailed timing is diagnostic output.  Timeouts and operator-facing
  // errors are reported elsewhere, so keep routine/slow timing silent unless
  // explicitly requested.
  if (verbose & VERBOSE_DUPE) {
    Serial.printf(
        "DUPE ACK phase=%s kind=%c id=%u call=%s "
        "total=%luus create_tx=%luus tx_ack=%luus ack_result=%luus "
        "tx_result=%luus rx_commit=%luus "
        "sub=%luus transport=%luus qso=%u hist=%u cache=%u\n",
        phase, dupechk_query_kind, query_id, dupechk_query_call,
        (unsigned long)total_us, (unsigned long)create_to_tx_us,
        (unsigned long)tx_to_ack_us, (unsigned long)ack_to_rx_us,
        (unsigned long)tx_to_rx_us, (unsigned long)rx_to_commit_us,
        (unsigned long)sub_search_us, (unsigned long)transport_us,
        qso_scanned, hist_scanned, cache_hit ? 1U : 0U);
  }
#endif
}

#ifdef DVPLOGGER_EXT
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
#endif

static void dupechk_show_remote_error() {
#ifndef DVPLOGGER_EXT
  if (!dupechk_query_is_operator_entry) return;
  // Do not silently treat a communication failure as "not worked".
  // Keep the warning visible to the operator, but avoid refreshing the
  // flash display for every incoming cluster spot.
  uint32_t now = millis();
  if (dupechk_last_lcd_warning == 0 ||
      (uint32_t)(now - dupechk_last_lcd_warning) >= 5000) {
    upd_display_info_flash("DUPE CHECK ERROR\nSUBCPU NO RESPONSE\nRESULT UNKNOWN");
    dupechk_last_lcd_warning = now;
  }
#endif
}

static bool dupechk_remote_query_allowed() {
  if (dupechk_breaker_until == 0) return true;
  if ((int32_t)(millis() - dupechk_breaker_until) >= 0) {
    dupechk_breaker_until = 0;
    return true;
  }
  return false;
}

static void dupechk_remote_query_succeeded() {
  bool was_unavailable = dupechk_remote_unavailable;
  dupechk_query_is_operator_entry = false;
  dupechk_timeout_streak = 0;
  dupechk_breaker_until = 0;
  dupechk_remote_unavailable = false;
  dupechk_last_lcd_warning = 0;
#ifndef DVPLOGGER_EXT
  if (was_unavailable) {
    upd_display_info_flash("DUPE CHECK\nRECOVERED");
  }
#endif
}

static bool async_query_matches_radio(struct radio *radio) {
  return radio != NULL && dupechk_async_active &&
         radio == dupechk_async_radio &&
         strcmp(radio->callsign + 2, dupechk_async_call) == 0 &&
         bandmode(radio) == dupechk_async_bandmode &&
         plogw->mask == dupechk_async_mask;
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

void dupechk_background_exact_cancel() {
  if (!dupechk_background_exact_active) return;
  if (dupechk != NULL && dupechk->dupechk_status == 1 &&
      dupechk->dupechk_query_id == dupechk_background_exact_query_id) {
    dupechk->dupechk_status = 0;
    dupechk->dupechk_timeout = 0;
  }
  dupechk_background_exact_active = false;
  dupechk_background_exact_query_id = 0;
  dupechk_cluster_query_active = false;
}

bool dupechk_background_exact_start(const char *call, byte bm, byte mask) {
  if (dupechk == NULL || dupechk->dupechk_at != 1 || !call || !*call)
    return false;
  if (dupechk_background_exact_active || dupechk->dupechk_status == 1)
    return false;
  const uint32_t now = millis();
  if (dupechk_cluster_backoff_until != 0 &&
      (int32_t)(now - dupechk_cluster_backoff_until) < 0)
    return false;
  if (!dupechk_remote_query_allowed()) return false;

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk_background_exact_query_id = dupechk->dupechk_query_id;
  dupechk_background_exact_active = true;
  dupechk_cluster_query_active = true;
  dupechk_query_is_operator_entry = false;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = now + 1500U;
  dupechk->dupechk_dupe = 0;
  dupechk->dupechk_getexch = 0;
  dupechk_exact_response_confirmed = false;
  dupechk->dupechk_exch[0] = '\0';
  dupechk_note_query('E', call);

  char buf[80];
  snprintf(buf, sizeof(buf), "dupec%u|%.*s|%u|%u|0",
           dupechk_background_exact_query_id, LEN_CALLSIGN, call,
           (unsigned int)bm, (unsigned int)mask);
  dupechk_send_query_packet((unsigned char *)buf, strlen(buf));
  return true;
}

bool dupechk_background_exact_poll(bool *confirmed, bool *is_dupe) {
  if (confirmed) *confirmed = false;
  if (is_dupe) *is_dupe = false;
  if (!dupechk_background_exact_active) return true;
  if (dupechk != NULL && dupechk->dupechk_status == 1 &&
      dupechk->dupechk_query_id == dupechk_background_exact_query_id)
    return false;

  if (confirmed) *confirmed = dupechk_exact_response_confirmed;
  if (is_dupe) *is_dupe = dupechk_exact_response_confirmed &&
                           dupechk != NULL && dupechk->dupechk_dupe != 0;
  dupechk_background_exact_active = false;
  dupechk_background_exact_query_id = 0;
  dupechk_cluster_query_active = false;
  return true;
}

// Send a dupe query to the subcpu and wait for the matching response.
// want_exch requests an exchange from any previous QSO with the same callsign.
static bool query_dupechk_subcpu(const char *call, byte bandmode, byte mask,
                                 bool want_exch) {
  char buf[80];

  dupechk_background_exact_cancel();
  if (!dupechk_remote_query_allowed()) return false;
  dupechk_query_is_operator_entry = false;

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = millis() +
      (dupechk_cluster_query_active ? 1500U : 500U);
  dupechk->dupechk_dupe = 0;
  dupechk->dupechk_getexch = 0;
  dupechk_exact_response_confirmed = false;
  dupechk->dupechk_exch[0] = '\0';
  dupechk_note_query('E', call);

  snprintf(buf, sizeof(buf), "dupec%u|%.*s|%u|%u|%u",
           (unsigned int)dupechk->dupechk_query_id,
           LEN_CALLSIGN, call,
           (unsigned int)bandmode, (unsigned int)mask,
           want_exch ? 1U : 0U);
  if (verbose & 4) console->println(buf);
  dupechk_send_query_packet((unsigned char *)buf, strlen(buf));

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

  dupechk_background_exact_cancel();
  if (entry_list == NULL) return 0;
  dupechk_query_is_operator_entry = false;
  if (!dupechk_remote_query_allowed()) {
    entry_list->nentry = 0;
    entry_list->dupe = 0;
    return 0;
  }

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = millis() + 500;
  dupechk_partial_entry_list = entry_list;
  dupechk_note_query('P', call);

  snprintf(buf, sizeof(buf), "dupep%u|%.*s|%u|%u|%u",
           (unsigned int)dupechk->dupechk_query_id,
           LEN_CALLSIGN, call,
           (unsigned int)bandmode, (unsigned int)mask,
           (unsigned int)min(entry_list->nmax_entry, 10));
  if (verbose & 4) console->println(buf);
  dupechk_send_query_packet((unsigned char *)buf, strlen(buf));

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

  if (sscanf(s, "%u|%16[^|]|%u|%u|%u",
             &query_id, call, &bandmode, &mask, &max_entries) != 5) {
    snprintf(response, sizeof(response), "dupepr:0|0|0");
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)response, strlen(response));
    return;
  }

  if (max_entries > 10) max_entries = 10;
  used = snprintf(response, sizeof(response), "dupepr:%u|0|0", query_id);

  qso_start_us = micros();
  for (int i = 0; i < dupechk->ncallsign && count < (int)max_entries; i++) {
    qso_scanned++;
    const bool uncertain = strchr(call, '-') != NULL;
    const bool exact_match = !uncertain && dupe_callsign_equal(dupechk->callsign[i], call);
    if (!dupe_callsign_partial_match(dupechk->callsign[i], call)) continue;
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
    const bool uncertain = strchr(call, '-') != NULL;
    const bool exact_match = !uncertain && dupe_callsign_equal(hc, call);
    if (!dupe_callsign_partial_match(hc, call)) continue;
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

// Cancel only the outstanding operator-entry query.  This is needed when the
// callsign is erased below the DUPE threshold; otherwise the old 500 ms timer
// remains armed and later displays a misleading SUBCPU NO RESPONSE warning.
static void cancel_async_dupe_query() {
  if (dupechk_async_active) {
    dupechk->dupechk_status = 0;
    dupechk->dupechk_timeout = 0;
  }
  dupechk_async_active = false;
  dupechk_async_result_valid = false;
  dupechk_async_pending_sp_send = false;
  dupechk_partial_entry_list = NULL;
  dupechk_query_is_operator_entry = false;
}

// Assign a callsign obtained from bandmap, Web UI, partial-check selection,
// QSO recall, or another non-keyboard source.  Direct buffer assignments used
// to bypass the normal callsign-change detector and therefore left radio->dupe
// and CALLHIST stale.  Centralise those assignments here.
bool set_callsign_and_request_dupe(struct radio *radio, const char *callsign,
                                   bool include_partial) {
  if (radio == NULL || callsign == NULL) return false;

  strlcpy(radio->callsign + 2, callsign, LEN_CALL_WINDOW + 1);
  radio->callsign[1] = strlen(radio->callsign + 2);
  radio->dupe = 0;

  request_async_dupe_partial(radio, include_partial);
  if (strlen(radio->callsign + 2) >= 3)
    request_dupe_aware_display_update();
  else
    request_display_update_on_demand();
  return true;
}

// Start a non-blocking combined DUPE/partial/CALLHIST query for normal
// operator entry.  Local-main-CPU configurations remain fast and are handled
// immediately; the subcpu configuration returns through dupepr.
void request_async_dupe_partial(struct radio *radio, bool include_partial) {
  if (radio == NULL) return;
  // Operator entry owns the single SUBCPU query slot.  A stale response from
  // a cancelled background query is rejected by its old query id.
  dupechk_background_exact_cancel();
  const char *call = radio->callsign + 2;

  // Any edit invalidates the old visible result and any deferred S&P send.
  radio->dupe = 0;
  dupechk_async_result_valid = false;
  dupechk_async_pending_sp_send = false;
  if (strlen(call) < 3) {
    radio->check_entry_list.nentry = 0;
    radio->check_entry_list.cursor = 0;
    radio->callsign_prev[2] = '\0';
    cancel_async_dupe_query();
    return;
  }

  if (dupechk->dupechk_at != 1) {
    // '-' is an RTTY decoder uncertainty marker, not a literal callsign
    // character.  Never declare an exact DUPE while it remains; use only the
    // partial candidate list.
    if (strchr(radio->callsign + 2, '-') == NULL)
      radio->dupe = dupe_check(radio, radio->callsign + 2, bandmode(radio),
                               plogw->mask, true) ? 1 : 0;
    else
      radio->dupe = 0;
    if (include_partial && (plogw->f_partial_check & PARTIAL_CHECK_CALLSIGN_AUTO))
      ui_perform_partial_check(radio);
    return;
  }

  if (!dupechk_remote_query_allowed()) {
    dupechk_async_active = false;
    dupechk_async_result_valid = false;
    dupechk_async_pending_sp_send = false;
    dupechk_partial_entry_list = NULL;
    return;
  }

  dupechk->dupechk_query_id++;
  if (dupechk->dupechk_query_id == 0) dupechk->dupechk_query_id = 1;
  dupechk_query_is_operator_entry = true;
  dupechk->dupechk_status = 1;
  dupechk->dupechk_timeout = millis() + 500;
  dupechk_partial_entry_list = &radio->check_entry_list;
  radio->check_entry_list.cursor = 0;
  radio->check_entry_list.nentry = 0;
  radio->check_entry_list.nmax_entry = include_partial ? 5 : 1;

  dupechk_async_radio = radio;
  strncpy(dupechk_async_call, call, LEN_CALLSIGN);
  dupechk_async_call[LEN_CALLSIGN] = '\0';
  dupechk_async_bandmode = bandmode(radio);
  dupechk_async_mask = plogw->mask;
  dupechk_async_active = true;

  char buf[80];
  dupechk_note_query('A', call);
  snprintf(buf, sizeof(buf), "dupep%u|%.*s|%u|%u|%u",
           (unsigned int)dupechk->dupechk_query_id, LEN_CALLSIGN, call,
           (unsigned int)dupechk_async_bandmode,
           (unsigned int)dupechk_async_mask,
           (unsigned int)radio->check_entry_list.nmax_entry);
  dupechk_send_query_packet((unsigned char *)buf, strlen(buf));
}

// S&P Enter waits for a matching DUPE result when necessary, but the DUPE
// outcome itself never cancels an explicit operator transmit request.
// Return true when transmission may proceed immediately.
bool request_sp_send_after_dupe(struct radio *radio) {
  if (radio == NULL || strlen(radio->callsign + 2) < 3) return true;
  if (dupechk->dupechk_at != 1) {
    // Complete the local DUPE/CALLHIST update before sending.  A positive
    // DUPE result remains visible to the operator, but does not veto Enter.
    radio->dupe = dupe_check(radio, radio->callsign + 2, bandmode(radio),
                             plogw->mask, true) ? 1 : 0;
    return true;
  }

  if (async_query_matches_radio(radio) && dupechk->dupechk_status == 1) {
    dupechk_async_pending_sp_send = true;
    return false;
  }

  // A completed matching result is represented by the saved snapshot and an
  // inactive transport.  The result updates the DUPE UI, but never vetoes an
  // explicit Enter operation.
  if (dupechk_async_radio == radio && !dupechk_async_active &&
      dupechk_async_result_valid &&
      strcmp(radio->callsign + 2, dupechk_async_call) == 0 &&
      bandmode(radio) == dupechk_async_bandmode &&
      plogw->mask == dupechk_async_mask)
    return true;

  request_async_dupe_partial(radio, true);
  dupechk_async_pending_sp_send = true;
  return false;
}


static void append_main_callhist_partial(const char *call,
                                         struct check_entry_list *entry_list) {
  if (!call || !*call || !entry_list || !plogw->enable_callhist ||
      callhist_at != 0 || callhist_list == NULL) return;

  for (int i = 0; i < n_callhist_list &&
                  entry_list->nentry < entry_list->nmax_entry &&
                  entry_list->nentry < 10; ++i) {
    if (callhist_list[i] == NULL || callhist_list[i][0] == '\0') continue;
    char item[LEN_CALLSIGN + LEN_EXCH + 4];
    strlcpy(item, callhist_list[i], sizeof(item));
    char *sp = strchr(item, ' ');
    if (!sp) continue;
    *sp++ = '\0';
    while (*sp == ' ') ++sp;
    const bool exact_match = dupe_callsign_equal(item, call);
    if (strstr(item, call) == NULL && !exact_match) continue;

    bool duplicate = false;
    for (int j = 0; j < entry_list->nentry; ++j) {
      if (strcmp(entry_list->entryl[j].callsign, item) == 0) {
        duplicate = true; break;
      }
    }
    if (duplicate) continue;

    struct check_entry *e = &entry_list->entryl[entry_list->nentry++];
    memset(e, 0, sizeof(*e));
    strlcpy(e->callsign, item, sizeof(e->callsign));
    strlcpy(e->exch, sp, sizeof(e->exch));
    e->flag = CHECK_ENTRY_FLAG_CALLHIST_LIST;
    if (exact_match) e->flag |= CHECK_ENTRY_FLAG_EXACT_MATCH;
  }
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

  uint32_t sub_search_us = 0;
  unsigned int qso_scanned = 0, hist_scanned = 0;
  char *first_entry = strtok_r(NULL, "|", &saveptr);
  if (first_entry && strncmp(first_entry, "T=", 2) == 0) {
    unsigned long total_us = 0, hist_us = 0;
    sscanf(first_entry + 2, "%lu,%u,%lu,%u", &total_us, &qso_scanned,
           &hist_us, &hist_scanned);
    sub_search_us = (uint32_t)total_us;
    first_entry = NULL;
  }

  if (dupechk->dupechk_status != 1 ||
      query_id != dupechk->dupechk_query_id ||
      dupechk_partial_entry_list == NULL) {
    if (verbose & 4) console->println("ignored stale dupepr response");
    return;
  }

  struct check_entry_list *entry_list = dupechk_partial_entry_list;
  entry_list->nentry = 0;
  entry_list->dupe = ndupe;

  while (entry_list->nentry < count && entry_list->nentry < 10) {
    if (first_entry != NULL) { tok = first_entry; first_entry = NULL; }
    else tok = strtok_r(NULL, "|", &saveptr);
    if (tok == NULL) break;
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

  // When CALLHISTSUB could not fit and fell back to MAIN-PSRAM, the SUBCPU
  // response contains QSO-history matches only.  Merge MAIN Call History here
  // so normal 3-character partial lookup behaves the same in either placement.
  append_main_callhist_partial(dupechk_async_call, entry_list);

  dupechk_log_timing("partial", query_id, sub_search_us, qso_scanned,
                     hist_scanned, false);
  dupechk->dupechk_status = 0;
  dupechk_remote_query_succeeded();

  if (dupechk_async_active && entry_list == &dupechk_async_radio->check_entry_list) {
    struct radio *radio = dupechk_async_radio;
    bool still_current = strcmp(radio->callsign + 2, dupechk_async_call) == 0 &&
                         bandmode(radio) == dupechk_async_bandmode &&
                         plogw->mask == dupechk_async_mask;
    bool pending_send = dupechk_async_pending_sp_send;
    dupechk_async_pending_sp_send = false;
    dupechk_async_active = false;
    dupechk_async_result_valid = still_current;
    dupechk_partial_entry_list = NULL;

    if (still_current) {
      radio->dupe = ndupe > 0 ? 1 : 0;
      strncpy(radio->callsign_prev + 2, dupechk_async_call, LEN_CALLSIGN);
      radio->callsign_prev[LEN_CALLSIGN + 2] = '\0';

      // Use a completely matching history entry only while EXCH is untouched.
      if (radio->recv_exch[2] == '\0') {
        for (int i = 0; i < entry_list->nentry; i++) {
          struct check_entry *e = &entry_list->entryl[i];
          if ((e->flag & CHECK_ENTRY_FLAG_EXACT_MATCH) && e->exch[0]) {
            strncpy(radio->recv_exch + 2, e->exch, LEN_EXCH);
            radio->recv_exch[LEN_EXCH + 2] = '\0';
            radio->recv_exch[1] = strlen(radio->recv_exch + 2);
            radio->multi = multi_check(radio->recv_exch + 2, radio->bandid);
            break;
          }
        }
      }
      if (plogw->f_partial_check & PARTIAL_CHECK_CALLSIGN_AUTO)
        display_partial_check(radio);
      upd_display();

      // Enter is an operator command.  Once the matching result is known,
      // send regardless of whether the station is DUPE; keep radio->dupe set
      // so the display and logging state still show the correct judgement.
      if (pending_send && radio->cq[radio->modetype] == LOG_SandP)
        ui_send_mycall(radio);
    }
  }
}

// only check dupe no call history retrieval
bool dupe_check_nocallhist(const char *call, byte bandmode, byte mask) {
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

// Check for a duplicate and, independently, obtain an exact-match exchange.
// The exchange is taken from the newest QSO first, then from CALLHIST.
bool dupe_check_with_exch(const char *call, byte bandmode, byte mask,
                          char *exch, size_t exch_size) {
  if (exch && exch_size) exch[0] = '\0';
  if (!call || !*call) return false;

  if (dupechk->dupechk_at == 1) {
    const bool dupe = query_dupechk_subcpu(call, bandmode, mask, true);
    if (exch && exch_size && dupechk->dupechk_getexch) {
      strncpy(exch, dupechk->dupechk_exch, exch_size - 1);
      exch[exch_size - 1] = '\0';
    }
    return dupe;
  }

  bool dupe = false;
  bool have_exch = false;
  for (int i = dupechk->ncallsign - 1; i >= 0; i--) {
    if (!dupe_callsign_equal(dupechk->callsign[i], call)) continue;
    if (!have_exch && exch && exch_size && dupechk->exch[i][0]) {
      strncpy(exch, dupechk->exch[i], exch_size - 1);
      exch[exch_size - 1] = '\0';
      have_exch = true;
    }
    if ((dupechk->bandmode[i] & mask) == (bandmode & mask)) dupe = true;
    if (dupe && have_exch) break;
  }

  if (!have_exch && exch && exch_size && plogw->enable_callhist &&
      callhist_at == 0) {
    char callbuf[LEN_CALLSIGN + 1];
    strncpy(callbuf, call, LEN_CALLSIGN);
    callbuf[LEN_CALLSIGN] = '\0';
    if (search_callhist_getexch(callbuf, exch)) {
      exch[exch_size - 1] = '\0';
    }
  }
  return dupe;
}

bool dupe_check_with_exch_confirmed(const char *call, byte bandmode,
                                     byte mask, char *exch,
                                     size_t exch_size, bool *is_dupe) {
  if (is_dupe) *is_dupe = false;
  if (exch && exch_size) exch[0] = '\0';
  if (!call || !*call) return false;

  if (dupechk->dupechk_at == 1) {
    const uint32_t now = millis();
    // A single query slot is shared with interactive partial checking.  Do not
    // overwrite an in-flight operator query; the cluster spot may be safely
    // discarded and a later spot will refresh it.
    if (dupechk->dupechk_status == 1) return false;
    if (dupechk_cluster_backoff_until != 0 &&
        (int32_t)(now - dupechk_cluster_backoff_until) < 0) return false;
    if (!dupechk_remote_query_allowed()) return false;

    dupechk_cluster_query_active = true;
    const bool dupe = query_dupechk_subcpu(call, bandmode, mask, true);
    dupechk_cluster_query_active = false;
    if (!dupechk_exact_response_confirmed) return false;
    if (is_dupe) *is_dupe = dupe;
    if (exch && exch_size && dupechk->dupechk_getexch) {
      strncpy(exch, dupechk->dupechk_exch, exch_size - 1);
      exch[exch_size - 1] = '\0';
    }
    return true;
  }

  const bool dupe = dupe_check_with_exch(call, bandmode, mask,
                                          exch, exch_size);
  if (is_dupe) *is_dupe = dupe;
  return true;
}

// Process "id|call|bandmode|mask|want_exch" on the subcpu.
void process_dupechk_query_subcpu(char *s) {
  unsigned int query_id, bandmode, mask, want_exch;
  char callsign[LEN_CALLSIGN + 1];
  char response[96];
  char exch[LEN_EXCH + 1] = "";
  int dupe = 0;
  int has_exch = 0;

  if (sscanf(s, "%u|%16[^|]|%u|%u|%u",
             &query_id, callsign, &bandmode, &mask, &want_exch) != 5) {
    snprintf(response, sizeof(response), "duper:0 0 0 -");
    mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                           (unsigned char *)response, strlen(response));
    return;
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
    if (!ret && f_callhist && callhist_check && plogw->enable_callhist && callhist_at == 0) {
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
  makedupe_sent_count++;
}

void begin_makedupe_subcpu(unsigned char mask) {
  char buf[32];
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  makedupe_done_ack = false;
  makedupe_score_received[0] = false;
  makedupe_score_received[1] = false;
  makedupe_sent_count = 0;
  makedupe_accepted_count = 0;
  snprintf(buf, sizeof(buf), "dupebulkbegin%u", (unsigned int)mask);
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)buf, strlen(buf));
}

void process_makedupe_diag_maincpu(char *s) {
  unsigned long rx = 0, accepted = 0, duplicate = 0;
  unsigned long malformed = 0, overflow = 0, invalid = 0;
  int n = sscanf(s, "%lu,%lu,%lu,%lu,%lu,%lu",
                 &rx, &accepted, &duplicate,
                 &malformed, &overflow, &invalid);
  if (n == 6) {
    console->printf(
      "MAKEDUPE SUBCPU diag rx=%lu accepted=%lu duplicate=%lu "
      "malformed=%lu overflow=%lu invalid=%lu\n",
      rx, accepted, duplicate, malformed, overflow, invalid);
    if (rx != makedupe_sent_count) {
      console->printf(
        "MAKEDUPE WARNING: MAIN sent=%lu but SUBCPU received=%lu "
        "(missing=%ld)\n",
        (unsigned long)makedupe_sent_count, rx,
        (long)makedupe_sent_count - (long)rx);
    }
  } else {
    console->printf("MAKEDUPE SUBCPU diag parse error: <%s>\n", s);
  }
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

static bool makedupe_finish_pending = false;
static uint32_t makedupe_finish_timeout = 0;

void start_finish_makedupe_subcpu() {
  if (dupechk == NULL || dupechk->dupechk_at != 1) {
    makedupe_finish_pending = false;
    return;
  }
  makedupe_done_ack = false;
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"dupebulkend", strlen("dupebulkend"));
  makedupe_finish_timeout = millis() + 5000;
  makedupe_finish_pending = true;
}

bool poll_finish_makedupe_subcpu() {
  if (!makedupe_finish_pending) return true;
  if (f_mux_transport) mux_transport.recv_pkt();
  if (!makedupe_done_ack &&
      (int32_t)(millis() - makedupe_finish_timeout) < 0)
    return false;

  makedupe_finish_pending = false;

  uint32_t worked_total = 0;
  for (int group = 0; group < 2; group++) {
    for (int bandid = 1; bandid < N_BAND; bandid++)
      worked_total += score.worked[group][bandid - 1];
  }

  bool valid = makedupe_done_ack &&
               worked_total == makedupe_accepted_count &&
               makedupe_accepted_count <= makedupe_sent_count;
  console->printf("MAKEDUPE summary sent=%lu accepted=%lu worked=%lu done=%u valid=%u\n",
                  (unsigned long)makedupe_sent_count,
                  (unsigned long)makedupe_accepted_count,
                  (unsigned long)worked_total,
                  makedupe_done_ack ? 1U : 0U, valid ? 1U : 0U);
  if (!valid) {
    console->println("MAKEDUPE invalid: clearing partial score/multiplier result");
    memset(score.worked, 0, sizeof(score.worked));
    memset(score.nmulti, 0, sizeof(score.nmulti));
    clear_multi_worked();
  }
  return true;
}

void finish_makedupe_subcpu() {
  if (dupechk == NULL || dupechk->dupechk_at != 1) return;
  start_finish_makedupe_subcpu();
  while (!poll_finish_makedupe_subcpu()) delay(1);
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

static int dupechk_reset_remote_ncallsign = -1;

void notify_dupechk_subcpu_reset(int remote_ncallsign) {
  dupechk_reset_remote_ncallsign = remote_ncallsign;
  dupechk_reset_ack = true;
}

bool reset_dupechk_subcpu() {
  if (dupechk == NULL || dupechk->dupechk_at != 1) return false;

  dupechk_reset_ack = false;
  dupechk_reset_remote_ncallsign = -1;
  const uint32_t reset_started = millis();
  mux_transport.send_pkt(MUX_PORT_MAIN_BRD_CTRL, MUX_PORT_EXT_BRD_CTRL,
                         (unsigned char *)"dupereset", strlen("dupereset"));

  // init_dupechk_subcpu() frees/reallocates and clears the complete remote
  // database.  Do not start MAKEDUPE until SUBCPU explicitly reports that
  // this work has finished.  Allow ample time on a busy SUBCPU.
  uint32_t timeout = millis() + 5000;
  while (!dupechk_reset_ack && (int32_t)(millis() - timeout) < 0) {
    if (f_mux_transport) mux_transport.recv_pkt();
    delay(1);
  }

  if (!dupechk_reset_ack) {
    console->printf("reset_dupechk_subcpu() timeout after %lu ms\n",
                    (unsigned long)(millis() - reset_started));
    return false;
  }
  console->printf("SUBCPU DUPE reset done: ncallsign=%d elapsed=%lu ms\n",
                  dupechk_reset_remote_ncallsign,
                  (unsigned long)(millis() - reset_started));
  if (dupechk_reset_remote_ncallsign != 0) {
    console->println("SUBCPU DUPE reset invalid: ncallsign is not zero");
    return false;
  }
  return true;
}

void entry_dupechk_call_exch_bandmode(char *callsign,char *recv_exch,unsigned char bandmode) {
#ifdef DVPLOGGER_EXT
  invalidate_dupe_exact_cache();
#endif
  // entry current qso into dupecheck
  if (dupechk->ncallsign < dupechk->nmaxqso) {
    //
    int i;
    i = dupechk->ncallsign;
    strcpy(dupechk->callsign[i], callsign);
    strcpy(dupechk->exch[i], recv_exch );
    dupechk->bandmode[i] = bandmode;
    dupechk->ncallsign++;
  }
}

#ifdef DVPLOGGER_EXT
void begin_makedupe_bulk_subcpu(unsigned char mask) {
  dupechk_current_mask = mask;
  makedupe_bulk_mask = dupechk_current_mask;
  memset(makedupe_bulk_worked, 0, sizeof(makedupe_bulk_worked));
}

void entry_makedupe_bulk_subcpu(char *s) {
  char callsign[LEN_CALLSIGN + 1];
  char recv_exch[LEN_EXCH + 1];
  char *sep1 = strchr(s, '|');
  char *sep2 = sep1 ? strchr(sep1 + 1, '|') : NULL;
  int bandmode;
  if (!sep1 || !sep2) return;
  *sep1 = '\0';
  *sep2 = '\0';
  bandmode = atoi(sep2 + 1);
  if (strlen(s) > LEN_CALLSIGN || strlen(sep1 + 1) > LEN_EXCH ||
      bandmode < 0 || bandmode > 255) return;

  strncpy(callsign, s, LEN_CALLSIGN);
  callsign[LEN_CALLSIGN] = '\0';
  strncpy(recv_exch, sep1 + 1, LEN_EXCH);
  recv_exch[LEN_EXCH] = '\0';

  for (int i = 0; i < dupechk->ncallsign; i++) {
    if (((dupechk->bandmode[i] & makedupe_bulk_mask) ==
         (((unsigned char)bandmode) & makedupe_bulk_mask)) &&
        dupe_callsign_equal(dupechk->callsign[i], callsign)) {
      return;
    }
  }

  if (dupechk->ncallsign >= dupechk->nmaxqso) return;
  entry_dupechk_call_exch_bandmode(callsign, recv_exch,
                                   (unsigned char)bandmode);

  // Tell MAIN which QSO was accepted.  MAIN owns the contest multiplier
  // definitions, so it performs multiplier accounting from this response.
  char accepted[48];
  snprintf(accepted, sizeof(accepted), "dupebulka:%u|%.*s",
           (unsigned int)bandmode, LEN_EXCH, recv_exch);
  mux_transport.send_pkt(MUX_PORT_EXT_BRD_CTRL, MUX_PORT_MAIN_BRD_CTRL,
                         (unsigned char *)accepted, strlen(accepted));

  int bandid = bandmode / 4;
  int modetype = bandmode % 4;
  if (bandid >= 1 && bandid < N_BAND) {
    makedupe_bulk_worked[modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;
  }
}

void finish_makedupe_bulk_subcpu() {
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
  init_dupechk(NMAXQSO_SUBCPU,2);
}

void task_dupechk()
{
  // main loop task to check timeout
  if (dupechk->dupechk_status == 1) {
    // now querying
    if ((int32_t)(millis() - dupechk->dupechk_timeout) >= 0) {
      // timeout reached
      const bool cluster_background = dupechk_cluster_query_active;
      if (!cluster_background) {
        dupechk_timeout_streak++;
        dupechk_remote_unavailable = true;
      } else {
        // Avoid a tight retry loop while preserving the operator-facing DUPE
        // state.  Cluster spots during this short interval are discarded.
        dupechk_cluster_backoff_until = millis() + 2000U;
      }
      Serial.printf(cluster_background ?
                    "DUPE BACKGROUND TIMEOUT kind=%c id=%u call=%s age=%lu ms operator=%u\n" :
                    "DUPE TIMEOUT kind=%c id=%u call=%s age=%lu ms operator=%u\n",
                    dupechk_query_kind, (unsigned int)dupechk->dupechk_query_id,
                    dupechk_query_call,
                    (unsigned long)(dupechk_query_create_us ?
                      (uint32_t)(micros() - dupechk_query_create_us) / 1000U : 0U),
                    dupechk_query_is_operator_entry ? 1U : 0U);
      if (!cluster_background) {
        dupechk_show_remote_error();
        if (dupechk_timeout_streak >= 2) {
          dupechk_breaker_until = millis() + 10000;
          console->println("task_dupechk() timeout: remote dupe check paused 10 s");
        } else {
          console->println("task_dupechk() timeout");
        }
      } else {
        console->println("task_dupechk() background timeout: cluster checks paused 2 s");
      }
      dupechk->dupechk_status=0; // reset
      dupechk->dupechk_timeout=0;
      dupechk->dupechk_dupe=0; // communication failure is not a real dupe
      dupechk_query_is_operator_entry = false;
      if (dupechk_async_active) {
        dupechk_async_active = false;
        dupechk_async_result_valid = false;
        dupechk_async_pending_sp_send = false;
        dupechk_partial_entry_list = NULL;
      }
    }
  }
}

void init_dupechk(int nmaxqso,int dupechk_at) {
  // dupechk_at == 1 means the actual database is on the SUBCPU.  The MAIN
  // side only needs one work/query entry, regardless of the requested size.
  if (dupechk_at == 1) {
    nmaxqso = 1;
  }

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
  dupechk_timeout_streak = 0;
  dupechk_breaker_until = 0;
  dupechk_remote_unavailable = false;
  dupechk_last_lcd_warning = 0;
#ifdef DVPLOGGER_EXT
  invalidate_dupe_exact_cache();
#endif
  
  for (int i = 0; i < nmaxqso; i++) {
    strcpy(dupechk->callsign[i], "");
    strcpy(dupechk->exch[i], "");    
    dupechk->bandmode[i] = 0;
  }
  dupechk->ncallsign = 0;
}
