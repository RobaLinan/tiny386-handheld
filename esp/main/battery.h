#ifndef BATTERY_H
#define BATTERY_H

/* Percent from the 1S Li-ion curve, -1 while no valid reading is available. */
int battery_get_percent(void);
int battery_get_mv(void);

void battery_init(void);

#endif /* BATTERY_H */
