/*
 * linux/drivers/i2c/busses/i2c-ns9xxx.c
 *
 * based on old i2c-ns9xxx.c by Digi International Inc.
 *
 * Copyright (C) 2008 by Digi International Inc.
 * Modifications (C) 2015 by Levien van Zon (levien AT zonnetjes.net)
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/i2c-ns9xxx.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>

#include <asm/gpio.h>
#include <asm/io.h>

/* registers */
#define I2C_CMD				0x00
#define I2C_STATUS			0x00
#define I2C_MASTERADDR			0x04
#define I2C_SLAVEADDR			0x08
#define I2C_CONFIG			0x0c

/* command bit fields */
#define I2C_CMD_TXVAL			(1 << 13)
#define I2C_MASTERADDR_7BIT		0
#define I2C_MASTERADDR_10BIT		1

/* configuration masks */
#define I2C_CONFIG_CLREFMASK		0x000001ff
#define I2C_MASTERADDR_ADDRMASK		0x000007ff

/* configuration bitfields */
#define I2C_CONFIG_IRQD			(1 << 15)
#define I2C_CONFIG_TMDE			(1 << 14)
#define I2C_CONFIG_VSCD			(1 << 13)

/* configuration shifts */
#define I2C_MASTERADDR_ADDRSHIFT	1
#define I2C_CONFIG_SFW_SHIFT		9

/* shifted i2c commands */
#define I2C_CMD_NOP			0
#define I2C_CMD_READ			(4 << 8)
#define I2C_CMD_WRITE			(5 << 8)
#define I2C_CMD_STOP			(6 << 8)

/* interrupt causes */
#define I2C_IRQ_MASK			(0xf << 8)
#define I2C_IRQ_ARBITLOST		(1 << 8)
#define I2C_IRQ_NOACK			(2 << 8)
#define I2C_IRQ_TXDATA			(3 << 8)
#define I2C_IRQ_RXDATA			(4 << 8)
#define I2C_IRQ_CMDACK			(5 << 8)

#define I2C_NORMALSPEED			100000
#define I2C_HIGHSPEED			400000

/* status bits */
#define I2C_STATUS_BSTS			0x8000		/* Bus status */
#define I2C_STATUS_RDE			0x4000		/* Receive datus available */
#define I2C_STATUS_MCMDL		0x1000		/* Master command lock */
#define I2C_STATUS_IRQCD_MASK	0x0f00		/* IRQ code (mask) */
#define I2C_STATUS_RXDATA_MASK	0x00ff		/* Received data (mask) */


/* SCL_DELAY
 * To fine adjust the formulas to the hardware reference manual
 * we define different SCL_DELAY per platform
 */
#if defined(CONFIG_MACH_CC9P9215JS) || defined(CONFIG_MACH_CCW9P9215JS)
	#define SCL_DELAY		16
#elif defined(CONFIG_MACH_CME9210JS)
	//#define SCL_DELAY		306			// Original value
	#define SCL_DELAY		25			// Alternative value, seems much more stable
#elif defined(CONFIG_MACH_CC9P9360JS)
	#define SCL_DELAY		12
#elif defined(CONFIG_MACH_CC9CJS) || defined(CONFIG_MACH_CCW9CJS)
	#define SCL_DELAY		2
#else
	#define SCL_DELAY		0
#endif

#define DRIVER_NAME			"i2c-ns9xxx"

static int scl_delay = SCL_DELAY;
module_param(scl_delay, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(scl_delay, "SCL delay parameter for NS9xxx I2C");

enum i2c_int_state {
	I2C_INT_AWAITING,
	I2C_INT_OK,
	I2C_INT_RETRY,
	I2C_INT_ERROR,
	I2C_INT_ABORT
};

struct ns9xxx_i2c {
	struct i2c_adapter	adap;
	struct resource		*mem;
	struct clk		*clk;

	void __iomem		*ioaddr;

	spinlock_t		lock;
	wait_queue_head_t	wait_q;

	struct plat_ns9xxx_i2c	*pdata;

	char			*buf;
	int			irq;
	enum i2c_int_state	state;
};

static int ns9xxx_i2c_xfer(struct i2c_adapter *adap,
		struct i2c_msg msgs[], int num);

static u32 ns9xxx_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR
		| I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE
		| I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA;
}

