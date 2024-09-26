/* Copyright (c) 2021-2024 Eiichiro Araki
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.
*/

#include "ringbuf.h"

void write_ringbuf(struct ringbuf *p, char c) {
  if ((p->wptr + 1) % (p->len) != p->rptr) {
    p->buf[p->wptr++] = c;
    p->wptr %= p->len;
  }
}

int read_char_ringbuf(struct ringbuf *p, char *c) {
  if (p->wptr != p->rptr) {
    *c = p->buf[p->rptr];
    p->rptr = (p->rptr + 1) % p->len;
    return 1;
  } else {
    return 0;
  }
}

int readfrom_ringbuf(struct ringbuf *p, char *to, char delim, char ignchr, int ncount)
// ncount: maximum number of characters to read
// return with number of chars read
// delim: stop reading when encounter with delim
// if reached to delim, return with -1
// else return with number of read characters
{
  int count;
  char c;
  count = 0;
  while (p->wptr != p->rptr) {
    if (count < ncount) {
      c = p->buf[p->rptr];
      p->rptr = (p->rptr + 1) % p->len;
      if (c == delim) {
        *to = '\0';
        return -1;
      }
      if (c == ignchr) {
        continue;
      }
      *to++ = c;
      count++;
    } else {
      break;
    }
  }
  return count;
}
