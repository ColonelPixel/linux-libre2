// SPDX-License-Identifier: GPL-2.0+
/*
 * Azoteq IQS550/572/525 Trackpad/Touchscreen Controller
 *
 * Copyright (C) 2018
 * Author: Jeff LaBundy <jeff@labundy.com>
 *
 * These devices require firmware exported from a PC-based configuration tool
 * made available by the vendor. Firmware files may be pushed to the device's
 * nonvolatile memory by writing the filename to the 'fw_file' sysfs control.
 *
 * Link to PC-based configuration tool and data sheet: http://www.azoteq.com/
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#define IQS5XX_FW_FILE_LEN	64
#define IQS5XX_NUM_RETRIES	10
#define IQS5XX_NUM_POINTS	256
#define IQS5XX_NUM_CONTACTS	5
#define IQS5XX_WR_BYTES_MAX	2

#define IQS5XX_PROD_NUM_IQS550	40
#define IQS5XX_PROD_NUM_IQS572	58
#define IQS5XX_PROD_NUM_IQS525	52
#define IQS5XX_PROJ_NUM_A000	0
#define IQS5XX_PROJ_NUM_B000	15
#define IQS5XX_MAJOR_VER_MIN	2

#define IQS5XX_RESUME		0x00
#define IQS5XX_SUSPEND		0x01

#define IQS5XX_SW_INPUT_EVENT	0x10
#define IQS5XX_SETUP_COMPLETE	0x40
#define IQS5XX_EVENT_MODE	0x01
#define IQS5XX_TP_EVENT		0x04

#define IQS5XX_FLIP_X		0x01
#define IQS5XX_FLIP_Y		0x02
#define IQS5XX_SWITCH_XY_AXIS	0x04

#define IQS5XX_PROD_NUM		0x0000
#define IQS5XX_ABS_X		0x0016
#define IQS5XX_ABS_Y		0x0018
#define IQS5XX_SYS_CTRL0	0x0431
#define IQS5XX_SYS_CTRL1	0x0432
#define IQS5XX_SYS_CFG0		0x058E
#define IQS5XX_SYS_CFG1		0x058F
#define IQS5XX_TOTAL_RX		0x063D
#define IQS5XX_TOTAL_TX		0x063E
#define IQS5XX_XY_CFG0		0x0669
#define IQS5XX_X_RES		0x066E
#define IQS5XX_Y_RES		0x0670
#define IQS5XX_CHKSM		0x83C0
#define IQS5XX_APP		0x8400
#define IQS5XX_CSTM		0xBE00
#define IQS5XX_PMAP_END		0xBFFF
#define IQS5XX_END_COMM		0xEEEE

#define IQS5XX_CHKSM_LEN	(IQS5XX_APP - IQS5XX_CHKSM)
#define IQS5XX_APP_LEN		(IQS5XX_CSTM - IQS5XX_APP)
#define IQS5XX_CSTM_LEN		(IQS5XX_PMAP_END + 1 - IQS5XX_CSTM)
#define IQS5XX_PMAP_LEN		(IQS5XX_PMAP_END + 1 - IQS5XX_CHKSM)

#define IQS5XX_REC_HDR_LEN	4
#define IQS5XX_REC_LEN_MAX	255
#define IQS5XX_REC_TYPE_DATA	0x00
#define IQS5XX_REC_TYPE_EOF	0x01

#define IQS5XX_BL_ADDR_MASK	0x40
#define IQS5XX_BL_CMD_VER	0x00
#define IQS5XX_BL_CMD_READ	0x01
#define IQS5XX_BL_CMD_EXEC	0x02
#define IQS5XX_BL_CMD_CRC	0x03
#define IQS5XX_BL_BLK_LEN_MAX	64
#define IQS5XX_BL_ID		0x0200
#define IQS5XX_BL_STATUS_RESET	0x00
#define IQS5XX_BL_STATUS_AVAIL	0xA5
#define IQS5XX_BL_STATUS_NONE	0xEE
#define IQS5XX_BL_CRC_PASS	0x00
#define IQS5XX_BL_CRC_FAIL	0x01
#define IQS5XX_BL_ATTEMPTS	3

struct iqs5xx_private {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct mutex lock;
	u8 bl_status;
};

struct iqs5xx_dev_id_info {
	__be16 prod_num;
	__be16 proj_num;
	u8 major_ver;
	u8 minor_ver;
	u8 bl_status;
} __packed;

struct iqs5xx_ihex_rec {
	char start;
	char len[2];
	char addr[4];
	char type[2];
	char data[2];
} __packed;

struct iqs5xx_touch_data {
	__be16 abs_x;
	__be16 abs_y;
	__be16 strength;
	u8 area;
} __packed;

static int iqs5xx_read_burst(struct i2c_client *client,
			     u16 reg, void *val, u16 len)
{
	__be16 reg_buf = cpu_to_be16(reg);
	int ret, i;
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(reg_buf),
			.buf = (u8 *)&reg_buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = (u8 *)val,
		},
	};

	/*
	 * The first addressing attempt outside of a communication window fails
	 * and must be retried, after which the device clock stretches until it
	 * is available.
	 */
	for (i = 0; i < IQS5XX_NUM_RETRIES; i++) {
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		if (ret == ARRAY_SIZE(msg))
			return 0;

		usleep_range(200, 300);
	}

	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "Failed to read from address 0x%04X: %d\n",
		reg, ret);

	return ret;
}

