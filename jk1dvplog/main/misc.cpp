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
#include "callhist.h"
#include "variables.h"
#include "misc.h"

// copy idx'th token in src to dest with separator character in sep
void copy_token(char *dest,char *src,int idx,char *sep) {
  char *p,*pd;
  int n;
  pd=dest;
  n=0;
  p=src;
  while (*p) {
    if (strchr(sep,*p)!=NULL) {
      // separator
      p++; n++;
      if (n>idx) {
	// end copying the token now
	*pd='\0';
	return;
      }
      continue;
    }
    if (n==idx) {
      // in the target token
      *pd++=*p++;
      continue;
    } else {
      p++;
      continue;
    }
  }
  *pd='\0';
  return;
}
      

// measure processing time in us and update

int time_measure_bank[N_TIME_MEASURE_BANK];
int time_measure_bank_tmp[N_TIME_MEASURE_BANK];

void time_measure_clear(int bank)
{
  if (bank<0|| bank >=N_TIME_MEASURE_BANK) return;
  time_measure_bank[bank]=0;
}

void time_measure_start(int bank)
{
  if (bank<0|| bank >=N_TIME_MEASURE_BANK) return;
  time_measure_bank_tmp[bank]=micros();
}

void time_measure_stop(int bank)
{
  if (bank<0|| bank >=N_TIME_MEASURE_BANK) return;
  time_measure_bank_tmp[bank]=micros()-time_measure_bank_tmp[bank];
  // maximum update
  if (time_measure_bank[bank]< time_measure_bank_tmp[bank]) {
    // update time_measure_bank
    time_measure_bank[bank]=time_measure_bank_tmp[bank];
  }
}

int time_measure_get(int bank)
{
  if (bank<0|| bank >=N_TIME_MEASURE_BANK) return 0;
  return     time_measure_bank[bank];
}

// concatenate string to represent bin in 2's binary for number of digits
void print_bin(char *print_to, unsigned int bin, int digits) {
  char *p;
  p=print_to;
  // make binary bits in reversed order
  for (int i=digits-1;i>=0;i--) {
    if ((1<<i) & bin) {
      *p++='1';
    } else {
      *p++='0';
    }
  }
  *p='\0'; // terminate
}

void release_memory() {
  plogw->ostream->print("freeing memory");
  close_callhist();
  release_callhist();
  //  release_sat();

  // release bandmap
  for (int idx = 0; idx < N_BAND; idx++) {
    if (bandmap[idx].nentry > 0) {
      free(bandmap[idx].entry);
      bandmap[idx].entry = NULL;
      bandmap[idx].nentry = 0;
      bandmap[idx].nstations = 0;
    }
  }
  bandmap_disp.cursor = 0;
  bandmap_disp.f_update = 0;

  plogw->ostream->println("memory free");
}

void print_memory()
{
  Serial.printf("===============================================================\n");
  Serial.printf("Mem Test\n");
  Serial.printf("===============================================================\n");
  Serial.printf("esp_get_free_heap_size()                              : %6d\n", esp_get_free_heap_size() );
  Serial.printf("esp_get_minimum_free_heap_size()                      : %6d\n", esp_get_minimum_free_heap_size() );
  //xPortGetFreeHeapSize()（データメモリ）ヒープの空きバイト数を返すFreeRTOS関数です。これはを呼び出すのと同じheap_caps_get_free_size(MALLOC_CAP_8BIT)です。
  Serial.printf("xPortGetFreeHeapSize()                                : %6d\n", xPortGetFreeHeapSize() );
  //xPortGetMinimumEverFreeHeapSize()また、関連heap_caps_get_minimum_free_size()するものを使用して、ブート以降のヒープの「最低水準点」を追跡できます。
  Serial.printf("xPortGetMinimumEverFreeHeapSize()                     : %6d\n", xPortGetMinimumEverFreeHeapSize() );
  //heap_caps_get_free_size() さまざまなメモリ機能の現在の空きメモリを返すためにも使用できます。
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_EXEC)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_EXEC) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_32BIT)             : %6d\n", heap_caps_get_free_size(MALLOC_CAP_32BIT) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_8BIT)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_DMA)               : %6d\n", heap_caps_get_free_size(MALLOC_CAP_DMA) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID2)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID2) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID3)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID3)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID4) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID4)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID5) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID5)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID6) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID6)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID7) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_PID7)              : %6d\n", heap_caps_get_free_size(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_SPIRAM)            : %6d\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_INTERNAL)          : %6d\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_DEFAULT)           : %6d\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT) );
  //Serial.printf("heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT)         : %6d\n", heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT) );
  Serial.printf("heap_caps_get_free_size(MALLOC_CAP_INVALID)           : %6d\n", heap_caps_get_free_size(MALLOC_CAP_INVALID) );
  //heap_caps_get_largest_free_block()ヒープ内の最大の空きブロックを返すために使用できます。これは、現在可能な最大の単一割り当てです。この値を追跡し、合計空きヒープと比較すると、ヒープの断片化を検出できます。
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_EXEC)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_EXEC) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_32BIT)    : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_32BIT) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_DMA)      : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_DMA) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID2)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID2) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID3)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID3)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID4) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID4)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID5) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID5)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID6) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID6)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID7) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_PID7)     : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)   : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)  : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) );
  //Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT): %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT) );
  Serial.printf("heap_caps_get_largest_free_block(MALLOC_CAP_INVALID)  : %6d\n", heap_caps_get_largest_free_block(MALLOC_CAP_INVALID) );
  //heap_caps_get_minimum_free_size()指定された機能を持つすべての領域の合計最小空きメモリを取得します。
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_EXEC)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_EXEC) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT)     : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_DMA)       : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_DMA) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID2)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID2) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID3)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID3)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID4) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID4)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID5) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID5)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID6) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID6)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID7) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_PID7)      : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_PID3) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)    : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)  : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)   : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) );
  //Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_IRAM_8BIT) : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_IRAM_8BIT) );
  Serial.printf("heap_caps_get_minimum_free_size(MALLOC_CAP_INVALID)   : %6d\n", heap_caps_get_minimum_free_size(MALLOC_CAP_INVALID) );
  //heap_caps_get_info()multi_heap_info_t上記の関数からの情報に加えて、ヒープ固有の追加データ（割り当て数など）を含む構造体を返します。
  //Skip
  // heap_caps_print_heap_info()が返す情報の要約をstdoutに出力しheap_caps_get_info()ます。
  //Serial.printf("heap_caps_print_heap_info(MALLOC_CAP_INTERNAL) :\n");
  //heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
  // heap_caps_dump()そしてheap_caps_dump_all()意志出力は、ヒープ内の各ブロックの構造に関する情報を詳述します。これは大量の出力になる可能性があることに注意してください。
  //Serial.printf("heap_caps_dump() :\n");
  //heap_caps_dump(MALLOC_CAP_INTERNAL);


  Serial.println("This task watermark: " + String(uxTaskGetStackHighWaterMark(NULL)) + " bytes");
  }


