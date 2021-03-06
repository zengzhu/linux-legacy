/* vim: set noet ts=8 sts=8 sw=8 : */
/*
 * Copyright © 2010 Saleem Abdulrasool <compnerd@compnerd.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/i2c.h>
#include <linux/sbs.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/input.h>
#include <linux/power_supply.h>

/* logging helpers */
#define ERROR(fmt, ...)		printk(KERN_ERR     "SBS: " fmt, ## __VA_ARGS__)
#define WARNING(fmt, ...)	printk(KERN_WARNING "SBS: " fmt, ## __VA_ARGS__)
#define INFO(fmt, ...)		printk(KERN_INFO    "SBS: " fmt, ## __VA_ARGS__)
#define DEBUG(fmt, ...)		printk(KERN_DEBUG   "SBS: " fmt, ## __VA_ARGS__)
#define CONTINUE(fmt, ...)	printk(KERN_CONT fmt, ## __VA_ARGS__)

/* Smart Battery Messages */
#define SBS_MANUFACTURER_ACCESS				(0x00)
#define SBS_REMAINING_CAPACITY_ALARM			(0x01)
#define SBS_REMAINING_TIME_ALARM			(0x02)
#define SBS_BATTERY_MODE				(0x03)
#define SBS_AT_RATE					(0x04)
#define SBS_AT_RATE_TIME_TO_FULL			(0x05)
#define SBS_AT_RATE_TIME_TO_EMPTY			(0x06)
#define SBS_AT_RATE_OK					(0x07)
#define SBS_TEMPERATURE					(0x08)
#define SBS_VOLTAGE					(0x09)
#define SBS_CURRENT					(0x0a)
#define SBS_AVERAGE_CURRENT				(0x0b)
#define SBS_MAX_ERROR					(0x0c)
#define SBS_RELATIVE_STATE_OF_CHARGE			(0x0d)
#define SBS_ABSOLUTE_STATE_OF_CHARGE			(0x0e)
#define SBS_REMAINING_CAPACITY				(0x0f)
#define SBS_FULL_CHARGE_CAPACITY			(0x10)
#define SBS_RUN_TIME_TO_EMPTY				(0x11)
#define SBS_AVERAGE_TIME_TO_EMPTY			(0x12)
#define SBS_AVERAGE_TIME_TO_FULL			(0x13)
#define SBS_CHARGING_CURRENT				(0x14)
#define SBS_CHARGING_VOLTAGE				(0x15)
#define SBS_BATTERY_STATUS				(0x16)
#define SBS_ALARM_WARNING				(0x16)
#define SBS_BATTERY_CYCLE_COUNT				(0x17)
#define SBS_DESIGN_CAPACITY				(0x18)
#define SBS_DESIGN_VOLTAGE				(0x19)
#define SBS_SPECIFICATION_INFO				(0x1a)
#define SBS_MANUFACTURE_DATE				(0x1b)
#define SBS_SERIAL_NUMBER				(0x1c)
#define SBS_MANUFACTURER_NAME				(0x20)
#define SBS_DEVICE_NAME					(0x21)
#define SBS_DEVICE_CHEMISTRY				(0x22)
#define SBS_MANUFACTURER_DATA				(0x23)

/* Battery Mode Flags */
#define MODE_INTERNAL_CHARGE_CONTROLLER_CAPABILITY	(1 << 0)
#define MODE_BATTERY_ROLE_CAPABILITY			(1 << 1)
#define MODE_CAPACITY_RELEARN				(1 << 7)
#define MODE_INTERNAL_CHARGE_CONTROLLER_ENABLED		(1 << 8)
#define MODE_PRIMARY_BATTERY				(1 << 9)
#define MODE_ALARM					(1 << 13)
#define MODE_CHARGER					(1 << 14)
#define MODE_CAPACITY					(1 << 15)

