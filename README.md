# esp32-jk1dvplog

手のひらサイズのアマチュア無線　コンテストロガーです。

Release にマニュアル、ログ変換プログラム、内蔵Bluetooth モデム用プログラム(Arduino)を置いています。

# License
下記の通りGPLv2です。
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



## binary のesp32 への書き込み
Release で配布されているバイナリイメージを書き込む方法は[esptool](https://github.com/espressif/esptool) をインストールした上で、bootloader.bin partition-table.bin jk1dvplog.bin をディレクトリに置き、

python esptool.py -p シリアルポート -b 460800 --before default_reset --after hard_reset --chip esp32  write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 jk1dvplog.bin

で書き込めるはずです。

