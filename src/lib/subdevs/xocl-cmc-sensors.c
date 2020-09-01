// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include "xmgmt-main.h"
#include "xocl-cmc-impl.h"

#define	CMC_12V_PEX_REG			0x20
#define	CMC_3V3_PEX_REG			0x2C
#define	CMC_3V3_AUX_REG			0x38
#define	CMC_12V_AUX_REG			0x44
#define	CMC_DDR4_VPP_BTM_REG		0x50
#define	CMC_SYS_5V5_REG			0x5C
#define	CMC_VCC1V2_TOP_REG		0x68
#define	CMC_VCC1V8_REG			0x74
#define	CMC_VCC0V85_REG			0x80
#define	CMC_DDR4_VPP_TOP_REG		0x8C
#define	CMC_MGT0V9AVCC_REG		0x98
#define	CMC_12V_SW_REG			0xA4
#define	CMC_MGTAVTT_REG			0xB0
#define	CMC_VCC1V2_BTM_REG		0xBC
#define	CMC_12V_PEX_I_IN_REG		0xC8
#define	CMC_12V_AUX_I_IN_REG		0xD4
#define	CMC_VCCINT_V_REG		0xE0
#define	CMC_VCCINT_I_REG		0xEC
#define	CMC_FPGA_TEMP			0xF8
#define	CMC_FAN_TEMP_REG		0x104
#define	CMC_DIMM_TEMP0_REG		0x110
#define	CMC_DIMM_TEMP1_REG		0x11C
#define	CMC_DIMM_TEMP2_REG		0x128
#define	CMC_DIMM_TEMP3_REG		0x134
#define	CMC_FAN_SPEED_REG		0x164
#define	CMC_SE98_TEMP0_REG		0x140
#define	CMC_SE98_TEMP1_REG		0x14C
#define	CMC_SE98_TEMP2_REG		0x158
#define	CMC_CAGE_TEMP0_REG		0x170
#define	CMC_CAGE_TEMP1_REG		0x17C
#define	CMC_CAGE_TEMP2_REG		0x188
#define	CMC_CAGE_TEMP3_REG		0x194
#define	CMC_HBM_TEMP_REG		0x260
#define	CMC_VCC3V3_REG			0x26C
#define	CMC_3V3_PEX_I_REG		0x278
#define	CMC_VCC0V85_I_REG		0x284
#define	CMC_HBM_1V2_REG			0x290
#define	CMC_VPP2V5_REG			0x29C
#define	CMC_VCCINT_BRAM_REG		0x2A8
#define	CMC_HBM_TEMP2_REG		0x2B4
#define	CMC_12V_AUX1_REG                0x2C0
#define	CMC_VCCINT_TEMP_REG             0x2CC
#define	CMC_3V3_AUX_I_REG               0x2F0
#define	CMC_HOST_MSG_OFFSET_REG		0x300
#define	CMC_HOST_MSG_ERROR_REG		0x304
#define	CMC_HOST_MSG_HEADER_REG		0x308
#define	CMC_VCC1V2_I_REG                0x314
#define	CMC_V12_IN_I_REG                0x320
#define	CMC_V12_IN_AUX0_I_REG           0x32C
#define	CMC_V12_IN_AUX1_I_REG           0x338
#define	CMC_VCCAUX_REG                  0x344
#define	CMC_VCCAUX_PMC_REG              0x350
#define	CMC_VCCRAM_REG                  0x35C

struct xocl_cmc_sensor {
	struct platform_device *pdev;
	struct cmc_reg_map reg_io;
	struct device *hwmon_dev;
};

static inline u32
cmc_reg_rd(struct xocl_cmc_sensor *cmc_sensor, u32 off)
{
	return ioread32(cmc_sensor->reg_io.crm_addr + off);
}

enum sensor_val_kind {
	SENSOR_MAX,
	SENSOR_AVG,
	SENSOR_INS,
};

#define	READ_SENSOR(cmc_sensor, off, val_kind)	\
	(cmc_reg_rd(cmc_sensor, off + sizeof(u32) * val_kind))

/*
 * Defining sysfs nodes for HWMON.
 */

