/**
 * @file   gpio_port.c
 * @author Erlang Solutions Ltd
 * @brief  GPIO erlang interface
 * @description
 *
 * @section LICENSE
 * Copyright (C) 2013 Erlang Solutions Ltd.
 * Copyright (C) 2014 Frank Hunleth
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <err.h>

#include "erlcmd.h"

/*
 * GPIO handling definitions and prototypes
 */
enum gpio_state {
    GPIO_CLOSED,
    GPIO_OUTPUT,
    GPIO_INPUT,
    GPIO_INPUT_WITH_INTERRUPTS
};

struct gpio {
    enum gpio_state state;
    int fd;
    int pin_number;

    /* 1 if the pin was already exported */
    int already_exported;
};

int gpio_open(struct gpio *pin, unsigned int pin_number, const char *dir);
int gpio_release(struct gpio *pin);

/**
 * @brief write a string to a sysfs file
 * @return returns 0 on failure, >0 on success
 */
int sysfs_write_file(const char *pathname, const char *value)
{
    int fd = open(pathname, O_WRONLY);
    if (fd < 0) {
	warn("Error opening %s", pathname);
	return 0;
    }

    size_t count = strlen(value);
    ssize_t written = write(fd, value, count);
    close(fd);

    if (written != count) {
	warn("Error writing '%s' to %s", value, pathname);
	return 0;
    }

    return written;
}

// GPIO functions

/**
 * @brief       Initialize a GPIO structure
 *
 * @param	pin           The pin structure
 */
void gpio_init(struct gpio *pin)
{
    pin->state = GPIO_CLOSED;
    pin->fd = -1;
    pin->pin_number = -1;
    pin->already_exported = 0;
}

/**
 * @brief	Open and configure a GPIO
 *
 * @param	pin           The pin structure
 * @param	pin_number    The GPIO pin
 * @param       dir           Direction of pin (input or output)
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_open(struct gpio *pin, unsigned int pin_number, const char *dir)
{
    /* If not closed, then release whatever pin is currently open. */
    if (pin->state != GPIO_CLOSED)
	gpio_release(pin);

    /* Construct the gpio control file paths */
    char direction_path[64];
    sprintf(direction_path, "/sys/class/gpio/gpio%d/direction", pin_number);

    char value_path[64];
    sprintf(value_path, "/sys/class/gpio/gpio%d/value", pin_number);

    /* Check if the gpio has been exported already. */
    if (access(value_path, F_OK) == -1) {
	/* Nope. Export it. */
	char pinstr[64];
	sprintf(pinstr, "%d", pin_number);
	if (!sysfs_write_file("/sys/class/gpio/export", pinstr))
	    return -1;

	pin->already_exported = 0;
    } else
	pin->already_exported = 1;

    const char *dirstr;
    if (strcmp(dir, "input") == 0) {
	dirstr = "in";
	pin->state = GPIO_INPUT;
    } else if (strcmp(dir, "output") == 0) {
	dirstr = "out";
	pin->state = GPIO_OUTPUT;
    } else
	return -1;

    /* The direction file may not exist if the pin only works one way.
       It is ok if the direction file doesn't exist, but if it does
       exist, we must be able to write it.
    */
    if (access(direction_path, F_OK) != -1) {
	if (!sysfs_write_file(direction_path, dirstr))
	    return -1;
    }

    pin->pin_number = pin_number;

    /* Open the value file for quick access later */
    pin->fd = open(value_path, pin->state == GPIO_OUTPUT ? O_RDWR : O_RDONLY);
    if (pin->fd < 0) {
	gpio_release(pin);
	return -1;
    }

    return 1;
}

