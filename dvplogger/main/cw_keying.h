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

#ifndef FILE_CW_KEYING_H
#define FILE_CW_KEYING_H
extern int f_cw_code_type; // 0: english  1: japanese (wabun)
extern int cw_bkin_state; // break in state used in tone_keying -1 TX OFF non_zero:TX ON timeout counter if zero make TX OFF
extern char cw_send_current ;
extern int cw_send_update ;


#define CW_MSCMD_SETRX1 8000
#define CW_MSCMD_SETRX1_CHR '!'
#define CW_MSCMD_SETRX2 8001
#define CW_MSCMD_SETRX2_CHR '@'
#define CW_MSCMD_SETRX3 8002
#define CW_MSCMD_SETRX3_CHR '#'
#define CW_MSCMD_END_OF_MSG 8003
#define CW_MSCMD_END_OF_MSG_CHR '$'
#define CW_MSCMD_RTTY_STX_CHR '['
#define CW_MSCMD_RTTY_STX 8010
#define CW_MSCMD_RTTY_ETX 8011
#define CW_MSCMD_RTTY_ETX_CHR ']'
#define CW_MSCMD_TONEKEY_ON 8020
#define CW_MSCMD_TONEKEY_OFF 8021



void set_tone(int note,int on);
void set_tone_keying(struct radio *radio);
void keying(int on);
void set_manual_cw_radio(int radio_idx);
void clear_manual_cw_radio();
int effective_cw_radio();
void interrupt_cw_send() ;
void init_cw_keying();
int send_the_dits_and_dahs(const char *cw_to_send) ;
void send_bits(byte code, int fig, int *figures) ;
void send_baudot(byte ascii, int *figures);
void RTTYbaudotDiagReset();
void RTTYbaudotDiagDump(Print *out);
void send_char(byte cw_char, byte omit_letterspace) ;
void set_tx_to_focused() ;
void set_tx_to_manual_radio(int radio_idx);
void cancel_current_tx_message() ;
char append_cwbuf_convchar(char c);
void append_cwbuf(char c) ;
char *cw_num_abbreviation(char *s, int level);
void set_rttymemory_string(struct radio *radio, int num, char *s);
void set_rttymemory_string_buf(char *s) ;
char *power_code(int bandid);
// char *expand_macro_string(char *p,char *s) ; // expand macro string s to p older
char *expand_macro_string(char *p,size_t p_size, const char *s) ; // expand macro string s to p
void append_cwbuf_string(const char *s) ;
void append_rtty_test1(char ch, int n);
void append_rtty_test_text(const char *s);
int cw_wptr_cw_send_buf_previous() ;
void delete_cwbuf();
void cancel_keying(struct radio *radio); // here radio indicates currently transmitting radio
void clear_cwbuf() ;
void display_cw_buf_lcd(char *buf) ;
void display_cwbuf() ;
#define CW_SPEED_MIN_WPM 10
#define CW_SPEED_MAX_WPM 60

extern int cw_spd;  // wpm
void clamp_cw_speed();
extern int cw_dah_ratio_bunshi ;
extern int cw_ratio_bunbo ;
extern int cw_duty_ratio ;
extern int f_so2r_chgstat_tx ;  // nonzero if changing so2r transmit requested
extern int f_so2r_chgstat_rx ;  // nonzero if changing so2r receive requested
extern int f_transmission ;     // 0 nothing   1 force transmission on active trx 2
extern int cw_count_ms;          // CW/RTTY scheduler countdown (ms)
extern volatile bool f_rtty_usb_lead_pending; // USB RTTY lead waits for PTT ON in main loop
//Ticker cw_sender, civ_reader;
#endif