static struct i2c_algorithm ns9xxx_i2c_algo = {
	.master_xfer	= ns9xxx_i2c_xfer,
	.functionality	= ns9xxx_i2c_func,
};

static int ns9xxx_i2c_set_clock(struct ns9xxx_i2c *dev_data, unsigned int freq);
static int ns9xxx_wait_while_busy(struct ns9xxx_i2c *dev);


static irqreturn_t ns9xxx_i2c_irq(int irqnr, void *dev_id)
{
	struct ns9xxx_i2c *dev_data = (struct ns9xxx_i2c *)dev_id;
	u32 status;

	/* acknowledge IRQ by reading the status register */
	status = readl(dev_data->ioaddr + I2C_STATUS);

	if (dev_data->state != I2C_INT_AWAITING)
		return IRQ_HANDLED;

	spin_lock(&dev_data->lock);
	
	switch (status & I2C_STATUS_IRQCD_MASK) {
	case I2C_IRQ_RXDATA:
		if (dev_data->buf)
			*dev_data->buf = status & I2C_STATUS_RXDATA_MASK;
	case I2C_IRQ_CMDACK:
	case I2C_IRQ_TXDATA:
		dev_data->state = I2C_INT_OK;
		break;
	case I2C_IRQ_NOACK:
		writel(I2C_CMD_STOP, dev_data->ioaddr + I2C_CMD);
		dev_data->state = I2C_INT_ABORT;
		break;
	case I2C_IRQ_ARBITLOST:
		dev_data->state = I2C_INT_RETRY;
		break;
	default:
		dev_data->state = I2C_INT_ERROR;
	}

	spin_unlock(&dev_data->lock);

	wake_up_interruptible(&dev_data->wait_q);

	return IRQ_HANDLED;
}

static int ns9xxx_i2c_send_cmd(struct ns9xxx_i2c *dev_data, unsigned int cmd)
{
	unsigned long flags;
	u32 status;
		
	status = readl(dev_data->ioaddr + I2C_STATUS);
	if (status & I2C_STATUS_MCMDL) {
		if (ns9xxx_wait_while_busy(dev_data)) {		/* Wait for previous command to finish */
			printk(KERN_WARNING "NS9XXX I2C: timeout waiting for I2C master module to unlock\n");
			return -ETIMEDOUT;
		}
	}
	
	spin_lock_irqsave(&dev_data->lock, flags);
	dev_data->state = I2C_INT_AWAITING;
	writel(cmd, dev_data->ioaddr + I2C_CMD);
	spin_unlock_irqrestore(&dev_data->lock, flags);
	
	if (!wait_event_interruptible_timeout(dev_data->wait_q,
				dev_data->state != I2C_INT_AWAITING,
				dev_data->adap.timeout)) {
		printk(KERN_WARNING "NS9XXX I2C: timeout waiting for interrupt (cmd = %u, timeout = %d)\n", cmd, (int)(dev_data->adap.timeout));
		
		if (ns9xxx_wait_while_busy(dev_data) == 0) {
			printk(KERN_WARNING "NS9XXX I2C: bus seems free after waiting, but not retrying.\n");
			
			#if 0
			// If bus is free, see if we can repeat the command
			printk(KERN_WARNING "NS9XXX I2C: bus seems free, resetting bus and repeating command\n");
			ns9xxx_i2c_reset_bitbang_blind(dev_data);		/* Try to reset the bus */
			spin_lock_irqsave(&dev_data->lock, flags);
			dev_data->state = I2C_INT_AWAITING;
			writel(cmd, dev_data->ioaddr + I2C_CMD);
			spin_unlock_irqrestore(&dev_data->lock, flags);
			if (wait_event_interruptible_timeout(dev_data->wait_q,
						dev_data->state != I2C_INT_AWAITING,
						dev_data->adap.timeout)) {
				return 0;
			} else {
				printk(KERN_WARNING "NS9XXX I2C: Timeout on second attempt, giving up.\n");
			}
			#endif
		}
		
		return -ETIMEDOUT;
	}

	if (dev_data->state != I2C_INT_OK) {
		printk(KERN_WARNING "NS9XXX I2C: state %d != I2C_INT_OK in ns9xxx_i2c_send_cmd()\n", dev_data->state);
		return -EIO;
	}
	
	return 0;
}

