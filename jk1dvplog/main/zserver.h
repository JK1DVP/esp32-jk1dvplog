#ifndef FILE_ZSERVER_H
#define FILE_ZSERVER_H
// definitions on zserver (zlog) modulxses
//コード	バンド

// receive buffer size
#define NCHR_ZSERVER_RINGBUF 1024
// send buffer size
#define NCHR_ZSERVER_CMD 1024

struct zserver {
  int timeout; // control cluster info reception
  int timeout_alive; // if no information received for more than a minute, try to reconnect
  int timeout_count;
  int stat;
  struct ringbuf ringbuf;
  char cmdbuf[NCHR_ZSERVER_CMD + 1];
  int cmdbuf_ptr; // number of char written in cmdbuf
  int cmdbuf_len;
};

int connect_zserver() ;
void init_zserver_info();
void zserver_process() ;
void zserver_send(char *buf);
int opmode2zLogmode(char *opmode);
void reconnect_zserver();
void zserver_freq_notification();
extern int f_show_zserver;
extern const char *zserver_freqcodes[]; /*={"1.9M","3.5M","7M","10M","14M","18M","21M","24M","28M","50M","144M","430M","1200M","2400M","5600M","10G" };*/
extern int zserver_bandid_freqcodes_map[];/*={0,0,1,2,4,6,8,9,10,11,12,13,14,15};*/

//コード	モードa
extern const char *zserver_modecodes[]; /* ={"CW","SSB","FM","AM","RTTY","FT4","FT8","OTHER"};*/
//電力コード	
//コード	電力

extern const char *zserver_powcodes[];/*={"1W","2W","5W","10W","20W","25W","50W","100W","200W","500W","1000W" };*/

extern const char *zserver_client_commands[]; /*={ "FREQ","QSOIDS","ENDQSOIDS","PROMPTUPDATE","NEWPX","PUTMESSAGE","!","POSTWANTED",
			"DELWANTED","SPOT","SPOT2","SPOT3","BSDATA","SENDSPOT","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			"CONNECTCLUSTER","PUTQSO","DELQSO","EXDELQSO","INSQSOAT","LOCKQSO","UNLOCKQSO","EDITQSOTO",
			"INSQSO","PUTLOGEX","PUTLOG","RENEW","SENDLOG" };*/


extern const char *zserver_server_commands[];/*={"GETQSOIDS","SENDCLUSTER","SENDPACKET","SENDSCRATCH",
			       "BSDATA","POSTWANTED","DELWANTED","CONNECTCLUSTER",
			       "GETLOGQSOID","SENDRENEW","DELQSO","EXDELQSO","RENEW",
			       "LOCKQSO","UNLOCKQSO","BAND","OPERATOR","FREQ","SPOT",
			       "SENDSPOT","PUTQSO","PUTLOG","EDITQSOTO","INSQSO",
			       "EDITQSOTO","SENDLOG" };*/

/*
QSOデータは下記の３１項目を"~"で区切ったテキストデータとする。		
項番	項目	内容
1	識別	"ZLOGQSODATA:"固定
2	日時	DateTime型の値を文字列で
3	コールサイン	"JR8PPG"
4	送信NR	"106H"
5	受信NR	"10M"
6	送信RST	"59"/"599"
7	受信RST	"59"/"599"
8	シリアルNO	"001","002","003"…
9	モードコード	各種コードシート参照
10	バンドコード	各種コードシート参照
11	電力コード	各種コードシート参照
12	マルチ１	マルチとなる文字列
13	マルチ２	マルチとなる文字列
14	ニューマルチフラグ１	"0":非ニューマルチ "1":ニューマルチ
15	ニューマルチフラグ２	"0":非ニューマルチ "1":ニューマルチ
16	得点	"0","1","2"…
17	OP名	任意文字列
18	メモ欄	任意文字列
19	CQフラグ	"0":非CQ "1":CQ
20	DUPEフラグ	"0";非DUPE "1":DUPE
21	Reserve	空
22	TX番号	"0","1","2"…
23	電力コード２	ARRLDXで使う電力 LMHPを変換したもの　初期値：500
24	Reserve2	空
25	Reserve3	QSOID
26	周波数	Freq
27	QSY違反フラグ	"0":違反なし "1":QSY違反
28	PC名	任意文字列
29	強制フラグ	"0":通常 "1":強制入力
30	QSL状態	"0":設定なし "1":PSEQSL "2":NOQSL
31	無効フラグ	"0":有効 "1":無効
datetime 型 
00:00:00 → 45384
2024/4/2 09:00:00 → 45384.375
2024/4/2 21:00:00 → 45384.875
JST/UTCの識別は電文内には存在しない

sample qsolog
#ZLOG# PUTLOG ZLOGQSODATA:~45385.551932662~JR8PPG~106H~106H~599~599~1~0~6~3~~~0~0~0~PC0~~0~0~10~0~500~0~9700~~0~PC0~0~0~1

QSOIDの計算方法					
QSOID =  tt * 100000000 + ss * 10000 + rr * 100					
	tt:送信機番号(max:21)　マイコン側は10固定などで良い				
	ss:送信機内での連番				
	rr:0-99の乱数				

*/
/*	Z-Server通信仕様				
fコマンドフォーマット			#ZLOG# <コマンド> <パラメータ><crlf>							
プロトコル：tcp							↑スペースで区切る							
待ち受けポート：23(telnet)							<crlf>は0x0d 0x0a							
															
															
				zLog					Z-Server						
															
		①接続ボタンクリック				connect()									
初期処理															
															
															
		②現在バンド通知			#ZLOG# BAND <バンドコード><crlf>					<バンドコード>は「各種コード」シート参照					
															
															
		③現在OP通知			#ZLOG# OPERATOR <OP名><crlf>					<OP名>は任意文字列（zLogではOPリストに登録した名前）					
															
															
		④PC名通知			#ZLOG# PCNAME <PC名><crlf>					<PC名>は任意文字列					
															
															
		初期処理はここまで													
															
															
任意タイミング		⑤QSO確定時			#ZLOG# PUTQSO <QSOデータ><crlf>					<QSOデータ>は「QSOデータ」シート参照					
															
															
		⑥QSY時			#ZLOG# BAND <バンドコード><crlf>										
															
															
															
															
															
					#ZLOG# PUTQSO <QSOデータ><crlf>				⑦他のzLogからのQSOデータ配信						
															
		交信リストUPDATE													
															
															
															
															
															
															
															
															
															
															
															
		⑲zLog終了時				disconnect()									
															
															
*/
#endif