/* Battery Status Flags */
#define STATUS_FULLY_DISCHARGED				(1 << 4)
#define STATUS_FULLY_CHARGED				(1 << 5)
#define STATUS_DISCHARGING				(1 << 6)
#define STATUS_INITIALIZED				(1 << 7)
#define STATUS_REMAINING_TIME_ALARM			(1 << 8)
#define STATUS_REMAINING_CAPACITY_ALARM			(1 << 10)
#define STATUS_TERMINATE_DISCHARGE_ALARM		(1 << 11)
#define STATUS_OVER_TEMP_ALARM				(1 << 12)
#define STATUS_TERMINATE_CHARGE_ALARM			(1 << 14)
#define STATUS_OVER_CHARGED_ALARM			(1 << 15)

#define SBS_STRING_REGISTER_LEN				(32)

/* module parameters */
static unsigned int cache_time = 2500;
module_param(cache_time, uint, 0644);
MODULE_PARM_DESC(cache_time, "cache time in milliseconds");

static unsigned int i2c_settle_time = 1500;
module_param(i2c_settle_time, uint, 0644);
MODULE_PARM_DESC(i2c_settle_time, "i2c settle time in milliseconds");

struct sbs_battery {
	struct i2c_client        *client;
	struct sbs_platform_data *platform;

	struct {
		u32   timestamp;

		/* dynamic information */
		u16   battery_mode;
		u16   temperature;
		s16   voltage;
		s16   _current;                 /* current is a macro */
		s16   average_current;
		u16   absolute_state_of_charge;
		u16   remaining_capacity;

		/* affected by battery_mode */
		u16   full_charge_capacity;
		u16   design_capacity;
		u16   battery_status;

		/* static information */
		u16   battery_cycle_count;
		u16   design_voltage;
		u16   specification_info;
		u16   _serial_number;           /* raw serial # */

		char *serial_number;            /* string form for PS driver */
		char *manufacturer_name;
		char *device_name;
		char *device_chemistry;

		struct __packed {
			unsigned info_valid : 1;
			unsigned            : 7;
		} flags;
	} cache;

	unsigned int vscale;
	unsigned int ipscale;

	struct power_supply battery;
	struct power_supply mains;

	struct mutex lock;
	struct delayed_work refresh;

	int present;
	int ac_present;
	int alarming; /* it is, isn't it? :) */

	struct work_struct insert_work;
	struct work_struct ac_work;
	struct work_struct alarm_work;
};

struct sbs_battery_register {
	u8     address;
	enum {
		SBS_REGISTER_INT,
		SBS_REGISTER_STRING,
	}      type;
	size_t offset;
};


static inline int read_battery_register(struct sbs_battery * const batt,
					const struct sbs_battery_register *reg)
{
	u8 * const cache = (u8 *) batt;
	int ret;

	switch (reg->type) {
	case SBS_REGISTER_INT:
		{
			u16 *data = (u16 *)(cache + reg->offset);

			ret = i2c_smbus_read_word_data(batt->client,
						       reg->address);
			if (ret < 0)
				return ret;

			*data = (u16) ret;
		}
		break;
	case SBS_REGISTER_STRING:
		{
			char **data = (char **)(cache + reg->offset);

			struct {
				u8 length;
				u8 data[SBS_STRING_REGISTER_LEN - 1];
			} buffer;

			BUILD_BUG_ON(sizeof(buffer) != SBS_STRING_REGISTER_LEN);
			BUILD_BUG_ON(sizeof(buffer) > I2C_SMBUS_BLOCK_MAX);

			ret = i2c_smbus_read_i2c_block_data(batt->client,
							    reg->address,
							    sizeof(buffer),
							    (u8 *) &buffer);
			if (ret < 0)
				return ret;

			WARN_ON(buffer.length > sizeof(buffer.data));
			buffer.length = min(buffer.length,
					    (u8) (sizeof(buffer.data) - 1));
			buffer.data[buffer.length] = '\0';

			if (*data)
				kfree(*data);

			*data = kstrndup(buffer.data, buffer.length,
					 GFP_KERNEL);
		}
		break;
	}

	return 0;
}

static inline bool battery_present(const struct sbs_battery * const batt)
{
	return batt->present;
}

static inline bool mains_present(const struct sbs_battery * const batt)
{
	return batt->ac_present;
}