/**
 * @brief	Release a GPIO pin
 *
 * @param	pin           The pin structure
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_release(struct gpio *pin)
{
    if (pin->state == GPIO_CLOSED)
	return 1;

    /* Close down the value file */
    if (pin->fd != -1)
	close(pin->fd);
    pin->fd = -1;

    /* Unexport the pin if we exported it initially. */
    if (!pin->already_exported) {
	char pinstr[64];
	sprintf(pinstr, "%d", pin->pin_number);
	sysfs_write_file("/sys/class/gpio/unexport", pinstr);
    }

    pin->state = GPIO_CLOSED;
    pin->pin_number = -1;
    return 1;
}

/**
 * @brief	Set pin with the value "0" or "1"
 *
 * @param	pin           The pin structure
 * @param       value         Value to set (0 or 1)
 *
 * @return 	1 for success, -1 for failure
 */
int gpio_write(struct gpio *pin, unsigned int val)
{
    if (pin->state != GPIO_OUTPUT)
	return -1;

    char buf = val ? '1' : '0';
    ssize_t amount_written = pwrite(pin->fd, &buf, sizeof(buf), 0);
    if (amount_written < sizeof(buf))
	err(EXIT_FAILURE, "pwrite");

    return 1;
}

/**
* @brief	Read the value of the pin
*
* @param	pin            The GPIO pin
*
* @return 	The pin value if success, -1 for failure
*/
int gpio_read(struct gpio *pin)
{
    if (pin->state == GPIO_CLOSED)
	return -1;

    char buf;
    ssize_t amount_read = pread(pin->fd, &buf, sizeof(buf), 0);
    if (amount_read < sizeof(buf))
	err(EXIT_FAILURE, "pread");

    return buf == '1' ? 1 : 0;
}

/**
 * Set isr as the interrupt service routine (ISR) for the pin. Mode
 * should be one of the strings "rising", "falling" or "both" to
 * indicate which edge(s) the ISR is to be triggered on. The function
 * isr is called whenever the edge specified occurs, receiving as
 * argument the number of the pin which triggered the interrupt.
 *
 * @param   pin	Pin number to attach interrupt to
 * @param   mode	Interrupt mode
 *
 * @return  Returns 1 on success.
 */
int gpio_set_int(struct gpio *pin, const char *mode)
{
    char path[64];
    sprintf(path, "/sys/class/gpio/gpio%d/edge", pin->pin_number);
    if (!sysfs_write_file(path, mode))
	return -1;

    pin->state = GPIO_INPUT_WITH_INTERRUPTS;
    return 1;
}

/**
 * Called after poll() returns when the GPIO sysfs file indicates
 * a status change.
 *
 * @param pin which pin to check
 */
void gpio_process(struct gpio *pin)
{
    int value = gpio_read(pin);

    ETERM *resp;
    if (value)
	resp = erl_format("{gpio_interrupt, rising}");
    else
	resp = erl_format("{gpio_interrupt, falling}");

    erlcmd_send(resp);
    erl_free_term(resp);
}

/**
 * @brief Handle a request from Erlang
 */
