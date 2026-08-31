/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GOODIX_M2481_H_
#define _GOODIX_M2481_H_

#include <linux/input-event-codes.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/workqueue.h>

struct goodix_ts_core;

#define GOODIX_M2481_COMPATIBLE "goodix,brl-d-m2481"
#define GOODIX_M2481_DEVICE_NAME "main_touch"
#define GOODIX_M2481_DOUBLE_TAP_KEY KEY_NEXT_FAVORITE
#define GOODIX_M2481_COORD_SCALE 10

struct goodix_m2481_state {
	struct goodix_ts_core *core;
	struct notifier_block psy_notifier;
	struct work_struct psy_work;
	struct mutex psy_lock; /* protects cached charger state */
	int usb_online;
	int wireless_online;
	bool usb_dirty;
	bool wireless_dirty;
	bool psy_registered;
};

bool goodix_m2481_is_device(const struct device_node *node);
int goodix_m2481_send_gesture(struct goodix_ts_core *cd,
			      unsigned int gesture_type);
int goodix_m2481_charge_init(struct goodix_ts_core *cd);
void goodix_m2481_charge_exit(struct goodix_ts_core *cd);
bool goodix_m2481_charge_lock(struct goodix_ts_core *cd);
void goodix_m2481_charge_unlock(struct goodix_ts_core *cd);
void goodix_m2481_charge_resume(struct goodix_ts_core *cd, bool locked);

#endif /* _GOODIX_M2481_H_ */
