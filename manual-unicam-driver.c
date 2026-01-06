// SPDX-License-Identifier: GPL-2.0-only
/*
 * Manual DMA Driver for BCM2711 Unicam and IMX219 Camera Sensor
 * Raspberry Pi 4B - High Performance Zero-Copy Image Capture
 * 
 * This driver bypasses videobuf2 for minimal latency and direct memory control
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/device.h>
#include "bcm2835-unicam-regs.h"
#include "imx219-regs.h"

#define DRIVER_NAME "manual-unicam"
#define DEVICE_NAME "unicam0"

/* DMA Buffer Configuration */
#define DMA_BUFFER_SIZE    (16 * 1024 * 1024)  /* 16MB continuous buffer */
#define FRAME_SIZE_640x480 (640 * 480 * 2)     /* RAW10 unpacked to 16-bit */
#define MAX_FRAMES         (DMA_BUFFER_SIZE / FRAME_SIZE_640x480)

/* Debug Macros */
#define unicam_dbg(dev, fmt, ...) \
	dev_dbg(dev, "%s: " fmt, __func__, ##__VA_ARGS__)

#define unicam_info(dev, fmt, ...) \
	dev_info(dev, "%s: " fmt, __func__, ##__VA_ARGS__)

#define unicam_err(dev, fmt, ...) \
	dev_err(dev, "%s: " fmt, __func__, ##__VA_ARGS__)

/* Driver Private Data Structure */
struct unicam_device {
	struct device *dev;
	struct platform_device *pdev;
	
	/* Hardware Resources */
	void __iomem *reg_base;
	int irq;
	struct clk *clock_lp;
	struct clk *clock_vpu;
	
	/* I2C Sensor Interface */
	struct i2c_client *sensor_client;
	
	/* DMA Memory Management */
	void *virt_addr;           /* Kernel virtual address */
	dma_addr_t bus_addr;       /* Bus address for DMA */
	size_t buf_size;
	
	/* Frame Tracking */
	atomic_t frame_count;
	u32 current_write_ptr;
	wait_queue_head_t frame_wait;
	spinlock_t lock;
	
	/* Device Node */
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	
	/* Status Flags */
	bool streaming;
	bool initialized;
};

/* Global device pointer for char device ops */
static struct unicam_device *g_unicam_dev;

/*
 * Low-level Register Access Functions
 */
static inline u32 unicam_reg_read(struct unicam_device *dev, u32 offset)
{
	return readl(dev->reg_base + offset);
}

static inline void unicam_reg_write(struct unicam_device *dev, u32 offset, u32 value)
{
	writel(value, dev->reg_base + offset);
}

/*
 * I2C Sensor Communication Functions
 */
static int imx219_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3];
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 3,
		.buf = buf,
	};
	
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;
	
	if (i2c_transfer(client->adapter, &msg, 1) != 1) {
		dev_err(&client->dev, "Failed to write reg 0x%04x\n", reg);
		return -EIO;
	}
	
	return 0;
}

static int imx219_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 buf[2];
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = val,
		}
	};
	
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	
	if (i2c_transfer(client->adapter, msgs, 2) != 2) {
		dev_err(&client->dev, "Failed to read reg 0x%04x\n", reg);
		return -EIO;
	}
	
	return 0;
}

/*
 * Configure IMX219 for Mode 7 (640x480 @ high FPS)
 */
static int imx219_configure_mode7(struct unicam_device *dev)
{
	struct i2c_client *client = dev->sensor_client;
	int i, ret;
	u8 model_id_h, model_id_l;
	
	unicam_info(dev->dev, "Configuring IMX219 sensor for Mode 7\n");
	
	/* Verify sensor model ID */
	ret = imx219_read_reg(client, IMX219_REG_MODEL_ID_H, &model_id_h);
	if (ret)
		return ret;
	
	ret = imx219_read_reg(client, IMX219_REG_MODEL_ID_L, &model_id_l);
	if (ret)
		return ret;
	
	if ((model_id_h << 8 | model_id_l) != IMX219_MODEL_ID) {
		unicam_err(dev->dev, "Invalid sensor model ID: 0x%02x%02x\n",
			   model_id_h, model_id_l);
		return -ENODEV;
	}
	
	unicam_info(dev->dev, "IMX219 sensor detected (ID: 0x%04x)\n",
		    model_id_h << 8 | model_id_l);
	
	/* Write all configuration registers */
	for (i = 0; i < IMX219_MODE7_REG_COUNT; i++) {
		ret = imx219_write_reg(client,
				       imx219_mode7_640x480[i].addr,
				       imx219_mode7_640x480[i].val);
		if (ret) {
			unicam_err(dev->dev, "Failed to write register %d\n", i);
			return ret;
		}
		
		/* Small delay for register settling */
		usleep_range(100, 200);
	}
	
	unicam_info(dev->dev, "IMX219 Mode 7 configuration complete\n");
	return 0;
}

