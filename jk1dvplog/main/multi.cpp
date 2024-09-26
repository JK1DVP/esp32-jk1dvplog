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

#include "multi.h"

const struct multi_item multi_hswas = {
  {
    "GL","350101","350102","350103","350104","350105","350106","350107","350108",
    "3502",    "3503","3504","3505","3508","3509","3510","3511","3512","3513",
    "3514","3515","3516",
    "35001","35007","35008","35010","35016",""
  },
  {
    "GLoc","HS-Naka","HS-Higashi","HS-Minami","HS-Nishi","HS-AsaMinami","HS-AsaKita","HS-Aki","HS-Saeki",
    "Kure","Takehara","Mihara","Onomichi","Fukuyama","Fuchu","Shobara","Otake","HigashiHS",
    "Hatsukaichi","AkiTakada","Edajima",
    "Aki-G","Jinseki-G","Sera-G","Toyoda-G","Yamagata-G",""
  }
};


const struct multi_item multi_kcj = {
  {
    "SY", "RM", "KK", "SC", "IS", "NM", "SB", "TC", "KR", "HD", "IR", "HY", "OM", "OH",
    "AM", "IT", "AT", "YM", "MG", "FS",
    "NI", "NN",
    "TK", "KN", "CB", "ST", "IB", "TG", "GM", "YN",
    "SO", "GF", "AC", "ME",
    "KT", "SI", "NR", "OS", "WK", "HG",
    "TY", "FI", "IK",
    "OY", "SN", "YG", "TT", "HS",
    "KA", "TS", "EH", "KC",
    "FO", "SG", "NS", "KM", "OT", "MZ", "KG", "ON", "OG", "MT",
    "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", ""
  },
  {
    "Soya", "Rumoi", "Kamikawa", "Sorachi", "Ishikari",
    "Nemuro", "Shiribeshi", "Tokachi", "Kushiro", "Hidaka",
    "Iburi", "Hiyama", "Oshima", "Okhotsk",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima",
    "Niigata", "Nagano",
    "Tokyo", "Kanagawa", "Chiba", "Saitama", "Ibaraki", "Tochigi", "Gumma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie",
    "Kyoto", "Shiga", "Nara", "Osaka", "Wakayama", "Hyogo",
    "Toyama", "Fukui", "Ishikawa",
    "Okayama", "Shimane", "Yamaguchi", "Tottori", "Hiroshima",
    "Kagawa", "Tokushima", "Ehime", "Kochi",
    "Fukuoka", "Saga", "Nagasaki", "Kumamoto", "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", "Minamitorishima",
    "NW NA", "NE NA", "W NA", "C NA", "E NA", "S NA", "C Am", "West Indies",
    "N SouthAm", "W SouthAm", "C South Am", "SW SouthAm", "SE SouthAm",
    "W Europe", "C Europe", "E Europe", "W Siberia", "C Siberia", "E Siberia",
    "Balkan", "SW Asia", "S Asia", "C Asia", "E Asia", "Japanese", "SE Asia",
    "Philippine", "Indonesian", "W Australia", "E Australia", "C Pacific",
    "New Zealand", "NW Africa", "NE Africa", "C Africa", "EquatorialAF",
    "E Africa", "S Africa", "Madagascar", "N Atlantic", ""
  }
};

