#ifndef FILE_MULTI_PROCESS_H
#define FILE_MULTI_PROCESS_H
#include "multi.h"
void init_multi(const struct multi_item *multi, int start_band, int stop_band) ;
//void init_multi() ;
void clear_multi_worked() ;
int multi_check_option(char *s,int bandid,int option);   // s: exch (such as in plogw->recv_exch +2)
int multi_check(char *s,int bandid);   // s: exch (such as in plogw->recv_exch +2)
//int multi_check(char *s);
int multi_check_old() ;
extern struct multi_list multi_list;
void entry_multiplier();
void reverse_search_multi() ;

#endif