/*
 * Stop IMX219 streaming
 */
static int imx219_stop_streaming(struct unicam_device *dev)
{
	return imx219_write_reg(dev->sensor_client,
				IMX219_REG_MODE_SELECT,
				IMX219_MODE_STANDBY);
}

/*
 * Initialize and Configure Unicam Hardware
 */
static int unicam_hw_init(struct unicam_device *dev)
{
	u32 val;
	
	unicam_info(dev->dev, "Initializing Unicam hardware\n");
	
	/* Step 1: Disable Unicam core */
	unicam_reg_write(dev, UNICAM_CTRL, 0);
	usleep_range(1000, 2000);
	
	/* Step 2: Configure analog settings for 2-lane operation */
	val = unicam_reg_read(dev, UNICAM_ANA);
	val &= ~UNICAM_AR;  /* Clear analog reset */
	val &= ~UNICAM_DDL; /* Not double data lane */
	unicam_reg_write(dev, UNICAM_ANA, val);
	
	/* Step 3: Configure clock lane */
	val = UNICAM_CLE | UNICAM_CLHSE | UNICAM_CLTRE;
	unicam_reg_write(dev, UNICAM_CLK, val);
	
	/* Step 4: Setup DMA addresses (CRITICAL: Use bus address) */
	unicam_reg_write(dev, UNICAM_IBSA0, (u32)dev->bus_addr);
	unicam_reg_write(dev, UNICAM_IBEA0, (u32)(dev->bus_addr + dev->buf_size));
	
	unicam_info(dev->dev, "DMA Buffer: bus_addr=0x%08x, size=%zu\n",
		    (u32)dev->bus_addr, dev->buf_size);
	
	/* Step 5: Configure data format (RAW10) */
	unicam_reg_write(dev, UNICAM_IDI0, UNICAM_DT_RAW10);
	
	/* Step 6: Configure image pipe (unpack RAW10 to 16-bit) */
	val = (UNICAM_PUM_UNPACK << UNICAM_PUM_SHIFT);
	unicam_reg_write(dev, UNICAM_IPIPE, val);
	
	/* Step 7: Enable frame end and frame start interrupts */
	val = UNICAM_FEIE | UNICAM_FSIE;
	unicam_reg_write(dev, UNICAM_ICTL, val);
	
	/* Step 8: Enable Unicam core */
	val = UNICAM_CPE;
	unicam_reg_write(dev, UNICAM_CTRL, val);
	
	unicam_info(dev->dev, "Unicam hardware initialization complete\n");
	
	return 0;
}

/*
 * Disable Unicam Hardware
 */
static void unicam_hw_disable(struct unicam_device *dev)
{
	/* Clear interrupts */
	unicam_reg_write(dev, UNICAM_ICTL, 0);
	
	/* Disable core */
	unicam_reg_write(dev, UNICAM_CTRL, 0);
	
	/* Disable clock lane */
	unicam_reg_write(dev, UNICAM_CLK, 0);
}

/*
 * Interrupt Service Routine
 */
static irqreturn_t unicam_isr(int irq, void *dev_id)
{
	struct unicam_device *dev = dev_id;
	u32 ista;
	unsigned long flags;
	
	/* Read interrupt status */
	ista = unicam_reg_read(dev, UNICAM_ISTA);
	
	if (!ista)
		return IRQ_NONE;
	
	/* Clear interrupts (write-1-to-clear) */
	unicam_reg_write(dev, UNICAM_ISTA, ista);
	
	spin_lock_irqsave(&dev->lock, flags);
	
	/* Frame End Interrupt */
	if (ista & UNICAM_FEI) {
		/* Read current write pointer */
		dev->current_write_ptr = unicam_reg_read(dev, UNICAM_IBWP);
		
		/* Increment frame counter */
		atomic_inc(&dev->frame_count);
		
		/* Wake up waiting processes */
		wake_up_interruptible(&dev->frame_wait);
	}
	
	/* Frame Start Interrupt */
	if (ista & UNICAM_FSI) {
		/* Frame capture started - could be used for timing */
	}
	
	/* Check for errors */
	if (ista & UNICAM_CRCE) {
		unicam_err(dev->dev, "CRC Error detected in frame\n");
	}
	
	spin_unlock_irqrestore(&dev->lock, flags);
	
	return IRQ_HANDLED;
}