const struct multi_item multi_kantou = {
  { "100101", "100102", "100103", "100104", "100105", "100106", "100107", "100108", "100109", "100110", "100111",
    "100112", "100113", "100114", "100115", "100116", "100117", "100118", "100119", "100120", "100121", "100122",
    "100123", "1002", "1003", "1004", "1005", "1006", "1007", "1008", "1009", "1010", "1011", "1012", "1013", "1014",
    "1015", "1016", "1019", "1020", "1021", "1022", "1023", "1024", "1025", "1026", "1028", "1029", "1030", "1101",
    "1102", "1103", "1104", "1105", "1106", "1107", "1108", "1109", "1110", "1111", "1112", "1113", "1114", "1115",
    "1116", "1117", "1118", "1119", "1201", "1202", "1203", "1204", "1205", "1206", "1207", "1208", "1210", "1211",
    "1212", "1213", "1215", "1216", "1217", "1218", "1219", "1220", "1221", "1222", "1223", "1224", "1225", "1226",
    "1227", "1228", "1229", "1230", "1231", "1232", "1233", "1234", "1235", "1236", "1237", "1238", "1239", "1302",
    "1303", "1304", "1306", "1307", "1308", "1309", "1310", "1311", "1312", "1314", "1315", "1316", "1317", "1318",
    "1319", "1321", "1322", "1323", "1324", "1325", "1327", "1328", "1329", "1330", "1331", "1332", "1333", "1334",
    "1336", "1337", "1338", "1339", "1340", "1341", "1342", "1343", "1344", "1345", "1346", "1401", "1402", "1403",
    "1404", "1405", "1407", "1408", "1410", "1412", "1414", "1415", "1416", "1417", "1419", "1420", "1421", "1422",
    "1423", "1424", "1425", "1426", "1427", "1428", "1429", "1430", "1431", "1432", "1433", "1434", "1435", "1436",
    "1437", "1501", "1502", "1503", "1504", "1505", "1506", "1508", "1509", "1510", "1511", "1513", "1514", "1515",
    "1516", "1601", "1602", "1603", "1604", "1605", "1606", "1607", "1608", "1609", "1610", "1611", "1612", "1701",
    "1702", "1704", "1705", "1706", "1707", "1708", "1709", "1710", "1711", "1712", "1713", "1714", "10002", "10004",
    "10005", "10006", "10007", "11001", "11002", "11003", "11004", "11006", "11007", "12001", "12002", "12004", "12006",
    "12008", "12011", "13001", "13002", "13003", "13004", "13006", "13007", "13008", "13009", "14001", "14003", "14004",
    "14005", "14008", "14012", "14014", "15004", "15005", "15006", "15007", "15008", "16001", "16003", "16004", "16005",
    "16007", "16009", "16010", "17002", "17003", "17004", "17007", "17008", "110101", "110102", "110103", "110104", "110105",
    "110106", "110107", "110108", "110109", "110110", "110111", "110112", "110113", "110114", "110115", "110116", "110117",
    "110118", "110301", "110302", "110303", "110304", "110305", "110306", "110307", "111001", "111002", "111003", "120101",
    "120102", "120103", "120104", "120105", "120106", "134401", "134402", "134403", "134404", "134405", "134406", "134407",
    "134408", "134409", "134410", ""
  },
  { "Chiyoda", "Chuo", "Minato", "Shinjuku", "Bunkyo", "Taito", "Sumida", "Koto", "Shinagawa", "Meguro", "Ota", "Setagaya",
    "Shibuya", "Nakano", "Suginami", "Toshima", "Kita", "Arakawa", "Itabashi", "Nerima", "Adachi", "Katsushika", "Edogawa",
    "Hachioji", "Tachikawa", "Musashino", "Mitaka", "Ome", "Fuchu", "Akishima", "Chofu", "Machida", "Koganei", "Kodaira",
    "Hino", "Higashimurayama", "Kokubunji", "Kunitachi", "Fussa", "Komae", "Higashiyamato", "Kiyose", "Higashikurume",
    "Musashimurayama", "Tama", "Inagi", "Hamura", "Akiruno", "Nishitokyo", "Yokohama", "Yokosuka", "Kawasaki", "Hiratsuka",
    "Kamakura", "Fujisawa", "Odawara", "Chigasaki", "Zushi", "Sagamihara", "Miura", "Hadano", "Atsugi", "Yamato", "Isehara",
    "Ebina", "Zama", "Minamiashigara", "Ayase", "Chiba", "Choshi", "Ichikawa", "Funabashi", "Tateyama", "Kisarazu", "Matsudo",
    "Noda", "Mobara", "Narita", "Sakura", "Togane", "Asahi", "Narashino", "Kashiwa", "Katsuura", "Ichihara", "Nagareyama",
    "Yachiyo", "Abiko", "Kamogawa", "Kimitsu", "Kamagaya", "Futtu", "Urayasu", "Yotsukaido", "Sodegaura", "Yachimata",
    "Inzai", "Shiroi", "Tomisato", "Minamiboso", "Sosa", "Katori", "Sanmu", "Isumi", "Oamishirasato", "Kawagoe", "Kumagaya",
    "Kawaguchi", "Gyoda", "Chichibu", "Tokorozawa", "Hanno", "Kazo", "Honjo", "Higashimatsuyama", "Kasukabe", "Sayama", "Hanyu",
    "Konosu", "Fukaya", "Ageo", "Soka", "Koshigaya", "Warabi", "Toda", "Iruma", "Asaka", "Shiki", "Wako", "Niiza", "Okegawa",
    "Kuki", "Kitamoto", "Yashio", "Fujimi", "Misato", "Hasuda", "Sakado", "Satte", "Tsurugashima", "Hidaka", "Yoshikawa",
    "Saitama", "Fujimino", "Shiraoka", "Mito", "Hitachi", "Tsuchiura", "Koga", "Ishioka", "Yuki", "Ryugasaki", "Shimotsuma",
    "Hitachiota", "Takahagi", "Kitaibaraki", "Kasama", "Toride", "Ushiku", "Tsukuba", "Hitachinaka", "Kashima", "Itako",
    "Moriya", "Hitachiomiya", "Naka", "Chikusei", "Bandou", "Inashiki", "Kasumigaura", "Sakuragawa", "Kamisu", "Namegata",
    "Hokota", "Joso", "Tsukubamirai", "Omitama", "Utsunomiya", "Ashikaga", "Tochigi", "Sano", "Kanuma", "Nikko", "Oyama",
    "Mooka", "Otawara", "Yaita", "Nasushiobara", "Sakura", "Nasukarasuyama", "Shimotsuke", "Maebashi", "Takasaki", "Kiryu",
    "Isesaki", "Ota", "Numata", "Tatebayashi", "Shibukawa", "Fujioka", "Tomioka", "Annaka", "Midori", "Kofu", "Fujiyoshida",
    "Tsuru", "Yamanashi", "Otsuki", "Nirasaki", "Minami-Alps", "Hokuto", "Kai", "Fuefuki", "Uenohara", "Koshu", "Chuo",
    "Nishitama", "Oshima-Shicho", "Miyake-Shicho", "Hachijo-Shicho", "Ogasawara-Shicho", "Aiko", "Ashigarakami", "Ashigarashimo",
    "Koza", "Naka", "Miura", "Awa", "Isumi", "Inba", "Katori", "Sambu", "Chosei", "Iruma", "Osato", "Kitaadachi",
    "Kitakatsushika", "Kodama", "Chichibu", "Hiki", "Minamisaitama", "Inashiki", "Kitasoma", "Kuji", "Sashima", "Naka",
    "Higashiibaraki", "Yuki", "Kawachi", "Shioya", "Shimotsuga", "Nasu", "Haga", "Agatsuma", "Ora", "Kanra", "Kitagumma",
    "Sawa", "Tano", "Tone", "Kitatsuru", "Nakakoma", "Nishiyatsushiro", "Minamikoma", "Minamitsuru", "YokohamaTsurumi",
    "YokohamaKanagawa", "YokohamaNishi", "YokohamaNaka", "YokohamaMinami", "YokohamaHodogaya", "YokohamaIsogo", "YokohamaKanazawa",
    "YokohamaKohoku", "YokohamaTotsuka", "YokohamaKonan", "YokohamaAsahi", "YokohamaMidori", "YokohamaSeya", "YokohamaSakae",
    "YokohamaIzumi", "YokohamaAoba", "YokohamaTsuzuki", "KawasakiKawasaki", "KawasakiSaiwai", "KawasakiNakahara", "KawasakiTakatsu",
    "KawasakiTama", "KawasakiMiyamae", "KawasakiAsao", "SagamiharaMidori", "SagamiharaChuo", "SagamiharaMinami", "ChibaChuo",
    "ChibaHanamigawa", "ChibaInage", "ChibaWakaba", "ChibaMidori", "ChibaMihama", "SaitamaNishi", "SaitamaKita", "SaitamaOmiya",
    "SaitamaMinuma", "SaitamaChuo", "SaitamaSakura", "SaitamaUrawa", "SaitamaMinami", "SaitamaMidori", "SaitamaIwatsuki", ""
  }
};