static int iqs5xx_read_word(struct i2c_client *client, u16 reg, u16 *val)
{
	__be16 val_buf;
	int error;

	error = iqs5xx_read_burst(client, reg, &val_buf, sizeof(val_buf));
	if (error)
		return error;

	*val = be16_to_cpu(val_buf);

	return 0;
}

static int iqs5xx_read_byte(struct i2c_client *client, u16 reg, u8 *val)
{
	return iqs5xx_read_burst(client, reg, val, sizeof(*val));
}

static int iqs5xx_write_burst(struct i2c_client *client,
			      u16 reg, const void *val, u16 len)
{
	int ret, i;
	u16 mlen = sizeof(reg) + len;
	u8 mbuf[sizeof(reg) + IQS5XX_WR_BYTES_MAX];

	if (len > IQS5XX_WR_BYTES_MAX)
		return -EINVAL;

	put_unaligned_be16(reg, mbuf);
	memcpy(mbuf + sizeof(reg), val, len);

	/*
	 * The first addressing attempt outside of a communication window fails
	 * and must be retried, after which the device clock stretches until it
	 * is available.
	 */
	for (i = 0; i < IQS5XX_NUM_RETRIES; i++) {
		ret = i2c_master_send(client, mbuf, mlen);
		if (ret == mlen)
			return 0;

		usleep_range(200, 300);
	}

	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "Failed to write to address 0x%04X: %d\n",
		reg, ret);

	return ret;
}

static int iqs5xx_write_word(struct i2c_client *client, u16 reg, u16 val)
{
	__be16 val_buf = cpu_to_be16(val);

	return iqs5xx_write_burst(client, reg, &val_buf, sizeof(val_buf));
}

static int iqs5xx_write_byte(struct i2c_client *client, u16 reg, u8 val)
{
	return iqs5xx_write_burst(client, reg, &val, sizeof(val));
}

static void iqs5xx_reset(struct i2c_client *client)
{
	struct iqs5xx_private *iqs5xx = i2c_get_clientdata(client);

	gpiod_set_value_cansleep(iqs5xx->reset_gpio, 1);
	usleep_range(200, 300);

	gpiod_set_value_cansleep(iqs5xx->reset_gpio, 0);
}

static int iqs5xx_bl_cmd(struct i2c_client *client, u8 bl_cmd, u16 bl_addr)
{
	struct i2c_msg msg;
	int ret;
	u8 mbuf[sizeof(bl_cmd) + sizeof(bl_addr)];

	msg.addr = client->addr ^ IQS5XX_BL_ADDR_MASK;
	msg.flags = 0;
	msg.len = sizeof(bl_cmd);
	msg.buf = mbuf;

	*mbuf = bl_cmd;

	switch (bl_cmd) {
	case IQS5XX_BL_CMD_VER:
	case IQS5XX_BL_CMD_CRC:
	case IQS5XX_BL_CMD_EXEC:
		break;
	case IQS5XX_BL_CMD_READ:
		msg.len += sizeof(bl_addr);
		put_unaligned_be16(bl_addr, mbuf + sizeof(bl_cmd));
		break;
	default:
		return -EINVAL;
	}

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1)
		goto msg_fail;

	switch (bl_cmd) {
	case IQS5XX_BL_CMD_VER:
		msg.len = sizeof(u16);
		break;
	case IQS5XX_BL_CMD_CRC:
		msg.len = sizeof(u8);
		/*
		 * This delay saves the bus controller the trouble of having to
		 * tolerate a relatively long clock-stretching period while the
		 * CRC is calculated.
		 */
		msleep(50);
		break;
	case IQS5XX_BL_CMD_EXEC:
		usleep_range(10000, 10100);
		fallthrough;
	default:
		return 0;
	}

	msg.flags = I2C_M_RD;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1)
		goto msg_fail;

	if (bl_cmd == IQS5XX_BL_CMD_VER &&
	    get_unaligned_be16(mbuf) != IQS5XX_BL_ID) {
		dev_err(&client->dev, "Unrecognized bootloader ID: 0x%04X\n",
			get_unaligned_be16(mbuf));
		return -EINVAL;
	}

	if (bl_cmd == IQS5XX_BL_CMD_CRC && *mbuf != IQS5XX_BL_CRC_PASS) {
		dev_err(&client->dev, "Bootloader CRC failed\n");
		return -EIO;
	}

	return 0;

