#ifndef FILE_UI_H
#define FILE_UI_H

void select_radio(int idx) ;
void change_focused_radio(int new_focus);

int ui_cursor_next_entry_partial_check (struct radio *radio);
int ui_pick_partial_check(struct radio *radio);
int ui_toggle_partial_check_flag();
int ui_perform_exch_partial_check(struct radio *radio);
int ui_perform_partial_check(struct radio *radio);
int ui_response_call_and_move_to_exch(struct radio *radio);


void on_key_down(MODIFIERKEYS modkey, uint8_t key, uint8_t c);
void function_keys(uint8_t key, MODIFIERKEYS modkey, uint8_t c) ;
int ui_perform_partial_check(struct radio *radio);
int ui_perform_exch_partial_check(struct radio *radio);

void process_enter(int option) ;
int check_edit_mode() ; // CW key input and Remarks  return 1 (insert) else return 0 (overwrite edit)
void logw_handler(char key, char c);
void switch_logw_entry(int option) ;
void sat_name_entered() ;
void print_help(int option) ;



#endif
