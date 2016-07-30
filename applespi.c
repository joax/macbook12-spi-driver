#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>

#include <linux/input.h>
#include <linux/input-polldev.h>

#define APPLESPI_PACKET_SIZE    256

/* polling interval in ms */
#define POLL_INTERVAL           20

#define PACKET_KEYBOARD         288
#define PACKET_TOUCHPAD         544
#define PACKET_NOTHING          53312

struct keyboard_protocol {
	u16		packet_type;
	u8		unknown1[9];
	u8		counter;
	u8		unknown2[5];
	u8		modifiers;
	u8		unknown3;
	u8		keys_pressed[6];
	u8		fn_pressed;
	u16		crc_16;
	u8		unused[228];
};

struct touchpad_protocol {
	u16		packet_type;
	u8		unknown1[4];
	u8		number_of_fingers;
	u8		unknown2[4];
	u8		counter;
	u8		unknown3[2];
	u8		number_of_fingers2;
	u8		unknown[2];
	u8		clicked;
	u8		rel_x;
	u8		rel_y;
	u8		unused[236];
};

struct applespi_data {
	struct spi_device		*spi;
	struct input_polled_dev *poll_dev;

	u8						*tx_buffer;
	u8						*rx_buffer;

	struct mutex 			mutex;

	u8						last_keys_pressed[6];
};

