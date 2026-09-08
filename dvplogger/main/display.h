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

#ifndef FILE_DISPLAY_H
#define FILE_DISPLAY_H
void init_display_dispatch();
void process_display_requests();
void request_dupe_aware_display_update();
void request_display_update_on_demand();
void request_bandmap_update_on_demand();
void process_dupe_aware_display_update();
bool display_is_main_loop();
void display_cw_buf_lcd(char *buf);
void select_left_display() ;
void select_right_display() ;
void display_printStr(const char *s, byte ycol) ;
void upd_display_info_flash(const char *s) ;
void upd_display_rtty_decoder(const char *s) ;
void upd_display_tm() ;
void upd_display_stat() ;
int upd_cursor_calc(int cursor, int wsize);
void upd_display_put_lcdbuf(char *s, int cursor, int wsize, int lcdpos) ;
void upd_cursor() ;
void upd_display_freq(unsigned int freq, char *opmode, int col) ;
void upd_display() ;
void right_display_sendBuffer();
void left_display_sendBuffer();
//void reset_display() ;
void init_display() ;
void upd_disp_rig_info() ;
void init_info_display() ;
void upd_display_info_signal();
void upd_display_info_contest_settings(struct radio *radio) ;
void upd_display_info_contest_band_nearby(struct radio *radio);
void upd_display_info_multi_nearby(struct radio *radio);
void upd_display_info_multi_bands(struct radio *radio);
void show_summary(Stream *out = nullptr) ;
void upd_display_info_to_work_bandmap() ;
void upd_display_info_qso(int option) ;
void upd_display_info() ;
void clear_display_emu(int side) ;
void upd_disp_info_qso_entry() ;
void upd_display_bandmap() ;
#endif
