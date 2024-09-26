#ifndef FILE_VARIABLES_H
#define FILE_VARIABLES_H
#include "FS.h"
extern int main_loop_revs;
extern union qso_union_tag qso;  // data is delimited by space in the file
//#include "DS3231.h"
#include "RTClib.h"
extern class DateTime rtctime, ntptime;
extern File f;
extern char buf[512];
extern struct score score;

extern int verbose;  // debug info level
extern int f_printkey;
extern bool f_capslock;  // keyboard 
// to test suppress limiting JA temporalily
extern int timeout_rtc ;
extern int timeout_interval ;
extern int timeout_interval_minute ;  // interval job every minute
extern int timeout_interval_sat ;     // interval job for satellite orbit management
extern int timeout_cat ;
extern int timeout_second ;  // interval job every second

extern int f_show_clock  ;

extern const char *mode_str[NMODEID] ;
// mode code given from rig ci-v
extern int modetype[NMODEID] ;
extern const char *modetype_str[4] ;
/// call buffer
extern struct logwindow logw;
extern struct logwindow *plogw;

extern struct disp disp;
extern struct disp *dp;


extern struct sat_info sat_info[N_SATELLITES];
extern int n_satellites;
extern const char *sat_names[N_SATELLITES];
extern bool f_sat_updated ;


extern char tcp_ringbuf_buf[NCHR_TCP_RINGBUF];
// SO3R
extern struct radio radio_list[3], *radio_selected;
extern struct rig rig_spec[N_RIG];  // specification of the rig_id th rig

extern struct info_display info_disp;

extern const char *settingsfn ;
extern struct bandmap bandmap[N_BAND];
extern int bandmap_mask ;  // suppress updating bandmap from telnet cluster if the corresponding bit 1<<(bandid-1) is set
extern struct bandmap_disp bandmap_disp;

extern struct dupechk dupechk;
extern bool display_flip;
extern int wifi_timeout ;
extern int wifi_enable;
extern int wifi_count ;
extern int wifi_status;
extern int count ;
extern uint8_t *dispbuf_r, *dispbuf_l;
extern int enable_usb_keying;
extern int rtcadj_count;
extern int callhistf_stat ;  // 0 not open 1 open for reading 2 open for writing
extern char qsologfn[20];    // qso log filename (append)
extern char callhistfn[20];  // call history file to read

#endif
