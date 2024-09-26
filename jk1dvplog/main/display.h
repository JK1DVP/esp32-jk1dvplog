#ifndef FILE_DISPLAY_H
#define FILE_DISPLAY_H
void display_cw_buf_lcd(char *buf);
void select_left_display() ;
void select_right_display() ;
void display_printStr(char *s, byte ycol) ;
void upd_display_info_flash(char *s) ;
void upd_display_tm() ;
void upd_display_stat() ;
int upd_cursor_calc(int cursor, int wsize);
void upd_display_put_lcdbuf(char *s, int cursor, int wsize, int lcdpos) ;
void upd_cursor() ;
void upd_display_freq(unsigned int freq, char *opmode, int col) ;
void upd_display() ;
void right_display_sendBuffer();
void left_display_sendBuffer();
void init_display() ;
void upd_disp_rig_info() ;
void init_info_display() ;
void upd_display_info_signal();
void upd_display_info_contest_settings() ;
void upd_display_info_to_work_bandmap() ;
void upd_display_info_qso(int option) ;
void upd_display_info() ;
void clear_display_emu(int side) ;
void upd_disp_info_qso_entry() ;
void upd_display_bandmap() ;
#endif
