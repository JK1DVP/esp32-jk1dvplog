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
#include "callhist.h"
#include "dupechk.h"
// dumb dupe check routine


// bandmode calculation 
unsigned char bandmode_param(int bandid,int modetype) {
  return bandid * 4 + modetype;
}

// get current bandmode
unsigned char bandmode() {
  struct radio *radio;
  radio = radio_selected;
  if (radio->modetype==LOG_MODETYPE_PH && radio->f_tone_keying) {
    return bandmode_param(radio->bandid,LOG_MODETYPE_CW); // TONE KEYING IS REGARDED AS CW QSO IN dupe check 
  } else {
    return bandmode_param(radio->bandid,radio->modetype);
  }
}

// only check dupe no call history retrieval
bool dupe_check_nocallhist(char *call, byte bandmode, byte mask) {
  int i;
  for (i = 0; i < dupechk.ncallsign; i++) {
    if ((dupechk.bandmode[i] & mask) == (bandmode & mask)) {
      if (strcmp(dupechk.callsign[i], call) == 0) {
        // dupe
        return 1;
      }
    }
  }
  return 0;  // not dupe
}

// dupe check and check and fill call history
//  callhist_check : true if check callhistory and obtain exchange to fill
bool dupe_check(char *call, byte bandmode, byte mask, bool callhist_check) 
{
  int ret;
  struct radio *radio;
  char *getexch;
  bool f_getexch=0; // flag if exchange is obtained
  bool f_callhist = 0;
  
  radio = radio_selected;
  getexch=radio->recv_exch + 2;

  if (strlen(getexch) == 0) {
    // not yet filled in the my exchange, search previous qso and fill it.
    f_callhist = 1;
  }
  
  ret=dupe_check_get_callhist(call, bandmode, mask, callhist_check,getexch,&f_getexch,&f_callhist);

  if (f_getexch) {
    //    strcpy(radio->recv_exch + 2, getexch);	   // here already copied
    // also need to locate cursor to the end of the string
    radio->recv_exch[1] = strlen(radio->recv_exch + 2);
  }
  return ret;
}

// f_callhist : flag if callhist check necessary
// f_getexch : flag if exchange is obtained from history
 bool dupe_check_get_callhist(char *call, byte bandmode, byte mask, bool callhist_check,char *getexch,bool *f_getexch,bool *f_callhist) {
  int i;
  bool ret,ret1;

  // check all qso
  ret = 0;
  for (i = 0; i < dupechk.ncallsign; i++) {
    if ((ret == 0) || (*f_callhist == 1)) {

      if ((dupechk.bandmode[i] & mask) == (bandmode & mask)) {
        // current band and mode
        if (strcmp(dupechk.callsign[i], call) == 0) {
          // dupe
          if (verbose & 1) {
            plogw->ostream->println("dupe");
          }
          ret = 1;
        }
      } else if (*f_callhist) {
        // other band and mode
        if (strcmp(dupechk.callsign[i], call) == 0) {
          // hit !

          strcpy(getexch, dupechk.exch[i]);	  
          *f_callhist = 0;  // no longer need to search for history


	  *f_getexch=1;
        }
      }
    } else {
      break;
    }
  }
  if ((ret != 1) && (*f_callhist == 1) && (callhist_check)) {
    // not dupe and not yet worked in other band
    // if enabled search for the history
    if (plogw->enable_callhist) {
      if (search_callhist_getexch(call,getexch)) {
	*f_getexch=1;
      }
    }
  }
  return ret;  // not dupe
}

void entry_dupechk() {
  struct radio *radio;
  radio = radio_selected;
  // entry current qso into dupecheck
  if (dupechk.ncallsign < NMAXQSO) {
    //
    int i;
    i = dupechk.ncallsign;
    strcpy(dupechk.callsign[i], radio->callsign + 2);
    strcpy(dupechk.exch[i], radio->recv_exch + 2);
    dupechk.bandmode[i] = bandmode();
    dupechk.ncallsign++;
  }
}

// initialize score statistics
void init_score() {
  for (int i = 0; i < N_BAND; i++) {
    score.worked[0][i] = 0;
    score.worked[1][i] = 0;
    score.nmulti[i] = 0;
  }
}


void init_dupechk() {
  for (int i = 0; i < NMAXQSO; i++) {
    strcpy(dupechk.callsign[i], "");
    dupechk.bandmode[i] = 0;
  }
  dupechk.ncallsign = 0;
}
