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
#include "multi.h"
#include "multi_process.h"
#include "display.h"
#include "log.h"

struct multi_list multi_list;

// initialize multi for the given bands with given multi_item
void init_multi(const struct multi_item *multi, int start_band, int stop_band) {
  //	multi_list.multi = &multi_tama;
  //	multi_list.multi = &multi_tokyouhf;
  //	multi_list.multi = &multi_cqzones;
  if (start_band<0) {
    // all band
    start_band=1;
    stop_band=N_BAND-1;
  }
  if (stop_band<0) {
    stop_band=N_BAND-1;
  }
  if (start_band>=N_BAND||stop_band>=N_BAND) {
    start_band=N_BAND-1;
    stop_band=N_BAND-1;
  }
  // each band init
  for (int iband=start_band;iband<=stop_band;iband++) {
    multi_list.multi[iband-1]=multi;     // set multi here
    if (multi==NULL) {
      //  if (multi_list.multi == NULL) {
      multi_list.n_multi[iband-1] = 0;
    } else {
      // count n_multi for each band
      if (!plogw->f_console_emu) {
	plogw->ostream->print("band:");
	plogw->ostream->print(iband);
	plogw->ostream->print(":");
	plogw->ostream->print(band_str[iband-1]);
      }
      for (int i = 0; i < N_MULTI; i++) {
	if (*multi_list.multi[iband-1]->mul[i] == '\0' ||
	    *multi_list.multi[iband-1]->name[i] == '\0' ) {
	  // end of the list
	  multi_list.n_multi[iband-1] = i;
	  break;
	} else {
	  if (!plogw->f_console_emu) {
	    //	    plogw->ostream->print(multi_list.multi[iband-1]->mul[i]);
	    //	    plogw->ostream->print(" ");
	    //	    plogw->ostream->print(multi_list.multi[iband-1]->name[i]);
	    //	    plogw->ostream->println(" ");
	  }
	}
      }
      if (!plogw->f_console_emu) {
	plogw->ostream->print(" N multi=");
	plogw->ostream->println(multi_list.n_multi[iband-1]);
      }
      // clear worked list
      for (int i = 0; i < multi_list.n_multi[iband-1]; i++) {
	//	for (int iband = 0; iband < N_BAND; iband++) {
	multi_list.multi_worked[iband-1][i] = 0;
      }
    }
  }
}

void clear_multi_worked() {
  // clear worked list
  for (int iband = 0; iband < N_BAND; iband++) {  
    for (int i = 0; i < multi_list.n_multi[iband-1]; i++) {
      multi_list.multi_worked[iband-1][i] = 0;
    }
  }
}

int multi_check(char *s,int bandid) {   // s: exch (such as in plogw->recv_exch +2)
  return multi_check_option(s,bandid,0);
}

int multi_check_option(char *s,int bandid,int option) {   // s: exch (such as in plogw->recv_exch +2)
  // now option is not used
  // check multi for the bandid

  if (bandid<=0|| bandid>N_BAND) return 0;
  if (multi_list.multi[bandid-1] == NULL) return 0;
  char exch_buf[10];
  int len;
  len = strlen(s);
  switch (plogw->multi_type) {
  case 0: // multi same as number 
  case 3:  // jarl contest power_code but no multi-check performed (like ACAG)
    break;
  case 1: // jarl contest: ignore last character (power code)
    if (verbose & 1) printf("JARL contest\n");
    // check power character
    char c;
    c = *(s + len - 1);
    if (c != 'P' && c != 'M' && c != 'L' && c != 'H' && c != 'Q') {
      if (verbose & 1) {
	printf("wrong powercode\n");
      }
      return -1;
    }
    len--;
    break;
  case 4: // ignore last character (JA8 contest) no check for the last character
    len--;
    break;

  case 5: // HS was  check GL or numbers
    if (len==4) {
      // check
      if (isalpha(s[0]) && isalpha(s[1]) && isdigit(s[2]) && isdigit(s[3])) {
	return 0; // acceptable GL multi 
      }
    }
    break;
  case 2:  // UEC contest H/I/L/UEC
    if (strcmp(s + len - 1, "I") == 0) {
      // I
      len--;
      break;
    } else if (strcmp(s + len - 1, "H") == 0) {
      len--;
      break;
    } else if (strcmp(s + len - 1, "L") == 0) {
      len--;
      break;
    } else {
      if (len >= 3) {
	if (strcmp(s + len - 3, "UEC") == 0) {
	  len -= 3;
	  break;
	}
      }
      return -1;
    }

    break;
  }

  //  log_d(VERBOSE_UI,"%s",s);
  //  log_d(VERBOSE_UI,":len=");
  //  log_d(VERBOSE_UI,"%d\n",len);

  if (len < 1) return -1;
  *exch_buf = '\0';
  strncat(exch_buf, s, len);
  for (int i = 0; i < multi_list.n_multi[bandid-1]; i++) {
    //    log_d(VERBOSE_UI,"mul :%s: exch :%s:\n",multi_list.multi->mul[i],exch_buf);
    if (strcmp(multi_list.multi[bandid-1]->mul[i], exch_buf) == 0) {
      // hit
      //      log_d(VERBOSE_UI,"hit %d\n",i);      
      return i;
    }
  }
  //  log_d(VERBOSE_UI,"not hit anything\n");
  return -1;
}



