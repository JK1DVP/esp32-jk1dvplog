#ifndef FILE_TCP_SERVER_H
#define FILE_TCP_SERVER_H
#define MAX_SRV_CLIENTS 1
extern class WiFiClient serverClients[MAX_SRV_CLIENTS];
extern int serverClients_status[MAX_SRV_CLIENTS] ;
extern int timeout_tcpserver ;

void tcpserver_process();
void init_tcpserver();
#endif