msg_fail:
	if (ret >= 0)
		ret = -EIO;

	if (bl_cmd != IQS5XX_BL_CMD_VER)
		dev_err(&client->dev,
			"Unsuccessful bootloader command 0x%02X: %d\n",
			bl_cmd, ret);

	return ret;
}

static int iqs5xx_bl_open(struct i2c_client *client)
{
	int error, i, j;

	/*
	 * The device opens a bootloader polling window for 2 ms following the
	 * release of reset. If the host cannot establish communication during
	 * this time frame, it must cycle reset again.
	 */
	for (i = 0; i < IQS5XX_BL_ATTEMPTS; i++) {
		iqs5xx_reset(client);

		for (j = 0; j < IQS5XX_NUM_RETRIES; j++) {
			error = iqs5xx_bl_cmd(client, IQS5XX_BL_CMD_VER, 0);
			if (!error || error == -EINVAL)
				return error;
		}
	}

	dev_err(&client->dev, "Failed to open bootloader: %d\n", error);

	return error;
}

static int iqs5xx_bl_write(struct i2c_client *client,
			   u16 bl_addr, u8 *pmap_data, u16 pmap_len)
{
	struct i2c_msg msg;
	int ret, i;
	u8 mbuf[sizeof(bl_addr) + IQS5XX_BL_BLK_LEN_MAX];

	if (pmap_len % IQS5XX_BL_BLK_LEN_MAX)
		return -EINVAL;

	msg.addr = client->addr ^ IQS5XX_BL_ADDR_MASK;
	msg.flags = 0;
	msg.len = sizeof(mbuf);
	msg.buf = mbuf;

	for (i = 0; i < pmap_len; i += IQS5XX_BL_BLK_LEN_MAX) {
		put_unaligned_be16(bl_addr + i, mbuf);
		memcpy(mbuf + sizeof(bl_addr), pmap_data + i,
		       sizeof(mbuf) - sizeof(bl_addr));

		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret != 1)
			goto msg_fail;

		usleep_range(10000, 10100);
	}

	return 0;

msg_fail:
	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "Failed to write block at address 0x%04X: %d\n",
		bl_addr + i, ret);

	return ret;
}

static int iqs5xx_bl_verify(struct i2c_client *client,
			    u16 bl_addr, u8 *pmap_data, u16 pmap_len)
{
	struct i2c_msg msg;
	int ret, i;
	u8 bl_data[IQS5XX_BL_BLK_LEN_MAX];

	if (pmap_len % IQS5XX_BL_BLK_LEN_MAX)
		return -EINVAL;

	msg.addr = client->addr ^ IQS5XX_BL_ADDR_MASK;
	msg.flags = I2C_M_RD;
	msg.len = sizeof(bl_data);
	msg.buf = bl_data;

	for (i = 0; i < pmap_len; i += IQS5XX_BL_BLK_LEN_MAX) {
		ret = iqs5xx_bl_cmd(client, IQS5XX_BL_CMD_READ, bl_addr + i);
		if (ret)
			return ret;

		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret != 1)
			goto msg_fail;

		if (memcmp(bl_data, pmap_data + i, sizeof(bl_data))) {
			dev_err(&client->dev,
				"Failed to verify block at address 0x%04X\n",
				bl_addr + i);
			return -EIO;
		}
	}

	return 0;

msg_fail:
	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "Failed to read block at address 0x%04X: %d\n",
		bl_addr + i, ret);

	return ret;
}