#define	HWMON_INDEX(sensor, val_kind)	(sensor | (val_kind << 24))
#define	HWMON_INDEX2SENSOR(index)	(index & 0xffffff)
#define	HWMON_INDEX2VAL_KIND(index)	((index & ~0xffffff) >> 24)

/* For voltage and current */
static ssize_t hwmon_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct xocl_cmc_sensor *cmc_sensor = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(da)->index;
	u32 val = READ_SENSOR(cmc_sensor, HWMON_INDEX2SENSOR(index),
		HWMON_INDEX2VAL_KIND(index));

	return sprintf(buf, "%d\n", val);
}
#define	HWMON_VOLT_CURR_GROUP(type, id) hwmon_##type##id##_attrgroup
#define	HWMON_VOLT_CURR_SYSFS_NODE(type, id, name, sensor)		\
	static ssize_t type##id##_label(struct device *dev,		\
		struct device_attribute *attr, char *buf)		\
	{								\
		return sprintf(buf, "%s\n", name);			\
	}								\
	static SENSOR_DEVICE_ATTR(type##id##_max, 0444, hwmon_show,	\
		NULL, HWMON_INDEX(sensor, SENSOR_MAX));			\
	static SENSOR_DEVICE_ATTR(type##id##_average, 0444, hwmon_show,	\
		NULL, HWMON_INDEX(sensor, SENSOR_AVG));			\
	static SENSOR_DEVICE_ATTR(type##id##_input, 0444, hwmon_show,	\
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static SENSOR_DEVICE_ATTR(type##id##_label, 0444, type##id##_label,    \
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static struct attribute *hwmon_##type##id##_attributes[] = {	\
		&sensor_dev_attr_##type##id##_max.dev_attr.attr,	\
		&sensor_dev_attr_##type##id##_average.dev_attr.attr,	\
		&sensor_dev_attr_##type##id##_input.dev_attr.attr,	\
		&sensor_dev_attr_##type##id##_label.dev_attr.attr,	\
		NULL							\
	};								\
	static const struct attribute_group HWMON_VOLT_CURR_GROUP(type, id) = {\
		.attrs = hwmon_##type##id##_attributes,			\
	}

/* For fan speed. */
#define	HWMON_FAN_SPEED_GROUP(id) hwmon_fan##id##_attrgroup
#define	HWMON_FAN_SPEED_SYSFS_NODE(id, name, sensor)			\
	static ssize_t fan##id##_label(struct device *dev,		\
		struct device_attribute *attr, char *buf)		\
	{								\
		return sprintf(buf, "%s\n", name);			\
	}								\
	static SENSOR_DEVICE_ATTR(fan##id##_input, 0444, hwmon_show,	\
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static SENSOR_DEVICE_ATTR(fan##id##_label, 0444, fan##id##_label,      \
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static struct attribute *hwmon_fan##id##_attributes[] = {	\
		&sensor_dev_attr_fan##id##_input.dev_attr.attr,		\
		&sensor_dev_attr_fan##id##_label.dev_attr.attr,		\
		NULL							\
	};								\
	static const struct attribute_group HWMON_FAN_SPEED_GROUP(id) = {      \
		.attrs = hwmon_fan##id##_attributes,			\
	}

/* For temperature */
static ssize_t hwmon_temp_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct xocl_cmc_sensor *cmc_sensor = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(da)->index;
	u32 val = READ_SENSOR(cmc_sensor, HWMON_INDEX2SENSOR(index),
		HWMON_INDEX2VAL_KIND(index));

	return sprintf(buf, "%d\n", val * 1000);
}
#define	HWMON_TEMPERATURE_GROUP(id) hwmon_temp##id##_attrgroup
#define	HWMON_TEMPERATURE_SYSFS_NODE(id, name, sensor)			\
	static ssize_t temp##id##_label(struct device *dev,		\
		struct device_attribute *attr, char *buf)		\
	{								\
		return sprintf(buf, "%s\n", name);			\
	}								\
	static SENSOR_DEVICE_ATTR(temp##id##_highest, 0444, hwmon_temp_show,   \
		NULL, HWMON_INDEX(sensor, SENSOR_MAX));			\
	static SENSOR_DEVICE_ATTR(temp##id##_input, 0444, hwmon_temp_show,     \
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static SENSOR_DEVICE_ATTR(temp##id##_label, 0444, temp##id##_label,    \
		NULL, HWMON_INDEX(sensor, SENSOR_INS));			\
	static struct attribute *hwmon_temp##id##_attributes[] = {	\
		&sensor_dev_attr_temp##id##_highest.dev_attr.attr,	\
		&sensor_dev_attr_temp##id##_input.dev_attr.attr,	\
		&sensor_dev_attr_temp##id##_label.dev_attr.attr,	\
		NULL							\
	};								\
	static const struct attribute_group HWMON_TEMPERATURE_GROUP(id) = {    \
		.attrs = hwmon_temp##id##_attributes,			\
	}

