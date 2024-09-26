#ifndef FILE_MISC_H
#define FILE_MISC_H
#define N_TIME_MEASURE_BANK 20
void copy_token(char *dest,char *src,int idx,char *sep) ;
void time_measure_clear(int bank);
void time_measure_start(int bank);
void time_measure_stop(int bank);
int time_measure_get(int bank);
void print_bin(char *print_to, unsigned int bin, int digits) ;
void set_location_gl_calc(char *locator) ;
void release_memory() ;
void print_memory();

#endif
