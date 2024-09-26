/* Copyright (c) 2021-2024 Eiichiro Araki
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.
*/

#ifndef FILE_DECL_H
#define FILE_DECL_H
// decl.h
#include "ringbuf.h"
#define ErrorFloat 1.e30

#include "usb_host.h"
#include <WiFi.h>
//#include "DS3231.h"
#include "RTClib.h"
#define LOG_MODETYPE_CW 1
#define LOG_MODETYPE_PH 2
#define LOG_MODETYPE_DG 3
#define LOG_MODETYPE_UNDEF 0
#define NCHR_TCP_RINGBUF 1024
#define INFO_DISP_QSO 3  // qso log browsing
#define INFO_DISP_BANDMAP 2
#define INFO_DISP_FLASH 4  // flash information such as progress of receiving sat information etc...
#define INFO_DISP_CONTEST_SETTINGS 6
#define INFO_DISP_SAT 1      // satellite
#define INFO_DISP_SAT_AOS 5  // checking satellite time to next AOS
#define INFO_DISP_SIGNAL 7

#define ONEPPM 1.0e-6
#define ONEPPTM 1.0e-7
#define CHR_BS 0x2a
#define CHR_DEL 0x4C

#define PARTIAL_CHECK_CALLSIGN_AUTO 1


//#define N_BAND 12 // 1.9 -
#define N_BAND 14 // 1.9 - 
//#define N_MULTI 310 // Kantou
#define N_MULTI 1346 // ACAG

#define NBANDMODE (N_BAND*2)
#define NMODEID 7

//#define N_CONTEST 15
//#define N_CONTEST 16
#define N_CONTEST 20



//
//
#define NMAX_TRANSVERTER 3

#define NCHR_CLUSTER_CMD 150
struct cluster {
  int timeout; // control cluster info reception
  int timeout_alive; // if no information received for more than a minute, try to reconnect
  int stat;
  struct ringbuf ringbuf;
  char cmdbuf[NCHR_CLUSTER_CMD + 1];
  int cmdbuf_ptr; // number of char written in cmdbuf
  int cmdbuf_len;
};

#include <HardwareSerial.h>
// specification of rig hardware
struct rig {
  const char *name; // name of the rig
  int band_mask; // rig specific band mask
  int cat_type ; // 0 civ   1 cat (yaesu) 2 Kenwood 3 Manual(NO CAT)
  int civaddr;   // civ address
  //  HardwareSerial *civport; // civport &Serial1, ...
  Stream  * civport; // civport &Serial1, ...
  //  HardwareSerial  * civport; // civport &Serial1, ...  
  int civport_num; // serial hardware port number attached to the rig -1 USB 0 port0(console) 1(port1 BT) 2(port2 CIV) 3(port3 SER-TTL)
  // now *civport can be indipendent to the civport_num which is tied to the hardware port
  // now usually rig#0 is attached to Serial1 rig#1 is attached to Serial2 and rig#3 is attached to Serial3(Software Serial), and are configured to attached to the hardware pins for civport_num, on the select_rig
  bool civport_reversed; // true if the serial port is reversed
  int civport_baud; // baudrate of the civport if nonzero if negative flip polarity
  int cwport; // cw output port 0/1
  int rig_type ; // 0 IC-705 1 IC-9700 2 FT-991  3 QCX mini 4 Manual
  int pttmethod; // how to ptt 0 port 0/1, 2... civ
  int rig_spec_idx; // index number of the rig specification
  char rig_identification[6]; // rig identification number (although cat control share the same protocol (Yaesu/Kenwood/Icom) behavior of each rig differs, so receive ID by ID; command (yaesu) and store them here.)
  int transverter_enable[NMAX_TRANSVERTER];
  unsigned int  transverter_freq[NMAX_TRANSVERTER][4]; // list of RF min max IF min max frequencies;
  // not defined if any of frequencies defined as zero

};

//plogw->sat_freq_tracking_mode
#define SAT_RX_FIX 0
#define SAT_TX_FIX 1
#define SAT_SAT_FIX 2
#define SAT_NO_TRACK 3 // do not track 

