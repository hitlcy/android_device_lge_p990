/*
 * Copyright (C) 2011 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


//#define LOG_NDEBUG 0
#define LOG_TAG "lights"

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;

char const*const LCD_FILE
        = "/sys/class/backlight/aat2870-backlight/brightness";

char const*const BUTTON_BRIGHTNESS
        = "/sys/class/leds/star_led/brightness";

char const*const BUTTON_STATE
        = "/sys/class/leds/star_led/enable";

char const*const BUTTON_PULSE_INTERVAL
        = "/sys/class/leds/star_led/pulse_interval";

char const*const BUTTON_PULSE
        = "/sys/class/leds/star_led/pulse";

char const*const AUTO_BRIGHT_FILE
        = "/sys/class/backlight/aat2870-backlight/brightness_mode";

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);

}

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int
read_int(char const* path)
{
    int fd;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buffer[1];
        int amt = read(fd, buffer, 1);
        close(fd);
        return amt == -1 ? -errno : atoi(buffer);
    } else {
        return -errno;
    }
}

static int
is_lit(struct light_state_t const* state)
{
    return (read_int(BUTTON_BRIGHTNESS) > 0) ? 1 : 0;
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color; // & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int
set_light_buttons(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int on = is_lit(state);
    long value = rgb_to_brightness(state);

    ALOGV("Setting button brightness to %ld", value);

    pthread_mutex_lock(&g_lock);
    /* Change the scale to 0-32 */
    if (value >= 8) {
		err = write_int(BUTTON_BRIGHTNESS, (int)(value/8));
    } else if (value > 0 && value < 8) {
        err = write_int(BUTTON_BRIGHTNESS, 1);
    } else {
    	err = write_int(BUTTON_BRIGHTNESS, 0);
    }
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    int alc_is_on = 0;
    ALOGV("Setting display brightness to %d",brightness);

    pthread_mutex_lock(&g_lock);
    err = write_int(LCD_FILE, (brightness));
    pthread_mutex_unlock(&g_lock);

    return err;
}

static int
set_light_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
	int bri = rgb_to_brightness(state);

	int flashMode = state->flashMode;
	int flashOnMS = state->flashOnMS;
	int flashOffMS = state->flashOffMS;

    ALOGV("Calling notification light with flashMode '%d', fon '%d', foff '%d', bri '%d'\n", flashMode, flashOnMS, flashOffMS, bri);
    pthread_mutex_lock(&g_lock);
    if (! bri) {
        err = write_int(BUTTON_STATE, 0);
    } else {
    	if (flashMode == LIGHT_FLASH_TIMED || flashMode == LIGHT_FLASH_HARDWARE)
    	{
			if (flashOnMS && flashOffMS)
			{
				err = write_int(BUTTON_PULSE, flashOnMS);
				if (!err) err = write_int(BUTTON_PULSE_INTERVAL, flashOffMS);
			}
    	}

		if (!err) err = write_int(BUTTON_BRIGHTNESS, bri);
        if (!err) err = write_int(BUTTON_STATE, 1);
    }
    pthread_mutex_unlock(&g_lock);
    return err;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
        set_light = set_light_backlight;
    }
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name)) {
        set_light = set_light_buttons;
    }
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
        set_light = set_light_notifications;
    }
    else {
        return -EINVAL;
    }

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}


static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "LGE Star lights Module",
    .author = "CyanogenMod Project",
    .methods = &lights_module_methods,
};
