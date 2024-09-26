#ifndef FILE_CTY_H
#define FILE_CTY_H
// cty.h
struct entity_info {
 const char *entity; 
 const char *entity_desc;
 const char *cqzone;
 const char *ituzone;
 const char *continent;
 const char *lat;
 const char *lon;
 const char *tz;
};
struct prefix_list {
    const char *prefix; 
    const int entity_idx; 
    const int ovrride_info_idx; 
};
extern const struct entity_info entity_infos[347];
extern const char *cty_ovrride_info[86];
extern int prefix_fwd_match_list_idx[36][2];
extern const struct prefix_list prefix_fwd_match_list[7542] ;
extern int prefix_exact_match_list_idx[36][2];
extern const struct prefix_list prefix_exact_match_list[21118];
#endif
