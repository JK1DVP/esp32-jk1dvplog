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
#include "multi_process.h"
#include "dupechk.h"
#include "multi.h"
#include "display.h"

void set_contest_id() {

  if (!plogw->f_console_emu) {
    plogw->ostream->print("contest_id:");
    plogw->ostream->println(plogw->contest_id);
  }
  switch (plogw->contest_id) {
    case 0:  // default no multi check
      strcpy(plogw->contest_name, "NOMULTI"); // contestname if start from NOMULTI no multi check
      plogw->mask = 0xff;  // CW/PH OK
      plogw->cw_pts = 1;
      plogw->multi_type = 0;
      //      multi_list.multi = NULL;
      //init_multi(NULL,-1,-1);
      init_multi(&multi_acag,-1,-1);
      break;
    case 1:  // tamagawa
      strcpy(plogw->contest_name, "TAMAGAWA");
      plogw->mask = 0xff;  // CW/PH OK
      plogw->cw_pts = 2;
      plogw->multi_type = 0;
      //multi_list.multi = &multi_tama;
      init_multi(&multi_tama,-1,-1);
      break;
    case 2:  // tokyo uhf
      strcpy(plogw->contest_name, "TOKYOUHF");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;
      //      multi_list.multi = &multi_tokyouhf;
      init_multi(&multi_tokyouhf,-1,-1);
      break;
    case 3:  // CQ WW
      strcpy(plogw->contest_name, "CQWW");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;

      //multi_list.multi = &multi_cqzones;
      init_multi(&multi_cqzones,-1,-1);
      break;
    case 4:  // Saitama int
      strcpy(plogw->contest_name, "Saitama-Int");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;

      //      multi_list.multi = &multi_saitama_int;
      init_multi(&multi_saitama_int,-1,-1);
      break;
    case 5:  // KCJ
      strcpy(plogw->contest_name, "KCJ");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;

      //multi_list.multi = &multi_kcj;
      init_multi(&multi_kcj,-1,-1);
      break;
    case 6:  // Kanto UHF
      strcpy(plogw->contest_name, "KantoUHF");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;

      //multi_list.multi = &multi_kantou;
      init_multi(&multi_kantou,-1,-1);
      break;
    case 7:  // AllJA
      strcpy(plogw->contest_name, "AllJA");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 1;  // JA contest tail character is power code (P,L,M, H)
      //      multi_list.multi = &multi_allja;
      init_multi(&multi_allja,-1,-1);
      break;
    case 8:  // JA multi non JARL
      strcpy(plogw->contest_name, "JA No PWR");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;  //
      //      multi_list.multi = &multi_allja;
      init_multi(&multi_allja,-1,-1);
      break;
    case 9:  // ACAG
      strcpy(plogw->contest_name, "ACAG(no multi)");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 3;  // JA contest tail character is power code (P,L,M, H) but not check multi
      //      multi_list.multi = NULL;
      init_multi(NULL,-1,-1);
      break;
    case 10:  // Kanagawa contest
      strcpy(plogw->contest_name, "KanagawaInt");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 0;  //
      //      multi_list.multi = &multi_knint;
      init_multi(&multi_knint,-1,-1);
      break;
    case 11:  // Yokohama contest
      strcpy(plogw->contest_name, "Yokohama");
      plogw->mask = 0xff;     // CW/PH OK
      plogw->cw_pts = 3;      // ---> wrong Yokohama CW 3pt Ph 2pt / Ext CW 1pt Ph 1 pt
      plogw->multi_type = 0;  //
      //      multi_list.multi = &multi_yk;
      init_multi(&multi_yk,-1,-1);
      break;
    case 12:  // UEC contest
      strcpy(plogw->contest_name, "UEC contest");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 2;       // ---> wrong Yokohama CW 3pt Ph 2pt / Ext CW 1pt Ph 1 pt
      plogw->multi_type = 2;   //
      //      multi_list.multi = &multi_allja;
      init_multi(&multi_allja,-1,-1);
      break;
    case 13:  // Tsurumigawa
      strcpy(plogw->contest_name, "Tsurumigawa");
      plogw->mask = 0xff;     // CW/PH OK
      plogw->cw_pts = 2;      //
      plogw->multi_type = 0;  //
      //multi_list.multi = &multi_tmtest;
      init_multi(&multi_tmtest,-1,-1);
      break;
    case 14:  // JA8 contest
      strcpy(plogw->contest_name, "JA8(int)contest");
      plogw->mask = 0xff - 3;  // CW/PH NG
      plogw->cw_pts = 1;
      plogw->multi_type = 4;   // JA8 last character is age and multi check ignore the last character
      //      multi_list.multi = &multi_ja8int;
      init_multi(&multi_ja8int,-1,-1);
      break;
  case 15: // arrl int'l cw
    //#ifdef notdef  //  somehow leading to reboot , maybe multi number mismatch
    strcpy(plogw->contest_name, "ARRL int'l");
    plogw->mask = 0xff - 3;  // CW/PH NG
    plogw->cw_pts = 1;
    plogw->multi_type = 0;  
    //    plogw->contest_band_mask = BAND_MASK_ALL;  // all frequency bands are permitted
    //    plogw->sent_exch_abbreviation_level=3; // 0 -> T 1-> A 9 -> N  // 弱小局は略さない方がよい
    //    multi_list.multi = &multi_arrl;
    init_multi(&multi_arrl,-1,-1);
    //    #endif
    break;
  case 16:  // HS WAS
    strcpy(plogw->contest_name, "HSWAScontest");
    plogw->mask = 0xff ;  // CW/PH OK
    plogw->cw_pts = 1;
    plogw->multi_type = 5;   
    //    plogw->contest_band_mask = BAND_MASK_ALL;  //
    //    multi_list.multi = &multi_hswas;
    init_multi(&multi_hswas,-1,-1);
    break;
  case 17:  // Yamanashi
    strcpy(plogw->contest_name, "YN contest");
    plogw->mask = 0xff ;  // CW/PH OK
    plogw->cw_pts = 1;
    plogw->multi_type = 0;   
    //    plogw->contest_band_mask = BAND_MASK_ALL;  //
    //    multi_list.multi = &multi_yntest;
    init_multi(&multi_yntest,-1,-1);
    break;

  case 18:  // ACAG with multi check
    strcpy(plogw->contest_name, "ACAG(multi chk)");
    plogw->mask = 0xff - 3;  // CW/PH NG
    plogw->cw_pts = 1;
    plogw->multi_type = 3;  // JA contest tail character is power code (P,L,M, H) but not check multi 
    //multi_list.multi = &multi_acag;
    init_multi(&multi_acag,-1,-1);
    break;
  case 19:  // FD
    strcpy(plogw->contest_name, "FD");
    plogw->mask = 0xff - 3;  // CW/PH NG
    plogw->cw_pts = 1;
    plogw->multi_type = 1;  // JA contest (tail character is power code (P,L,M, H) but not check multi)
    //multi_list.multi = &multi_acag;
    init_multi(&multi_allja,1,10); // to 1200 ken multi
    init_multi(&multi_acag,11,-1); // from 2400 shigun multi
    break;
    
    
  }
  upd_display_info_contest_settings();
}
