#ifndef FILE_CW_KEYING_H
#define FILE_CW_KEYING_H
extern int f_cw_code_type; // 0: english  1: japanese (wabun)
extern int cw_bkin_state; // break in state used in tone_keying -1 TX OFF non_zero:TX ON timeout counter if zero make TX OFF
void set_tone(int note,int on);
void set_tone_keying(struct radio *radio);
void keying(int on);
void interrupt_cw_send() ;
void init_cw_keying();
int send_the_dits_and_dahs(char *cw_to_send) ;
void send_bits(byte code, int fig, int *figures) ;
void send_baudot(byte ascii, int *figures);
void send_char(byte cw_char, byte omit_letterspace) ;
void set_tx_to_focused() ;
void cancel_current_tx_message() ;
void append_cwbuf(char c) ;
char *cw_num_abbreviation(char *s, int level);
void set_rttymemory_string(struct radio *radio, int num, char *s);
void set_rttymemory_string_buf(char *s) ;
char *power_code(int bandid);
void append_cwbuf_string(char *s) ;
int cw_wptr_cw_send_buf_previous() ;
void delete_cwbuf();
void clear_cwbuf() ;
void display_cw_buf_lcd(char *buf) ;
void display_cwbuf() ;
extern int cw_spd;  // wpm
extern int cw_dah_ratio_bunshi ;
extern int cw_ratio_bunbo ;
extern int cw_duty_ratio ;
extern int f_so2r_chgstat_tx ;  // nonzero if changing so2r transmit requested
extern int f_so2r_chgstat_rx ;  // nonzero if changing so2r receive requested
extern int f_transmission ;     // 0 nothing   1 force transmission on active trx 2
//Ticker cw_sender, civ_reader;
#endif
