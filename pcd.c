#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>

/* HD44780 commands */
#define LCD_CMD_CLEAR		0x01
#define LCD_CMD_HOME		0x02
#define LCD_CMD_ENTRY_MODE	0x06	/* Increment cursor, no shift */
#define LCD_CMD_DISPLAY_ON	0x0C	/* Display ON, cursor OFF, blink OFF */
#define LCD_CMD_FUNC_SET_4BIT	0x28	/* 4-bit, 2 lines, 5x8 font */
#define LCD_CMD_SET_DDRAM	0x80	/* Set DDRAM address (OR with address) */
#define LCD_LINE2_ADDR		0x40	/* Start address of line 2 */

#define LCD_COLS		16
#define LCD_ROWS		2
#define LCD_BUF_SIZE		(LCD_COLS * LCD_ROWS)

/* Per-device private data */
struct lcd_dev {
	struct gpio_desc *rs_gpio;
	struct gpio_desc *en_gpio;
	struct gpio_desc *d4_gpio;
	struct gpio_desc *d5_gpio;
	struct gpio_desc *d6_gpio;
	struct gpio_desc *d7_gpio;

	dev_t devnum;
	struct cdev cdev;
	struct class *class;
	struct device *device;
};

/* There is only one LCD instance; store a reference for fops */
static struct lcd_dev *lcd_instance;

/* ============ Low-Level LCD Functions ============ */

static void lcd_pulse_enable(struct lcd_dev *lcd)
{
	gpiod_set_value(lcd->en_gpio, 1);
	udelay(1);		/* HD44780 needs E high >= 450ns */
	gpiod_set_value(lcd->en_gpio, 0);
	udelay(100);		/* Command execution time > 37us */
}

static void lcd_write_nibble(struct lcd_dev *lcd, uint8_t nibble)
{
	gpiod_set_value(lcd->d4_gpio, (nibble >> 0) & 1);
	gpiod_set_value(lcd->d5_gpio, (nibble >> 1) & 1);
	gpiod_set_value(lcd->d6_gpio, (nibble >> 2) & 1);
	gpiod_set_value(lcd->d7_gpio, (nibble >> 3) & 1);
	lcd_pulse_enable(lcd);
}

static void lcd_write_byte(struct lcd_dev *lcd, uint8_t val, int rs)
{
	gpiod_set_value(lcd->rs_gpio, rs);	/* 0=command, 1=data */
	lcd_write_nibble(lcd, val >> 4);	/* High nibble first */
	lcd_write_nibble(lcd, val & 0x0F);	/* Low nibble second */
}

static void lcd_send_command(struct lcd_dev *lcd, uint8_t cmd)
{
	lcd_write_byte(lcd, cmd, 0);
	if (cmd == LCD_CMD_CLEAR || cmd == LCD_CMD_HOME)
		msleep(2);	/* Clear/Home need ~1.52ms */
}

static void lcd_send_data(struct lcd_dev *lcd, uint8_t data)
{
	lcd_write_byte(lcd, data, 1);
}

/*
 * HD44780 4-bit mode initialization sequence (per datasheet).
 */
static void lcd_init_hw(struct lcd_dev *lcd)
{
	msleep(50);		/* Wait > 40ms after power-on */

	gpiod_set_value(lcd->rs_gpio, 0);
	gpiod_set_value(lcd->en_gpio, 0);

	/* Special init sequence: 0x03 three times, then 0x02 for 4-bit */
	lcd_write_nibble(lcd, 0x03);
	msleep(5);
	lcd_write_nibble(lcd, 0x03);
	udelay(150);
	lcd_write_nibble(lcd, 0x03);
	udelay(150);
	lcd_write_nibble(lcd, 0x02);	/* Switch to 4-bit mode */
	udelay(150);

	lcd_send_command(lcd, LCD_CMD_FUNC_SET_4BIT);
	lcd_send_command(lcd, LCD_CMD_DISPLAY_ON);
	lcd_send_command(lcd, LCD_CMD_CLEAR);
	lcd_send_command(lcd, LCD_CMD_ENTRY_MODE);
}