const struct multi_item multi_saitama_int = {
  { "1302", "1303", "1304", "1306", "1307", "1308", "1309", "1310" , "1311" , "1312", "1314" , "1315", "1316", "1317", "1318",
    "1319", "1321", "1322" , "1323", "1324", "1325",
    "1327", "1328" , "1329", "1330" , "1331", "1332", "1333", "1334", "1336", "1337", "1338", "1339" , "1340", "1341", "1342", "1343",
    "134401", "134402", "134403", "134404", "134405", "134406", "134407" , "134408", "134409", "134410", "1345", "1346",
    "130012", "130014", "130015", "130026" , "130031", "130043", "130044" , "130062", "130063", "130064", "130072", "130073", "130074", "130075" ,
    "130079", "130081" , "130082", "130084" , "130085" , "130086" , "130087", "130089", "130093",
    "101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "111", " 112", "113", "114",
    "02", "03", "04", "05", "06", "07", "08", "09", "10",
    "11", "12", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47" , "48", ""
  },
  { "kawagoe" , "kumagaya", "kawaguchi", "gyoda", "chichibu", "tokorozawa", "hanno", "kazo", "honsho", "higashimatsuyama", "kasukabe", "sayama", "hanyu", "konosu",
    "fukaya", "ageo", "soka", "koshigaya",
    "warabi", "toda", "iruma", "asaka", "shiki", "wako", "niiza", "okegawa", "kuki", "kitamoto", "yashio", "fujimi", "misato", "hasuda", "sakado", "satte", "tsurugashima",
    "hidaka", "yoshikawa",
    "saitama-W", "saitama-N", "saitama-Omiya", "saitama-minuma", "saitama-chuo", "saitama-sakura", "saitama-urawa", "saitama-minami", "saitama-midori", "saitama-iwatsuki",
    "fujimino", "shiraoka",
    "ogose-T", "miyoshi-T", "moroyama-T", "yorii-T", "ina-T", "sugito-T", "matsubuse-T", "kamisato-T", "misato-T", "kamikawa-T", "oshikano-T", "nagatoro-T", "higashi-chichibu-V",
    "yokoze-T", "minano-T", "ogawa-T", "kawashima-T", "namerigawa-T", "hatoyama-T", "yoshimi-T", "ranzan-T", "tokigawa-T", "miyashiro-T",
    "Soya", "Rumoi", "Kamikawa", "Okhotsk", "Sorachi", "Ishikari", "Nemuro", "Shiribeshi", "Tokachi", "Kushiro", "Hidaka", "Iburi", "Hiyama", "Oshima",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima", "Niigata",
    "Nagano", "Tokyo", "Kanagawa", "Chiba", "Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie", "Kyoto", "Shiga", "Nara", "Osaka",
    "Wakayama", "Hyogo", "Toyama", "Fukui", "Ishikawa", "Okayama", "Shimane", "Yamaguchi",
    "Tottori", "Hiroshima", "Kagawa", "Tokushima", "Ehime", "Kochi", "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", ""
  }
};