// sat_vfo_mode の値によって、cat で受信したVFO の数値をsatのdn, up どの周波数に流すかフローを決める(　TX fix, RX fix , SAT fix も関係するが）
// また、計算したdn up 周波数をどのリグのどのVFOにセットするかも決まる。

//plogw->sat_vfo_mode  main or vfoa frequency is TX or RX
#define SAT_VFO_SINGLE_A_TX 0 // TX is radio_list[0] VFO A
#define SAT_VFO_SINGLE_A_RX 1 // RX is radio_list[0] VFO A
#define SAT_VFO_MULTI_TX_0 2  // TX is radio_list[0] RX [1]
#define SAT_VFO_MULTI_TX_1 3  // TX is           [1] RX [0]


// if two separate rig is set, VFOA here is regarded as MAIN RIG, and VFOB is regarded as SUB RIG


//  set_vfo_frequency_rig(plogw->up_f,SAT_TX);
#define SAT_TX 1
#define SAT_RX 0
#define VFO_A 0
#define VFO_B 1

#define LEN_CALLSIGN 12
#define LEN_EXCH 10
#define LEN_REMARKS 80

#define LEN_HOST_NAME 30
#define LEN_IP_ADDRESS (3*4+3+2)
#define LEN_CALL_WINDOW (LEN_CALLSIGN+1)
#define LEN_EXCH_WINDOW (LEN_EXCH+1)
#define LEN_CONTEST_NAME 20
#define LEN_RST_WINDOW 3
#define LEN_SATNAME_WINDOW 8
#define LEN_RIG_NAME 20
#define LEN_RIG_SPEC_STR 40
#define LEN_POWER_WINDOW 13
#define LEN_CWMSG_WINDOW 30
#define LEN_EMAIL_ADDR_WINDOW 40
#define LEN_WIFI_SSID_WINDOW 30
#define LEN_WIFI_PASSWD_WINDOW 30

//#define LEN_REMARKS_WINDOW 37
#define LEN_REMARKS_WINDOW (LEN_REMARKS+1)
#define LEN_CLUSTER_CMD 120
#define N_CWMSG 7 // cw function key message F1-F7

#define LOG_CQ 1
#define LOG_SandP 0

//#define N_RIG 9
#define N_RIG 13
//#define N_RIG 5
// number of radios in operation so3r = 3 
#define N_RADIO 3

struct check_entry {
  char callsign[30];
  char exch[20];
  unsigned char bandmode; // when checked in dupecheck_list bandmode entry is copied to show worked band
  int  flag; // 1: DUPE 2:DUPECHECK_LIST 4:CALLHIST_LIST 8:EXACT_MATCH  16: DISPLAYED (call already displayed previously-> skip display)
} ;

struct check_entry_list {
  struct check_entry entryl[10];
  int nmax_entry; 
  int nentry;
  int dupe; // set dupe check result
  int cursor; // currently selected entry
};

#define CHECK_ENTRY_FLAG_DUPECHECK_LIST 4
#define CHECK_ENTRY_FLAG_EXACT_MATCH 8
#define CHECK_ENTRY_FLAG_DISPLAYED 16
#define CHECK_ENTRY_FLAG_DUPE 1
#define CHECK_ENTRY_FLAG_CALLHIST_LIST 2


// frequency unit (need to be 10 to allow  frequency > 2.4G)
#define FREQ_UNIT 10

#define F_QSL_NOQSL 0
#define F_QSL_JARL 1
#define F_QSL_HQSL 2

struct radio {
  // the following entry need to be defined per radio
  char callsign[LEN_CALL_WINDOW + 3];
  char sent_rst[LEN_RST_WINDOW + 3];
  char recv_rst[LEN_RST_WINDOW + 3];
  char recv_exch[LEN_EXCH_WINDOW + 3];
  char rig_name[LEN_RIG_NAME + 3];
  char rig_spec_str[LEN_RIG_SPEC_STR + 3];
  char remarks[LEN_REMARKS_WINDOW + 3]; // remarks