static int ns9xxx_i2c_read(struct ns9xxx_i2c *dev_data, int count)
{
	unsigned long flags;
	int ret = 0;

	while (count-- > 1) {
		spin_lock_irqsave(&dev_data->lock, flags);
		dev_data->buf++;
		spin_unlock_irqrestore(&dev_data->lock, flags);

		ret = ns9xxx_i2c_send_cmd(dev_data, I2C_CMD_NOP);
		if (ret)
			break;
	}

	return ret;
}

static int ns9xxx_i2c_write(struct ns9xxx_i2c *dev_data,
		const char *buf, int count)
{
	int ret = 0;

	while (count--) {
		ret = ns9xxx_i2c_send_cmd(dev_data,
				I2C_CMD_NOP | I2C_CMD_TXVAL | *buf);
		if (ret)
			break;
		buf++;
	}

	return ret;
}

static int ns9xxx_i2c_bitbang(struct ns9xxx_i2c *dev_data, struct i2c_msg *msg)
{
	int i, nr_bits, ret;

	disable_irq(dev_data->irq);		/* Disable our interrupt for a while */	
		
	gpio_direction_output(dev_data->pdata->gpio_sda, 1);
	gpio_direction_output(dev_data->pdata->gpio_scl, 1);
	mdelay(10);

	/* start */
	gpio_set_value(dev_data->pdata->gpio_sda, 0);
	mdelay(1);
	gpio_set_value(dev_data->pdata->gpio_scl, 0);
	mdelay(1);

	nr_bits = (msg->flags & I2C_M_TEN) ? 10 : 7;
	printk(KERN_DEBUG "NS9XXX I2C: sending %d bits using bitbang-routine\n", nr_bits);
	for (i = 0; i < nr_bits; i++) {
		/* set data */
		if (msg->addr & (1 << (nr_bits - i - 1)))
			gpio_set_value(dev_data->pdata->gpio_sda, 1);
		else
			gpio_set_value(dev_data->pdata->gpio_sda, 1);
		mdelay(1);

		/* toggle clock */
		gpio_set_value(dev_data->pdata->gpio_scl, 1);
		mdelay(1);
		gpio_set_value(dev_data->pdata->gpio_scl, 0);
		mdelay(1);
	}

	/* read ack */
	gpio_direction_input(dev_data->pdata->gpio_sda);
	gpio_set_value(dev_data->pdata->gpio_scl, 1);
	mdelay(1);
	ret = gpio_get_value(dev_data->pdata->gpio_sda);

	/* stop */
	gpio_direction_output(dev_data->pdata->gpio_sda, 1);

	/* reset gpios to hardware i2c */
	dev_data->pdata->gpio_configuration_func();
	
	enable_irq(dev_data->irq);		/* Reenable our interrupt */

	return ret ? 0 : -ENODEV;
}


static void ns9xxx_i2c_stop_bitbang(struct ns9xxx_i2c *dev_data)
{
	// Use GPIO to force a STOP 
	
	gpio_direction_output(dev_data->pdata->gpio_scl, 1);
	mdelay(1);
	gpio_direction_output(dev_data->pdata->gpio_sda, 1);
	mdelay(1);
	
	/* reset gpios to hardware i2c */
	dev_data->pdata->gpio_configuration_func();

}


