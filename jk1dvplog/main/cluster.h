#ifndef FILE_CLUSTER_H
#define FILE_CLUSTER_H
#include "Arduino.h"
//#include <HTTPClient.h>

//extern WiFiClient client;
extern char cluster_server[40];
extern int cluster_port ;
#define NCHR_CLUSTER_RINGBUF 1024
extern char cluster_buf[NCHR_CLUSTER_RINGBUF];
extern struct cluster cluster;
void print_cluster_info(struct bandmap_entry *entry, int bandid, int idx );
void get_info_cluster(char *s) ;
void upd_bandmap_cluster(char *s);
void cluster_process() ;
void init_cluster_info() ;
void disconnect_cluster() ;
void disconnect_cluster_temp() ;
int connect_cluster() ;
extern const char * callsign ;
extern const char * cluster_cmd[3] ;
void set_cluster() ;
void send_cluster_cmd() ;
extern int f_show_cluster;


#endif
