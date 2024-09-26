#ifndef  FILE_LOG_H
#define  FILE_LOG_H
void print_band_mask(struct radio *radio);
int freq2bandid(unsigned int freq) ;
extern const char *band_str[N_BAND];
int modeid_string(const char *s) ;
int modetype_string(const char *s) ;
const char *switch_rigmode() ;
void wipe_log_entry() ;
void new_log_entry();
void set_log_rst(struct radio *radio) ;
void enable_radios(int idx_radio, int radio_cmd) ;
void switch_bands(struct radio *radio) ;
void band_change(int bandid, struct radio *radio);
void init_logwindow() ;
void display_partial_check(struct radio * radio);
int exch_partial_check(char *exch,unsigned char bandmode,unsigned char mask,int callhist_check,struct check_entry_list *entry_list);
int dupe_partial_check(char *call,unsigned char bandmode,unsigned char mask, int callhist_check,struct check_entry_list *entry_list);
int dupe_callhist_check(char *call,unsigned char bandmode, unsigned char mask,int callhist_check, char **exch_history) ;

#endif
