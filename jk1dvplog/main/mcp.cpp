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
#include <Adafruit_MCP23X17.h>
#include "decl.h"
#include "variables.h"

Adafruit_MCP23X17 mcp;

// I2C device 0x20 : MCP23017 IO expander
// connected to
// A7  RX2-R (was SO3R Phone SW #3 RELAY RESET )
// A6  RX2-L (was SO3R Phone SW #3 RELAY SET)
// A5  RX1-R
// A4  RX0-R
// A3  RELAY1 connected to 4p connector
// A2  RELAY2 connected to 4p connector
// A1  RX1-L
// A0  RX0-L
#define RELAY2 2
#define RELAY1 3
#define SO3R_RX1L 0
#define SO3R_RX1R 1
#define SO3R_RX2L 4
#define SO3R_RX2R 5
#define SO3R_RX3L 6
#define SO3R_RX3R 7

#define SO3R_3_RL_SET 6
#define SO3R_3_RL_RESET 7

// B7 Reserved
// B6 Ant SW 6
// B5 Ant SW 5
// B4 Ant SW 4
// B3 Ant SW 3
// B2 Ant SW 2
// B1 Ant SW 1
// B0  Ant SW 0
#define ANT_SW_0 8
#define ANT_SW_1 9
#define ANT_SW_2 10
#define ANT_SW_3 11
#define ANT_SW_4 12
#define ANT_SW_5 13
#define ANT_SW_6 14

#define  MIC1_EN 8
#define  MIC2_EN 9
#define PTT1_CTRL 11
#define PTT2_CTRL 12

#define I2C_GPIO_ADDR 0x20

void init_mcp_port() {
  // uncomment appropriate mcp.begin
  if (!mcp.begin_I2C(0x20, &Wire)) {
    //if (!mcp.begin_SPI(CS_PIN)) {
    plogw->ostream->println("Error.");
  }
  //  mcp.digitalWrite(CW_KEY1, 0);
  //  mcp.digitalWrite(CW_KEY2, 0);
  //  mcp.digitalWrite(SO3R_3_RL_RESET, 0);
  //  mcp.digitalWrite(SO3R_3_RL_SET, 0);
  mcp.digitalWrite(SO3R_RX1L, 1);
  mcp.digitalWrite(SO3R_RX1R, 1);
  mcp.digitalWrite(SO3R_RX2L, 0);
  mcp.digitalWrite(SO3R_RX2R, 0);
  mcp.digitalWrite(SO3R_RX3L, 0);
  mcp.digitalWrite(SO3R_RX3R, 0);
  mcp.digitalWrite(RELAY1, 0);
  mcp.digitalWrite(RELAY2, 0);


  mcp.digitalWrite(MIC1_EN, 1);
  mcp.digitalWrite(MIC2_EN, 0);
  mcp.digitalWrite(PTT1_CTRL, 0);
  mcp.digitalWrite(PTT2_CTRL, 0);

  mcp.pinMode(MIC1_EN,OUTPUT);
  mcp.pinMode(MIC2_EN,OUTPUT);  
  mcp.pinMode(PTT1_CTRL,OUTPUT);
  mcp.pinMode(PTT2_CTRL,OUTPUT);  
  
  mcp.pinMode(SO3R_RX1L, OUTPUT);
  mcp.pinMode(SO3R_RX1R, OUTPUT);
  mcp.pinMode(SO3R_RX2L, OUTPUT);
  mcp.pinMode(SO3R_RX2R, OUTPUT);
  mcp.pinMode(SO3R_RX3L, OUTPUT);
  mcp.pinMode(SO3R_RX3R, OUTPUT);


  // mcp.pinMode(CW_KEY1, OUTPUT);
  // mcp.pinMode(CW_KEY2, OUTPUT);
  //  mcp.pinMode(SO3R_3_RL_SET, OUTPUT);
  //  mcp.pinMode(SO3R_3_RL_RESET, OUTPUT);
  mcp.pinMode(RELAY1, OUTPUT);
  mcp.pinMode(RELAY2, OUTPUT);
}

// operate onboard phone switch
// here switch control(left, right) is logic sum of bit phone number (1:RX1 2: RX2 4: RX3)
void phone_switch(int left, int right) {

  if (!plogw->f_console_emu) {
    plogw->ostream->print("phone_switch():L");
    plogw->ostream->print(left);
    plogw->ostream->print(" R");
    plogw->ostream->println(right);
  }
  if (left & 1) {
    // RX1 on the left`
    mcp.digitalWrite(SO3R_RX1L, 1);
  } else {
    mcp.digitalWrite(SO3R_RX1L, 0);
  }
  if (left & 2) {
    mcp.digitalWrite(SO3R_RX2L, 1);
  } else {
    mcp.digitalWrite(SO3R_RX2L, 0);
  }
  if (left & 4) {
    mcp.digitalWrite(SO3R_RX3L, 1);
  } else {
    mcp.digitalWrite(SO3R_RX3L, 0);
  }
  if (right & 1) {
    // RX1 on the right`
    mcp.digitalWrite(SO3R_RX1R, 1);
  } else {
    mcp.digitalWrite(SO3R_RX1R, 0);
  }
  if (right & 2) {
    mcp.digitalWrite(SO3R_RX2R, 1);
  } else {
    mcp.digitalWrite(SO3R_RX2R, 0);
  }
  if (right & 4) {
    mcp.digitalWrite(SO3R_RX3R, 1);
  } else {
    mcp.digitalWrite(SO3R_RX3R, 0);
  }

}
// mapping mcp23017          
// GPA6 JP4 1   (PH3 L)       6
//   A7 JP4 2   (PH3 R)       7
// GPB0 JP4 3   MIC1_EN       8
//   B1     4   MIC2_EN       9
//   B2     5  (MIC3_EN)     10
//   B3     6   PTT1_CTRL    11
//   B4     7   PTT2_CTRL    12
//   B5     8  (PTT3_CTRL)   13
//   B6     9                14
//   B7    10                15
// sel: 0 MIC1 sel 1 MIC2 sel

void mic_switch(int sel) 
{
  switch(sel) {
  case 0: // MIC1 SEL
    mcp.digitalWrite(MIC2_EN, 0);    
    mcp.digitalWrite(MIC1_EN, 1);
    break;
  case 1: // MIC2 SEL
    mcp.digitalWrite(MIC1_EN, 0);    
    mcp.digitalWrite(MIC2_EN, 1);    
    break;
  default: // other : both mic off
    mcp.digitalWrite(MIC1_EN, 0);    
    mcp.digitalWrite(MIC2_EN, 0);    
    break;
  }
}

// assume only one rig can be transmitted 
void ptt_switch(int sel,int on) 
{
  switch(sel) {
  case 0: // PTT1 SEL
    mcp.digitalWrite(PTT1_CTRL, on);
    mcp.digitalWrite(PTT2_CTRL, 0);   
    break;
  case 1: // PTT2 SEL
    mcp.digitalWrite(PTT1_CTRL, 0);    
    mcp.digitalWrite(PTT2_CTRL, on);    
    break;
  default: // other : both PTT off
    mcp.digitalWrite(PTT1_CTRL, 0);    
    mcp.digitalWrite(PTT2_CTRL, 0);    
    break;
  }
}

void write_mcp_gpio(int tmp,int tmp1)
{
  mcp.digitalWrite(tmp, tmp1);
}

int read_mcp_gpio(int i)
{
  return mcp.digitalRead(i);
}
