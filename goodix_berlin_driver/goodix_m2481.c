// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/string.h>

#include "goodix_ts_core.h"
#include "goodix_m2481.h"

#define GOODIX_M2481_GESTURE_CMD		0xa6
#define GOODIX_M2481_TAP_DISABLE_MASK	0x1fff
#define GOODIX_M2481_FOD_DISABLE_MASK	0x2000

enum goodix_m2481_charge_state {
	GOODIX_M2481_USB_ONLINE = 1,
	GOODIX_M2481_USB_OFFLINE = 2,
	GOODIX_M2481_WIRELESS_ONLINE = 10,
	GOODIX_M2481_WIRELESS_OFFLINE = 20,
};

bool goodix_m2481_is_device(const struct device_node *node)
{
	return of_device_is_compatible(node, GOODIX_M2481_COMPATIBLE);
}

static int goodix_m2481_charge_params(unsigned long state,
				      unsigned char *command,
				      unsigned char *data)
{
	switch (state) {
	case GOODIX_M2481_USB_ONLINE:
		*command = 0xaf;
		*data = 1;
		break;
	case GOODIX_M2481_USB_OFFLINE:
		*command = 0xc6;
		*data = 1;
		break;
	case GOODIX_M2481_WIRELESS_ONLINE:
		*command = 0xaf;
		*data = 0;
		break;
	case GOODIX_M2481_WIRELESS_OFFLINE:
		*command = 0xc6;
		*data = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int goodix_m2481_send_gesture(struct goodix_ts_core *cd,
			      unsigned int gesture_type)
{
	struct goodix_ts_cmd command = {
		.len = 6,
		.cmd = GOODIX_M2481_GESTURE_CMD,
	};
	u16 mask = GOODIX_M2481_TAP_DISABLE_MASK |
		   GOODIX_M2481_FOD_DISABLE_MASK;
	int ret;

	if (gesture_type & GESTURE_DOUBLE_TAP)
		mask &= ~GOODIX_M2481_TAP_DISABLE_MASK;
	if (gesture_type & GESTURE_FOD_PRESS)
		mask &= ~GOODIX_M2481_FOD_DISABLE_MASK;
	command.data[0] = mask & 0xff;
	command.data[1] = mask >> 8;
	goodix_append_checksum(&command.buf[2], command.len - 2,
			       CHECKSUM_MODE_U8_LE);

	mutex_lock(&cd->cmd_lock);
	ret = cd->hw_ops->write(cd, cd->ic_info.misc.cmd_addr,
				command.buf, command.len + 2);
	if (ret < 0)
		ts_err("failed send gesture cmd");
	else
		msleep(20);
	mutex_unlock(&cd->cmd_lock);

	return ret < 0 ? ret : 0;
}

static int goodix_m2481_send_charge_state(struct goodix_ts_core *cd,
					  unsigned long state)
{
	struct goodix_ts_cmd command = { 0 };
	int ret;

	ret = goodix_m2481_charge_params(state, &command.cmd,
					 &command.data[0]);
	if (ret)
		return ret;

	command.len = 5;
	ret = cd->hw_ops->send_cmd(cd, &command);
	if (ret)
		ts_err("failed to set M2481 charger state %lu: %d", state, ret);

	return ret;
}

static int goodix_m2481_supply_online(const char *name, bool *online)
{
	union power_supply_propval value;
	struct power_supply *supply;
	int ret;

	supply = power_supply_get_by_name(name);
	if (!supply)
		return -ENODEV;

	ret = power_supply_get_property(supply, POWER_SUPPLY_PROP_ONLINE,
					&value);
	power_supply_put(supply);
	if (ret)
		return ret;

	*online = !!value.intval;
	return 0;
}

static void goodix_m2481_charge_work(struct work_struct *work)
{
	struct goodix_m2481_state *state =
		container_of(work, struct goodix_m2481_state, psy_work);
	struct goodix_ts_core *cd = state->core;
	bool usb_online;
	bool wireless_online;
	int usb_ret;
	int wireless_ret;
	int ret;

	usb_ret = goodix_m2481_supply_online("usb", &usb_online);
	wireless_ret = goodix_m2481_supply_online("wireless",
						  &wireless_online);

	mutex_lock(&state->psy_lock);
	if (!usb_ret && state->usb_online != usb_online) {
		state->usb_online = usb_online;
		state->usb_dirty = true;
	}
	if (!wireless_ret && state->wireless_online != wireless_online) {
		state->wireless_online = wireless_online;
		state->wireless_dirty = true;
	}
	if (!atomic_read(&cd->suspended) && state->usb_dirty) {
		unsigned long charge_state = state->usb_online ?
			GOODIX_M2481_USB_ONLINE : GOODIX_M2481_USB_OFFLINE;

		ret = goodix_m2481_send_charge_state(cd, charge_state);
		state->usb_dirty = ret != 0;
	}
	if (!atomic_read(&cd->suspended) && state->wireless_dirty) {
		unsigned long charge_state = state->wireless_online ?
			GOODIX_M2481_WIRELESS_ONLINE :
			GOODIX_M2481_WIRELESS_OFFLINE;

		ret = goodix_m2481_send_charge_state(cd, charge_state);
		state->wireless_dirty = ret != 0;
	}
	mutex_unlock(&state->psy_lock);
}

static int goodix_m2481_charge_notifier(struct notifier_block *notifier,
					unsigned long event, void *data)
{
	struct goodix_m2481_state *state =
		container_of(notifier, struct goodix_m2481_state, psy_notifier);
	struct power_supply *supply = data;
	const char *name;

	if (event != PSY_EVENT_PROP_CHANGED || !supply || !supply->desc)
		return NOTIFY_DONE;

	name = supply->desc->name;
	if (strcmp(name, "usb") && strcmp(name, "wireless"))
		return NOTIFY_DONE;

	schedule_work(&state->psy_work);
	return NOTIFY_OK;
}

int goodix_m2481_charge_init(struct goodix_ts_core *cd)
{
	struct goodix_m2481_state *state = &cd->m2481;
	int ret;

	state->core = cd;
	INIT_WORK(&state->psy_work, goodix_m2481_charge_work);
	mutex_init(&state->psy_lock);
	state->usb_online = -1;
	state->wireless_online = -1;
	state->psy_notifier.notifier_call = goodix_m2481_charge_notifier;
	ret = power_supply_reg_notifier(&state->psy_notifier);
	if (ret)
		return ret;

	state->psy_registered = true;
	schedule_work(&state->psy_work);
	return 0;
}

void goodix_m2481_charge_exit(struct goodix_ts_core *cd)
{
	struct goodix_m2481_state *state = &cd->m2481;

	if (!state->psy_registered)
		return;

	power_supply_unreg_notifier(&state->psy_notifier);
	cancel_work_sync(&state->psy_work);
	state->psy_registered = false;
	state->core = NULL;
}

bool goodix_m2481_charge_lock(struct goodix_ts_core *cd)
{
	struct goodix_m2481_state *state = &cd->m2481;

	if (!state->psy_registered)
		return false;

	mutex_lock(&state->psy_lock);
	return true;
}

void goodix_m2481_charge_unlock(struct goodix_ts_core *cd)
{
	mutex_unlock(&cd->m2481.psy_lock);
}

static void
goodix_m2481_restore_charge_state_locked(struct goodix_ts_core *cd)
{
	struct goodix_m2481_state *state = &cd->m2481;
	int ret;

	if (state->usb_online >= 0) {
		unsigned long charge_state = state->usb_online ?
			GOODIX_M2481_USB_ONLINE : GOODIX_M2481_USB_OFFLINE;

		ret = goodix_m2481_send_charge_state(cd, charge_state);
		state->usb_dirty = ret != 0;
	}
	if (state->wireless_online >= 0) {
		unsigned long charge_state = state->wireless_online ?
			GOODIX_M2481_WIRELESS_ONLINE :
			GOODIX_M2481_WIRELESS_OFFLINE;

		ret = goodix_m2481_send_charge_state(cd, charge_state);
		state->wireless_dirty = ret != 0;
	}
}

void goodix_m2481_charge_resume(struct goodix_ts_core *cd, bool locked)
{
	struct goodix_m2481_state *state = &cd->m2481;

	if (!locked)
		return;

	goodix_m2481_restore_charge_state_locked(cd);
	mutex_unlock(&state->psy_lock);
	schedule_work(&state->psy_work);
}