const struct multi_item multi_tama = {
  { "OO", "TM" , "SE", "IN", "HA", "AN", "AK", "HM", "TA", "OK", "OU", "KA", "FU", "SA", "CH", "NA", "HI", "TT", "KU", "TK", "FS", "KO", "X", ""},
  { "Ota", "Tama", "Setagaya", "Inagi", "Hachioji", "Akiruno", "Akishima", "Hamura", "Tachikawa", "Okutama", "Oume", "Kawasaki-ku", "Fuchu", "Saiwai-ku", "Chofu", "Nakahara-ku", "Hino", "Takatsu-ku",
    "Kunitachi", "Tama-ku", "Fussa", "Komae", "Ext.", ""
  }
};

const struct multi_item multi_tokyouhf = {
  { "002", "003", "004", "005", "006", "007", "008", "009", "010", "011", "012", "013", "014", "015", "016", "019", "020", "021", "022", "023", "024", "025", "026", "028", "029", "030",
    "101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "111", "112", "113", "114", "115", "116", "117", "118", "119", "120", "121", "122", "123",
    "201", "202", "203", "204",
    "401", "402", "403", "404", "411", "412", "421", "422", "431",
    "01", "02", "03", "04", "05", "06", "07", "08", "09",
    "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47" , ""
  },
  { "Hachioji", "Tachikawa", "Musashino", "Mitaka", "Oume", "Fuchu", "Akishima",
    "Chofu", "Machida", "Koganei", "Kodaira", "Hino", "Higashi-Murayama", "Kokubunji",
    "Kunitachi", "Fussa", "Komae", "Higashi-Yamato", "Kiyose", "Higashi-Kurume", "Musashi-Murayama",
    "Tama", "Inagi", "Hamura", "Akiruno", "Nishi-Tokyo",
    "Chiyoda-ku", "Chuo-ku", "Minato-ku", "Shinjuku-ku", "Bunkyo-ku", "Taito-ku", "Sumida-ku", "Koto-ku", "Shinagawa-ku",
    "Meguro-ku", "Ota-ku", "Setagaya-ku", "Shibuya-ku", "Nakano-ku", "Suginami-ku", "Toshima-ku", "Kita-ku", "Arakawa-ku", "Itabashi-ku", "Nerima-ku", "Adachi-ku", "Katsushika-ku", "Edogawa-ku",
    "Mizuho-cho", "Hinode-cho", "Hinohara-mura", "Okutama-cho",
    "Oshima-cho", "Toshima-mura", "Niijima-mura", "Kozuzima-mura", "Miyake-mura", "Mikurajima-mura",
    "Hachijo-cho", "Aogashima-mura", "Ogasawara-mura",
    "Hokkaido", "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima", "Niigata",
    "Nagano", "Kanagawa", "Chiba", "Saitama", "Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie", "Kyoto", "Shiga", "Nara", "Osaka",
    "Wakayama", "Hyogo", "Toyama", "Fukui", "Ishikawa", "Okayama", "Shimane", "Yamaguchi",
    "Tottori", "Hiroshima", "Kagawa", "Tokushima", "Ehime", "Kochi", "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", ""
  }
};
const struct multi_item multi_cqzones = {
  { "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", ""
  },
  { "NW NA", "NE NA", "W NA", "C NA", "E NA", "S NA", "C Am", "West Indies",
    "N SouthAm", "W SouthAm", "C South Am", "SW SouthAm", "SE SouthAm",
    "W Europe", "C Europe", "E Europe", "W Siberia", "C Siberia", "E Siberia",
    "Balkan", "SW Asia", "S Asia", "C Asia", "E Asia", "Japanese", "SE Asia",
    "Philippine", "Indonesian", "W Australia", "E Australia", "C Pacific",
    "New Zealand", "NW Africa", "NE Africa", "C Africa", "Equatorial Africa",
    "E Africa", "S Africa", "Madagascar", "N Atlantic", ""
  }
};

