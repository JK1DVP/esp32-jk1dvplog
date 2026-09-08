/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef FILE_CONSOLE_PROCESS_H
#define FILE_CONSOLE_PROCESS_H

#include <stdint.h>
class Stream;

void console_process();
bool console_sdput_begin(const char *name, uint32_t size, uint32_t crc32, Stream *out);
bool console_ymodem_begin(Stream *out);
bool console_ymodem_active();
bool console_ymodem_packet_in_progress();
void print_status_console();
int in_keys(char c, const uint8_t *keys, int nkeys);
void emulate_keyboard(char c);

#endif