  char callsign_prev[LEN_CALL_WINDOW + 3]; // store callsign checked previously

  int f_qsl; // 0 N(OQSL) 1:J(arl) 2:hQSL

  char tm_loaded[20]; // time of the QSO loaded from SD
  int seqnr_loaded; // sequential number of the QSO loaded from SD
  
  char opmode_loaded[6];
  unsigned int freq_loaded;

  int qsodata_loaded ; // if set, indicates QSO data is loaded from previous data for editing
  // Ctrl-'-' will load the qso data
  // callsign sent_rst recv rst recv exch remarks are editable
  // freq, mode, time, seqnr is shown for the previous data not editable
  // rig number on the left of time indicates 'E' for editing
  // on confirming editing (Enter), remarks adds "*EDIT" flag and saved to SD memory
  // wiping (Ctrl-W) will clear the flag and go back to normal editing
  // any operation that change QSO data pointer will overwrite target QSO data and discard previous editing

  int att,preamp; // preamp and attenetour status 

  int ptr_curr;  // which is currently editing 0: callsign, 1: recv_exch 2: sent_exch 3:recv_rst, 4:sent_rst 5:freq 6: opmode 7: my_callsign

  char callsign_previously_sent[LEN_CALL_WINDOW + 3]; // check sent callsign on ; button and if the callsign changed after last sent, resend call before TU

  int keyer_mode; // 1 key type sent to cw send buffer 0 sent to current editing buffer

  int f_freqchange_pending; // set when frequency change attempt from program is ongoing
  int bandid; // bandid for the current frequency from 1
  int bandid_prev;
  
  int bandid_bandmap;// current bandid for the bandmap display

  int band_mask; // corresponding band of set bit is inhibited to switch (apply bandmap selection to select radio and manually switch band )
  int band_mask_priority; // if band_mask_priority bit is set this rig is priotized to focus in the band when selected in the bandmap 

  unsigned int freqbank[N_BAND][2][4]; // current operating frequency for cq 1 cq 0 s&p and modetype  mode 1 cw 2 ph 3 digi 0 not defined
  unsigned int cq_bank[N_BAND][4]; // state of cq/sp (1bit) in each band and modetype (2bit) 
  unsigned int modetype_bank[N_BAND]; // state of modetype (2bit) in each band  
  int filtbank[N_BAND][2][4]; // current filter setting for the mode
  int modebank[N_BAND][2][4];

  unsigned int freq, freq_prev;
  unsigned int freq_target;
  int freqchange_timer;
  int freqchange_retry;
  int transverter_in_use;


  int ptt; // tx 1 from cat
  int ptt_stat ; // 2 if transmitting from rig PTT   
  int ptt_stat_prev; // to check previous status of ptt in ci-v/cat function
  // these interractive buffer the first byte is current editing pointer, second byte size of the buffer from the third byte contents

  char opmode[6];
  int filt; // current filter
  int dupe; // set to 1 when dupe
  struct check_entry_list check_entry_list; // partial and dupe check 

  bool cq[4]; // current operation 1 cq 0 s&p      /// cq[modetype]

  int multi; // multiplier id of the qso (start from zero)
  int modetype; // operation mode type 0 not defined (initial state) 1 cw 2 ph  3 digi
  int modetype_prev; // to check modetype change

  // s meter reading
  int smeter; int smeter_peak;
  int smeter_stat; // 1 obtain peak of the smeter reading
  int smeter_record[4]; // composite record of s-meter reading in dBm and azimuth (if available) index is status of relay relay1+relay2*2
  int smeter_azimuth[4]; // -1 if not available rotator
  char f_smeter_record_ready; // 1 when a new smeter record is ready 

  // f2a and rtty afsk tone  when 1 
  bool f_tone_keying; 


  int rig_spec_idx; // index number of the rig specification
  int rig_idx; // main rig(left/up) 0 sub rig(right/down) 1
  
