#ifndef FILE_QSO_H
#define FILE_QSO_H
extern File qsologf;            // qso logf
void init_qsofiles() ;
void init_qso() ;
void makedupe_qso_entry() ;
void reformat_qso_entry() ;
void read_qso_log(int option) ;
int read_qso_log_to_file() ;
void set_qsodata_from_qso_entry() ;
void create_new_qso_log() ;
void open_qsolog() ;
void print_qso_entry_file(File *f) ;
void print_qso_entry() ;
void string_trim_right(char *s, char c);
void print_qso_logfile() ;

void print_qso_log() ;
// operation options in read_qso_log  or'ed
#define READQSO_MAKEDUPE 1
#define READQSO_PRINT 2
void make_qsolog_entry() ;
void make_zlogqsodata(char *buf);
void dump_qso_log() ;
void dump_qso_bak(char *numstr);
#endif