/*
 * Character Device File Operations
 */
static int unicam_open(struct inode *inode, struct file *filp)
{
	struct unicam_device *dev = g_unicam_dev;
	
	if (!dev)
		return -ENODEV;
	
	filp->private_data = dev;
	
	unicam_info(dev->dev, "Device opened\n");
	
	return 0;
}

static int unicam_release(struct inode *inode, struct file *filp)
{
	struct unicam_device *dev = filp->private_data;
	
	unicam_info(dev->dev, "Device closed\n");
	
	return 0;
}

static ssize_t unicam_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct unicam_device *dev = filp->private_data;
	u32 frame_info[2];
	
	if (count < sizeof(frame_info))
		return -EINVAL;
	
	/* Return frame count and current write pointer */
	frame_info[0] = atomic_read(&dev->frame_count);
	frame_info[1] = dev->current_write_ptr;
	
	if (copy_to_user(buf, frame_info, sizeof(frame_info)))
		return -EFAULT;
	
	return sizeof(frame_info);
}

static int unicam_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct unicam_device *dev = filp->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret;
	
	if (size > dev->buf_size) {
		unicam_err(dev->dev, "mmap size too large: %lu > %zu\n",
			   size, dev->buf_size);
		return -EINVAL;
	}
	
	/* Map DMA buffer to user space */
	ret = dma_mmap_coherent(dev->dev, vma, dev->virt_addr,
				dev->bus_addr, size);
	
	if (ret) {
		unicam_err(dev->dev, "mmap failed: %d\n", ret);
		return ret;
	}
	
	unicam_info(dev->dev, "Mapped %lu bytes to user space\n", size);
	
	return 0;
}

static unsigned int unicam_poll(struct file *filp, poll_table *wait)
{
	struct unicam_device *dev = filp->private_data;
	
	poll_wait(filp, &dev->frame_wait, wait);
	
	/* Data is always available in circular buffer */
	return POLLIN | POLLRDNORM;
}

/* IOCTL Commands */
#define UNICAM_IOC_MAGIC 'U'
#define UNICAM_IOC_START_STREAM  _IO(UNICAM_IOC_MAGIC, 1)
#define UNICAM_IOC_STOP_STREAM   _IO(UNICAM_IOC_MAGIC, 2)
#define UNICAM_IOC_GET_BUFFER    _IOR(UNICAM_IOC_MAGIC, 3, unsigned long)

static long unicam_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct unicam_device *dev = filp->private_data;
	int ret = 0;
	
	switch (cmd) {
	case UNICAM_IOC_START_STREAM:
		if (dev->streaming) {
			unicam_info(dev->dev, "Already streaming\n");
			break;
		}
		
		/* Initialize hardware */
		ret = unicam_hw_init(dev);
		if (ret)
			break;
		
		/* Configure sensor */
		ret = imx219_configure_mode7(dev);
		if (ret) {
			unicam_hw_disable(dev);
			break;
		}
		
		dev->streaming = true;
		atomic_set(&dev->frame_count, 0);
		
		unicam_info(dev->dev, "Streaming started\n");
		break;
		
	case UNICAM_IOC_STOP_STREAM:
		if (!dev->streaming) {
			unicam_info(dev->dev, "Not streaming\n");
			break;
		}
		
		/* Stop sensor */
		imx219_stop_streaming(dev);
		
		/* Disable hardware */
		unicam_hw_disable(dev);
		
		dev->streaming = false;
		
		unicam_info(dev->dev, "Streaming stopped\n");
		break;
		
	case UNICAM_IOC_GET_BUFFER:
		if (copy_to_user((void __user *)arg, &dev->buf_size,
				 sizeof(dev->buf_size)))
			return -EFAULT;
		break;
		
	default:
		return -ENOTTY;
	}
	
	return ret;
}