static void lcd_print_line(struct lcd_dev *lcd, const char *text, int row)
{
	uint8_t addr;
	int i, len;

	addr = (row == 0) ? 0x00 : LCD_LINE2_ADDR;
	lcd_send_command(lcd, LCD_CMD_SET_DDRAM | addr);

	len = strlen(text);
	if (len > LCD_COLS)
		len = LCD_COLS;

	for (i = 0; i < len; i++)
		lcd_send_data(lcd, text[i]);

	for (; i < LCD_COLS; i++)
		lcd_send_data(lcd, ' ');
}

/* ============ Character Device File Operations ============ */

static int lcd_open(struct inode *inode, struct file *filp)
{
	filp->private_data = lcd_instance;
	pr_info("lcd: device opened\n");
	return 0;
}

static int lcd_release(struct inode *inode, struct file *filp)
{
	pr_info("lcd: device closed\n");
	return 0;
}

static ssize_t lcd_write_fop(struct file *filp, const char __user *buf,
			     size_t count, loff_t *f_pos)
{
	struct lcd_dev *lcd = filp->private_data;
	char kbuf[LCD_BUF_SIZE + 1];
	char line1[LCD_COLS + 1] = {0};
	char line2[LCD_COLS + 1] = {0};
	char *nl;
	size_t to_copy;

	if (count == 0)
		return 0;

	to_copy = min(count, (size_t)LCD_BUF_SIZE);
	if (copy_from_user(kbuf, buf, to_copy))
		return -EFAULT;
	kbuf[to_copy] = '\0';

	/* Strip trailing newline */
	if (to_copy > 0 && kbuf[to_copy - 1] == '\n')
		kbuf[to_copy - 1] = '\0';

	/* Split on embedded newline for two-line display */
	nl = strchr(kbuf, '\n');
	if (nl) {
		*nl = '\0';
		strncpy(line1, kbuf, LCD_COLS);
		strncpy(line2, nl + 1, LCD_COLS);
	} else {
		strncpy(line1, kbuf, LCD_COLS);
		if (strlen(kbuf) > LCD_COLS)
			strncpy(line2, kbuf + LCD_COLS, LCD_COLS);
	}

	lcd_send_command(lcd, LCD_CMD_CLEAR);
	lcd_print_line(lcd, line1, 0);
	lcd_print_line(lcd, line2, 1);

	pr_info("lcd: displayed \"%s\" / \"%s\"\n", line1, line2);

	*f_pos += count;
	return count;
}

static ssize_t lcd_read_fop(struct file *filp, char __user *buf,
			    size_t count, loff_t *f_pos)
{
	const char msg[] = "AlphaStar\n";
	size_t len = sizeof(msg) - 1;

	if (*f_pos >= len)
		return 0;
	if (count > len - *f_pos)
		count = len - *f_pos;
	if (copy_to_user(buf, msg + *f_pos, count))
		return -EFAULT;

	*f_pos += count;
	return count;
}

static struct file_operations lcd_fops = {
	.owner   = THIS_MODULE,
	.open    = lcd_open,
	.release = lcd_release,
	.write   = lcd_write_fop,
	.read    = lcd_read_fop,
};

static char *lcd_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

/* ============ Platform Driver Probe / Remove ============ */