void gpio_handle_request(ETERM *emsg, void *cookie)
{
    struct gpio *pin = (struct gpio *) cookie;

    ETERM *emsg_type = erl_element(1, emsg);
    if (emsg_type == NULL)
	errx(EXIT_FAILURE, "erl_element(emsg_type)");

    if (strcmp(ERL_ATOM_PTR(emsg_type), "init") == 0) {
	ETERM *arg1p = erl_element(2, emsg);
	ETERM *arg2p = erl_element(3, emsg);
	if (arg1p == NULL || arg2p == NULL)
	    errx(EXIT_FAILURE, "init: arg1p or arg2p was NULL");

	/* convert erlang terms to usable values */
	int pin_number = ERL_INT_VALUE(arg1p);

	ETERM *resp;
	if (gpio_open(pin, pin_number, ERL_ATOM_PTR(arg2p)))
	    resp = erl_format("ok");
	else
	    resp = erl_format("{error, gpio_init_fail}");

	erlcmd_send(resp);
	erl_free_term(arg1p);
	erl_free_term(arg2p);
	erl_free_term(resp);
    } else if (strcmp(ERL_ATOM_PTR(emsg_type), "cast") == 0) {
	ETERM *arg1p = erl_element(2, emsg);
	if (arg1p == NULL)
	    errx(EXIT_FAILURE, "cast: arg1p was NULL");

	if (strcmp(ERL_ATOM_PTR(arg1p), "release") == 0) {
	    gpio_release(pin);
	} else
	    errx(EXIT_FAILURE, "cast: bad command");

	erl_free_term(arg1p);
    } else if (strcmp(ERL_ATOM_PTR(emsg_type), "call") == 0) {
	ETERM *refp = erl_element(2, emsg);
        ETERM *tuplep = erl_element(3, emsg);
	if (refp == NULL || tuplep == NULL)
	    errx(EXIT_FAILURE, "call: refp or tuplep was NULL");

	ETERM *fnp = erl_element(1, tuplep);
	if (fnp == NULL)
	    errx(EXIT_FAILURE, "tuplep: fnp was NULL");

	ETERM *resp = 0;
	if (strcmp(ERL_ATOM_PTR(fnp), "write") == 0) {
	    ETERM *arg1p = erl_element(2, tuplep);
	    if (arg1p == NULL)
		errx(EXIT_FAILURE, "write: arg1p was NULL");

	    int value = ERL_INT_VALUE(arg1p);
	    if(gpio_write(pin, value))
		resp = erl_format("ok");
	    else
		resp = erl_format("{error, gpio_write_failed}");
	    erl_free_term(arg1p);
	} else if (strcmp(ERL_ATOM_PTR(fnp), "read") == 0) {
	    int value = gpio_read(pin);
	    if (value !=-1)
		resp = erl_format("~i", value);
	    else
		resp = erl_format("{error, gpio_read_failed}");
	} else if (strcmp(ERL_ATOM_PTR(fnp), "set_int") == 0) {
	    ETERM *arg1p = erl_element(2, tuplep);

	    if (gpio_set_int(pin, ERL_ATOM_PTR(arg1p)))
		resp = erl_format("ok");
	    else
		resp = erl_format("{error, gpio_set_int_failed}");
	    erl_free_term(arg1p);
	}

	ETERM *fullresp = erl_format("{port_reply,~w,~w}", refp, resp);
	erlcmd_send(fullresp);

	erl_free_term(fullresp);
	erl_free_term(resp);
	erl_free_term(fnp);
	erl_free_term(tuplep);
	erl_free_term(refp);
     } else {
	errx(EXIT_FAILURE, "unexpected element");
     }

     erl_free_term(emsg_type);
}

/**
 * @brief The main function.
 * It waits for data in the buffer and calls the driver.
 */
int main()
{
    struct gpio pin;
    struct erlcmd handler;

    gpio_init(&pin);
    erlcmd_init(&handler, gpio_handle_request, &pin);

    for (;;) {
	struct pollfd fdset[2];

	fdset[0].fd = STDIN_FILENO;
	fdset[0].events = POLLIN;
	fdset[0].revents = 0;

	fdset[1].fd = pin.fd;
	fdset[1].events = POLLPRI;
	fdset[1].revents = 0;

	/* Always fill out the fdset structure, but only have poll() monitor
	 * the sysfs file if interrupts are enabled.
	 */
	int rc = poll(fdset, pin.state == GPIO_INPUT_WITH_INTERRUPTS ? 2 : 1, -1);
	if (rc < 0) {
	    /* Retry if EINTR */
	    if (errno == EINTR)
		continue;

	    err(EXIT_FAILURE, "poll");
	}

	if (fdset[0].revents & (POLLIN | POLLHUP))
	    erlcmd_process(&handler);

	if (fdset[1].revents & POLLPRI)
	    gpio_process(&pin);
    }

    return 0;
}