static int iqs5xx_set_state(struct i2c_client *client, u8 state)
{
	struct iqs5xx_private *iqs5xx = i2c_get_clientdata(client);
	int error1, error2;

	if (iqs5xx->bl_status == IQS5XX_BL_STATUS_RESET)
		return 0;

	mutex_lock(&iqs5xx->lock);

	/*
	 * Addressing the device outside of a communication window prompts it
	 * to assert the RDY output, so disable the interrupt line to prevent
	 * the handler from servicing a false interrupt.
	 */
	disable_irq(client->irq);

	error1 = iqs5xx_write_byte(client, IQS5XX_SYS_CTRL1, state);
	error2 = iqs5xx_write_byte(client, IQS5XX_END_COMM, 0);

	usleep_range(50, 100);
	enable_irq(client->irq);

	mutex_unlock(&iqs5xx->lock);

	if (error1)
		return error1;

	return error2;
}

static int iqs5xx_open(struct input_dev *input)
{
	struct iqs5xx_private *iqs5xx = input_get_drvdata(input);

	return iqs5xx_set_state(iqs5xx->client, IQS5XX_RESUME);
}

static void iqs5xx_close(struct input_dev *input)
{
	struct iqs5xx_private *iqs5xx = input_get_drvdata(input);

	iqs5xx_set_state(iqs5xx->client, IQS5XX_SUSPEND);
}

static int iqs5xx_axis_init(struct i2c_client *client)
{
	struct iqs5xx_private *iqs5xx = i2c_get_clientdata(client);
	struct touchscreen_properties prop;
	struct input_dev *input;
	int error;
	u16 max_x, max_x_hw;
	u16 max_y, max_y_hw;
	u8 val;

	if (!iqs5xx->input) {
		input = devm_input_allocate_device(&client->dev);
		if (!input)
			return -ENOMEM;

		input->name = client->name;
		input->id.bustype = BUS_I2C;
		input->open = iqs5xx_open;
		input->close = iqs5xx_close;

		input_set_capability(input, EV_ABS, ABS_MT_POSITION_X);
		input_set_capability(input, EV_ABS, ABS_MT_POSITION_Y);
		input_set_capability(input, EV_ABS, ABS_MT_PRESSURE);

		input_set_drvdata(input, iqs5xx);
		iqs5xx->input = input;
	}

	touchscreen_parse_properties(iqs5xx->input, true, &prop);

	error = iqs5xx_read_byte(client, IQS5XX_TOTAL_RX, &val);
	if (error)
		return error;
	max_x_hw = (val - 1) * IQS5XX_NUM_POINTS;

	error = iqs5xx_read_byte(client, IQS5XX_TOTAL_TX, &val);
	if (error)
		return error;
	max_y_hw = (val - 1) * IQS5XX_NUM_POINTS;

	error = iqs5xx_read_byte(client, IQS5XX_XY_CFG0, &val);
	if (error)
		return error;

	if (val & IQS5XX_SWITCH_XY_AXIS)
		swap(max_x_hw, max_y_hw);

	if (prop.swap_x_y)
		val ^= IQS5XX_SWITCH_XY_AXIS;

	if (prop.invert_x)
		val ^= prop.swap_x_y ? IQS5XX_FLIP_Y : IQS5XX_FLIP_X;

	if (prop.invert_y)
		val ^= prop.swap_x_y ? IQS5XX_FLIP_X : IQS5XX_FLIP_Y;

	error = iqs5xx_write_byte(client, IQS5XX_XY_CFG0, val);
	if (error)
		return error;

	if (prop.max_x > max_x_hw) {
		dev_err(&client->dev, "Invalid maximum x-coordinate: %u > %u\n",
			prop.max_x, max_x_hw);
		return -EINVAL;
	} else if (prop.max_x == 0) {
		error = iqs5xx_read_word(client, IQS5XX_X_RES, &max_x);
		if (error)
			return error;

		input_abs_set_max(iqs5xx->input,
				  prop.swap_x_y ? ABS_MT_POSITION_Y :
						  ABS_MT_POSITION_X,
				  max_x);
	} else {
		max_x = (u16)prop.max_x;
	}

	if (prop.max_y > max_y_hw) {
		dev_err(&client->dev, "Invalid maximum y-coordinate: %u > %u\n",
			prop.max_y, max_y_hw);
		return -EINVAL;
	} else if (prop.max_y == 0) {
		error = iqs5xx_read_word(client, IQS5XX_Y_RES, &max_y);
		if (error)
			return error;

		input_abs_set_max(iqs5xx->input,
				  prop.swap_x_y ? ABS_MT_POSITION_X :
						  ABS_MT_POSITION_Y,
				  max_y);
	} else {
		max_y = (u16)prop.max_y;
	}

	/*
	 * Write horizontal and vertical resolution to the device in case its
	 * original defaults were overridden or swapped as per the properties
	 * specified in the device tree.
	 */
	error = iqs5xx_write_word(client,
				  prop.swap_x_y ? IQS5XX_Y_RES : IQS5XX_X_RES,
				  max_x);
	if (error)
		return error;

	error = iqs5xx_write_word(client,
				  prop.swap_x_y ? IQS5XX_X_RES : IQS5XX_Y_RES,
				  max_y);
	if (error)
		return error;

	error = input_mt_init_slots(iqs5xx->input, IQS5XX_NUM_CONTACTS,
				    INPUT_MT_DIRECT);
	if (error)
		dev_err(&client->dev, "Failed to initialize slots: %d\n",
			error);

	return error;
}