static void ns9xxx_i2c_reset_bitbang(struct ns9xxx_i2c *dev_data)
{
	// Use GPIO to force a bus-reset
	
	int i, scl, sda, prev_sda;
	u32 status, masteraddr, config;
	int effective_cycles = 0;
	
	disable_irq(dev_data->irq);		/* Disable our interrupt for a while */	
	
	gpio_direction_input(dev_data->pdata->gpio_scl);
	gpio_direction_input(dev_data->pdata->gpio_sda);
	mdelay(1);
	scl = gpio_get_value(dev_data->pdata->gpio_scl);
	sda = gpio_get_value(dev_data->pdata->gpio_sda);

	if (sda && scl) {
		printk(KERN_DEBUG "NS9XXX I2C: bus-reset requested but bus seems idle, reset not needed?\n");
	}
	
	if (!sda) {
		printk(KERN_DEBUG "NS9XXX I2C: SDA seems to be held low externally.\n");
	}
	
	prev_sda = sda;
	
	for (i = 0; i < 9; i++) {
		/* toggle clock */

		scl = gpio_get_value(dev_data->pdata->gpio_scl);
		if (!scl) {
			printk(KERN_DEBUG "NS9XXX I2C: SCL held low at start of clock cycle %d, continuing anyway.\n", i);
		} else
			effective_cycles++;
		gpio_direction_output(dev_data->pdata->gpio_scl, 0);
		mdelay(1);
		gpio_direction_input(dev_data->pdata->gpio_scl);
		sda = gpio_get_value(dev_data->pdata->gpio_sda);
		scl = gpio_get_value(dev_data->pdata->gpio_scl);
		if (sda != prev_sda) {
			printk(KERN_DEBUG "NS9XXX I2C: SDA changed from %d to %d after %d clock cycles.\n", prev_sda, sda, i);
			prev_sda = sda;
		}
		if (!scl) {
			printk(KERN_DEBUG "NS9XXX I2C: SCL held low at end of clock cycle %d, 1 ms delay.\n", i);
			mdelay(1);
		}
	}

	/* Send a STOP */
	
	mdelay(1);
	gpio_direction_input(dev_data->pdata->gpio_sda);
	mdelay(1);
	scl = gpio_get_value(dev_data->pdata->gpio_scl);
	sda = gpio_get_value(dev_data->pdata->gpio_sda);
	
	if (scl == 0) {
		printk(KERN_ERR "NS9XXX I2C: SCL seems to be held low externally?\n");
	}
	if (sda == 0) {
		printk(KERN_ERR "NS9XXX I2C: SDA seems to be held low externally?\n");
	}
	
	if (scl && sda) {
		printk(KERN_DEBUG "NS9XXX I2C: reset successful, bus seems to be idle\n");
	} else {
		printk(KERN_DEBUG "NS9XXX I2C: reset unsuccessful after %d effective cycles\n", effective_cycles);
	}
	
	/* reset gpios to hardware i2c */
	dev_data->pdata->gpio_configuration_func();

	status = readl(dev_data->ioaddr + I2C_STATUS);
	masteraddr = readl(dev_data->ioaddr + I2C_MASTERADDR);
	config = readl(dev_data->ioaddr + I2C_CONFIG);
	printk(KERN_WARNING "NS9XXX I2C: STATUS %lx, MASTERADDR %lx, CONFIG %lx, state %lx\n", (unsigned long)status, (unsigned long)masteraddr, (unsigned long)config, (unsigned long)dev_data->state);

	enable_irq(dev_data->irq);		/* Reenable our interrupt */
}