/* Battery Information */
static const struct sbs_battery_register sbs_info_registers[] = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
	{ SBS_BATTERY_CYCLE_COUNT,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.battery_cycle_count), },
#endif
	{ SBS_DESIGN_VOLTAGE,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.design_voltage), },
	{ SBS_SPECIFICATION_INFO,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.specification_info), },
	{ SBS_SERIAL_NUMBER,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache._serial_number), },
	{ SBS_MANUFACTURER_NAME,
	  SBS_REGISTER_STRING,
	  offsetof(struct sbs_battery, cache.manufacturer_name), },
	{ SBS_DEVICE_NAME,
	  SBS_REGISTER_STRING,
	  offsetof(struct sbs_battery, cache.device_name), },
	{ SBS_DEVICE_CHEMISTRY,
	  SBS_REGISTER_STRING,
	  offsetof(struct sbs_battery, cache.device_chemistry), },
};

static inline unsigned int ipow(const int base, int exp)
{
	unsigned int value = base;

	if (unlikely(!exp))
		return 1;

	while (--exp)
		value *= base;

	return value;
}

static void sbs_get_battery_info(struct sbs_battery *batt)
{
	unsigned int i;
	int ret = 0;

	BUG_ON(!mutex_is_locked(&batt->lock));

	if (!battery_present(batt) || batt->cache.flags.info_valid)
		return;

	for (i = 0; i < ARRAY_SIZE(sbs_info_registers); i++)
		ret = ret || read_battery_register(batt, &sbs_info_registers[i]);

	batt->vscale = ipow(10, (batt->cache.specification_info >> 8) & 0xf);
	batt->ipscale = ipow(10, (batt->cache.specification_info >> 12) & 0xf);

	batt->cache.flags.info_valid = (ret == 0);
}


/* Battery State */
static enum power_supply_property sbs_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
	POWER_SUPPLY_PROP_CYCLE_COUNT,
#endif
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,

	/* Current */
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,

	/* Power */
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,

	POWER_SUPPLY_PROP_HEALTH,
};

static inline int __chem_to_tech(const char * const chem)
{
	if (!chem)
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;

	if (!strcasecmp(chem, "LSO2")) /* Lithium Sulfur Dioxide */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "LMnO")) /* Lithium Manganese Dioxide */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "LCFx")) /* Lithium */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "PbAc")) /* Lead Acid */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "LION")) /* Lithium Ion */
		return POWER_SUPPLY_TECHNOLOGY_LION;
	if (!strcasecmp(chem, "NiCd")) /* Nickel Cadmium */
		return POWER_SUPPLY_TECHNOLOGY_NiCd;
	if (!strcasecmp(chem, "NiMH")) /* Nickel Metal Hydride */
		return POWER_SUPPLY_TECHNOLOGY_NiMH;
	if (!strcasecmp(chem, "NiZn")) /* Nickel Zinc */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "RAM"))  /* Rechargable Alkaline-Manganese */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "ZnAr")) /* Zinc Air */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "LiP"))  /* Lithium Polymer */
		return POWER_SUPPLY_TECHNOLOGY_LIPO;
	if (!strcasecmp(chem, "H2FC")) /* Hydrogen Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "BHFC")) /* NaBH Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "RMFC")) /* Reformed Methanol Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "DMFC")) /* Direct Methanol Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "FAFC")) /* Formic Acid Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "BSFC")) /* Butane Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "PSFC")) /* Propane Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	if (!strcasecmp(chem, "SOFC")) /* Solid Oxide Fuel Cell */
		return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
#if defined(CONFIG_MACH_MX51_EFIKASB)
	if (!strcasecmp(chem, "LGC0")) /* Lithium Ion */
		return POWER_SUPPLY_TECHNOLOGY_LION;
#endif

	DEBUG("Unknown Device Chemistry: %s", chem);
	return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
}

static inline const int __mV_2_uV(const struct sbs_battery * const batt,
				  const int mv)
{
	return batt->vscale * mv * 1000;
}

static inline const int __mA_2_uA(const struct sbs_battery * const batt,
				  const int ma)
{
	return batt->ipscale * ma * 1000;
}