static int iqs5xx_dev_init(struct i2c_client *client)
{
	struct iqs5xx_private *iqs5xx = i2c_get_clientdata(client);
	struct iqs5xx_dev_id_info *dev_id_info;
	int error;
	u8 val;
	u8 buf[sizeof(*dev_id_info) + 1];

	error = iqs5xx_read_burst(client, IQS5XX_PROD_NUM,
				  &buf[1], sizeof(*dev_id_info));
	if (error)
		return iqs5xx_bl_open(client);

	/*
	 * A000 and B000 devices use 8-bit and 16-bit addressing, respectively.
	 * Querying an A000 device's version information with 16-bit addressing
	 * gives the appearance that the data is shifted by one byte; a nonzero
	 * leading array element suggests this could be the case (in which case
	 * the missing zero is prepended).
	 */
	buf[0] = 0;
	dev_id_info = (struct iqs5xx_dev_id_info *)&buf[(buf[1] > 0) ? 0 : 1];

	switch (be16_to_cpu(dev_id_info->prod_num)) {
	case IQS5XX_PROD_NUM_IQS550:
	case IQS5XX_PROD_NUM_IQS572:
	case IQS5XX_PROD_NUM_IQS525:
		break;
	default:
		dev_err(&client->dev, "Unrecognized product number: %u\n",
			be16_to_cpu(dev_id_info->prod_num));
		return -EINVAL;
	}

	switch (be16_to_cpu(dev_id_info->proj_num)) {
	case IQS5XX_PROJ_NUM_A000:
		dev_err(&client->dev, "Unsupported project number: %u\n",
			be16_to_cpu(dev_id_info->proj_num));
		return iqs5xx_bl_open(client);
	case IQS5XX_PROJ_NUM_B000:
		break;
	default:
		dev_err(&client->dev, "Unrecognized project number: %u\n",
			be16_to_cpu(dev_id_info->proj_num));
		return -EINVAL;
	}

	if (dev_id_info->major_ver < IQS5XX_MAJOR_VER_MIN) {
		dev_err(&client->dev, "Unsupported major version: %u\n",
			dev_id_info->major_ver);
		return iqs5xx_bl_open(client);
	}

	switch (dev_id_info->bl_status) {
	case IQS5XX_BL_STATUS_AVAIL:
	case IQS5XX_BL_STATUS_NONE:
		break;
	default:
		dev_err(&client->dev,
			"Unrecognized bootloader status: 0x%02X\n",
			dev_id_info->bl_status);
		return -EINVAL;
	}

	error = iqs5xx_axis_init(client);
	if (error)
		return error;

	error = iqs5xx_read_byte(client, IQS5XX_SYS_CFG0, &val);
	if (error)
		return error;

	val |= IQS5XX_SETUP_COMPLETE;
	val &= ~IQS5XX_SW_INPUT_EVENT;
	error = iqs5xx_write_byte(client, IQS5XX_SYS_CFG0, val);
	if (error)
		return error;

	val = IQS5XX_TP_EVENT | IQS5XX_EVENT_MODE;
	error = iqs5xx_write_byte(client, IQS5XX_SYS_CFG1, val);
	if (error)
		return error;

	error = iqs5xx_write_byte(client, IQS5XX_END_COMM, 0);
	if (error)
		return error;

	iqs5xx->bl_status = dev_id_info->bl_status;

	/*
	 * Closure of the first communication window that appears following the
	 * release of reset appears to kick off an initialization period during
	 * which further communication is met with clock stretching. The return
	 * from this function is delayed so that further communication attempts
	 * avoid this period.
	 */
	msleep(100);

	return 0;
}

