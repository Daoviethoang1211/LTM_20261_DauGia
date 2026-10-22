#ifndef STORAGE_H
#define STORAGE_H
#include <sqlite3.h>
int storage_open(sqlite3 **db, const char *path, long long *starts);
#endif