static inline const int __dK_2_dC(const int dk)
{
	return dk - 2730;
}

static inline const int __mW_2_uW(const struct sbs_battery * const batt,
				  const int mw)
{
	/* SBS reports mWh in 10 mWh units */
	return batt->vscale * batt->ipscale * mw * 1000 * 10;
}

static const struct sbs_battery_register sbs_state_registers[] = {
	{ SBS_BATTERY_MODE,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.battery_mode), },
	{ SBS_TEMPERATURE,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.temperature), },
	{ SBS_VOLTAGE,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.voltage), },
	{ SBS_CURRENT,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache._current), },
	{ SBS_AVERAGE_CURRENT,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.average_current), },
	{ SBS_ABSOLUTE_STATE_OF_CHARGE,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.absolute_state_of_charge), },
	{ SBS_REMAINING_CAPACITY,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.remaining_capacity), },
	{ SBS_FULL_CHARGE_CAPACITY,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.full_charge_capacity), },
	{ SBS_BATTERY_STATUS,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.battery_status), },
	{ SBS_DESIGN_CAPACITY,
	  SBS_REGISTER_INT,
	  offsetof(struct sbs_battery, cache.design_capacity), },
};

static void sbs_get_battery_state(struct sbs_battery *batt)
{
	unsigned int i;

	BUG_ON(!mutex_is_locked(&batt->lock));

	if (likely(batt->cache.timestamp))
		if (time_before(jiffies,
				batt->cache.timestamp + msecs_to_jiffies(cache_time)))
			return;

	if (!battery_present(batt))
		return;

	for (i = 0; i < ARRAY_SIZE(sbs_state_registers); i++)
		read_battery_register(batt, &sbs_state_registers[i]);

	batt->cache.timestamp = jiffies;

	if (batt->cache.serial_number)
		kfree(batt->cache.serial_number);

	batt->cache.serial_number = kasprintf(GFP_KERNEL,
					      "%u", batt->cache._serial_number);

}

static int sbs_get_battery_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct sbs_battery *batt =
		container_of(psy, struct sbs_battery, battery);
	int retval = 0;

	if (!battery_present(batt) && psp != POWER_SUPPLY_PROP_PRESENT)
		return -ENODEV;

	val->intval = 0;

	mutex_lock(&batt->lock);

	sbs_get_battery_info(batt);

	switch (psp) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = __chem_to_tech(batt->cache.device_chemistry);
		goto out;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = batt->cache.battery_cycle_count;
		goto out;