static void ns9xxx_reinit_i2c(struct ns9xxx_i2c *dev_data)
{
	u32 status;
	int ret;
	
	ns9xxx_i2c_reset_bitbang(dev_data);		/* Try to reset the bus */

	status = readl(dev_data->ioaddr + I2C_STATUS);
	if (status & I2C_STATUS_MCMDL) {
	
		/* Master module still locked, try to reinitialise the I2C hardware */
		
		printk(KERN_WARNING "NS9XXX I2C: master module still locked (STATUS 0x%lx), trying to reinitialise hardware\n", (unsigned long)status);
		
		disable_irq(dev_data->irq);
		
		writel(I2C_CONFIG_IRQD | (0xf << I2C_CONFIG_SFW_SHIFT),
			   dev_data->ioaddr + I2C_CONFIG);
	
		if (dev_data->pdata->speed)
			ret = ns9xxx_i2c_set_clock(dev_data, dev_data->pdata->speed);
		else
			ret = ns9xxx_i2c_set_clock(dev_data, I2C_NORMALSPEED);
		if (ret) {
			printk(KERN_ERR "NS9XXX I2C: Error setting bus clock\n");
		}
		
		enable_irq(dev_data->irq);
		
		writel(readl(dev_data->ioaddr + I2C_CONFIG) & ~I2C_CONFIG_IRQD,
			dev_data->ioaddr + I2C_CONFIG);	
	} else {
		printk(KERN_DEBUG "NS9XXX I2C: master module idle (STATUS 0x%lx)\n", (unsigned long)status);
	}
}


/* timeout waiting for the controller to respond (taken from the STU300 driver) */
#define NS9XXX_TIMEOUT (msecs_to_jiffies(1000))
#define BUSY_RELEASE_ATTEMPTS 10

static int ns9xxx_wait_while_busy(struct ns9xxx_i2c *dev)
{
	/*
	 * Waits for the busy bit to go low by repeated polling.
	 */
	 
	unsigned long timeout;
	int i, status;

	status = readl(dev->ioaddr + I2C_STATUS);
	if ((status & I2C_STATUS_MCMDL) == 0)
		return 0;								// Module is free to receive commands
	
	// Module seems to be locked
	
	for (i = 0; i < BUSY_RELEASE_ATTEMPTS; i++) {
		timeout = jiffies + NS9XXX_TIMEOUT;

		while (!time_after(jiffies, timeout)) {
			/* Is not busy? */
			status = readl(dev->ioaddr + I2C_STATUS);
			if ((status & I2C_STATUS_MCMDL) == 0) {
				msleep(1);
				status = readl(dev->ioaddr + I2C_STATUS);
				printk(KERN_DEBUG "NS9XXX I2C: master module idle (STATUS 0x%lx)\n", (unsigned long)status);
				return 0;
			}
			msleep(1);
		}

		printk(KERN_WARNING "transaction timed out waiting for device to be free (not busy). Attempt: %d\n", i+1);
		
		ns9xxx_reinit_i2c(dev);
	}

	printk(KERN_ERR "giving up after %d attempts to reset the bus.\n",  BUSY_RELEASE_ATTEMPTS);

	return -ETIMEDOUT;	
}


static int ns9xxx_i2c_xfer(struct i2c_adapter *adap,
		struct i2c_msg msgs[], int num)
{
	struct ns9xxx_i2c *dev_data = (struct ns9xxx_i2c *)adap->algo_data;
	int len, i, ret = 0, retry = 10;
	unsigned long flags = 0;
	unsigned int cmd, reg;
	char *buf = NULL;

	dev_data->state = I2C_INT_OK;