/* For power */
uint64_t cmc_get_power(struct xocl_cmc_sensor *cmc_sensor,
	enum sensor_val_kind kind)
{
	u32 v_pex, v_aux, v_3v3, c_pex, c_aux, c_3v3;
	u64 val = 0;

	v_pex = READ_SENSOR(cmc_sensor, CMC_12V_PEX_REG, kind);
	v_aux = READ_SENSOR(cmc_sensor, CMC_12V_AUX_REG, kind);
	v_3v3 = READ_SENSOR(cmc_sensor, CMC_3V3_PEX_REG, kind);
	c_pex = READ_SENSOR(cmc_sensor, CMC_12V_PEX_I_IN_REG, kind);
	c_aux = READ_SENSOR(cmc_sensor, CMC_12V_AUX_I_IN_REG, kind);
	c_3v3 = READ_SENSOR(cmc_sensor, CMC_3V3_PEX_I_REG, kind);

	val = (u64)v_pex * c_pex + (u64)v_aux * c_aux + (u64)v_3v3 * c_3v3;

	return val;
}
static ssize_t hwmon_power_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct xocl_cmc_sensor *cmc_sensor = dev_get_drvdata(dev);
	int index = to_sensor_dev_attr(da)->index;
	u64 val = cmc_get_power(cmc_sensor, HWMON_INDEX2VAL_KIND(index));

	return sprintf(buf, "%lld\n", val);
}
#define	HWMON_POWER_GROUP(id) hwmon_power##id##_attrgroup
#define	HWMON_POWER_SYSFS_NODE(id, name)				\
	static ssize_t power##id##_label(struct device *dev,		\
		struct device_attribute *attr, char *buf)		\
	{								\
		return sprintf(buf, "%s\n", name);			\
	}								\
	static SENSOR_DEVICE_ATTR(power##id##_average, 0444, hwmon_power_show,\
		NULL, HWMON_INDEX(0, SENSOR_MAX));			\
	static SENSOR_DEVICE_ATTR(power##id##_input, 0444, hwmon_power_show,  \
		NULL, HWMON_INDEX(0, SENSOR_INS));			\
	static SENSOR_DEVICE_ATTR(power##id##_label, 0444, power##id##_label, \
		NULL, HWMON_INDEX(0, SENSOR_INS));			\
	static struct attribute *hwmon_power##id##_attributes[] = {	\
		&sensor_dev_attr_power##id##_average.dev_attr.attr,	\
		&sensor_dev_attr_power##id##_input.dev_attr.attr,	\
		&sensor_dev_attr_power##id##_label.dev_attr.attr,	\
		NULL							\
	};								\
	static const struct attribute_group HWMON_POWER_GROUP(id) = {	\
		.attrs = hwmon_power##id##_attributes,			\
	}

