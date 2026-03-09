/*
 * JHD162A 16x2 LCD Character Device Driver (HD44780, 4-bit mode)
 *
 * Pure char device driver — no platform/device-tree dependency.
 *
 * 4-bit mode wiring (Raspberry Pi 4):
 *   RS  -> GPIO26 (Pin 37)
 *   E   -> GPIO19 (Pin 35)
 *   D4  -> GPIO13 (Pin 33)
 *   D5  -> GPIO6  (Pin 31)
 *   D6  -> GPIO5  (Pin 29)
 *   D7  -> GPIO20 (Pin 38)
 *   RW  -> GND    (Pin 9)   [write-only mode]
 *
 * Usage:
 *   sudo insmod pcd.ko
 *   echo "Hello World" > /dev/lcd_jhd162a
 *   cat /dev/lcd_jhd162a
 *   sudo rmmod pcd
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/string.h>

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

/* BCM GPIO numbers — match your wiring */
#define GPIO_RS		26
#define GPIO_EN		19
#define GPIO_D4		13
#define GPIO_D5		 6
#define GPIO_D6		 5
#define GPIO_D7		20

#define DEVICE_NAME	"lcd_jhd162a"

/* Per-device private data */
struct lcd_dev {
	unsigned int rs_gpio;
	unsigned int en_gpio;
	unsigned int d4_gpio;
	unsigned int d5_gpio;
	unsigned int d6_gpio;
	unsigned int d7_gpio;

	dev_t devnum;
	struct cdev cdev;
	struct class *class;
	struct device *device;
};

static struct lcd_dev lcd_instance;

/* ============ Low-Level LCD Functions ============ */

static void lcd_pulse_enable(struct lcd_dev *lcd)
{
	gpio_set_value(lcd->en_gpio, 1);
	udelay(1);		/* HD44780 needs E high >= 450ns */
	gpio_set_value(lcd->en_gpio, 0);
	udelay(100);		/* Command execution time > 37us */
}

static void lcd_write_nibble(struct lcd_dev *lcd, uint8_t nibble)
{
	gpio_set_value(lcd->d4_gpio, (nibble >> 0) & 1);
	gpio_set_value(lcd->d5_gpio, (nibble >> 1) & 1);
	gpio_set_value(lcd->d6_gpio, (nibble >> 2) & 1);
	gpio_set_value(lcd->d7_gpio, (nibble >> 3) & 1);
	lcd_pulse_enable(lcd);
}

static void lcd_write_byte(struct lcd_dev *lcd, uint8_t val, int rs)
{
	gpio_set_value(lcd->rs_gpio, rs);	/* 0=command, 1=data */
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

	gpio_set_value(lcd->rs_gpio, 0);
	gpio_set_value(lcd->en_gpio, 0);

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
	filp->private_data = &lcd_instance;
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

/* ============ GPIO Request / Free Helpers ============ */

struct gpio_pin {
	unsigned int gpio;
	const char *label;
};

static const struct gpio_pin lcd_gpios[] = {
	{ GPIO_RS, "lcd_rs" },
	{ GPIO_EN, "lcd_en" },
	{ GPIO_D4, "lcd_d4" },
	{ GPIO_D5, "lcd_d5" },
	{ GPIO_D6, "lcd_d6" },
	{ GPIO_D7, "lcd_d7" },
};

#define NUM_LCD_GPIOS	ARRAY_SIZE(lcd_gpios)

static void lcd_free_gpios(int count)
{
	int i;

	for (i = 0; i < count; i++)
		gpio_free(lcd_gpios[i].gpio);
}

static int lcd_request_gpios(void)
{
	int i, ret;

	for (i = 0; i < NUM_LCD_GPIOS; i++) {
		ret = gpio_request(lcd_gpios[i].gpio, lcd_gpios[i].label);
		if (ret) {
			pr_err("lcd: failed to request GPIO%u (%s): %d\n",
			       lcd_gpios[i].gpio, lcd_gpios[i].label, ret);
			lcd_free_gpios(i);
			return ret;
		}

		ret = gpio_direction_output(lcd_gpios[i].gpio, 0);
		if (ret) {
			pr_err("lcd: failed to set GPIO%u as output: %d\n",
			       lcd_gpios[i].gpio, ret);
			lcd_free_gpios(i + 1);
			return ret;
		}
	}

	return 0;
}

/* ============ Module Init / Exit ============ */

static int __init lcd_init(void)
{
	struct lcd_dev *lcd = &lcd_instance;
	int ret;

	/* Request and configure GPIO pins */
	ret = lcd_request_gpios();
	if (ret)
		return ret;

	lcd->rs_gpio = GPIO_RS;
	lcd->en_gpio = GPIO_EN;
	lcd->d4_gpio = GPIO_D4;
	lcd->d5_gpio = GPIO_D5;
	lcd->d6_gpio = GPIO_D6;
	lcd->d7_gpio = GPIO_D7;

	/* Initialize the LCD hardware and display "AlphaStar" */
	lcd_init_hw(lcd);
	lcd_print_line(lcd, "AlphaStar", 0);
	pr_info("lcd: displayed 'AlphaStar' on JHD162A\n");

	/* Allocate character device region */
	ret = alloc_chrdev_region(&lcd->devnum, 0, 1, DEVICE_NAME);
	if (ret) {
		pr_err("lcd: alloc_chrdev_region failed: %d\n", ret);
		goto err_gpio;
	}

	/* Initialize and add cdev */
	cdev_init(&lcd->cdev, &lcd_fops);
	lcd->cdev.owner = THIS_MODULE;
	ret = cdev_add(&lcd->cdev, lcd->devnum, 1);
	if (ret) {
		pr_err("lcd: cdev_add failed: %d\n", ret);
		goto err_unreg;
	}

	/* Create device class and device node */
	lcd->class = class_create(DEVICE_NAME);
	if (IS_ERR(lcd->class)) {
		ret = PTR_ERR(lcd->class);
		pr_err("lcd: class_create failed: %d\n", ret);
		goto err_cdev;
	}
	lcd->class->devnode = lcd_devnode;

	lcd->device = device_create(lcd->class, NULL, lcd->devnum,
				    NULL, DEVICE_NAME);
	if (IS_ERR(lcd->device)) {
		ret = PTR_ERR(lcd->device);
		pr_err("lcd: device_create failed: %d\n", ret);
		goto err_class;
	}

	pr_info("lcd: JHD162A char driver loaded (/dev/%s)\n", DEVICE_NAME);
	return 0;

err_class:
	class_destroy(lcd->class);
err_cdev:
	cdev_del(&lcd->cdev);
err_unreg:
	unregister_chrdev_region(lcd->devnum, 1);
err_gpio:
	lcd_free_gpios(NUM_LCD_GPIOS);
	return ret;
}

static void __exit lcd_exit(void)
{
	struct lcd_dev *lcd = &lcd_instance;

	/* Clear the LCD before removing */
	lcd_send_command(lcd, LCD_CMD_CLEAR);

	device_destroy(lcd->class, lcd->devnum);
	class_destroy(lcd->class);
	cdev_del(&lcd->cdev);
	unregister_chrdev_region(lcd->devnum, 1);
	lcd_free_gpios(NUM_LCD_GPIOS);

	pr_info("lcd: JHD162A char driver removed\n");
}

module_init(lcd_init);
module_exit(lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alphastar");
MODULE_DESCRIPTION("JHD162A 16x2 LCD char device driver for Raspberry Pi 4");
