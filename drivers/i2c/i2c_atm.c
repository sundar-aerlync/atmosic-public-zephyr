/*
 * Copyright (C) Atmosic 2021-2024
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT atmosic_atm_i2c

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_atm, CONFIG_I2C_LOG_LEVEL);

#include <soc.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/util.h>
#include "at_clkrstgen.h"

#include "arch.h"
#include "i2c-priv.h"
#include "at_wrpr.h"
#include "at_pinmux.h"
#include "at_apb_i2c_regs_core_macro.h"

#ifndef __I2C_TRANSACTION_SETUP_MACRO__
#define I2C(DEF) (I2C0_##DEF)
#else
#define I2C(DEF) (I2C_##DEF)
#endif

typedef enum i2c_head_e { I2C_HEAD_START = 0, I2C_HEAD_STALL } i2c_head_t;

typedef enum i2c_rw_e { I2C_WRITE = 0, I2C_READ } i2c_rw_t;

typedef enum i2c_ack_e {
	/* ACK is active low */
	I2C_ACK = 0,
	I2C_NACK
} i2c_ack_t;

typedef enum i2c_tail_e { I2C_TAIL_STOP = 0, I2C_TAIL_STALL, I2C_TAIL_RESTART } i2c_tail_t;

struct i2c_atm_data {
	uint32_t config;
	struct k_sem xfer_sem;
};

typedef void (*set_callback_t)(void);

struct i2c_atm_config {
	int instance;
	CMSDK_AT_APB_I2C_TypeDef *base;
	bool sda_pullup;
	set_callback_t config_pins;
	uint32_t mode;
	uint32_t speed;
};

static int i2c_out_sync(struct device const *dev, i2c_head_t head, uint8_t val, i2c_tail_t tail)
{
	struct i2c_atm_config const *config = dev->config;

	/* Master drives all 8 data bits */
	config->base->OUTGOING_DATA = (config->sda_pullup ? I2C(OUTGOING_DATA__DATA_PU__MASK) : 0) |
				      I2C(OUTGOING_DATA__DATA_OE__WRITE(~val)) |
				      I2C(OUTGOING_DATA__DATA_O__WRITE(val));

	/* Assert GO */
	config->base->TRANSACTION_SETUP = I2C(TRANSACTION_SETUP__GO__MASK) |
					  I2C(TRANSACTION_SETUP__ACK_VALUE_TO_DRIVE__MASK) |
#ifdef I2C_TRANSACTION_SETUP__MSTR__MASK
					  I2C(TRANSACTION_SETUP__MSTR__WRITE(1)) |
#endif
					  I2C(TRANSACTION_SETUP__TAIL__WRITE(tail)) |
					  I2C(TRANSACTION_SETUP__HEAD__WRITE(head));

#ifdef I2C_TRANSACTION_STATUS__DONE__MASK
	while (!(I2C_TRANSACTION_STATUS__DONE__READ(config->base->TRANSACTION_STATUS)))
#else
	while (config->base->TRANSACTION_STATUS & I2C(TRANSACTION_STATUS__RUNNING__MASK))
#endif
	{
		int i = 0;
		if (i++ > CONFIG_I2C_ATM_TIMEOUT) {
			config->base->TRANSACTION_SETUP = 0;
			LOG_ERR("I2C communication timed out: %#x",
				config->base->TRANSACTION_STATUS);
			return -EIO;
		}
		YIELD();
	}

	/* ACK is active low */
	int ret = (config->base->TRANSACTION_STATUS & I2C(TRANSACTION_STATUS__ACK_VALUE__MASK))
			  ? -EIO
			  : 0;

	/* Deassert GO */
	config->base->TRANSACTION_SETUP =
#ifdef I2C_TRANSACTION_SETUP__MSTR__MASK
		I2C(TRANSACTION_SETUP__MSTR__WRITE(1)) |
#endif
		I2C(TRANSACTION_SETUP__GO__WRITE(0));

	return ret;
}