  struct rig *rig_spec;

  int enabled ; // enabled cat communication
  int f_civ_response_expected;
  int civ_response_timer;

  // buffer for radio control ci-v and cat
  char *bt_buf;
  int r_ptr = 0, w_ptr = 0;
  char cmdbuf[256];
  int cmd_ptr = 0;

};


#define NCHR_TCP_CMD 150

#define RADIO_MODE_SAT 1
#define RADIO_MODE_SO1R 0
#define RADIO_MODE_SO2R 2


#define SMETER_MINIMUM_DBM -2000
#define SMETER_UNIT_DBM 10    // corresponding to 1 dBm 


struct logwindow {
  Stream *ostream ; // Serial or tcp stream to print infos .

  char relay[2]; // antenna relay status 
  char f_antalt; // if 1 alternate antenna relay upon receiving rig signal strength of f_antalt readings.
  int  antalt_count;
  char f_antalt_switched; // 1 when switched antenna (in order not to use the next reading. Reset zero when read the first. 
  
  
  int show_smeter; // if set, show Smeter value (or dBm) on the number of QSO area

  int show_qso_interval; // if set show qso_interval second
  int qso_interval_timer; 
  
  // repeat function key operation 関係
  int repeat_func_key ;
  int repeat_func_radio; // memory which radio when started repeat function
  int f_repeat_func_stat; // ctrl-Func (or Shift-Func) to start repeat function  ESC to stop.  0 not set 1 repeat func triggered 2 repeat time expired  3 requested next repeat function command 
  int repeat_func_timer, repeat_func_timer_set; 
  MODIFIERKEYS repeat_func_key_modifier; // repeat func設定した際のmodifier を憶えておく。
  
 // f_repeat_func_stat: 0 repeat しない。 1 repeat してCQ 出している。 2 CQ 出した後受信に移ったところ。 3 次のrepeat function をトリガするように。 
 // 何かkey input がrecv にあるとrepeat 止める。
 //  repeat_func_timer_set*100m sec タイマー。 repeat の場合、cq 送出時に終了時にタイマー(repeat_func_timer)を起動するための特別の文字($)を挿入し、CW送出字にタイマーを起動し、0になったタイミングでfunction_keyを起動。
// phone 等の場合、送信終了しているか確認したらrepeat_func_timerをセットするようにできるかな。
    
	// Alt right / left をbandmap の切り替えにする。のと、band毎のリグの対応をチェックしてradio をAlt Spc でselect したときにfocused radio 切り替えるようにすること。
	// バンド毎に最後に運用したリグを憶えておけばよい？
	// 23/1/11
	
  int help_idx; // 現在選ばれているhelp menu の番号
  
  int f_straightkey; // straight key enable flag (Right Shift) 

  // SO-2R related
  int so2r_tx; // selected tx 0,1
  int so2r_rx; // selected rx 0,1
  int so2r_stereo; // stereo rx 0,1
  int focused_radio; // radio currently focused
  int focused_radio_prev; // previously focused radio will be saved to here (used on toggling stereo mode to select both current focus and previous focus)
  // \ (backspace) will switch actively receiving radio but not transmitting radio
  //   at the same time, switch focused display by changing radio_selected.
  
  
  int cat_radio_shared[5][3][2]; // 同じシリアルポートをshareしているradio のリスト
  // radio comm_port#  0:radio_idx 1:CI-V id 
  
  // nextaos related
  int f_nextaos;
  int nextaos_count;
  int nextaos_satidx;
  int nextaos_showidx; //　何番目からaos-los el 情報を表示するか？
  
  int f_console_emu; // console_emulation mode fiag これがTRUEだと、terminal から入力した文字をvt220 emulation と解釈してkeyboard コマンドとして送り、teraterm 等でconsole画面として操作できるようにする。
  
  char f_show_signal; // 1 to display signal information periodically 
  
