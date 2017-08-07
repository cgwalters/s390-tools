/*
 * ipl_tools - Linux for System z reipl and shutdown tools
 *
 * CCW device functions
 *
 * Copyright IBM Corp. 2008, 2017
 *
 * s390-tools is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipl_tools.h"

/*
 * Check if the specified device number is a valid device number
 * which can be found in the /sys/bus/ccw/drivers/dasd-eckd/
 * structure.
 *
 * This does not work when booting from tape.
 */
int ccw_is_device(const char *busid)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path),
		 "/sys/bus/ccw/drivers/dasd-eckd/%s", busid);
	if (access(path, R_OK) == 0)
		return 1;
	snprintf(path, sizeof(path),
		 "/sys/bus/ccw/drivers/virtio_ccw/%s", busid);
	if (access(path, R_OK) == 0)
		return 1;
	snprintf(path, sizeof(path), "/sys/bus/ccw/drivers/dasd-fba/%s", busid);
	if (access(path, R_OK) == 0)
		return 1;
	return 0;
}

/*
 * Return CCW Bus ID (old sysfs)
 */
static int ccw_busid_get_sysfs_old(const char *device, char *busid)
{
	char path[PATH_MAX];
	char buf[4096];
	int rc = 0;
	FILE *fh;

	snprintf(path, sizeof(path), "/sys/block/%s/uevent", device);
	fh = fopen(path, "r");
	if (fh == NULL)
		return -1;
	/*
	 * The uevent file contains an entry like this:
	 * PHYSDEVPATH=/devices/css0/0.0.206a/0.0.7e78
	 */
	while (fscanf(fh, "%s", buf) >= 0) {
		if (strstr(buf, "PHYSDEVPATH") != NULL) {
			strcpy(busid, strrchr(buf, '/') + 1);
			goto out_fclose;
		}
	}
	rc = -1;
out_fclose:
	fclose(fh);
	return rc;
}

/*
 * Return CCW Bus ID (new sysfs)
 */
static int ccw_busid_get_sysfs_new(const char *device, char *busid)
{
	char path[PATH_MAX], buf[4096];

	memset(buf, 0, sizeof(buf));
	snprintf(path, sizeof(path), "/sys/block/%s/device", device);
	if (realpath(path, buf) == NULL)
		return -1;

	/*
	 * The output has the following format:
	 * /sys/devices/css0/0.0.0119/0.0.3f19/block/dasda
	 * /sys/devices/css0/0.0.0000/0.0.0000/virtio0/block/vda
	 */
	if (sscanf(buf, "/sys/devices/css0/%*[0-9a-f.]/%[0-9a-f.]", busid) != 1)
		return -1;
	return 0;
}

/*
 * Return the device number for a device
 * dasda can be found in /sys/block/dasda/uevent or in a
 * symbolic link in the same directory. the first file only
 * contains the relevant information if we run on a kernel with
 * has the following kernel option enabled:
 * CONFIG_SYSFS_DEPRECATED
 *
 * This does not work when booting from tape
 */
void ccw_busid_get(const char *device, char *busid)
{
	if (ccw_busid_get_sysfs_old(device, busid) == 0)
		return;
	if (ccw_busid_get_sysfs_new(device, busid) == 0)
		return;
	ERR_EXIT("Could not lookup device number for \"%s\"", device);
}