static const unsigned char applespi_scancodes[] = {
	0,  0,  0,  0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

static irqreturn_t applespi_irq_handler(int irq, void *dev_id)
{
   // the actions that the interrupt should perform
   //pr_info("GOT INTERRUPT");
   return IRQ_HANDLED;
}

static ssize_t
applespi_sync(struct applespi_data *applespi, struct spi_message *message)
{
	struct spi_device *spi;
	int status;

	spi = applespi->spi;

	status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}

static inline ssize_t
applespi_sync_write_and_response(struct applespi_data *applespi)
{

	struct spi_transfer     t1 = {
		.tx_buf         = applespi->tx_buffer,
		.len            = APPLESPI_PACKET_SIZE,
		.cs_change      = 1,
		.speed_hz   = 400000
	};
	struct spi_transfer     t2 = {
		.rx_buf         = applespi->rx_buffer,
		.len            = 4,
		.cs_change      = 1,
		.speed_hz   = 400000
	};
	struct spi_transfer     t3 = {
		.rx_buf         = applespi->rx_buffer,
		.len            = APPLESPI_PACKET_SIZE,
		.speed_hz   = 400000
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t1, &m);
	spi_message_add_tail(&t2, &m);
	spi_message_add_tail(&t3, &m);
	return applespi_sync(applespi, &m);
}

static inline ssize_t
applespi_sync_read(struct applespi_data *applespi)
{

	struct spi_transfer     t = {
		.rx_buf         = applespi->rx_buffer,
		.len            = APPLESPI_PACKET_SIZE,
		.speed_hz   = 400000
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return applespi_sync(applespi, &m);
}


static void applespi_open(struct input_polled_dev *dev)
{
	pr_info("opened");
}

static void applespi_close(struct input_polled_dev *dev)
{
	pr_info("closed");
}

static void
applespi_got_data(struct applespi_data *applespi)
{
	struct keyboard_protocol keyboard_protocol;
	int i, j;
	bool still_pressed;

	memcpy(&keyboard_protocol, applespi->rx_buffer, APPLESPI_PACKET_SIZE);

	if (keyboard_protocol.packet_type == PACKET_NOTHING) {
		return;
	} else if (keyboard_protocol.packet_type == PACKET_KEYBOARD) {
//		pr_info("---");
//		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
		for (i=0; i<6; i++) {
			still_pressed = false;
			for (j=0; j<6; j++) {
				if (applespi->last_keys_pressed[i] == keyboard_protocol.keys_pressed[j]) {
					still_pressed = true;
					break;
				}
			}

			if (! still_pressed) {
				input_report_key(applespi->poll_dev->input, applespi_scancodes[applespi->last_keys_pressed[i]], 0);
			}
		}

		for (i=0; i<6; i++) {
			if (keyboard_protocol.keys_pressed[i] < sizeof(applespi_scancodes) && keyboard_protocol.keys_pressed[i] > 0) {
				input_report_key(applespi->poll_dev->input, applespi_scancodes[keyboard_protocol.keys_pressed[i]], 1);
			}
		}

		// Check control keys
		for (i=0; i<8; i++) {
			if (test_bit(i, (long unsigned int *)&keyboard_protocol.modifiers)) {
				input_report_key(applespi->poll_dev->input, applespi_controlcodes[i], 1);
			} else {
				input_report_key(applespi->poll_dev->input, applespi_controlcodes[i], 0);
			}
		}

		input_sync(applespi->poll_dev->input);
		memcpy(&applespi->last_keys_pressed, keyboard_protocol.keys_pressed, sizeof(applespi->last_keys_pressed));
	} else if (keyboard_protocol.packet_type == PACKET_TOUCHPAD) {
		pr_info("--- %d", keyboard_protocol.packet_type);
		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);

		input_report_rel(applespi->poll_dev->input, REL_X, ((struct touchpad_protocol*)&keyboard_protocol)->rel_x);
		input_report_rel(applespi->poll_dev->input, REL_Y, ((struct touchpad_protocol*)&keyboard_protocol)->rel_y);
		input_sync(applespi->poll_dev->input);
	} else {
//		pr_info("--- %d", keyboard_protocol.packet_type);
//		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
	}

}

static void applespi_poll(struct input_polled_dev *dev)
{
	struct applespi_data *applespi = dev->private;

//	pr_info("read: %ld", applespi_sync_read(applespi));
//	print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);

	mutex_lock(&applespi->mutex);
	applespi_sync_read(applespi);
	applespi_got_data(applespi);
	mutex_unlock(&applespi->mutex);

	// THIS LEAKS MEMORY!
	// t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL);
	// m = kzalloc(sizeof(struct spi_message), GFP_KERNEL);
	// t->rx_buf = applespi->rx_buffer;
	// t->len = APPLESPI_PACKET_SIZE;

	// // pr_info("polling");

	// spi_message_init(m);
	// spi_message_add_tail(t, m);
	// m->complete = applespi_got_data;
	// m->context = applespi;

	// spi_async(applespi->spi, m);

	// pr_info("polled");
}

static int applespi_probe(struct spi_device *spi)
{
	struct applespi_data *applespi;

	int result, i;

	u8 *wtfbufs[] = {
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x01\xD0\x00\x00\x04\x00\x00\x40\x89\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x00\x00\x00\x04\x00\x00\x60\x19\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x01\x00\x00\x04\x00\x00\x61\xC8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x02\x00\x00\x04\x00\x00\x61\xFB\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x03\x00\x00\x04\x00\x00\x60\x2A\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x04\x00\x00\x04\x00\x00\x61\x9D\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\x01\x00\x00\x00\x00\x0A\x00\x32\xBF\x00\x00\x08\x00\x00\x00\xCE\x66\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x2D\xFF",
"\x40\x01\x00\x00\x00\x00\x0A\x00\x32\x02\x00\x01\x1E\x00\x00\x00\x9A\xE5\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x2D\xFF",
"\x40\x01\x00\x00\x00\x00\x0E\x00\x52\x09\x00\x02\x04\x00\x04\x00\x09\x00\x00\x00\x0D\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x5F\x19",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x01\x00\x00\x04\x00\x00\x53\xC9\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x01\x00\x00\x04\x00\x00\x53\xC9\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
"\x40\x01\x00\x00\x00\x00\x0C\x00\x51\x01\x00\x03\x02\x00\x02\x00\x01\x00\x6D\xDE\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x66\x6A",
"\x40\x02\x00\x00\x00\x00\x0C\x00\x52\x02\x00\x00\x02\x00\x02\x00\x02\x01\x7B\x11\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x23\xAB",
"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x02\x00\x00\x04\x00\x00\x53\xFA\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62"};

	pr_info("module probe start");

	/* Allocate driver data */
	applespi = devm_kzalloc((struct device*)spi, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;


	/* Initialize the driver data */
	applespi->spi = spi;

	mutex_init(&applespi->mutex);

	/* Create our buffers */
	applespi->tx_buffer = kmalloc(APPLESPI_PACKET_SIZE, GFP_KERNEL);
	applespi->rx_buffer = kmalloc(APPLESPI_PACKET_SIZE, GFP_KERNEL);

	// Store the driver data
	spi_set_drvdata(spi, applespi);

	pr_info("acpi spi hz: %d", spi->max_speed_hz);
	pr_info("acpi spi bpw: %d", spi->bits_per_word);
	pr_info("acpi spi mode: %d", spi->mode);
	pr_info("acpi slave irq: %d", spi->irq);

	pr_info("read: %ld", applespi_sync_read(applespi));
	print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);

	for (i=0; i < 14; i++) {
		pr_info("wtfbuf %d", i);
		memcpy(applespi->tx_buffer, wtfbufs[i], 256);
		applespi_sync_write_and_response(applespi);
		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);
		mdelay(10);
	}

	for (i=0; i<10; i++) {
		pr_info("read: %ld", applespi_sync_read(applespi));
		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);
		mdelay(100);
	}

	result = request_irq(spi->irq, applespi_irq_handler, IRQF_SHARED, "applespi", applespi);
	pr_info("The interrupt request result is: %d\n", result);

	applespi->poll_dev = devm_input_allocate_polled_device((struct device*)applespi->spi);
	applespi->poll_dev->private = applespi;
	applespi->poll_dev->poll_interval = POLL_INTERVAL;
	applespi->poll_dev->open = applespi_open;
	applespi->poll_dev->close = applespi_close;
	applespi->poll_dev->poll = applespi_poll;

	applespi->poll_dev->input->name = "Apple SPI Keyboard / Mouse";
