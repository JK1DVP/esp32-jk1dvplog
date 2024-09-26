#ifndef FILE_DUPECHK_H
#define FILE_DUPECHK_H
unsigned char bandmode() ;
unsigned char bandmode_param(int bandid,int modetype) ;
bool dupe_check_nocallhist(char *call, byte bandmode, byte mask) ;
bool dupe_check(char *call, byte bandmode, byte mask, bool callhist_check) ;
bool dupe_check_get_callhist(char *call, byte bandmode, byte mask, bool callhist_check,char *getexch,bool *f_getexch,bool *f_callhist);
void entry_dupechk() ;
void init_score() ;
void init_dupechk() ;
#endif
