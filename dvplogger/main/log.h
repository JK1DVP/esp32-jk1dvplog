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

#ifndef  FILE_LOG_H
#define  FILE_LOG_H
void print_band_mask(struct radio *radio);
int freq2bandid(unsigned int freq) ;
extern const char *band_str[N_BAND];
extern const char *band_str_adif[N_BAND];
int modeid_string(const char *s) ;
int modetype_string(const char *s) ;
const char *switch_rigmode() ;
void wipe_log_entry_radio(struct radio *radio) ;

void wipe_log_entry() ;
void new_log_entry();
void set_log_rst(struct radio *radio) ;
void enable_radios(int idx_radio, int radio_cmd) ;
void switch_bands(struct radio *radio) ;
void band_change(int bandid, struct radio *radio);
void band_change_from(int bandid, struct radio *radio, const char *source);
void switch_bands_from(struct radio *radio, const char *source);
void init_logwindow() ;
void display_partial_check(struct radio * radio);
int exch_partial_check(struct radio *radio,char *exch,unsigned char bandmode,unsigned char mask,int callhist_check,struct check_entry_list *entry_list);
int dupe_partial_check(const char *call,unsigned char bandmode,unsigned char mask, int callhist_check,struct check_entry_list *entry_list);
int dupe_callhist_check(const char *call,unsigned char bandmode, unsigned char mask,int callhist_check, char **exch_history) ;

#endif