HWMON_VOLT_CURR_SYSFS_NODE(in, 0, "12V PEX", CMC_12V_PEX_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 1, "12V AUX", CMC_12V_AUX_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 2, "3V3 PEX", CMC_3V3_PEX_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 3, "3V3 AUX", CMC_3V3_AUX_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 4, "5V5 SYS", CMC_SYS_5V5_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 5, "1V2 TOP", CMC_VCC1V2_TOP_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 6, "1V2 BTM", CMC_VCC1V2_BTM_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 7, "1V8 TOP", CMC_VCC1V8_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 8, "12V SW", CMC_12V_SW_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 9, "VCC INT", CMC_VCCINT_V_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 10, "0V9 MGT", CMC_MGT0V9AVCC_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 11, "0V85", CMC_VCC0V85_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 12, "MGT VTT", CMC_MGTAVTT_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 13, "DDR VPP BOTTOM", CMC_DDR4_VPP_BTM_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 14, "DDR VPP TOP", CMC_DDR4_VPP_TOP_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 15, "VCC 3V3", CMC_VCC3V3_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 16, "1V2 HBM", CMC_HBM_1V2_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 17, "2V5 VPP", CMC_VPP2V5_REG);
HWMON_VOLT_CURR_SYSFS_NODE(in, 18, "VCC INT BRAM", CMC_VCCINT_BRAM_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 1, "12V PEX Current", CMC_12V_PEX_I_IN_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 2, "12V AUX Current", CMC_12V_AUX_I_IN_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 3, "VCC INT Current", CMC_VCCINT_I_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 4, "3V3 PEX Current", CMC_3V3_PEX_I_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 5, "VCC 0V85 Current", CMC_VCC0V85_I_REG);
HWMON_VOLT_CURR_SYSFS_NODE(curr, 6, "3V3 AUX Current", CMC_3V3_AUX_I_REG);
HWMON_TEMPERATURE_SYSFS_NODE(1, "PCB TOP FRONT", CMC_SE98_TEMP0_REG);
HWMON_TEMPERATURE_SYSFS_NODE(2, "PCB TOP REAR", CMC_SE98_TEMP1_REG);
HWMON_TEMPERATURE_SYSFS_NODE(3, "PCB BTM FRONT", CMC_SE98_TEMP2_REG);
HWMON_TEMPERATURE_SYSFS_NODE(4, "FPGA TEMP", CMC_FPGA_TEMP);
HWMON_TEMPERATURE_SYSFS_NODE(5, "TCRIT TEMP", CMC_FAN_TEMP_REG);
HWMON_TEMPERATURE_SYSFS_NODE(6, "DIMM0 TEMP", CMC_DIMM_TEMP0_REG);
HWMON_TEMPERATURE_SYSFS_NODE(7, "DIMM1 TEMP", CMC_DIMM_TEMP1_REG);
HWMON_TEMPERATURE_SYSFS_NODE(8, "DIMM2 TEMP", CMC_DIMM_TEMP2_REG);
HWMON_TEMPERATURE_SYSFS_NODE(9, "DIMM3 TEMP", CMC_DIMM_TEMP3_REG);
HWMON_TEMPERATURE_SYSFS_NODE(10, "HBM TEMP", CMC_HBM_TEMP_REG);
HWMON_TEMPERATURE_SYSFS_NODE(11, "QSPF 0", CMC_CAGE_TEMP0_REG);
HWMON_TEMPERATURE_SYSFS_NODE(12, "QSPF 1", CMC_CAGE_TEMP1_REG);
HWMON_TEMPERATURE_SYSFS_NODE(13, "QSPF 2", CMC_CAGE_TEMP2_REG);
HWMON_TEMPERATURE_SYSFS_NODE(14, "QSPF 3", CMC_CAGE_TEMP3_REG);
HWMON_FAN_SPEED_SYSFS_NODE(1, "FAN SPEED", CMC_FAN_SPEED_REG);
HWMON_POWER_SYSFS_NODE(1, "POWER");