#endif
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:                   /* uV */
		val->intval = __mV_2_uV(batt, batt->cache.design_voltage);
		goto out;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = batt->cache.device_name;
		goto out;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = batt->cache.manufacturer_name;
		goto out;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = batt->cache.serial_number;
		goto out;
	default:
		break;
	}

	sbs_get_battery_state(batt);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (batt->cache._current < 0)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (batt->cache._current > 0)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battery_present(batt);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:                          /* uV */
		val->intval = __mV_2_uV(batt, batt->cache.voltage);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:                          /* µA */
		val->intval = __mA_2_uA(batt, abs(batt->cache._current));
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:                          /* µA */
		val->intval = __mA_2_uA(batt, abs(batt->cache.average_current));
		break;
	case POWER_SUPPLY_PROP_CAPACITY:                             /* % */
		val->intval = batt->cache.absolute_state_of_charge;
		break;
	case POWER_SUPPLY_PROP_TEMP:                                 /* .1 °C */
		val->intval = __dK_2_dC(batt->cache.temperature);
		break;

	/* Current */
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:                   /* µAh */
		if (~batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mA_2_uA(batt,
						batt->cache.design_capacity);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:                          /* µAh */
		if (~batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mA_2_uA(batt,
						batt->cache.full_charge_capacity);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:                           /* µAh */
		if (~batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mA_2_uA(batt,
						batt->cache.remaining_capacity);
		break;

	/* Power */
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:                   /* µWh */
		if (batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mW_2_uW(batt,
						batt->cache.design_capacity);
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:                          /* µWh */
		if (batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mW_2_uW(batt,
						batt->cache.full_charge_capacity);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:                           /* µWh */
		if (batt->cache.battery_mode & MODE_CAPACITY)
			val->intval = __mW_2_uW(batt,
						batt->cache.remaining_capacity);
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		if (batt->cache.battery_mode & MODE_CAPACITY_RELEARN)
			val->intval = POWER_SUPPLY_HEALTH_RELEARN_REQUEST;

		if (~batt->cache.battery_status & STATUS_INITIALIZED)
			val->intval = POWER_SUPPLY_HEALTH_UNINITIALIZED;

		if (val->intval == 0)
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;

	default:
		retval = -EINVAL;
		break;
	}

out:
	mutex_unlock(&batt->lock);
	return retval;
}

static struct power_supply sbs_battery = {
	.name           = "battery",
	.type           = POWER_SUPPLY_TYPE_BATTERY,
	.properties     = sbs_battery_properties,
	.num_properties = ARRAY_SIZE(sbs_battery_properties),
	.get_property   = sbs_get_battery_property,
};


/* Mains Information */
static enum power_supply_property sbs_mains_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int sbs_get_mains_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct sbs_battery *batt =
		container_of(psy, struct sbs_battery, mains);
	int retval = 0;

	val->intval = 0;

	mutex_lock(&batt->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = mains_present(batt);
		break;
	default:
		retval = -EINVAL;
		break;
	}
	mutex_unlock(&batt->lock);

	return retval;
}


static char *sbs_supplied_to[] = {
	"battery",
};

static struct power_supply sbs_mains = {
	.name           = "mains",
	.type           = POWER_SUPPLY_TYPE_MAINS,
	.supplied_to	= sbs_supplied_to,
	.num_supplicants = ARRAY_SIZE(sbs_supplied_to),
	.properties     = sbs_mains_properties,
	.num_properties = ARRAY_SIZE(sbs_mains_properties),
	.get_property   = sbs_get_mains_property,
};


static void sbs_refresh_battery_info(struct work_struct *work)
{
	struct sbs_battery * const batt =
		container_of(work, struct sbs_battery, refresh.work);

	mutex_lock(&batt->lock);
	sbs_get_battery_info(batt);
	mutex_unlock(&batt->lock);
}


static void sbs_battery_insert_handler(struct work_struct *work)
{
	struct sbs_battery * const batt =
		container_of(work, struct sbs_battery, insert_work);

	memset(&batt->cache, 0, sizeof(batt->cache));

	schedule_delayed_work(&batt->refresh,
		      msecs_to_jiffies(i2c_settle_time));

	power_supply_changed(&batt->battery);
}

static void sbs_ac_insert_handler(struct work_struct *work)
{
	struct sbs_battery * const batt =
		container_of(work, struct sbs_battery, ac_work);

	power_supply_changed(&batt->mains);
}

static void sbs_battery_alarm_handler(struct work_struct *work)
{
	struct sbs_battery * const batt =
		container_of(work, struct sbs_battery, alarm_work);

	/* do nothing for now.. */
	(void) batt;
}

static void sbs_event_handler(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	struct sbs_battery *batt = (struct sbs_battery *) handle->handler->private;

	if (type == EV_SW) {
		switch(code) {
			case SW_BATTERY_INSERT:
				batt->present = value;
				schedule_work(&batt->insert_work);
			break;
			case SW_BATTERY_LOW:
				batt->alarming = value;
				schedule_work(&batt->alarm_work);
			break;
			case SW_AC_INSERT:
				batt->ac_present = value;
				schedule_work(&batt->ac_work);
			break;
		}
	}
}

static int sbs_event_connect(struct input_handler *handler, struct input_dev *dev,
			      const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);

	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "sbs";

	DEBUG("Battery structure 0x%x\n", (unsigned int) handler->private);


	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

err_unregister_handle:
	input_unregister_handle(handle);

err_free_handle:
	kfree(handle);
	return error;
}


static void sbs_event_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id sbs_events_table[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_SW) },
	},
	{ },
};
MODULE_DEVICE_TABLE(input, sbs_events_table);