static const struct file_operations unicam_fops = {
	.owner = THIS_MODULE,
	.open = unicam_open,
	.release = unicam_release,
	.read = unicam_read,
	.mmap = unicam_mmap,
	.poll = unicam_poll,
	.unlocked_ioctl = unicam_ioctl,
};

/*
 * Platform Driver Probe
 */
static int unicam_probe(struct platform_device *pdev)
{
	struct unicam_device *dev;
	struct resource *res;
	struct device_node *i2c_node;
	struct i2c_adapter *adapter;
	struct i2c_board_info board_info = {
		.type = "imx219",
		.addr = IMX219_I2C_ADDR,
	};
	int ret;
	
	dev_info(&pdev->dev, "Probing manual Unicam driver\n");
	
	/* Allocate device structure */
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	
	dev->dev = &pdev->dev;
	dev->pdev = pdev;
	platform_set_drvdata(pdev, dev);
	g_unicam_dev = dev;
	
	/* Initialize synchronization primitives */
	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->frame_wait);
	atomic_set(&dev->frame_count, 0);
	
	/* Get memory resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get memory resource\n");
		return -ENODEV;
	}
	
	/* Map registers */
	dev->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dev->reg_base)) {
		dev_err(&pdev->dev, "Failed to map registers\n");
		return PTR_ERR(dev->reg_base);
	}
	
	dev_info(&pdev->dev, "Registers mapped at %p (phys: 0x%08x)\n",
		 dev->reg_base, (u32)res->start);
	
	/* Get IRQ */
	dev->irq = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		dev_err(&pdev->dev, "Failed to get IRQ\n");
		return dev->irq;
	}
	
	/* Request IRQ */
	ret = devm_request_irq(&pdev->dev, dev->irq, unicam_isr,
			       IRQF_SHARED, DRIVER_NAME, dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			dev->irq, ret);
		return ret;
	}
	
	dev_info(&pdev->dev, "IRQ %d registered\n", dev->irq);
	
	/* Get clocks */
	dev->clock_lp = devm_clk_get(&pdev->dev, "lp");
	if (IS_ERR(dev->clock_lp)) {
		dev_err(&pdev->dev, "Failed to get LP clock\n");
		return PTR_ERR(dev->clock_lp);
	}
	
	dev->clock_vpu = devm_clk_get(&pdev->dev, "vpu");
	if (IS_ERR(dev->clock_vpu)) {
		dev_err(&pdev->dev, "Failed to get VPU clock\n");
		return PTR_ERR(dev->clock_vpu);
	}
	
	/* Enable clocks */
	ret = clk_prepare_enable(dev->clock_lp);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable LP clock\n");
		return ret;
	}
	
	ret = clk_prepare_enable(dev->clock_vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable VPU clock\n");
		clk_disable_unprepare(dev->clock_lp);
		return ret;
	}
	
	dev_info(&pdev->dev, "Clocks enabled (LP: %lu Hz, VPU: %lu Hz)\n",
		 clk_get_rate(dev->clock_lp),
		 clk_get_rate(dev->clock_vpu));
	
	/* Allocate DMA buffer */
	dev->buf_size = DMA_BUFFER_SIZE;
	dev->virt_addr = dma_alloc_coherent(&pdev->dev, dev->buf_size,
					    &dev->bus_addr, GFP_KERNEL);
	if (!dev->virt_addr) {
		dev_err(&pdev->dev, "Failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto err_clocks;
	}
	
	dev_info(&pdev->dev, "DMA buffer allocated: %zu bytes\n"
		 "  Virtual: %p\n"
		 "  Bus:     0x%08x\n",
		 dev->buf_size, dev->virt_addr, (u32)dev->bus_addr);
	
	/* Find I2C adapter for camera */
	i2c_node = of_parse_phandle(pdev->dev.of_node, "i2c-bus", 0);
	if (!i2c_node) {
		/* Try default I2C bus */
		adapter = i2c_get_adapter(10); /* I2C-10 is common for camera */
		if (!adapter)
			adapter = i2c_get_adapter(0);
	} else {
		adapter = of_find_i2c_adapter_by_node(i2c_node);
		of_node_put(i2c_node);
	}
	
	if (!adapter) {
		dev_err(&pdev->dev, "Failed to get I2C adapter\n");
		ret = -ENODEV;
		goto err_dma;
	}
	
	/* Create I2C client for sensor */
	dev->sensor_client = i2c_new_client_device(adapter, &board_info);
	i2c_put_adapter(adapter);
	
	if (IS_ERR(dev->sensor_client)) {
		dev_err(&pdev->dev, "Failed to create I2C client\n");
		ret = PTR_ERR(dev->sensor_client);
		goto err_dma;
	}
	
	dev_info(&pdev->dev, "I2C sensor client created (addr: 0x%02x)\n",
		 dev->sensor_client->addr);
	
	/* Create character device */
	ret = alloc_chrdev_region(&dev->devt, 0, 1, DEVICE_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to allocate char device region\n");
		goto err_i2c;
	}
	
	cdev_init(&dev->cdev, &unicam_fops);
	dev->cdev.owner = THIS_MODULE;
	
	ret = cdev_add(&dev->cdev, dev->devt, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add char device\n");
		goto err_chrdev;
	}
	
	/* Create device class */
	dev->class = class_create(DRIVER_NAME);
	if (IS_ERR(dev->class)) {
		dev_err(&pdev->dev, "Failed to create device class\n");
		ret = PTR_ERR(dev->class);
		goto err_cdev;
	}
	
	/* Create device node */
	if (!device_create(dev->class, &pdev->dev, dev->devt,
			   NULL, DEVICE_NAME)) {
		dev_err(&pdev->dev, "Failed to create device node\n");
		ret = -ENOMEM;
		goto err_class;
	}
	
	dev->initialized = true;
	
	dev_info(&pdev->dev, "Manual Unicam driver initialized successfully\n");
	dev_info(&pdev->dev, "Device node: /dev/%s\n", DEVICE_NAME);
	dev_info(&pdev->dev, "Max frames in buffer: %d\n", MAX_FRAMES);
	
	return 0;

err_class:
	class_destroy(dev->class);
err_cdev:
	cdev_del(&dev->cdev);
err_chrdev:
	unregister_chrdev_region(dev->devt, 1);
err_i2c:
	i2c_unregister_device(dev->sensor_client);
err_dma:
	dma_free_coherent(&pdev->dev, dev->buf_size,
			  dev->virt_addr, dev->bus_addr);
err_clocks:
	clk_disable_unprepare(dev->clock_vpu);
	clk_disable_unprepare(dev->clock_lp);
	
	return ret;
}