int multi_check_old() {
  struct radio *radio;
  radio = radio_selected;
  if (multi_list.multi[radio->bandid-1] == NULL) return 0;
  char exch_buf[10];
  int len;
  len = strlen(radio->recv_exch + 2);
  switch (plogw->multi_type) {
    case 0:
    case 3:  // jarl contest power_code but no multi-check performed
      break;
    case 1:
      // jarl contest ignore last character
      if (verbose & 1) plogw->ostream->println("JARL contest");
      // check power character
      char c;
      c = *(radio->recv_exch + 2 + len - 1);
      if (c != 'P' && c != 'M' && c != 'L' && c != 'H' && c != 'Q') {
        if (verbose & 1) {
          plogw->ostream->println("wrong powercode");
        }
        return -1;
      }
      len--;
      break;
    case 4:
      // ignore last character (JA8 contest)
      len--;
      break;
    case 2:  // UEC contest H/I/L/UEC
      if (strcmp(radio->recv_exch + 2 + len - 1, "I") == 0) {
        // I
        len--;
        break;
      } else if (strcmp(radio->recv_exch + 2 + len - 1, "H") == 0) {
        len--;
        break;
      } else if (strcmp(radio->recv_exch + 2 + len - 1, "L") == 0) {
        len--;
        break;
      } else {
        if (len >= 3) {
          if (strcmp(radio->recv_exch + 2 + len - 3, "UEC") == 0) {
            len -= 3;
            break;
          }
        }
        return -1;
      }

      break;
  }
  if (verbose & 1) {
    plogw->ostream->print(radio->recv_exch + 2);
    plogw->ostream->print(":len=");
    plogw->ostream->println(len);
  }
  if (len < 1) return -1;
  *exch_buf = '\0';
  strncat(exch_buf, radio->recv_exch + 2, len);
  for (int i = 0; i < multi_list.n_multi[radio->bandid-1]; i++) {
    //	plogw->ostream->print(i);
    //	plogw->ostream->print(":");
    //	plogw->ostream->print(multi_list.multi->mul[i]);
    //	plogw->ostream->print(":");
    //	plogw->ostream->println(plogw->recv_exch+2);
    if (strcmp(multi_list.multi[radio->bandid-1]->mul[i], exch_buf) == 0) {
      // hit
      return i;
    }
  }
  return -1;
}

void entry_multiplier() {
  struct radio *radio;
  radio = radio_selected;
  if (multi_list.multi[radio->bandid-1] == NULL) return;
  if (radio->multi < 0) return;
  if (radio->multi >= multi_list.n_multi[radio->bandid-1]) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("errortic multi id:");
      plogw->ostream->println(radio->multi);
    }
    return;
  }
  // new multi check ?
  if (multi_list.multi_worked[radio->bandid - 1][radio->multi] == 0) {
    // new multi found
    // if (verbose & 1) {
    //	  plogw->ostream->print("new multi:");plogw->ostream->println(plogw->multi);
    // }
    score.nmulti[radio->bandid - 1]++;
  }

  multi_list.multi_worked[radio->bandid - 1][radio->multi] = 1;
}


// reverse search multi name and
// return with the index of multi if found name, otherwise return -1
void reverse_search_multi() {
  struct radio *radio;
  radio = radio_selected;
  
  if (multi_list.multi[radio->bandid-1] == NULL) return;
  // input is remarks until space
  // copy to buf
  char buf[128];
  int count;
  count = 0;
  char *p, *p1;
  p = radio->remarks + 2;
  p1 = buf;
  while (*p && count < 128) {
    if (*p == ' ') {
      break;
    }
    if (*p == '_') {
      *p1++ = ' '; // replace _ to space
      p++;
    } else {
      *p1++ = *p++;
    }
    count++;
  }
  *p1 = '\0';
  if (!plogw->f_console_emu) {
    plogw->ostream->print("reverse searching:");
    plogw->ostream->println(buf);
    sprintf(dp->lcdbuf, "reverse searching\n%s\n",p1);    
    upd_display_info_flash(dp->lcdbuf);    
  }
  int len;
  len=strlen(buf);

  for (int i = 0; i < multi_list.n_multi[radio->bandid-1]; i++) {
    if (strncasecmp(multi_list.multi[radio->bandid-1]->name[i], buf,len) == 0) {
      // hit
      //multi_list.multi->mul[i];
      //return i;
      if (!plogw->f_console_emu) {
        plogw->ostream->print("found ");
        plogw->ostream->println(multi_list.multi[radio->bandid-1]->mul[i]);
	sprintf(dp->lcdbuf, "found=%s\n%s\n",
		multi_list.multi[radio->bandid-1]->mul[i],
		multi_list.multi[radio->bandid-1]->name[i]);
	upd_display_info_flash(dp->lcdbuf);    
      }

      // replace recv_exch with searched multi
      strcpy(radio->recv_exch + 2, multi_list.multi[radio->bandid-1]->mul[i]);
      radio->recv_exch[1] = strlen(radio->recv_exch + 2);  // cursor to the end of multi
      radio->ptr_curr = 1;
      upd_display();
      return;
    }
  }
  sprintf(dp->lcdbuf, "not found\n");
  upd_display_info_flash(dp->lcdbuf);    
  // not found
  return;
}
