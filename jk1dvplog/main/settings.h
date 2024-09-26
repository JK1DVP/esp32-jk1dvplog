#ifndef FILE_SETTINGS_H
#define FILE_SETTINGS_H
extern int n_settings_dict;
extern struct dict_item settings_dict[N_SETTINGS_DICT];

int readline(File *f, char *buf, int term, int size) ;
void init_settings_dict();
int assign_settings(char *line, struct dict_item *dict) ;
int dump_settings(Stream *f, struct dict_item *dict) ;
int load_settings(char *fn) ;
int save_settings(char *fn) ;

#endif
