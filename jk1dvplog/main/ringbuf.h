#ifndef FILE_RINGBUF_H
#define FILE_RINGBUF_H
struct ringbuf {
  char *buf;
  int rptr, wptr, len;
};
void write_ringbuf(struct ringbuf *p, char c) ;
int readfrom_ringbuf(struct ringbuf *p, char *to, char delim, char ignchr, int ncount);
#endif