	for (i = 0; i < num; i++) {
		if (dev_data->state == I2C_INT_RETRY) {
			ret = ns9xxx_i2c_send_cmd(dev_data, I2C_CMD_STOP);
			--retry;
			if (ret || !retry)
				return -EIO;
		}

		len = msgs[i].len;
		buf = msgs[i].buf;

		spin_lock_irqsave(&dev_data->lock, flags);
		dev_data->buf = buf;
		spin_unlock_irqrestore(&dev_data->lock, flags);

		if (msgs[i].len == 0) {
			/* send using use bitbang mode */
			ret = ns9xxx_i2c_bitbang(dev_data, &msgs[i]);
		} else {
			if (!(msgs[i].flags & I2C_M_NOSTART)) {
				/* set device address */
				reg = ((msgs[i].addr & I2C_MASTERADDR_ADDRMASK)
						<< I2C_MASTERADDR_ADDRSHIFT);

				if (msgs[i].flags & I2C_M_TEN)
					reg |= I2C_MASTERADDR_10BIT;
				else
					reg |= I2C_MASTERADDR_7BIT;

				spin_lock_irqsave(&dev_data->lock, flags);
				writel(reg, dev_data->ioaddr +
						I2C_MASTERADDR);
				spin_unlock_irqrestore(&dev_data->lock, flags);

				if (msgs[i].flags & I2C_M_RD)
					cmd = I2C_CMD_READ;
				else {
					cmd = I2C_CMD_WRITE | I2C_CMD_TXVAL;
					cmd |= *buf;
					len--;
					buf++;
				}

				ret = ns9xxx_i2c_send_cmd(dev_data, cmd);
				if (ret) {
					if (dev_data->state == I2C_INT_RETRY) {
						i = 0;
						continue;
					}
					break;
				}
			}

			if (msgs[i].flags & I2C_M_RD)
				ret = ns9xxx_i2c_read(dev_data, len);
			else
				ret = ns9xxx_i2c_write(dev_data, buf, len);
			if (ret) {
				if (dev_data->state == I2C_INT_RETRY) {
					i = 0;
					continue;
				}
				break;
			}
		}
	}

	if (ns9xxx_i2c_send_cmd(dev_data, I2C_CMD_STOP)) {
		printk(KERN_WARNING "NS9XXX I2C: interface seems to be stuck, trying to unlock (state %lx)\n", (unsigned long)dev_data->state);
		/* sometimes interface gets stucked
		 * try to fix this by send "start, nop, start" */
		ns9xxx_i2c_send_cmd(dev_data, I2C_CMD_NOP);
		if (ns9xxx_i2c_send_cmd(dev_data, I2C_CMD_STOP)) {
			printk(KERN_WARNING "NS9XXX I2C: interface still stuck, forcing bus-reset using GPIO\n");
			ns9xxx_i2c_reset_bitbang(dev_data);
		}
	}
	
	spin_lock_irqsave(&dev_data->lock, flags);
	dev_data->buf = NULL;
	spin_unlock_irqrestore(&dev_data->lock, flags);

	/* return ERROR or number of transmits */
	return ((ret < 0) ? ret : i);
}

static int ns9xxx_i2c_set_clock(struct ns9xxx_i2c *dev_data, unsigned int freq)
{
	u32 config;

	config = readl(dev_data->ioaddr + I2C_CONFIG) & ~I2C_CONFIG_CLREFMASK;

	switch (freq) {
	case I2C_NORMALSPEED:
		/* Set standard mode */
		config &= ~I2C_CONFIG_TMDE;
		/* Clear VSCD divider */
		config &= ~I2C_CONFIG_VSCD;
		/* Calculate CLKREF */
		config |= (((clk_get_rate(dev_data->clk) / (4 * freq)) - 4 -
			  scl_delay) / 2) &
			  I2C_CONFIG_CLREFMASK;
		break;
#ifndef CONFIG_MACH_CME9210JS
	case I2C_HIGHSPEED:
		/* Set fast mode */
		config |= I2C_CONFIG_TMDE;
		/* Clear VSCD divider */
		config &= ~I2C_CONFIG_VSCD;
		/* Calculate CLKREF */
		config |= (((clk_get_rate(dev_data->clk) / (4 * freq)) - 4 -
			  scl_delay) * 2 / 3) &
			  I2C_CONFIG_CLREFMASK;
		break;
#endif
	default:
		pr_warning(DRIVER_NAME ": wrong clock configuration,"
				" please use a frequency of 100KHz or 400KHz\n");
		return -EINVAL;
	}

	writel(config, dev_data->ioaddr + I2C_CONFIG);
	
	printk(KERN_INFO "NS9XXX I2C: bus frequency set to %u, CPU clock = %lu, scl_delay = %u, config -> 0x%lx\n", freq, (unsigned long)(clk_get_rate(dev_data->clk)), (unsigned int)scl_delay, (unsigned long)config);


	return 0;
}

