#ifndef FILE_MULTI_H
#define FILE_MULTI_H
#include "decl.h"
#include "variables.h"


struct multi_item {
  const char *mul[N_MULTI];
  const char *name[N_MULTI];
};

struct multi_list {
  const struct multi_item *multi[N_BAND]; // multi item definition for each band
  // index is iband-1 (iband=1... N_BAND)
  int n_multi[N_BAND]; // number of multi defined for each band
  bool multi_worked[N_BAND][N_MULTI];
};
//extern void init_multi();
//extern int multi_check(char *s);
//extern int multi_check_old();
//extern void set_arrl_multi();

//extern struct multi_item multi_arrl;
extern const struct multi_item multi_hswas ;
extern const struct multi_item multi_kcj;
extern const struct multi_item multi_saitama_int ;
extern const struct multi_item multi_tokyouhf ;
extern const struct multi_item multi_cqzones ;
extern const struct multi_item multi_tama;
extern const struct multi_item multi_tmtest;

extern const struct multi_item multi_kantou;
extern const struct multi_item multi_allja;
extern const struct multi_item multi_knint;
extern const struct multi_item multi_yk;
extern const struct multi_item multi_ja8int;
extern const struct multi_item multi_yntest;
extern const struct multi_item multi_acag;
extern const struct multi_item multi_arrl;

#endif
