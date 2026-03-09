#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/string.h>

/* JHD162A LCD pin mapping to RPi4 BCM GPIO (from wiring diagram) */
#define LCD_RS	26	/* Register Select: GPIO26 (Pin 37) */
#define LCD_E	19	/* Enable:          GPIO19 (Pin 35) */
#define LCD_D4	13	/* Data bit 4:      GPIO13 (Pin 33) */
#define LCD_D5	6	/* Data bit 5:      GPIO6  (Pin 31) */
#define LCD_D6	5	/* Data bit 6:      GPIO5  (Pin 29) */
#define LCD_D7	20	/* Data bit 7:      GPIO20 (Pin 38) */
/* RW pin is tied to GND (write-only mode) */

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

static dev_t device_number;
static struct cdev lcd_cdev;
static struct class *class_lcd;
static struct device *device_lcd;

/* GPIO pin array for easy iteration */
static const int lcd_gpios[] = { LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7 };
static const char *lcd_gpio_labels[] = { "LCD_RS", "LCD_E", "LCD_D4", "LCD_D5", "LCD_D6", "LCD_D7" };
#define NUM_LCD_GPIOS	ARRAY_SIZE(lcd_gpios)

/*
 * Pulse the Enable pin to latch data/command into the LCD.
 * HD44780 requires E high for >= 450ns; we use 1us for margin.
 */
static void lcd_pulse_enable(void)
{
	gpio_set_value(LCD_E, 1);
	udelay(1);
	gpio_set_value(LCD_E, 0);
	udelay(100);	/* commands need > 37us to execute */
}

/*
 * Send a 4-bit nibble to the LCD on D4-D7.
 */
static void lcd_write_nibble(uint8_t nibble)
{
	gpio_set_value(LCD_D4, (nibble >> 0) & 1);
	gpio_set_value(LCD_D5, (nibble >> 1) & 1);
	gpio_set_value(LCD_D6, (nibble >> 2) & 1);
	gpio_set_value(LCD_D7, (nibble >> 3) & 1);
	lcd_pulse_enable();
}

/*
 * Send a full byte to the LCD in 4-bit mode (high nibble first).
 */
static void lcd_write_byte(uint8_t val, int rs)
{
	gpio_set_value(LCD_RS, rs);	/* 0 = command, 1 = data */
	lcd_write_nibble(val >> 4);	/* High nibble first */
	lcd_write_nibble(val & 0x0F);	/* Low nibble second */
}

static void lcd_send_command(uint8_t cmd)
{
	lcd_write_byte(cmd, 0);
	if (cmd == LCD_CMD_CLEAR || cmd == LCD_CMD_HOME)
		msleep(2);	/* Clear and Home need ~1.52ms */
}

static void lcd_send_data(uint8_t data)
{
	lcd_write_byte(data, 1);
}

/*
 * Initialize the HD44780 in 4-bit mode.
 * Follows the datasheet power-on initialization sequence.
 */
static void lcd_init_hw(void)
{
	/* Wait > 40ms after power on (LCD internal reset) */
	msleep(50);

	gpio_set_value(LCD_RS, 0);
	gpio_set_value(LCD_E, 0);

	/*
	 * Special initialization sequence for 4-bit mode.
	 * Must send 0x03 three times, then 0x02 to switch to 4-bit.
	 */
	lcd_write_nibble(0x03);
	msleep(5);		/* Wait > 4.1ms */
	lcd_write_nibble(0x03);
	udelay(150);		/* Wait > 100us */
	lcd_write_nibble(0x03);
	udelay(150);
	lcd_write_nibble(0x02);	/* Switch to 4-bit mode */
	udelay(150);

	/* Now in 4-bit mode — configure display */
	lcd_send_command(LCD_CMD_FUNC_SET_4BIT);	/* Function set: 4-bit, 2 lines, 5x8 */
	lcd_send_command(LCD_CMD_DISPLAY_ON);		/* Display ON, cursor OFF */
	lcd_send_command(LCD_CMD_CLEAR);		/* Clear display */
	lcd_send_command(LCD_CMD_ENTRY_MODE);		/* Entry mode: increment, no shift */
}

/*
 * Display a string on a given row (0 or 1).
 * Truncates to LCD_COLS characters.
 */
static void lcd_print_line(const char *text, int row)
{
	uint8_t addr;
	int i, len;

	addr = (row == 0) ? 0x00 : LCD_LINE2_ADDR;
	lcd_send_command(LCD_CMD_SET_DDRAM | addr);

	len = strlen(text);
	if (len > LCD_COLS)
		len = LCD_COLS;

	for (i = 0; i < len; i++)
		lcd_send_data(text[i]);

	/* Pad remainder with spaces */
	for (; i < LCD_COLS; i++)
		lcd_send_data(' ');
}

/* ============ Character Device File Operations ============ */

static int lcd_open(struct inode *inode, struct file *filp)
{
	pr_info("lcd: device opened\n");
	return 0;
}

static int lcd_release(struct inode *inode, struct file *filp)
{
	pr_info("lcd: device closed\n");
	return 0;
}