static int i2c_in_sync(struct device const *dev, i2c_ack_t ack, i2c_tail_t tail)
{
	struct i2c_atm_config const *config = dev->config;

	/* Master listens all 8 data bits */
	config->base->OUTGOING_DATA = (config->sda_pullup ? I2C(OUTGOING_DATA__DATA_PU__MASK) : 0);

	/* Assert GO */
	config->base->TRANSACTION_SETUP = I2C(TRANSACTION_SETUP__GO__MASK) |
					  I2C(TRANSACTION_SETUP__ACK_VALUE_TO_DRIVE__WRITE(ack)) |
					  I2C(TRANSACTION_SETUP__MASTER_DRIVES_ACK__MASK) |
#ifdef I2C_TRANSACTION_SETUP__MSTR__MASK
					  I2C(TRANSACTION_SETUP__MSTR__WRITE(1)) |
#endif
					  I2C(TRANSACTION_SETUP__TAIL__WRITE(tail)) |
					  I2C(TRANSACTION_SETUP__HEAD__WRITE(I2C_HEAD_STALL));

	int i = 0;
#ifdef I2C_TRANSACTION_STATUS__DONE__MASK
	while (!(I2C_TRANSACTION_STATUS__DONE__READ(config->base->TRANSACTION_STATUS)))
#else
	while (config->base->TRANSACTION_STATUS & I2C(TRANSACTION_STATUS__RUNNING__MASK))
#endif
	{
		if (i++ > CONFIG_I2C_ATM_TIMEOUT) {
			config->base->TRANSACTION_SETUP = 0;
			LOG_ERR("I2C communication timed out: %#x",
				config->base->TRANSACTION_STATUS);
			return -EIO;
		}
		YIELD();
	}

	int ret = config->base->INCOMING_DATA;

	/* Deassert GO */
	config->base->TRANSACTION_SETUP =
#ifdef I2C_TRANSACTION_SETUP__MSTR__MASK
		I2C(TRANSACTION_SETUP__MSTR__WRITE(1)) |
#endif
		I2C(TRANSACTION_SETUP__GO__WRITE(0));

	return ret;
}

static int i2c_atm_read_msg(struct device const *dev, uint16_t addr, struct i2c_msg msg)
{
	if (!msg.len) {
		LOG_ERR("Invalid message length. Received: %d", msg.len);
		return -EINVAL;
	}

	int ret = i2c_out_sync(dev, I2C_HEAD_START, (addr << 1) | I2C_READ, I2C_TAIL_STALL);
	if (ret < 0) {
		return ret;
	}

	/* Compensate for last read being outside of loop */
	for (int i = 0; i < msg.len - 1; i++) {
		int val = i2c_in_sync(dev, I2C_ACK, I2C_TAIL_STALL);
		if (val < 0) {
			return val;
		}
		msg.buf[i] = val;
	}
	/* Last read */
	{
		bool stop = (msg.flags & I2C_MSG_STOP) ? true : false;
		int val = i2c_in_sync(dev, I2C_NACK, stop ? I2C_TAIL_STOP : I2C_TAIL_RESTART);
		if (val < 0) {
			return val;
		}
		msg.buf[msg.len - 1] = val;
	}

	return 0;
}

static int i2c_atm_write_msg(struct device const *dev, uint16_t addr, struct i2c_msg msg,
			     uint8_t msg_idx)
{
	if (!msg.len && (msg.flags != I2C_MSG_STOP)) {
		LOG_ERR("Invalid message length. Received: %d flags %08x\n", msg.len, msg.flags);
		return -EINVAL;
	}

	// msg_idx == 0 implies first txn to the specified device
	// head = I2C_HEAD_START for the first txn and I2C_HEAD_STALL for rest
	if (!msg_idx) {
		int ret = i2c_out_sync(dev, I2C_HEAD_START, (addr << 1) | I2C_WRITE,
				       (msg.len) ? I2C_TAIL_STALL : I2C_TAIL_STOP);
		if (ret < 0) {
			return ret;
		}

		/* Handle zero length writes */
		if (!msg.len) {
			return ret;
		}
	}

	/* Compensate for last write being outside of loop */
	for (int i = 0; i < msg.len - 1; i++) {
		int ret = i2c_out_sync(dev, I2C_HEAD_STALL, msg.buf[i], I2C_TAIL_STALL);
		if (ret < 0) {
			return ret;
		}
	}

	/* Last write */
	// stop == false implies tail = I2C_TAIL_STALL for all txns
	// stop == true implies tail = I2C_TAIL_STOP for the last txn
	bool stop = (msg.flags & I2C_MSG_STOP) ? true : false;
	i2c_tail_t tail = stop ? I2C_TAIL_STOP : I2C_TAIL_STALL;
	return i2c_out_sync(dev, I2C_HEAD_STALL, msg.buf[msg.len - 1], tail);
}

static int i2c_atm_transfer(struct device const *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			    uint16_t addr)
{
	struct i2c_atm_data *data = dev->data;

	if (data->config & I2C_ADDR_10_BITS) {
		LOG_ERR("10-bit I2C address not supported. Received: %#x", addr);
		return -ENOTSUP;
	}

	k_sem_take(&data->xfer_sem, K_FOREVER);

	/* Mask out unused address bits, and make room for R/W bit */
	int ret = 0;
	for (uint8_t i = 0; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_RW_MASK) == I2C_MSG_WRITE) {
			ret = i2c_atm_write_msg(dev, addr, msgs[i], i);
		} else {
			ret = i2c_atm_read_msg(dev, addr, msgs[i]);
		}

		if (ret < 0) {
			break;
		}
	}

	k_sem_give(&data->xfer_sem);

	return ret;
}