static irqreturn_t iqs5xx_irq(int irq, void *data)
{
	struct iqs5xx_private *iqs5xx = data;
	struct iqs5xx_touch_data touch_data[IQS5XX_NUM_CONTACTS];
	struct i2c_client *client = iqs5xx->client;
	struct input_dev *input = iqs5xx->input;
	int error, i;

	/*
	 * This check is purely a precaution, as the device does not assert the
	 * RDY output during bootloader mode. If the device operates outside of
	 * bootloader mode, the input device is guaranteed to be allocated.
	 */
	if (iqs5xx->bl_status == IQS5XX_BL_STATUS_RESET)
		return IRQ_NONE;

	error = iqs5xx_read_burst(client, IQS5XX_ABS_X,
				  touch_data, sizeof(touch_data));
	if (error)
		return IRQ_NONE;

	for (i = 0; i < ARRAY_SIZE(touch_data); i++) {
		u16 pressure = be16_to_cpu(touch_data[i].strength);

		input_mt_slot(input, i);
		if (input_mt_report_slot_state(input, MT_TOOL_FINGER,
					       pressure != 0)) {
			input_report_abs(input, ABS_MT_POSITION_X,
					 be16_to_cpu(touch_data[i].abs_x));
			input_report_abs(input, ABS_MT_POSITION_Y,
					 be16_to_cpu(touch_data[i].abs_y));
			input_report_abs(input, ABS_MT_PRESSURE, pressure);
		}
	}

	input_mt_sync_frame(input);
	input_sync(input);

	error = iqs5xx_write_byte(client, IQS5XX_END_COMM, 0);
	if (error)
		return IRQ_NONE;

	/*
	 * Once the communication window is closed, a small delay is added to
	 * ensure the device's RDY output has been deasserted by the time the
	 * interrupt handler returns.
	 */
	usleep_range(50, 100);

	return IRQ_HANDLED;
}

static int iqs5xx_fw_file_parse(struct i2c_client *client,
				const char *fw_file, u8 *pmap)
{
	const struct firmware *fw;
	struct iqs5xx_ihex_rec *rec;
	size_t pos = 0;
	int error, i;
	u16 rec_num = 1;
	u16 rec_addr;
	u8 rec_len, rec_type, rec_chksm, chksm;
	u8 rec_hdr[IQS5XX_REC_HDR_LEN];
	u8 rec_data[IQS5XX_REC_LEN_MAX];

	/*
	 * Firmware exported from the vendor's configuration tool deviates from
	 * standard ihex as follows: (1) the checksum for records corresponding
	 * to user-exported settings is not recalculated, and (2) an address of
	 * 0xFFFF is used for the EOF record.
	 *
	 * Because the ihex2fw tool tolerates neither (1) nor (2), the slightly
	 * nonstandard ihex firmware is parsed directly by the driver.
	 */
	error = reject_firmware(&fw, fw_file, &client->dev);
	if (error) {
		dev_err(&client->dev, "Failed to request firmware %s: %d\n",
			fw_file, error);
		return error;
	}

	do {
		if (pos + sizeof(*rec) > fw->size) {
			dev_err(&client->dev, "Insufficient firmware size\n");
			error = -EINVAL;
			break;
		}
		rec = (struct iqs5xx_ihex_rec *)(fw->data + pos);
		pos += sizeof(*rec);

		if (rec->start != ':') {
			dev_err(&client->dev, "Invalid start at record %u\n",
				rec_num);
			error = -EINVAL;
			break;
		}

		error = hex2bin(rec_hdr, rec->len, sizeof(rec_hdr));
		if (error) {
			dev_err(&client->dev, "Invalid header at record %u\n",
				rec_num);
			break;
		}

		rec_len = *rec_hdr;
		rec_addr = get_unaligned_be16(rec_hdr + sizeof(rec_len));
		rec_type = *(rec_hdr + sizeof(rec_len) + sizeof(rec_addr));

		if (pos + rec_len * 2 > fw->size) {
			dev_err(&client->dev, "Insufficient firmware size\n");
			error = -EINVAL;
			break;
		}
		pos += (rec_len * 2);

		error = hex2bin(rec_data, rec->data, rec_len);
		if (error) {
			dev_err(&client->dev, "Invalid data at record %u\n",
				rec_num);
			break;
		}

		error = hex2bin(&rec_chksm,
				rec->data + rec_len * 2, sizeof(rec_chksm));
		if (error) {
			dev_err(&client->dev, "Invalid checksum at record %u\n",
				rec_num);
			break;
		}

		chksm = 0;
		for (i = 0; i < sizeof(rec_hdr); i++)
			chksm += rec_hdr[i];
		for (i = 0; i < rec_len; i++)
			chksm += rec_data[i];
		chksm = ~chksm + 1;

		if (chksm != rec_chksm && rec_addr < IQS5XX_CSTM) {
			dev_err(&client->dev,
				"Incorrect checksum at record %u\n",
				rec_num);
			error = -EINVAL;
			break;
		}

		switch (rec_type) {
		case IQS5XX_REC_TYPE_DATA:
			if (rec_addr < IQS5XX_CHKSM ||
			    rec_addr > IQS5XX_PMAP_END) {
				dev_err(&client->dev,
					"Invalid address at record %u\n",
					rec_num);
				error = -EINVAL;
			} else {
				memcpy(pmap + rec_addr - IQS5XX_CHKSM,
				       rec_data, rec_len);
			}
			break;
		case IQS5XX_REC_TYPE_EOF:
			break;
		default:
			dev_err(&client->dev, "Invalid type at record %u\n",
				rec_num);
			error = -EINVAL;
		}

		if (error)
			break;

		rec_num++;
		while (pos < fw->size) {
			if (*(fw->data + pos) == ':')
				break;
			pos++;
		}
	} while (rec_type != IQS5XX_REC_TYPE_EOF);

	release_firmware(fw);

	return error;
}

