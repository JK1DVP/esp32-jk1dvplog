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

//#define N_MULTI 98
#include "Arduino.h"
#include "multi.h"
const struct multi_item multi_arrl = {
   {
"AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DC", "DE", "FL", 
"GA", "HI", "ID", "IL", "IN", "IA", "KS", "KY", "LA", "ME", 
"MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH", 
"NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI", 
"SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", 
"WY", "AB", "BC", "LB", "MB", "NB", "NF", "NS", "NT", "NU", 
"ON", "PE", "QC", "SK", "YT", "AGS", "BAC", "BCS", "CAM", "CHI", 
"CHH", "CMX", "COA", "COL", "DGO", "EMX", "GTO", "GRO", "HGO", "JAL", 
"MIC", "MOR", "NAY", "NLE", "OAX", "PUE", "QRO", "QUI", "SLP", "SIN", 
"SON", "TAB", "TAM", "TLX", "VER", "YUC", "ZAC", ""
},
{
"Alabama", "Alaska", "Arizona", "Arkansas", "California", "Colorado", "Connecticut", "District_of_Columbia", "Delaware", "Florida", 
    "Georgia", "Hawaii", "Idaho", "Illinois", "Indiana", "Iowa", "Kansas", "Kentucky", "Louisiana", "Maine", 
    "Maryland", "Massachusetts", "Michigan", "Minnesota", "Mississippi", "Missouri", "Montana", "Nebraska", "Nevada", "New_Hampshire", 
    "New_Jersey", "New_Mexico", "New_York", "North_Carolina", "North_Dakota", "Ohio", "Oklahoma", "Oregon", "Pennsylvania", "Rhode_Island", 
    "South_Carolina", "South_Dakota", "Tennessee", "Texas", "Utah", "Vermont", "Virginia", "Washington", "West Virginia", "Wisconsin", 
    "Wyoming", "Alberta_(VE6)", "British_Columbia (VE7)", "Labrador_(VO2)", "Manitoba_(VE4)", "New_Brunswick_(VE9)", "Newfoundland_and_Labrador_(VO1/VO2)", "Nova_Scotia_(VE1)", "Northwest_Territories_(VE8)", "Nunavut_(VY0)", 
    "Ontario_(VE3)", "Prince_Edward_Island_(VY2)", "Quebec_(VE2)", "Saskatchewan_(VE5)", "Yukon Territories (VY1)", "Aguascalientes", "Baja_California", "Baja_California_Sur", "Campeche", "Chiapas", 
    "Chihuahua", "Ciudad_de_México", "Coahuila", "Colima", "Durango", "Estado_de_México", "Guanajuato", "Guerrero", "Hidalgo", "Jalisco", 
    "Michoacán", "Morelos", "Nayarit", "Nuevo_León", "Oaxaca", "Puebla", "Querétaro", "Quintana_Roo", "San_Luis_Potosí", "Sinaloa", 
    "Sonora", "Tabasco", "Tamaulipas", "Tlaxcala", "Veracruz", "Yucatán", "Zacatecas", ""
  }
};