const struct multi_item multi_allja = {
  {
    "101", "102", "103", "104", "105", "106", "107", "108", "109", "110",
    "111", "112", "113", "114",
    "02", "03", "04", "05", "06", "07",
    "08",  "09",
    "10",    "11", "12", "13", "14", "15", "16", "17",
    "18", "19",    "20", "21",
    "22", "23",	"24", "25", "26", "27",
    "28", "29",    "30",
    "31", "32", "33", "34", "35",
    "36", "37", "38", "39",
    "40", "41", "42", "43",
    "44", "45", "46", "47" , "48", ""
  },
  {
    "Soya", "Rumoi", "Kamikawa", "Okhotsk", "Sorachi", "Ishikari", "Nemuro", "Shiribeshi", "Tokachi", "Kushiro", "Hidaka", "Iburi", "Hiyama", "Oshima",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima",
    "Niigata",    "Nagano",
    "Tokyo", "Kanagawa", "Chiba", "Saitama", "Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie",
    "Kyoto", "Shiga", "Nara", "Osaka",    "Wakayama", "Hyogo",
    "Toyama", "Fukui", "Ishikawa",
    "Okayama", "Shimane", "Yamaguchi",    "Tottori", "Hiroshima",
    "Kagawa", "Tokushima", "Ehime", "Kochi",
    "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", ""
  }
};

