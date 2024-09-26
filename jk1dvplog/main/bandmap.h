#ifndef FILE_BANDMAP_H
#define FILE_BANDMAP_H
//#include "Arduino.h"
void print_bandmap_mask();
int compare_bandmap_entry(const void *a, const void *b) ;
int find_on_freq_bandmap(int bandid, unsigned int freq, int tolerance) ;
void sort_bandmap(int bandid) ;
int find_appropriate_radio(int bandid) ;
int select_appropriate_radio(int bandid) ;
int set_station_entry(struct radio *radio, char *station, unsigned int freq, const char *opmode);
int search_bandmap(int bandid, char *stn, int md) ;
int new_entry_bandmap(int bandid) ;
void delete_old_entry(int bandid, int life);
void delete_entry_bandmap() ;
void set_current_rig_priority() ;
void pick_entry_bandmap() ;
void pick_onfreq_station() ;
void init_bandmap_entry(struct bandmap_entry *p) ;
void init_bandmap() ;
void set_info_bandmap(int bandid, char *stn, int modeid, unsigned int ifreq, char *remarks);
void adjust_callsign(char *stn) ;
//void print_cluster_info(struct bandmap_entry *entry, int bandid, int idx );
//void get_info_cluster(char *s) ;
void register_current_callsign_bandmap() ;
void upd_display_bandmap_show_entry(struct bandmap_entry *p, int count) ;
int in_contest_frequency(int ifreq) ; 
extern const char *bandid_str[N_BAND];
double check_frequency(String s);
char *ltrim(const char *string) ;
char *rtrim(const char *string) ;
char *trim(const char *string) ;
void select_bandmap_entry(int dir) ;
void set_target_station_info(char *arg);
#endif