static int __devinit ns9xxx_i2c_probe(struct platform_device *pdev)
{
	struct ns9xxx_i2c *dev_data;
	int ret;

	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data) {
		dev_dbg(&pdev->dev, "%s: err_alloc_dd\n", __func__);
		ret = -ENOMEM;
		goto err_alloc_dd;
	}
	platform_set_drvdata(pdev, dev_data);

	dev_data->pdata = pdev->dev.platform_data;
	if (!dev_data->pdata) {
		dev_dbg(&pdev->dev, "%s: err_pdata\n", __func__);
		ret = -ENOENT;
		goto err_pdata;
	}

	snprintf(dev_data->adap.name, ARRAY_SIZE(dev_data->adap.name),
			DRIVER_NAME);
	dev_data->adap.owner = THIS_MODULE;
	dev_data->adap.algo = &ns9xxx_i2c_algo;
	dev_data->adap.algo_data = dev_data;
	dev_data->adap.retries = 1;
	dev_data->adap.timeout = HZ / 10;
	dev_data->adap.class = I2C_CLASS_HWMON;
	dev_data->buf = NULL;

	spin_lock_init(&dev_data->lock);
	init_waitqueue_head(&dev_data->wait_q);

	dev_data->irq = platform_get_irq(pdev, 0);
	if (dev_data->irq <= 0) {
		dev_dbg(&pdev->dev, "%s: err_irq\n", __func__);
		ret = -ENOENT;
		goto err_irq;
	}

	dev_data->mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!dev_data->mem) {
		dev_dbg(&pdev->dev, "%s: err_mem\n", __func__);
		ret = -ENOENT;
		goto err_mem;
	}

	if (!request_mem_region(dev_data->mem->start,
				dev_data->mem->end - dev_data->mem->start + 1,
				DRIVER_NAME)) {
		dev_dbg(&pdev->dev, "%s: err_req_mem\n", __func__);
		ret = -EBUSY;
		goto err_req_mem;
	}

	dev_data->ioaddr = ioremap(dev_data->mem->start,
			dev_data->mem->end - dev_data->mem->start + 1);
	if (dev_data->ioaddr <= 0) {
		dev_dbg(&pdev->dev, "%s: err_map_mem\n", __func__);
		ret = -EBUSY;
		goto err_map_mem;
	} else
		dev_dbg(&pdev->dev, "mapped I2C interface to virtual address"
				"0x%x\n", (int)dev_data->ioaddr);

	if (gpio_request(dev_data->pdata->gpio_scl, DRIVER_NAME)) {
		dev_dbg(&pdev->dev, "%s: err_gpio_scl\n", __func__);
		ret = -EBUSY;
		goto err_gpio_scl;
	}

	if (gpio_request(dev_data->pdata->gpio_sda, DRIVER_NAME)) {
		dev_dbg(&pdev->dev, "%s: err_gpio_sda\n", __func__);
		ret = -EBUSY;
		goto err_gpio_sda;
	}

	dev_data->clk = clk_get(&pdev->dev, DRIVER_NAME);
	if (IS_ERR(dev_data->clk)) {
		dev_dbg(&pdev->dev, "%s: err_clk_get\n", __func__);
		ret = PTR_ERR(dev_data->clk);
		goto err_clk_get;
	}

	ret = clk_enable(dev_data->clk);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: err_clk_enable\n", __func__);
		goto err_clk_enable;
	}

	/* configure i2c interface */
	if (!dev_data->pdata->gpio_configuration_func) {
		dev_dbg(&pdev->dev, "%s: err_cfg_gpio\n", __func__);
		ret = -ENOENT;
		goto err_cfg_gpio;
	}
	dev_data->pdata->gpio_configuration_func();

	/* Initially, disable interrupt, set standard mode and
	 * set VSCD to zero.
	 * Set spike filter width to maximum value to workaround communication
	 * problems on the cc9p9360 module */
	writel(I2C_CONFIG_IRQD | (0xf << I2C_CONFIG_SFW_SHIFT),
	       dev_data->ioaddr + I2C_CONFIG);

	if (dev_data->pdata->speed)
		ret = ns9xxx_i2c_set_clock(dev_data, dev_data->pdata->speed);
	else
		ret = ns9xxx_i2c_set_clock(dev_data, I2C_NORMALSPEED);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: err_set_clk\n", __func__);
		goto err_set_clk;
	}

	ret = request_irq(dev_data->irq, ns9xxx_i2c_irq, 0,
			DRIVER_NAME, dev_data);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: err_req_irq\n", __func__);
		ret = -EBUSY;
		goto err_req_irq;
	}

	/* Enable I2C interrupt now */
	writel(readl(dev_data->ioaddr + I2C_CONFIG) & ~I2C_CONFIG_IRQD,
		dev_data->ioaddr + I2C_CONFIG);

	ret = i2c_add_numbered_adapter(&dev_data->adap);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "%s: err_add_adap\n", __func__);
		goto err_add_adap;
	}

	dev_info(&pdev->dev, "NS9XXX I2C adapter\n");

	return 0;