/*
 * Platform Driver Remove
 */
static void unicam_remove(struct platform_device *pdev)
{
	struct unicam_device *dev = platform_get_drvdata(pdev);
	
	dev_info(&pdev->dev, "Removing manual Unicam driver\n");
	
	/* Stop streaming if active */
	if (dev->streaming) {
		imx219_stop_streaming(dev);
		unicam_hw_disable(dev);
	}
	
	/* Cleanup device */
	device_destroy(dev->class, dev->devt);
	class_destroy(dev->class);
	cdev_del(&dev->cdev);
	unregister_chrdev_region(dev->devt, 1);
	
	/* Cleanup I2C */
	i2c_unregister_device(dev->sensor_client);
	
	/* Free DMA buffer */
	dma_free_coherent(&pdev->dev, dev->buf_size,
			  dev->virt_addr, dev->bus_addr);
	
	/* Disable clocks */
	clk_disable_unprepare(dev->clock_vpu);
	clk_disable_unprepare(dev->clock_lp);
	
	g_unicam_dev = NULL;
	
	dev_info(&pdev->dev, "Manual Unicam driver removed\n");
}

/*
 * Device Tree Matching
 */
static const struct of_device_id unicam_of_match[] = {
	{ .compatible = "vendor,manual-unicam" },
	{ }
};
MODULE_DEVICE_TABLE(of, unicam_of_match);

/*
 * Platform Driver Structure
 */
static struct platform_driver unicam_driver = {
	.probe = unicam_probe,
	.remove = unicam_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = unicam_of_match,
	},
};

module_platform_driver(unicam_driver);

MODULE_AUTHOR("Embedded Vision Systems");
MODULE_DESCRIPTION("Manual DMA Driver for BCM2711 Unicam and IMX219");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");