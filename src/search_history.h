#ifndef __SEARCH_HISTORY_H__
#define __SEARCH_HISTORY_H__

#define SEARCH_HISTORY_MAX 10

void SearchHistory_init(void);
int SearchHistory_count(void);
const char* SearchHistory_get(int index);
void SearchHistory_add(const char* query);
void SearchHistory_clear(void);

#endif
