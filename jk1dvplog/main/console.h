#ifndef FILE_CONSOLE_PROCESS_H
#define FILE_CONSOLE_PROCESS_H

void console_process();
void print_status_console();
int in_keys(char c, const uint8_t *keys, int nkeys);
void emulate_keyboard(char c);

#endif