static int iqs5xx_fw_file_write(struct i2c_client *client, const char *fw_file)
{
	struct iqs5xx_private *iqs5xx = i2c_get_clientdata(client);
	int error;
	u8 *pmap;

	if (iqs5xx->bl_status == IQS5XX_BL_STATUS_NONE)
		return -EPERM;

	pmap = kzalloc(IQS5XX_PMAP_LEN, GFP_KERNEL);
	if (!pmap)
		return -ENOMEM;

	error = iqs5xx_fw_file_parse(client, fw_file, pmap);
	if (error)
		goto err_kfree;

	mutex_lock(&iqs5xx->lock);

	/*
	 * Disable the interrupt line in case the first attempt(s) to enter the
	 * bootloader don't happen quickly enough, in which case the device may
	 * assert the RDY output until the next attempt.
	 */
	disable_irq(client->irq);

	iqs5xx->bl_status = IQS5XX_BL_STATUS_RESET;

	error = iqs5xx_bl_cmd(client, IQS5XX_BL_CMD_VER, 0);
	if (error) {
		error = iqs5xx_bl_open(client);
		if (error)
			goto err_reset;
	}

	error = iqs5xx_bl_write(client, IQS5XX_CHKSM, pmap, IQS5XX_PMAP_LEN);
	if (error)
		goto err_reset;

	error = iqs5xx_bl_cmd(client, IQS5XX_BL_CMD_CRC, 0);
	if (error)
		goto err_reset;

	error = iqs5xx_bl_verify(client, IQS5XX_CSTM,
				 pmap + IQS5XX_CHKSM_LEN + IQS5XX_APP_LEN,
				 IQS5XX_CSTM_LEN);
	if (error)
		goto err_reset;

	error = iqs5xx_bl_cmd(client, IQS5XX_BL_CMD_EXEC, 0);

err_reset:
	if (error) {
		iqs5xx_reset(client);
		usleep_range(10000, 10100);
	}

	error = iqs5xx_dev_init(client);
	if (!error && iqs5xx->bl_status == IQS5XX_BL_STATUS_RESET)
		error = -EINVAL;

	enable_irq(client->irq);

	mutex_unlock(&iqs5xx->lock);

err_kfree:
	kfree(pmap);

	return error;
}

