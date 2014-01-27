/**
 * @file   i2c_ei.c
 * @author Ivan Iacono <ivan.iacono@erlang-solutions.com> - Erlang Solutions Ltd
 * @brief  I2C erlang interface
 * @description
 *
 * @section LICENSE
 * Copyright (C) 2013 Erlang Solutions Ltd.
 **/

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "erlcmd.h"

struct i2c_info
{
    int fd;
    unsigned int addr;
};

/**
 * @brief	Initialises the devname I2C device
 *
 * @param	devname    The sysfs entry
 *
 * @return 	file descriptor if success, -1 if fails
 */
static void i2c_init(struct i2c_info *i2c)
{
    memset(i2c, 0, sizeof(*i2c));
}

static void i2c_open(struct i2c_info *i2c, char *devpath, unsigned int addr)
{
    // Fail hard on error. May need to be nicer if this makes the
    // Erlang side too hard to debug.
    i2c->fd = open(devpath, O_RDWR);
    if (i2c->fd < 0)
	err(EXIT_FAILURE, "open %s", devpath);

    if (ioctl(i2c->fd, I2C_SLAVE, addr) < 0)
	err(EXIT_FAILURE, "ioctl(I2C_SLAVE %d)", addr);

    i2c->addr = addr;
}

/**
 * @brief	I2C write operation
 *
 * @param	data	Data to write into the device
 * @param	len     Length of data
 *
 * @return 	1 for success, -1 for failure
 */
static int i2c_write(struct i2c_info *i2c, unsigned char *data, unsigned int len)
{
    if (write(i2c->fd, data, len) != len) {
        warn("I2C write (address: 0x%X) of %d bytes failed", i2c->addr, len);
	return -1;
    }

    return 1;
}

/**
 * @brief	I2C read operation
 *
 * @param	data	Pointer to the read buffer
 * @param	len	Length of data
 *
 * @return 	1 for success, -1 for failure
 */
static int i2c_read(struct i2c_info *i2c, char *data, unsigned int len)
{
    if (read(i2c->fd, data, len) != len) {
        warn("I2C read (address: 0x%X) of %d bytes failed", i2c->addr, len);
	return -1;
    }
    return 1;
}

static void i2c_handle_request(ETERM *emsg, void *cookie)
{
    struct i2c_info *i2c = (struct i2c_info *) cookie;

    ETERM *emsg_type = erl_element(1, emsg);
    if (emsg_type == NULL)
	errx(EXIT_FAILURE, "erl_element(emsg_type)");

    if (strcmp(ERL_ATOM_PTR(emsg_type), "i2c_write") == 0) {
	ETERM *edata = erl_element(2, emsg);

	// calls the i2c_write function and returns 1 if success or -1 if fails
        int res = i2c_write(i2c,
	                    ERL_BIN_PTR(edata),
			    ERL_BIN_SIZE(edata));
	ETERM *eresult = erl_mk_int(res);
	erlcmd_send(eresult);
	erl_free_term(eresult);
	erl_free_term(edata);
    } else if (strcmp(ERL_ATOM_PTR(emsg_type), "i2c_read") == 0) {
	ETERM *elen = erl_element(2, emsg);
	int len = ERL_INT_VALUE(elen);
	if (len > I2C_SMBUS_BLOCK_MAX)
	    errx(EXIT_FAILURE, "Can't get more than %d bytes at time: %d", I2C_SMBUS_BLOCK_MAX, len);

	char data[len];

	ETERM *eresult;

	// calls the i2c_read function and returns an erlang tuple with data or -1 if fails
	if (i2c_read(i2c, data, len) < 0) {
	    eresult = erl_mk_int(-1);
	} else {
	    eresult = erl_mk_binary(data, len);
	}
	erlcmd_send(eresult);
	erl_free_term(eresult);
    } else {
	errx(EXIT_FAILURE, "unexpected request %s", ERL_ATOM_PTR(emsg_type));
    }

    erl_free_term(emsg_type);
}

/**
 * @brief The main function.
 * It waits for data in the buffer and calls the driver.
 */
int main(int argc, char *argv[])
{
    if (argc != 3)
	errx(EXIT_FAILURE, "Must pass device path and device address as arguments");

    struct i2c_info i2c;
    i2c_init(&i2c);

    struct erlcmd handler;
    erlcmd_init(&handler, i2c_handle_request, &i2c);

    // Open the specified I2C device and
    i2c_open(&i2c, argv[1], strtoul(argv[2], 0, 0));

    for (;;) {
	// Loop forever and process requests from Erlang.
	erlcmd_process(&handler);
    }

    return 1;
}