  int f_rotator_track; // 1 to track current satellite
  int f_rotator_enable; // 1 to query rotator azimuth 
  int rotator_az; // current rotator azimuth value received from controller 
  int rotator_target_az; // rotator_target_azimuth 
  int rotator_target_az_tolerance;  // tracking tolerance if current az is within this tolerance do not send tracking target 
  int rotator_target_az_sweep; // target value in sweeping
  int rotator_sweep_step_default; //default step for sweep (2deg)
  int rotator_sweep_step ; // step sweeping in deg
  char f_rotator_sweep_suspend;
  char f_rotator_sweep; // 1 to sweep action 
 // char f_rotator_target_az_ready; // 1 to signal to send target azimuth (not yet used)
  int rotator_sweep_timeout; // timeout in rotator sweeping 
  
  
  int radios_enabled; // bit assembly of radios enabled status

  // ~ will toggle receive stereo/mono

  // when transmitting (by manual key, Func key), always check and clear currently CW sending buffer then switch tx radio and transmit.
  // 送信の制御はso2r_txを参照してやっている。(SO2R)   selected_radio (SO1R, SAT)でやっている。
  // 受信の制御は

  int radio_mode ; // 0 so1r  1 sat two radio (main transmit sub receive)
  // 2 so2r main selected 3 so2r sub selected 4 ... ?

  int f_so2rmini; // use SO2R mini box to control SO2R
  
  // the following entry are common to both radio
  char my_callsign[LEN_CALL_WINDOW + 3];
  char sent_exch[LEN_EXCH_WINDOW + 3];
  int sent_exch_abbreviation_level;
  
  char zserver_name[LEN_HOST_NAME + 3];
  char hostname[LEN_HOST_NAME + 3];  
  char cluster_name[LEN_HOST_NAME + 3];
  char cluster_cmd[LEN_CLUSTER_CMD + 3]; //
  char my_name[LEN_CALL_WINDOW + 3];
  
  char jcc[LEN_EXCH_WINDOW + 3];
  char power_code[LEN_POWER_WINDOW +3 ];
  char cw_msg[N_CWMSG][LEN_CWMSG_WINDOW + 3];
  char rtty_msg[N_CWMSG][LEN_CWMSG_WINDOW + 3];
  char email_addr[LEN_EMAIL_ADDR_WINDOW + 3];

  char wifi_ssid[LEN_WIFI_SSID_WINDOW + 3];
  char wifi_passwd[LEN_WIFI_PASSWD_WINDOW + 3];    

  char tm[30]; // current time

  int voice_memory_enable;

  // contest type related
  int mask ; // dupe check mask PH/CW 0xff dupe in CW and PH 0xff-3 dupe in operating mode

  int f_partial_check;// if set, when station callsign is input by keyboard invoke partial check

  // the following are not implemented yet 220208
  int cw_pts; // points for cw qsos
  int inner_pts; // points for inner stations
  int contest_band_mask; // workable band in this contest ( changing this also reflected to bandmap_mask  (, need reverse bits ), ), this mask also affects switch_bands in ui.c, but not for frequency change on the rig side.
  char contest_name[10 + 3]; // contest name
  int contest_id; // contest id number
  int multi_type; // multiplier type 1 JARL contest with power code  0 other  3 JARL contest power code but not check multi(ACAG) 2 UEC 4 multi check ignoring the last character (for JA8 contest)
  int seqnr; // sequential number of the QSO
  int txnum;
  int qsoid; // qsoid of the current qso for zLog


 // int bandid_bandmap; // current bandid for the bandmap display


  // satellite operation related
  int sat; // satellite operation flag  1 satellite 0 contest qso
  // in satellite mode, transmitter and receiver may be controlled by the program
  // frequency adjustment by up/down key (?) Shift+ up down adjusts TX/RX offset

#define LEN_GL 7
  char grid_locator[LEN_GL + 3]; // current location  grid locator  ... editable
  char grid_locator_set[LEN_GL + 3]; // current location  grid locator  ... editable


