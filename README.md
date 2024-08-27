# esp32-jk1dvplog

手のひらサイズのアマチュア無線　コンテストロガーです。

Release にマニュアル、ログ変換プログラム、内蔵Bluetooth モデム用プログラム(Arduino)を置いています。

## binary のesp32 への書き込み
Release で配布されているバイナリイメージを書き込む方法は[esptool](https://github.com/espressif/esptool) をインストールした上で、bootloader.bin partition-table.bin jk1dvplog.bin をディレクトリに置き、

python esptool.py -p シリアルポート -b 460800 --before default_reset --after hard_reset --chip esp32  write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 jk1dvplog.bin

で書き込めるはずです。

