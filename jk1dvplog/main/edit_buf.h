#ifndef FILE_EDIT_BUF_H
#define FILE_EDIT_BUF_H
void backspace_buf(char *buf) ;
void delete_buf(char *buf) ;
void left_buf(char *buf) ;
void right_buf(char *buf);
void insert_buf(char c, char *buf) ;
void overwrite_buf(char c, char *buf) ;
void clear_buf(char *p) ;
void init_buf(char *p, int siz) ;
#endif