  int  sat_freq_mode; // 0... rig freq indipendent of sat  1.. receive frequency fixed transmit freq control  2.. transmission freq fixed receive freq control
  char sat_name[LEN_SATNAME_WINDOW + 3];  //
  char sat_name_set[LEN_SATNAME_WINDOW + 3];  //
  int sat_idx_selected; // selected index number of the satellite
  int sat_freq_tracking_mode; // satellite frequency tracking mode 0: RX fix 1: TX fix  2: Sat fix
  //
  int up_f, dn_f, satup_f, satdn_f; // uplink and down link frequency satup_f for satellite uplink
  int up_f_prev, dn_f_prev; // previously set value for up_f and dn_f
  int freq_tolerance;
  int sat_vfo_mode ; // allocation of VFO for split op.
  // 0 .. VFO A TX  not set VFO B
  // 1 .. VFO A(or currently selected) RX/ VFO B TX
  int f_inband_txp; // flag current satellite frequency in the transponder or not
  int beacon_f; // beacon receive freq calculated.
  unsigned long tle_unixtime; // time of tle information

  // command buffer of console (receive from Serial)
  char cmdbuf[128];
  int cmd_ptr;

  // command buffer of tcp console 
  struct ringbuf tcp_ringbuf;
  char tcp_cmdbuf[NCHR_TCP_CMD+1];
  int tcp_cmdbuf_ptr; // number of char written in cmdbuf
  int tcp_cmdbuf_len;



  // enable callhist search in operation
  int enable_callhist; // 1 is enabled
  int bandmap_summary_idx; // from this offset show bandmap summary

};


// style spefication on the left display sat/bandmap/misc info
struct info_display {
  int show_info; // 0: nothing 1: sat 2: bandmap 3: previous qso 4: logger setting
  int show_info_prev;
  int timer;

  int pos; // qsologf.position()

  int multi_ofs; // offset index for displaying worked multi

};
#define DICT_VALUE_TYPE_CHARARRAY 0
#define DICT_VALUE_TYPE_INT 1
//#define DICT_VALUE_TYPE_INT_INDEX 2
struct dict_item {
  const char *name;// name index
  void *value; // pointer to the value
  int value_type; // 0: char pointer 1:int (2: float)
//  int index_max; // range index of integer array variable if DICT_VALUE_TYPE_INT_INDEX 
};

#define N_SETTINGS_DICT 29


// data structure for logger file
union qso_union_tag {
  struct qso_tag {
    char type[2]; // usually 'Q' deleted qso has 'D'
    char seqnr[5]; // 4 digit qso sequence number
    char tm[18]; // yy/mm/dd-hh:mm:ss style time
    char freq[11]; // qso frequency in Hz  1295000000
    char band[3]; // band code in decimal 1.. 1.8M ..
    char mode[3]; // contest op mode  CW, PH, DG ...
    char opmode[6]; // actual operation mode CW, LSB, USB ...
//    char mycall[11];
    char mycall[LEN_CALLSIGN+1];
    char sentrst[4];
//    char sentexch[11];
//    char hiscall[11];
    char sentexch[LEN_EXCH+1];
    char hiscall[LEN_CALLSIGN+1];
    char rcvrst[4];
//    char rcvexch[11];
    char rcvexch[LEN_EXCH+1];
//    char remarks[40];
    char remarks[LEN_REMARKS+1];
  } entry;
  uint8_t all[256];
};


/// bandmap structure
// band map for each band
// bandmap data is dynamically allocated based on number of stations
// allocation is  0 stations in each band initially
// initial allocation for the band is 50
// if more than that, allocation added by 50?
#define NSTN_BANDMAP 50

#define BANDMAP_ENTRY_FLAG_NEWMULTI 4
#define BANDMAP_ENTRY_FLAG_WORKED 2
#define BANDMAP_ENTRY_FLAG_ONFREQ 1
struct bandmap_entry {
  unsigned int freq;
//  char station[10];
  char station[LEN_CALLSIGN+1];
  byte mode;
  char remarks[30];
  int time;
  byte type ; // 1 .. qso/s&p bit 2 ... cluster bit  ... (3= qso and cluster)
  byte flag; // flag to identify printed category (0 default 1 near current frequency)
};