err_add_adap:
	free_irq(dev_data->irq, dev_data);
err_req_irq:
err_set_clk:
err_cfg_gpio:
	clk_disable(dev_data->clk);
err_clk_enable:
	clk_put(dev_data->clk);
err_clk_get:
	gpio_free(dev_data->pdata->gpio_sda);
err_gpio_sda:
	gpio_free(dev_data->pdata->gpio_scl);
err_gpio_scl:
	iounmap(dev_data->ioaddr);
err_map_mem:
	release_mem_region(dev_data->mem->start,
			dev_data->mem->end - dev_data->mem->start + 1);
err_req_mem:
err_mem:
err_irq:
err_pdata:
err_alloc_dd:
	kfree(dev_data);

	return ret;
}

static int __devexit ns9xxx_i2c_remove(struct platform_device *pdev)
{
	struct ns9xxx_i2c *dev_data = platform_get_drvdata(pdev);

	i2c_del_adapter(&dev_data->adap);

	free_irq(dev_data->irq, dev_data);

	clk_disable(dev_data->clk);
	clk_put(dev_data->clk);

	gpio_free(dev_data->pdata->gpio_sda);
	gpio_free(dev_data->pdata->gpio_scl);

	iounmap(dev_data->ioaddr);
	release_mem_region(dev_data->mem->start,
			dev_data->mem->end - dev_data->mem->start + 1);

	kfree(dev_data);

	return 0;
}

static struct platform_driver ns9xxx_i2c_driver = {
	.probe		= ns9xxx_i2c_probe,
	.remove		= __devexit_p(ns9xxx_i2c_remove),
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	}
};

static int __init ns9xxx_i2c_init(void)
{
	return platform_driver_register(&ns9xxx_i2c_driver);
}

static void __exit ns9xxx_i2c_exit(void)
{
	platform_driver_unregister(&ns9xxx_i2c_driver);
}

module_init(ns9xxx_i2c_init);
module_exit(ns9xxx_i2c_exit);

MODULE_AUTHOR("Matthias Ludwig");
MODULE_DESCRIPTION("Digi NS9xxx I2C Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
