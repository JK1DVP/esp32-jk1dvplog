#ifndef FILE_CAT_H
#define FILE_CAT_H
#include "Arduino.h"
#include "decl.h"
extern byte civ_buf[65];
extern int civ_buf_idx ;
byte dec2bcd(byte data) ;
byte bcd2dec(byte data) ;
int rig_modenum(const char *opmode) ;
char *opmode_string(int modenum) ;
void set_frequency_rig_radio(unsigned int freq, struct radio *radio) ;
void set_frequency_rig(unsigned int freq);
void print_cat_cmdbuf(struct radio *radio);
void attach_interrupt_civ();
void send_head_civ(struct radio *radio);
void send_tail_civ(struct radio *radio);
void set_ptt_rig(struct radio *radio, int on) ;
void send_voice_memory(struct radio *radio, int num);
void send_rtty_memory(struct radio *radio, int num);
void add_civ_buf(byte c) ;
void clear_civ_buf() ;
void send_civ_buf(Stream *civport);
void send_cat_cmd(struct radio *radio, char *cmd);
void receive_cat_data(struct radio *radio) ;
int freq_width_mode(char *opmode);
void set_scope() ;
void send_rit_setting(struct radio *radio, int rit, int xit) ;
void send_rit_freq_civ(struct radio *radio, int freq) ;
void send_freq_set_civ(struct radio *radio, unsigned int freq) ;
void send_mode_set_civ(const char *opmode, int filnr) ;
void send_freq_query_civ(struct radio *radio) ;
void send_mode_query_civ(struct radio *radio) ;
void send_rotator_head_civ(int from, int to) ;
void send_rotator_tail_civ() ;
void send_rotator_az_query_civ() ;
void send_rotator_az_set_civ(int az) ;
void send_rotator_command_civ(byte *cmds, int n) ;
void send_ptt_query_civ(struct radio *radio) ;
void send_preamp_query_civ(struct radio *radio) ;
void send_identification_query_civ(struct radio *radio) ;
void send_att_query_civ(struct radio *radio) ;
void send_smeter_query_civ(struct radio *radio) ;
void set_frequency(int freq, struct radio *radio) ;
void set_mode_nonfil(const char *opmode, struct radio *radio) ;
void set_mode(const char *opmode, byte filt, struct radio *radio) ;
void check_repeat_function() ;
void get_cat_kenwood(struct radio *radio) ;
void get_cat(struct radio *radio) ;
void conv_smeter(struct radio *radio) ;
void smeter_postprocess(struct radio *radio);
void get_civ(struct radio *radio) ;
void print_civ(struct radio *radio) ;
void print_cat(struct radio *radio) ;
extern int receive_civport_count; // debugging receive_civport
void receive_civport(struct radio *radio) ;
void receive_civport_1() ;
void clear_civ(struct radio *radio) ;
int antenna_switch_commands(char *cmd);
int signal_command(char *s);
int rotator_commands(char *s);
void RELAY_control(int relay, int on);
void switch_radios(int idx_radio, int radio_cmd);
void alternate_antenna_relay();
void select_rig(struct radio *radio) ;
void init_cat_kenwood() ;
void init_cat_serialport();
void init_rig() ;
void init_all_radio() ;
void print_radio_info(int idx_radio);
int check_rig_conflict(int rig_idx,struct rig *rig_spec);
void init_radio(struct radio *radio, const char *rig_name);
void set_rig_spec_from_str(struct radio *radio,char *s);
void set_rig_spec_str_from_spec(struct radio *radio); // reverse set rig_spec_string from rig_spec
void set_rig_from_name(struct radio *radio) ;
void switch_transverter() ;
void signal_process();
void civ_process() ;
int receive_civ(struct radio *radio) ;
int unique_num_radio(int i) ;
void Control_TX_process() ;
void rotator_sweep_process();
void set_sat_opmode(struct radio *radio, char *opmode) ;

void adjust_frequency(int dfreq);
int antenna_alternate_command(char *s);

void save_freq_mode_filt(struct radio *radio) ;
void recall_freq_mode_filt(struct radio *radio) ;
int bandid2freq(int bandid, struct radio *radio) ;
char *default_opmode(int bandid, int modetype) ;
int default_filt(char *opmode) ;


#endif