struct bandmap {
  int bandid; // band code for this bandmap
  int nstations; // number of station registered to this bandmap
  int nentry; // number of entry currently allocated
  struct bandmap_entry *entry;
};


struct bandmap_disp {
  // variables related with bandmap display on the left
  int cursor; // number of cursor colum in the display (not including on-freq display) (edit will be performed on the column
  int top_column; // display offset column to start display from this column (scroll like appearance)
  // これをバンド毎にする必要あり。22/10/11
  // または、局数に合わせて調整？
  
  byte sort_type; // 0 time 1 freq
  // on freq station information
  int ncount1; // number of entry to be displayed (excepting on_freq display)

  int f_update; // update bandmap display when this flag is set

  byte f_onfreq; // 1 if on-freq display exists
  byte on_freq_modeid;
//  char on_freq_station[10];
  char on_freq_station[LEN_CALLSIGN+1];

  // on cursor station information
  int on_cursor_freq;
  byte on_cursor_modeid;
//  char on_cursor_station[10];
  char on_cursor_station[LEN_CALLSIGN+1];

};

// callsign of existing QSOs
#define NMAXQSO 1200
//#define NMAXQSO 200

// dupe check link for each band/mode
struct dupechk {
  char callsign[NMAXQSO][LEN_CALLSIGN+1];
  char exch[NMAXQSO][LEN_EXCH+1]; // exchange in the previous contact
  int ncallsign; // number of currently registered callsigns
  // if CW,SSB both OK contest bandid will be capital for phone
  byte bandmode[NMAXQSO]; // band and mode identity for each callsign
};
// call history lookup table
#define NMAXCALLHIST 2000
//int nmaxcallhist=NMAXCALLHIST;
#define CALLHIST_CALLEXCH_SIZE 32
typedef union {
  struct callhist_entry {
    char callsign[LEN_CALLSIGN+1];
    char exch[LEN_EXCH+1];
  } entry;
  uint8_t buffer[LEN_CALLSIGN+1+LEN_EXCH+1];
} callhist_union;

struct callhist {
  callhist_union u;
  unsigned long pos; // position in the file
  int nstations; // number of stations assigned for the same prefix
};


struct score {
  int worked[2][N_BAND]; // number of QSOs for the band , 0 LOG_MODETYPE_CW 1 else
  int nmulti[N_BAND]; // number of multi
};

//#define N_SATELLITES 21
#define N_SATELLITES 26
struct sat_info {
  // RS-44
  // 1 44909U 19096E   21294.50091848  .00000021  00000-0  33973-4 0  9996
  // 2 44909  82.5264  29.0502 0217951 158.1945 202.8611 12.79711234 84943
  // 0123456789012345678901234567890123456789012345678901234567890123456789
  // 00000000001         2         3         4         5         6

  char name[20];
  int YEAR;
  double EPOCH;
  double INCLINATION;
  double RAAN;
  double ECCENTRICITY;
  double ARGUMENT_PEDIGREE;
  double MEAN_ANOMALY;
  double MEAN_MOTION;
  double TIME_MOTION_D;
  double EPOCH_ORBIT;

  int up_f0, dn_f0;
  int up_f1, dn_f1;
  char up_mode[10], dn_mode[10];
  int offset_freq;
  int bc_f0;

  DateTime nextaos,nextlos; // store calculated next aos time here
  double maxel; // maximum elevation during the aos-los

};

// name, up min max mode  DOWN min max mode    BC
struct sat_info2 {
  const char *name;
  double up_f0;
  double up_f1;
  const char *up_mode;
  double dn_f0;
  double dn_f1;
  const char *dn_mode;
  double bc_f0;
  double offset_freq;
};



// LCD related variables
struct disp {
  byte hcol[2], wcol; // pixels for each column and width
  char lcdbuf[20 * 18]; // line character buffer
} ;



#endif