static const struct attribute_group *hwmon_cmc_attrgroups[] = {
	&HWMON_VOLT_CURR_GROUP(in, 0),
	&HWMON_VOLT_CURR_GROUP(in, 1),
	&HWMON_VOLT_CURR_GROUP(in, 2),
	&HWMON_VOLT_CURR_GROUP(in, 3),
	&HWMON_VOLT_CURR_GROUP(in, 4),
	&HWMON_VOLT_CURR_GROUP(in, 5),
	&HWMON_VOLT_CURR_GROUP(in, 6),
	&HWMON_VOLT_CURR_GROUP(in, 7),
	&HWMON_VOLT_CURR_GROUP(in, 8),
	&HWMON_VOLT_CURR_GROUP(in, 9),
	&HWMON_VOLT_CURR_GROUP(in, 10),
	&HWMON_VOLT_CURR_GROUP(in, 11),
	&HWMON_VOLT_CURR_GROUP(in, 12),
	&HWMON_VOLT_CURR_GROUP(in, 13),
	&HWMON_VOLT_CURR_GROUP(in, 14),
	&HWMON_VOLT_CURR_GROUP(in, 15),
	&HWMON_VOLT_CURR_GROUP(in, 16),
	&HWMON_VOLT_CURR_GROUP(in, 17),
	&HWMON_VOLT_CURR_GROUP(in, 18),
	&HWMON_VOLT_CURR_GROUP(curr, 1),
	&HWMON_VOLT_CURR_GROUP(curr, 2),
	&HWMON_VOLT_CURR_GROUP(curr, 3),
	&HWMON_VOLT_CURR_GROUP(curr, 4),
	&HWMON_VOLT_CURR_GROUP(curr, 5),
	&HWMON_VOLT_CURR_GROUP(curr, 6),
	&HWMON_TEMPERATURE_GROUP(1),
	&HWMON_TEMPERATURE_GROUP(2),
	&HWMON_TEMPERATURE_GROUP(3),
	&HWMON_TEMPERATURE_GROUP(4),
	&HWMON_TEMPERATURE_GROUP(5),
	&HWMON_TEMPERATURE_GROUP(6),
	&HWMON_TEMPERATURE_GROUP(7),
	&HWMON_TEMPERATURE_GROUP(8),
	&HWMON_TEMPERATURE_GROUP(9),
	&HWMON_TEMPERATURE_GROUP(10),
	&HWMON_TEMPERATURE_GROUP(11),
	&HWMON_TEMPERATURE_GROUP(12),
	&HWMON_TEMPERATURE_GROUP(13),
	&HWMON_TEMPERATURE_GROUP(14),
	&HWMON_FAN_SPEED_GROUP(1),
	&HWMON_POWER_GROUP(1),
	NULL
};

void cmc_sensor_remove(struct platform_device *pdev)
{
	struct xocl_cmc_sensor *cmc_sensor =
		(struct xocl_cmc_sensor *)cmc_pdev2sensor(pdev);

	if (cmc_sensor && cmc_sensor->hwmon_dev)
		xocl_subdev_unregister_hwmon(pdev, cmc_sensor->hwmon_dev);
}

static char *cmc_get_vbnv(struct xocl_cmc_sensor *cmc_sensor)
{
	int ret;
	char *vbnv;
	struct platform_device *mgmt_leaf =
		xocl_subdev_get_leaf_by_id(cmc_sensor->pdev,
		XOCL_SUBDEV_MGMT_MAIN, PLATFORM_DEVID_NONE);

	if (mgmt_leaf == NULL)
		return NULL;

	ret = xocl_subdev_ioctl(mgmt_leaf, XOCL_MGMT_MAIN_GET_VBNV, &vbnv);
	(void) xocl_subdev_put_leaf(cmc_sensor->pdev, mgmt_leaf);
	if (ret)
		return NULL;
	return vbnv;
}

int cmc_sensor_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl)
{
	struct xocl_cmc_sensor *cmc_sensor;
	char *vbnv;

	cmc_sensor = devm_kzalloc(DEV(pdev), sizeof(*cmc_sensor), GFP_KERNEL);
	if (!cmc_sensor)
		return -ENOMEM;

	cmc_sensor->pdev = pdev;
	/* Obtain register maps we need to read sensor values. */
	cmc_sensor->reg_io = regmaps[IO_REG];

	vbnv = cmc_get_vbnv(cmc_sensor);
	/*
	 * Make a parent call to ask root to register. If we register using
	 * platform device, we'll be treated as ISA device, not PCI device.
	 */
	cmc_sensor->hwmon_dev = xocl_subdev_register_hwmon(pdev,
		vbnv ? vbnv : "golden-image", cmc_sensor, hwmon_cmc_attrgroups);
	if (cmc_sensor->hwmon_dev == NULL)
		xocl_err(pdev, "failed to create HWMON device");

	kfree(vbnv);
	*hdl = cmc_sensor;
	return 0;
}