static int lcd_probe(struct platform_device *pdev)
{
	struct lcd_dev *lcd;
	int ret;

	lcd = devm_kzalloc(&pdev->dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	/* Acquire GPIO descriptors from the device tree */
	lcd->rs_gpio = devm_gpiod_get(&pdev->dev, "rs", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->rs_gpio)) {
		dev_err(&pdev->dev, "failed to get RS gpio\n");
		return PTR_ERR(lcd->rs_gpio);
	}

	lcd->en_gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->en_gpio)) {
		dev_err(&pdev->dev, "failed to get Enable gpio\n");
		return PTR_ERR(lcd->en_gpio);
	}

	lcd->d4_gpio = devm_gpiod_get(&pdev->dev, "d4", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->d4_gpio)) {
		dev_err(&pdev->dev, "failed to get D4 gpio\n");
		return PTR_ERR(lcd->d4_gpio);
	}

	lcd->d5_gpio = devm_gpiod_get(&pdev->dev, "d5", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->d5_gpio)) {
		dev_err(&pdev->dev, "failed to get D5 gpio\n");
		return PTR_ERR(lcd->d5_gpio);
	}

	lcd->d6_gpio = devm_gpiod_get(&pdev->dev, "d6", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->d6_gpio)) {
		dev_err(&pdev->dev, "failed to get D6 gpio\n");
		return PTR_ERR(lcd->d6_gpio);
	}

	lcd->d7_gpio = devm_gpiod_get(&pdev->dev, "d7", GPIOD_OUT_LOW);
	if (IS_ERR(lcd->d7_gpio)) {
		dev_err(&pdev->dev, "failed to get D7 gpio\n");
		return PTR_ERR(lcd->d7_gpio);
	}

	/* Initialize the LCD hardware and display "AlphaStar" */
	lcd_init_hw(lcd);
	lcd_print_line(lcd, "AlphaStar", 0);
	dev_info(&pdev->dev, "displayed 'AlphaStar' on JHD162A\n");

	/* Register character device */
	ret = alloc_chrdev_region(&lcd->devnum, 0, 1, "lcd_jhd162a");
	if (ret) {
		dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&lcd->cdev, &lcd_fops);
	lcd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&lcd->cdev, lcd->devnum, 1);
	if (ret) {
		dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
		goto err_unreg;
	}

	lcd->class = class_create(THIS_MODULE, "lcd_class");
	if (IS_ERR(lcd->class)) {
		ret = PTR_ERR(lcd->class);
		dev_err(&pdev->dev, "class_create failed: %d\n", ret);
		goto err_cdev;
	}
	lcd->class->devnode = lcd_devnode;

	lcd->device = device_create(lcd->class, &pdev->dev, lcd->devnum,
				    NULL, "lcd_jhd162a");
	if (IS_ERR(lcd->device)) {
		ret = PTR_ERR(lcd->device);
		dev_err(&pdev->dev, "device_create failed: %d\n", ret);
		goto err_class;
	}

	/* Store for fops access and platform driver data */
	lcd_instance = lcd;
	platform_set_drvdata(pdev, lcd);

	dev_info(&pdev->dev, "JHD162A LCD driver probed (/dev/lcd_jhd162a)\n");
	return 0;

err_class:
	class_destroy(lcd->class);
err_cdev:
	cdev_del(&lcd->cdev);
err_unreg:
	unregister_chrdev_region(lcd->devnum, 1);
	return ret;
}

static int lcd_remove(struct platform_device *pdev)
{
	struct lcd_dev *lcd = platform_get_drvdata(pdev);

	/* Clear the LCD before removing */
	lcd_send_command(lcd, LCD_CMD_CLEAR);

	device_destroy(lcd->class, lcd->devnum);
	class_destroy(lcd->class);
	cdev_del(&lcd->cdev);
	unregister_chrdev_region(lcd->devnum, 1);

	lcd_instance = NULL;

	dev_info(&pdev->dev, "JHD162A LCD driver removed\n");
	return 0;
}

/* Device Tree match table — must match "compatible" in the overlay */
static const struct of_device_id lcd_of_match[] = {
	{ .compatible = "alphastar,lcd-jhd162a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lcd_of_match);

static struct platform_driver lcd_platform_driver = {
	.probe  = lcd_probe,
	.remove = lcd_remove,
	.driver = {
		.name           = "lcd_jhd162a",
		.of_match_table = lcd_of_match,
		.owner          = THIS_MODULE,
	},
};

module_platform_driver(lcd_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alphastar");
MODULE_DESCRIPTION("JHD162A 16x2 LCD platform driver for Raspberry Pi 4");
