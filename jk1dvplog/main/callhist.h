#ifndef FILE_CALLHIST_H
#define FILE_CALLHIST_H
int release_callhist_list_contents();
int init_callhist_list();
int read_callhist_list(char *fn);
void print_callhist_list(const char **callhist_list,int n);
char *callhist_call(const char *callsign);
char *callsign_body(const char *callsign);
char *exch_callhist(const char *callsign);
int count_callhist(const char **callhist_list);
int search_callhist_list_exch(const char **callhist_list,char *callsign, int match_body,char **exch_history);
int search_callhist_list(const char **callhist_list,char *callsign, int match_body);


void copy_tail_character(char *dest, char *s);
void init_callhist() ;
void close_callhist() ;
int search_callhist(char *callsign) ;
int search_callhist_getexch (char *callsign,char *getexch) ;
void open_callhist() ;
void release_callhist() ;
void set_callhistfn(char *fn) ;
void write_callhist(char *s);
void close_callhistf();

#endif