static int i2c_atm_set_speed(struct device const *dev, uint32_t speed)
{
	static uint32_t const s2f_tbl[] = { [I2C_SPEED_STANDARD] = KHZ(100),
					    [I2C_SPEED_FAST] = KHZ(400),
					    [I2C_SPEED_FAST_PLUS] = MHZ(1),
					    [I2C_SPEED_HIGH] = 0,
					    [I2C_SPEED_ULTRA] = 0 };

	uint32_t hertz = s2f_tbl[speed];

	if (!hertz) {
		LOG_ERR("I2C speed not supported. Received: %d", speed);
		return -ENOTSUP;
	}
	uint32_t clkdiv = (at_clkrstgen_get_bp() / (hertz * 4)) - 1;
	struct i2c_atm_config const *config = dev->config;
	config->base->CLOCK_CONTROL = I2C(CLOCK_CONTROL__CLKDIV__WRITE(clkdiv));

	return 0;
}

static int i2c_atm_configure(struct device const *dev, uint32_t cfg)
{
	struct i2c_atm_config const *config = dev->config;
	struct i2c_atm_data *data = dev->data;

	if (!(cfg & I2C_MODE_CONTROLLER)) {
		LOG_ERR("I2C slave mode not supported. Received: %#x", cfg);
		return -ENOTSUP;
	}

	data->config = cfg;
	config->config_pins();

#ifdef PSEQ_CTRL0__I2C_LATCH_OPEN__MASK
	WRPR_CTRL_SET(CMSDK_PSEQ, WRPR_CTRL__CLK_ENABLE);
	{
		PSEQ_CTRL0__I2C_LATCH_OPEN__CLR(CMSDK_PSEQ->CTRL0);
	}
	WRPR_CTRL_SET(CMSDK_PSEQ, WRPR_CTRL__CLK_DISABLE);
#endif

	return i2c_atm_set_speed(dev, I2C_SPEED_GET(cfg));
}

static struct i2c_driver_api const i2c_atm_driver_api = {
	.configure = i2c_atm_configure,
	.transfer = i2c_atm_transfer,
};

static int i2c_atm_init(struct device const *dev)
{
	struct i2c_atm_config const *config = dev->config;
	uint32_t bitrate = i2c_map_dt_bitrate(config->speed);
	struct i2c_atm_data *data = dev->data;

	k_sem_init(&data->xfer_sem, 1, 1);

	return i2c_atm_configure(dev, config->mode | bitrate);
}

#define I2C_SCK(n) CONCAT(CONCAT(I2C, DT_INST_PROP(n, instance)), _SCK)
#define I2C_SDA(n) CONCAT(CONCAT(I2C, DT_INST_PROP(n, instance)), _SDA)
#define I2C_BASE(n) CONCAT(CMSDK_I2C, DT_INST_PROP(n, instance))
#define I2C_DEVICE_INIT(n)                                                                         \
	static void i2c_atm_config_pins_##n(void)                                                  \
	{                                                                                          \
		/* Configure pinmux (and pullup) for the given intance */                          \
		PIN_SELECT(DT_INST_PROP(n, scl_pin), I2C_SCK(n));                                  \
		PIN_SELECT(DT_INST_PROP(n, sda_pin), I2C_SDA(n));                                  \
		WRPR_CTRL_SET(I2C_BASE(n), WRPR_CTRL__CLK_ENABLE);                                 \
		if (DT_INST_PROP(n, scl_pullup)) {                                                 \
			PIN_PULLUP(DT_INST_PROP(n, scl_pin));                                      \
		}                                                                                  \
	}                                                                                          \
	static struct i2c_atm_config const i2c_atm_config_##n = {                                  \
		.instance = n,                                                                     \
		.base = I2C_BASE(n),                                                               \
		.sda_pullup = DT_INST_PROP(n, sda_pullup),                                         \
		.config_pins = i2c_atm_config_pins_##n,                                            \
		.mode = I2C_MODE_CONTROLLER,                                                       \
		.speed = DT_INST_PROP(n, clock_frequency),                                         \
	};                                                                                         \
	static struct i2c_atm_data i2c_atm_data_##n;                                               \
	DEVICE_DT_INST_DEFINE(n, &i2c_atm_init, NULL, &i2c_atm_data_##n, &i2c_atm_config_##n,      \
			      POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &i2c_atm_driver_api);         \
	BUILD_ASSERT(I2C_BASE(n) == (CMSDK_AT_APB_I2C_TypeDef *)DT_REG_ADDR(DT_NODELABEL(          \
					    CONCAT(i2c, DT_INST_PROP(n, instance)))));

DT_INST_FOREACH_STATUS_OKAY(I2C_DEVICE_INIT)
