#ifndef WEBSRV_H
#define WEBSRV_H

#include <stdbool.h>

bool websrv_start(const char *password);
void websrv_stop(void);
bool websrv_running(void);

#endif /* WEBSRV_H */
