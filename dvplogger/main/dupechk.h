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

#ifndef FILE_DUPECHK_H
#define FILE_DUPECHK_H

unsigned char bandmode(struct radio *radio) ;
unsigned char bandmode_param(int bandid,int modetype) ;
// Normalize only well-known portable suffixes for exact DUPE/CALLHIST matching.
// The original callsign used for display/logging is never modified.
bool normalize_dupe_callsign(const char *src, char *dst, size_t dst_size);
bool dupe_callsign_equal(const char *a, const char *b);
// Partial callsign match.  If pattern contains '-', each dash is one
// unknown character; otherwise preserve the historical substring match.
bool dupe_callsign_partial_match(const char *candidate, const char *pattern);
bool dupe_check_nocallhist(const char *call, byte bandmode, byte mask) ;
bool dupe_check_with_exch(const char *call, byte bandmode, byte mask,
                          char *exch, size_t exch_size);
bool dupe_check_with_exch_confirmed(const char *call, byte bandmode,
                                     byte mask, char *exch,
                                     size_t exch_size, bool *is_dupe);
void process_dupechk_query_subcpu(char *s);
void process_dupechk_partial_query_subcpu(char *s);
int query_dupechk_partial_subcpu(const char *call, byte bandmode, byte mask,
                                 struct check_entry_list *entry_list);
void process_dupechk_partial_response_maincpu(char *s);
void request_async_dupe_partial(struct radio *radio, bool include_partial);
// Assign a callsign from a non-keyboard source and always start the same
// DUPE/CALLHIST/partial-check path used by normal operator entry.
bool set_callsign_and_request_dupe(struct radio *radio, const char *callsign,
                                   bool include_partial);
bool request_sp_send_after_dupe(struct radio *radio);

bool dupe_check(struct radio *radio,char *call, byte bandmode, byte mask, bool callhist_check) ;
bool dupe_check_get_callhist(char *call, byte bandmode, byte mask, bool callhist_check,char *getexch,bool *f_getexch,bool *f_callhist);
void entry_dupechk_subcpu(char *s);
void entry_dupechk_call_exch_bandmode(char *callsign,char *recv_exch,unsigned char bandmode);
void entry_dupechk_data(const char *callsign, const char *recv_exch, unsigned char bandmode);
bool reset_dupechk_subcpu();
void notify_dupechk_subcpu_reset(int remote_ncallsign);
void sync_dupechk_mask_subcpu(unsigned char mask);
void set_dupechk_mask_subcpu(unsigned char mask);
unsigned char get_dupechk_mask_subcpu();
void begin_makedupe_bulk_subcpu(unsigned char mask);
void entry_makedupe_bulk_subcpu(char *s);
void finish_makedupe_bulk_subcpu();
void begin_makedupe_subcpu(unsigned char mask);
void finish_makedupe_subcpu();
void start_finish_makedupe_subcpu();
bool poll_finish_makedupe_subcpu();
void note_makedupe_accepted_maincpu();
bool dupechk_remote_query_pending();
bool dupechk_remote_ack_received();
// Non-blocking exact DUPE query for low-priority background jobs.
// start returns false while the single SUBCPU query slot is busy.
bool dupechk_background_exact_start(const char *call, byte bandmode, byte mask);
// poll returns true when the query has completed. confirmed distinguishes a
// real response from timeout/cancel; is_dupe is valid only when confirmed.
bool dupechk_background_exact_poll(bool *confirmed, bool *is_dupe);
void dupechk_background_exact_cancel();
void process_makedupe_score_maincpu(char *s, int group);
void process_makedupe_diag_maincpu(char *s);
void entry_makedupe_subcpu_data(const char *callsign, const char *recv_exch, unsigned char bandmode);
void entry_dupechk(struct radio *radio) ;
void init_score() ;
//void init_dupechk() ;

void init_dupechk_maincpu();
void init_dupechk_subcpu();
void init_dupechk(int nmaxqso,int iam);
void task_dupechk();
int get_dupechk_nmaxqso();
int get_dupechk_ncallsign();
//extern struct dupechk *dupechk;
void dupechk_log_timing(const char *phase, unsigned int query_id, uint32_t sub_search_us, unsigned int qso_scanned, unsigned int hist_scanned, bool cache_hit);

// Four-point MAIN-side timing hook: mark entry into the matching DUPE response
// handler before parsing/committing the result.
void dupechk_note_main_ack(unsigned int query_id);
void dupechk_note_main_rx();
void dupechk_note_exact_response_success(unsigned int query_id);
#endif
int reset_dupechk_subcpu_database();