static struct input_handler sbs_input_handler = {
	.event = sbs_event_handler,
	.connect = sbs_event_connect,
	.disconnect = sbs_event_disconnect,
	.name = "sbs",
	.id_table = sbs_events_table,
};


static int __devinit sbs_probe(struct i2c_client *client,
			       const struct i2c_device_id *id)
{
	struct sbs_battery *batt;
	int ret = 0;

	batt = kzalloc(sizeof(*batt), GFP_KERNEL);
	if (!batt)
		return -ENOMEM;
	mutex_init(&batt->lock);

	mutex_lock(&batt->lock);

	batt->client = client;
	batt->platform = client->dev.platform_data;

	batt->mains = sbs_mains;
	batt->battery = sbs_battery;

	/* NOTE: active low, never use these ever again! */
	if (batt->platform->mains_status)
		batt->ac_present = !batt->platform->mains_status();

	if (batt->platform->battery_status)
		batt->present = !batt->platform->battery_status();

	if (batt->platform->alarm_status)
		batt->alarming = !batt->platform->alarm_status();

	DEBUG("Initial State: %s%s%s\n",
		batt->present ? "Present " : "",
		batt->ac_present ? "Powered " : "",
		batt->alarming ? "Low" : "");

	INIT_WORK(&batt->insert_work, sbs_battery_insert_handler);
	INIT_WORK(&batt->alarm_work, sbs_battery_alarm_handler);
	INIT_WORK(&batt->ac_work, sbs_ac_insert_handler);

	sbs_input_handler.private = (void *) batt;

	if ((ret = input_register_handler(&sbs_input_handler)) < 0) {
		DEBUG("Couldn't register input handler, battery/ac/alarm events will not be handled\n");
	}

	i2c_set_clientdata(client, batt);

	if ((ret = power_supply_register(&client->dev, &batt->battery)) < 0)
		goto error;

	if ((ret = power_supply_register(&client->dev, &batt->mains)) < 0) {
		power_supply_unregister(&batt->battery);
		goto error;
	}

	INIT_DELAYED_WORK(&batt->refresh, sbs_refresh_battery_info);

	mutex_unlock(&batt->lock);

	return 0;

error:
	mutex_unlock(&batt->lock);
	i2c_set_clientdata(client, NULL);
	kfree(batt);
	return ret;
}

static int __devexit sbs_remove(struct i2c_client *client)
{
	struct sbs_battery *batt;

	batt = i2c_get_clientdata(client);
	if (batt) {
		input_unregister_handler(&sbs_input_handler);

		mutex_lock(&batt->lock);

		power_supply_unregister(&batt->mains);
		power_supply_unregister(&batt->battery);

		if (batt->cache.manufacturer_name)
			kfree(batt->cache.manufacturer_name);

		if (batt->cache.device_name)
			kfree(batt->cache.device_name);

		if (batt->cache.device_chemistry)
			kfree(batt->cache.device_chemistry);

		if (batt->cache.serial_number)
			kfree(batt->cache.serial_number);

		mutex_unlock(&batt->lock);

		mutex_destroy(&batt->lock);

		i2c_set_clientdata(client, NULL);
		kfree(batt);
	}

	return 0;
}

static const struct i2c_device_id sbs_device_table[] = {
	{ "smart-battery", 0 },
	{ },
};

static struct i2c_driver sbs_driver = {
	.driver   = { .name = "sbs", },
	.probe    = sbs_probe,
	.remove   = sbs_remove,
	.id_table = sbs_device_table,
};

static int __init sbs_init(void)
{
	return i2c_add_driver(&sbs_driver);
}

static void __exit sbs_exit(void)
{
	i2c_del_driver(&sbs_driver);
}

module_init(sbs_init);
module_exit(sbs_exit);

MODULE_AUTHOR("Saleem Abdulrasool <compnerd@compnerd.org>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Smart Battery");
MODULE_DEVICE_TABLE(i2c, sbs_device_table);