/*
 * Write handler: data written to /dev/lcd_jhd162a is displayed on the LCD.
 * First 16 bytes go to line 1, next 16 bytes go to line 2.
 * A newline character ('\n') also advances to line 2.
 */
static ssize_t lcd_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *f_pos)
{
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

	/* Strip trailing newline if it's the very last character */
	if (to_copy > 0 && kbuf[to_copy - 1] == '\n')
		kbuf[to_copy - 1] = '\0';

	/* Check for embedded newline to split into two lines */
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

	lcd_send_command(LCD_CMD_CLEAR);
	lcd_print_line(line1, 0);
	lcd_print_line(line2, 1);

	pr_info("lcd: displayed \"%s\" / \"%s\"\n", line1, line2);

	*f_pos += count;
	return count;
}

/*
 * Read handler: returns what's currently on the LCD (not very useful
 * for real hardware, but keeps the char device interface complete).
 */
static ssize_t lcd_read(struct file *filp, char __user *buf,
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
	.write   = lcd_write,
	.read    = lcd_read,
};

static char *lcd_devnode(struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;
}

/* ============ GPIO Setup / Teardown ============ */

static int lcd_gpio_setup(void)
{
	int i, ret;

	for (i = 0; i < NUM_LCD_GPIOS; i++) {
		ret = gpio_request(lcd_gpios[i], lcd_gpio_labels[i]);
		if (ret) {
			pr_err("lcd: failed to request GPIO %d (%s): %d\n",
			       lcd_gpios[i], lcd_gpio_labels[i], ret);
			goto err_free;
		}
		ret = gpio_direction_output(lcd_gpios[i], 0);
		if (ret) {
			pr_err("lcd: failed to set GPIO %d as output: %d\n",
			       lcd_gpios[i], ret);
			gpio_free(lcd_gpios[i]);
			goto err_free;
		}
	}
	return 0;

err_free:
	while (--i >= 0)
		gpio_free(lcd_gpios[i]);
	return ret;
}

static void lcd_gpio_teardown(void)
{
	int i;

	for (i = 0; i < NUM_LCD_GPIOS; i++) {
		gpio_set_value(lcd_gpios[i], 0);
		gpio_free(lcd_gpios[i]);
	}
}

/* ============ Module Init / Exit ============ */

static int __init lcd_driver_init(void)
{
	int ret;

	/* 1. Request and configure GPIOs */
	ret = lcd_gpio_setup();
	if (ret)
		return ret;

	/* 2. Initialize the LCD hardware */
	lcd_init_hw();

	/* 3. Display "AlphaStar" on the LCD */
	lcd_print_line("AlphaStar", 0);
	pr_info("lcd: displayed 'AlphaStar' on JHD162A\n");

	/* 4. Register char device */
	ret = alloc_chrdev_region(&device_number, 0, 1, "lcd_jhd162a");
	if (ret) {
		pr_err("lcd: failed to allocate chrdev region: %d\n", ret);
		goto err_gpio;
	}
	pr_info("lcd: device number %d:%d\n", MAJOR(device_number), MINOR(device_number));

	cdev_init(&lcd_cdev, &lcd_fops);
	lcd_cdev.owner = THIS_MODULE;
	ret = cdev_add(&lcd_cdev, device_number, 1);
	if (ret) {
		pr_err("lcd: cdev_add failed: %d\n", ret);
		goto err_unreg;
	}

	class_lcd = class_create(THIS_MODULE, "lcd_class");
	if (IS_ERR(class_lcd)) {
		ret = PTR_ERR(class_lcd);
		pr_err("lcd: class_create failed: %d\n", ret);
		goto err_cdev;
	}
	class_lcd->devnode = lcd_devnode;

	device_lcd = device_create(class_lcd, NULL, device_number, NULL, "lcd_jhd162a");
	if (IS_ERR(device_lcd)) {
		ret = PTR_ERR(device_lcd);
		pr_err("lcd: device_create failed: %d\n", ret);
		goto err_class;
	}

	pr_info("lcd: JHD162A LCD driver loaded (/dev/lcd_jhd162a)\n");
	return 0;

err_class:
	class_destroy(class_lcd);
err_cdev:
	cdev_del(&lcd_cdev);
err_unreg:
	unregister_chrdev_region(device_number, 1);
err_gpio:
	lcd_gpio_teardown();
	return ret;
}

static void __exit lcd_driver_exit(void)
{
	/* Clear the LCD before unloading */
	lcd_send_command(LCD_CMD_CLEAR);

	device_destroy(class_lcd, device_number);
	class_destroy(class_lcd);
	cdev_del(&lcd_cdev);
	unregister_chrdev_region(device_number, 1);
	lcd_gpio_teardown();

	pr_info("lcd: JHD162A LCD driver unloaded\n");
}

module_init(lcd_driver_init);
module_exit(lcd_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("alphastar");
MODULE_DESCRIPTION("JHD162A 16x2 LCD character device driver for Raspberry Pi 4");