//	applespi->poll_dev->input->phys = (struct device*)applespi->spi;

	applespi->poll_dev->input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP) | BIT_MASK(EV_REL);
	applespi->poll_dev->input->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	applespi->poll_dev->input->ledbit[0] = BIT_MASK(LED_CAPSL);

	for (i = 0; i<sizeof(applespi_scancodes); i++) {
		if (applespi_scancodes[i]) {
			input_set_capability(applespi->poll_dev->input, EV_KEY, applespi_scancodes[i]);
		}
	}

	for (i = 0; i<sizeof(applespi_controlcodes); i++) {
		if (applespi_controlcodes[i]) {
			input_set_capability(applespi->poll_dev->input, EV_KEY, applespi_controlcodes[i]);
		}
	}

	result = input_register_polled_device(applespi->poll_dev);
	if (result) {
		pr_info("Unabled to register polled input device (%d)", result);
		return result;
	}

	pr_info("module probe done");

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);

	mutex_lock(&applespi->mutex);

	pr_info("freeing irq");
	free_irq(spi->irq, applespi);

	kfree(applespi->rx_buffer);
	applespi->rx_buffer = NULL;

	kfree(applespi->tx_buffer);
	applespi->tx_buffer = NULL;

	if (applespi->poll_dev) {
		pr_info("releasing polldev");
		input_unregister_polled_device(applespi->poll_dev);
		pr_info("freeing polldev");
		input_free_polled_device(applespi->poll_dev);
	}

	mutex_unlock(&applespi->mutex);

	pr_info("module exit");

	return 0;
}

static const struct acpi_device_id applespi_acpi_match[] = {
        { "APP000D", 0 },
        { },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static struct spi_driver applespi_driver = {
	.driver = {
		.name             = "applespi",
		.owner            = THIS_MODULE,

		.acpi_match_table = ACPI_PTR(applespi_acpi_match),
	},
	.probe		= applespi_probe,
	.remove     = applespi_remove,
};
module_spi_driver(applespi_driver)

MODULE_LICENSE("GPL");
