#ifndef FILE_MCP_H
#define FILE_MCP_H
void init_mcp_port() ;
void phone_switch(int left, int right) ;
void write_mcp_gpio(int tmp,int tmp1);
int read_mcp_gpio(int i);
void mic_switch(int sel) ;
void ptt_switch(int sel,int on) ;
#endif
