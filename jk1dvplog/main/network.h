#ifndef FILE_NETWORK_H
#define FILE_NETWORK_H
void init_multiwifi() ;
void print_wifiinfo() ;
void localip_to_string(char *buf);
void init_network() ;
int check_wifi() ;
void multiwifi_addap(char *ssid,char *passwd);
int println_tcpserver(void *arg,const char *s);
void ftp_service_loop();

#endif