// 横浜コンテスト
const struct multi_item multi_yk = {
  {
    "01", "02", "03", "04", "05", "06", "07", "08", "09", "10",
    "11", "12", "13", "14", "15", "16", "17", "18", "00", ""
  },
  { "Tsurumi", "Kanagawa", "Nishi", "Naka", "Minami", "Hodogaya", "Isogo", "KanazawaKu", "Kohoku", "Totsuka",
    "Konan", "Asahi", "Midori", "Seya", "Sakae", "Izumi", "Aoba", "Tsuzuki", "Ext", ""
  }
};

const struct multi_item multi_tmtest = {
	{
		"TS","KO","TZ","MI","AO",
		"KN","SA","NA","AS","TT",
		"MY","MA","IN","X",""
	} ,
	{ 
	"Tsurumi","Kohoku","Tsuzuki","Midori","Aoba",
	"Kanagawa","Saiwai","Nakahara","Asao","Takatsu",
	"Miyamae","Machida","Inagi","Ext","" }
};

const struct multi_item multi_knint = {
  {
    "110101", "110102", "110103", "110104", "110105", "110106", "110107", "110108", "110109", "110110",
    "110111", "110112", "110113", "110114", "110115", "110116", "110117", "110118",
    "110301", "110302", "110303", "110304", "110305", "110306", "110307",
    "111001", "111002", "111003",
    "11001", "11002", "11003", "11004", "11006", "11007",
    "1102", "1104", "1105", "1106", "1107", "1108", "1109",
    "1111", "1112", "1113", "1114", "1115", "1116", "1117", "1118", "1119",

    "101", "102", "103", "104", "105", "106", "107", "108", "109", "110",
    "111", "112", "113", "114",
    "02", "03", "04", "05", "06", "07",
    "08",  "09",
    "10",    "12", "13", "14", "15", "16", "17",
    "18", "19",    "20", "21",
    "22", "23",	"24", "25", "26", "27",
    "28", "29",    "30",
    "31", "32", "33", "34", "35",
    "36", "37", "38", "39",
    "40", "41", "42", "43",
    "44", "45", "46", "47" , "48", ""
  },
  {
    "Tsurumi", "Kanagawa", "Nishi", "Naka", "Minami", "Hodogaya", "Isogo", "KanazawaKu", "Kohoku", "Totsuka",
    "Konan", "Asahi", "Midori", "Seya", "Sakae", "Izumi", "Aoba", "Tsuzuki",
    "Kawasaki", "Saiwai", "Nakahara", "Takatsu", "Tama", "Miyamae", "Aso",
    "SagamiMidori", "SagamiChuo", "SagamiMinami",
    "Aiko", "AshigaraKami", "AshigaraShimo", "Koza", "Naka", "MiuraGun",
    "Yokosuka", "Hiratsuka", "Kamakura", "Fujisawa", "Odawara",
    "Chigasaki", "Zushi", "MiuraC", "Hadano", "Atsugi", "Yamato", "Isehara",
    "Ebina", "Zama", "MinamiAshigara", "Ayase",
    "Soya", "Rumoi", "Kamikawa", "Okhotsk", "Sorachi", "Ishikari", "Nemuro", "Shiribeshi", "Tokachi", "Kushiro", "Hidaka", "Iburi", "Hiyama", "Oshima",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima",
    "Niigata",    "Nagano",
    "Tokyo", "Chiba", "Saitama", "Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie",
    "Kyoto", "Shiga", "Nara", "Osaka",    "Wakayama", "Hyogo",
    "Toyama", "Fukui", "Ishikawa",
    "Okayama", "Shimane", "Yamaguchi",    "Tottori", "Hiroshima",
    "Kagawa", "Tokushima", "Ehime", "Kochi",
    "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", ""
  }
};