static ssize_t fw_file_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct iqs5xx_private *iqs5xx = dev_get_drvdata(dev);
	struct i2c_client *client = iqs5xx->client;
	size_t len = count;
	bool input_reg = !iqs5xx->input;
	char fw_file[IQS5XX_FW_FILE_LEN + 1];
	int error;

	if (!len)
		return -EINVAL;

	if (buf[len - 1] == '\n')
		len--;

	if (len > IQS5XX_FW_FILE_LEN)
		return -ENAMETOOLONG;

	memcpy(fw_file, buf, len);
	fw_file[len] = '\0';

	error = iqs5xx_fw_file_write(client, fw_file);
	if (error)
		return error;

	/*
	 * If the input device was not allocated already, it is guaranteed to
	 * be allocated by this point and can finally be registered.
	 */
	if (input_reg) {
		error = input_register_device(iqs5xx->input);
		if (error) {
			dev_err(&client->dev,
				"Failed to register device: %d\n",
				error);
			return error;
		}
	}

	return count;
}

static DEVICE_ATTR_WO(fw_file);

static struct attribute *iqs5xx_attrs[] = {
	&dev_attr_fw_file.attr,
	NULL,
};

static const struct attribute_group iqs5xx_attr_group = {
	.attrs = iqs5xx_attrs,
};

static int __maybe_unused iqs5xx_suspend(struct device *dev)
{
	struct iqs5xx_private *iqs5xx = dev_get_drvdata(dev);
	struct input_dev *input = iqs5xx->input;
	int error = 0;

	if (!input)
		return error;

	mutex_lock(&input->mutex);

	if (input->users)
		error = iqs5xx_set_state(iqs5xx->client, IQS5XX_SUSPEND);

	mutex_unlock(&input->mutex);

	return error;
}

static int __maybe_unused iqs5xx_resume(struct device *dev)
{
	struct iqs5xx_private *iqs5xx = dev_get_drvdata(dev);
	struct input_dev *input = iqs5xx->input;
	int error = 0;

	if (!input)
		return error;

	mutex_lock(&input->mutex);

	if (input->users)
		error = iqs5xx_set_state(iqs5xx->client, IQS5XX_RESUME);

	mutex_unlock(&input->mutex);

	return error;
}

static SIMPLE_DEV_PM_OPS(iqs5xx_pm, iqs5xx_suspend, iqs5xx_resume);

static int iqs5xx_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct iqs5xx_private *iqs5xx;
	int error;

	iqs5xx = devm_kzalloc(&client->dev, sizeof(*iqs5xx), GFP_KERNEL);
	if (!iqs5xx)
		return -ENOMEM;

	i2c_set_clientdata(client, iqs5xx);
	iqs5xx->client = client;

	iqs5xx->reset_gpio = devm_gpiod_get(&client->dev,
					    "reset", GPIOD_OUT_LOW);
	if (IS_ERR(iqs5xx->reset_gpio)) {
		error = PTR_ERR(iqs5xx->reset_gpio);
		dev_err(&client->dev, "Failed to request GPIO: %d\n", error);
		return error;
	}

	mutex_init(&iqs5xx->lock);

	iqs5xx_reset(client);
	usleep_range(10000, 10100);

	error = iqs5xx_dev_init(client);
	if (error)
		return error;

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, iqs5xx_irq, IRQF_ONESHOT,
					  client->name, iqs5xx);
	if (error) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", error);
		return error;
	}

	error = devm_device_add_group(&client->dev, &iqs5xx_attr_group);
	if (error) {
		dev_err(&client->dev, "Failed to add attributes: %d\n", error);
		return error;
	}

	if (iqs5xx->input) {
		error = input_register_device(iqs5xx->input);
		if (error)
			dev_err(&client->dev,
				"Failed to register device: %d\n",
				error);
	}

	return error;
}

static const struct i2c_device_id iqs5xx_id[] = {
	{ "iqs550", 0 },
	{ "iqs572", 1 },
	{ "iqs525", 2 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, iqs5xx_id);

static const struct of_device_id iqs5xx_of_match[] = {
	{ .compatible = "azoteq,iqs550" },
	{ .compatible = "azoteq,iqs572" },
	{ .compatible = "azoteq,iqs525" },
	{ }
};
MODULE_DEVICE_TABLE(of, iqs5xx_of_match);

static struct i2c_driver iqs5xx_i2c_driver = {
	.driver = {
		.name		= "iqs5xx",
		.of_match_table	= iqs5xx_of_match,
		.pm		= &iqs5xx_pm,
	},
	.id_table	= iqs5xx_id,
	.probe		= iqs5xx_probe,
};
module_i2c_driver(iqs5xx_i2c_driver);

MODULE_AUTHOR("Jeff LaBundy <jeff@labundy.com>");
MODULE_DESCRIPTION("Azoteq IQS550/572/525 Trackpad/Touchscreen Controller");
MODULE_LICENSE("GPL");
