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

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
/// each log entry is fixed length text format
//Q,seqnr,yymmdd_hhmmss,band_code(1byte),mode_code(1byte),HISCALL(10b),SENTRST(3),SENTEXCH(10),RCVRST(3),RCVEXCH(10),freq_Hz(10)
// deleted qso will replace Q to D
// all space will be filled by 0x20 (SPC)
// remarks may be written to separate file  with sequence number

void backspace_buf(char *buf) {
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  if (verbose & 1) {
    plogw->ostream->print("backspace_buf:len=");
    plogw->ostream->println(len);
  }

  if (p <= 0) return;  // not change if no characters on the left
  /// 0123456789
  //  012456789
  //     p=3
  // shift data
  for (int i = p - 1; i < len - 1; i++) {
    if (i < 0) continue;
    buf[2 + i] = buf[2 + i + 1];
  }
  if (len > 0)
    buf[2 + len - 1] = '\0';
  // update pointer
  p = p - 1;
  if (p < 0) {
    p = 0;
  }
  buf[1] = p;
}



void delete_buf(char *buf) {
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  if (verbose & 1) {
    plogw->ostream->print("delete_buf:len=");
    plogw->ostream->println(len);
  }
  if (p >= len) return;  // no change if nothing on the right

  for (int i = p; i < len - 1; i++) {
    buf[2 + i] = buf[2 + i + 1];
  }
  if (len > 0)
    buf[2 + len - 1] = '\0';
  // update pointer
  if (p >= len) {
    p = p - 1;
    if (p < 0) p = 0;
    buf[1] = p;
  }
}

void left_buf(char *buf) {
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  p = p - 1;
  if (p >= 0) {
    buf[1] = p;
  }
}

void right_buf(char *buf)
// cursor position to the right
{
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  p = p + 1;
  if (p <= len) {
    buf[1] = p;
  }
}

void insert_buf(char c, char *buf) {
  ///   0123456
  ///   XXXXXX0
  //          P
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  if (verbose & 1) {
    plogw->ostream->print("insert_buf:len=");
    plogw->ostream->println(len);
    plogw->ostream->print("  siz=");
    plogw->ostream->println(siz);
  }
  if (p < siz) {
    int i;
    if (len > 0) {
      for (i = len - 1; i >= p; i--) {
        buf[2 + i + 1] = buf[2 + i];
      }
    }
    buf[p + 2] = c;
    // update pointer
    p++;
    if (p <= siz) {
      buf[1] = p;
    }
  }
}

// replace the character in the pointer with given character
// if pointer is at the end of string append .
void overwrite_buf(char c, char *buf) {
  int p;
  int siz;
  p = buf[1];
  siz = buf[0];
  int len;
  len = strlen(buf + 2);
  if (p < siz) {
    buf[p + 2] = c;
    p++;
    if (p <= siz) {
      buf[1] = p;
    }
  }
}

void clear_buf(char *p) {
  memset(p + 1, 0, p[0] - 1 + 3);  // pointer and contents clear
}

void init_buf(char *p, int siz) {
  memset(p, 0, siz + 3);
  p[0] = siz;
}