const struct multi_item multi_yntest = {

  {
    "1701", "1702", "1704",
    "1705" ,"1706" ,"1707", "1708",
    "1709", "1710" ,"1711", "1712",
    "1713" ,"1714",
    "17002", "17003",  "17004" ,
    "17007" ,"17008",
    "101", "102", "103", "104", "105", "106", "107", "108", "109", "110",
    "111", "112", "113", "114",
    "02", "03", "04", "05", "06", "07",
    "08",  "09",
    "10", "11", "12", "13", "14", "15", "16", 
    "18", "19",    "20", "21",
    "22", "23",	"24", "25", "26", "27",
    "28", "29",    "30",
    "31", "32", "33", "34", "35",
    "36", "37", "38", "39",
    "40", "41", "42", "43",
    "44", "45", "46", "47" , "48", ""
  },
  {
    "Kohu-c" , "FujiYoshida" , "Tsuru-c",
    "Yamanashi" , "Otsuki"  ,"Nirasaki" ,  "S-Alps",
    "Hokuto-c" ,"Kai-C" ,"Fuefuki-c" , "Uenohara-c",
    "Koshu-c" , "Chuo-c" ,
    "Kita-Tsuru-G", "Naka-Koma-G",
    "NishiYatsushiro-G","MinamiKoma-G",  "MinamiTsuru-G",
    "Soya", "Rumoi", "Kamikawa", "Okhotsk", "Sorachi", "Ishikari", "Nemuro", "Shiribeshi", "Tokachi", "Kushiro", "Hidaka", "Iburi", "Hiyama", "Oshima",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima",
    "Niigata",    "Nagano",
    "Tokyo", "Chiba", "Saitama", "Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie",
    "Kyoto", "Shiga", "Nara", "Osaka",    "Wakayama", "Hyogo",
    "Toyama", "Fukui", "Ishikawa",
    "Okayama", "Shimane", "Yamaguchi",    "Tottori", "Hiroshima",
    "Kagawa", "Tokushima", "Ehime", "Kochi",
    "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", ""
  }
};


const struct multi_item multi_ja8int = {
  {
"101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "202", "203", "204", "205", "206", "207", "208", "209", "210", "211", "212", "213", "214", "215", "216", "217", "218", "219", "220", "221", "222", "223", "224", "225", "226", "227", "228", "229", "230", "231", "233", "234", "235", "236", "303", "304", "331", "332", "333", "334", "337", "343", "345", "346", "347", "361", "362", "363", "364", "367", "370", "371", "391", "392", "393", "394", "395", "396", "397", "398", "399", "400", "401", "402", "403", "404", "405", "406", "407", "408", "409", "423", "424", "425", "427", "428", "429", "430", "431", "432", "433", "434", "436", "437", "438", "452", "453", "454", "455", "456", "457", "458", "459", "460", "461", "462", "463", "464", "465", "468", "469", "470", "471", "472", "481", "482", "483", "484", "485", "486", "487", "511", "512", "513", "514", "516", "517", "518", "519", "520", "543", "544", "545", "546", "547", "549", "550", "552", "555", "559", "560", "561", "562", "563", "564", "571", "575", "578", "581", "584", "585", "586", "601", "602", "604", "607", "608", "609", "610", "631", "632", "633", "634", "635", "636", "637", "638", "639", "641", "642", "643", "644", "645", "646", "647", "648", "649", "661", "662", "663", "664", "665", "667", "668", "691", "692", "693", "694", 
    "02", "03", "04", "05", "06", "07", "08", "09", "10",
    "11", "12", "13","14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47" , "48", ""
},
{
"sapporoC", "sapporoN", "sapporoE", "sapporoShiroishi", "sapporoToyohira", "sapporoS", "sapporoW", "sapporoAtsubetsu", "sapporoTeine", "sapporoKiyoda", "hakodateC", "otaruC", "asahikawaC", "muroranC", "kushiroC", "obihiroC", "kitamiC", "yuubariC", "iwamizawaC", "abashiriC", "rumoiC", "tomakomaiC", "wakkanaiC", "bibaiC", "ashibetsuC", "ebetsuC", "akabiraC", "mombetsuC", "shibetsuC", "nayoroC", "mikasaC", "nemuroC", "chitoseC", "takikawaC", "sunagawaC", "utashinaiC", "fukagawaC", "furanoC", "noboribetsuC", "eniwaC", "dateC", "kitahiroshimaC", "ishikarishi", "hokutoC", "tobetsuT", "shinshinotsuV", "matsumaeT", "fukushimaT", "shiriuchiT", "kikonaiT", "nanaeT", "shikabeT", "moriT", "yakumoT", "oshamambeT", "esashiT", "uenokuniT", "assabuT", "otobecho", "okushiriT", "imaganeT", "setanaT", "shimamakimura", "suttsuT", "kuromatsunaiT", "rankoshiT", "nisekoT", "makkariV", "rusutsuV", "kimobetsuT", "kyougokuT", "kutchanT", "kyouwaT", "iwanaiT", "hakuV", "kamoenaiV", "shakotanT", "furubiraT", "nikiT", "yoichiT", "akaigawaV", "namporoT", "naieT", "kamisunagawaT", "yuniT", "naganumaT", "kuriyamaT", "tsukigataT", "urausuT", "shintotsukawaT", "moseushiT", "chippubetsuT", "uryuuT", "hokuryuuT", "numataT", "takasuT", "higashikaguraT", "tomaT", "pippuT", "aibetsuT", "kamikawaT", "higashikawaT", "bieiT", "kamifuranoT", "nakafuranoT", "minamifuranoT", "shimukappuV", "wassamuT", "kembuchiT", "shimokawaT", "bifukaT", "otoineppuV", "nakagawaT", "horokanaiT", "mashikeT", "kodairaT", "tomamaeT", "hahoroT", "shosambetsuV", "embetsuT", "teshioT", "sarufutsuV", "hamatombetsuT", "nakatombetsuT", "esashiT", "toyotomiT", "rebunT", "rishiriT", "rishirifujiT", "horonobeT", "bihorocho", "tsubetsuT", "shariT", "kiyosatoT", "koshimizuT", "kunneppucho", "okitocho", "saromaT", "enkaruT", "yuubetsuT", "takigamiT", "okoppecho", "nishiokoppeV", "omucho", "ozoraT", "toyouraT", "sobetsuT", "shiraoiT", "atsumaT", "toyakoT", "abiraT", "mukawaT", "hidakaT", "hiratoriT", "niikappucho", "urakawaT", "samaniT", "erimoT", "shinhidakaT", "otoshinacho", "shihoroT", "kamishihoroT", "shikaoiT", "shintokuT", "shimizucho", "memuroT", "nakasatsunaiV", "sarabetsuV", "taijuT", "hirooT", "makubetsuT", "ikedacho", "toyokoroT", "hombetsuT", "ashoroT", "rikubetsuT", "urahoroT", "kushiroT", "akkeshiT", "hamanakaT", "shibechaT", "teshikutsucho", "tsuruiV", "shironukacho", "bekkaiT", "nakashibetsuT", "shibetsuT", "rausuT",
    "Aomori", "Iwate", "Akita", "Yamagata", "Miyagi", "Fukushima", "Niigata",
    "Nagano", "Tokyo", "Kanagawa", "Chiba", "Saitama","Ibaraki", "Tochigi", "Gunma", "Yamanashi",
    "Shizuoka", "Gifu", "Aichi", "Mie", "Kyoto", "Shiga", "Nara", "Osaka",
    "Wakayama", "Hyogo", "Toyama", "Fukui", "Ishikawa", "Okayama", "Shimane", "Yamaguchi",
    "Tottori", "Hiroshima", "Kagawa", "Tokushima", "Ehime", "Kochi", "Fukuoka", "Saga", "Nagasaki", "Kumamoto",
    "Oita", "Miyazaki", "Kagoshima", "Okinawa", "Ogasawara", ""
  }
};

