/*
 * zdev - Modify and display the persistent configuration of devices
 *
 * Copyright IBM Corp. 2016, 2017
 *
 * s390-tools is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attrib.h"
#include "ccw.h"
#include "dasd.h"
#include "device.h"
#include "devtype.h"
#include "misc.h"
#include "modprobe.h"
#include "module.h"
#include "path.h"
#include "setting.h"
#include "udev.h"

#define	DASD_MOD_NAME		"dasd_mod"
#define	DASD_ECKD_MOD_NAME	"dasd_eckd_mod"
#define	DASD_FBA_MOD_NAME	"dasd_fba_mod"
#define	DASD_DIAG_MOD_NAME	"dasd_diag_mod"

/* Subtype specific data. */
struct dasd_subtype_data {
	const char *ccwdrv;
	const char *module;
};

/* List of dependent modules. */
static const char *dasd_mods[] = {
	DASD_ECKD_MOD_NAME,
	DASD_FBA_MOD_NAME,
	DASD_DIAG_MOD_NAME,
	NULL,
};

/*
 * DASD type attributes.
 */

static struct attrib dasd_tattr_autodetect = {
	.name = "autodetect",
	.title = "Enable automatic DASD activation",
	.desc =
	"Control whether the DASD driver automatically enables all detected\n"
	"DASDs:\n"
	"  0: Only configured DASDs are enabled\n"
	"  1: All detected DASDs are automatically enabled\n",
	.defval = "0",
	.activerem = 1,
	.defunset = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_tattr_probeonly = {
	.name = "probeonly",
	.title = "Inhibit access to DASD device nodes",
	.desc =
	"Control whether the DASD driver allows access to DASD devices:\n"
	"  0: All enabled DASDs can be accessed normally\n"
	"  1: Reject any attempt to open DASD devices\n",
	.defval = "0",
	.activerem = 1,
	.defunset = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_tattr_nopav = {
	.name = "nopav",
	.title = "Deactivate the Parallel Access Volume feature",
	.desc = "Control the use of the Parallel Access Volume (PAV) feature:\n"
		"  0: PAV is used when supported by the storage system\n"
		"  1: The use of PAV is suppressed when running in LPAR\n",
	.defval = "0",
	.activerem = 1,
	.defunset = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_tattr_nofcx = {
	.name = "nofcx",
	.title = "Deactivate the High Performance FICON feature",
	.desc = "Control the use of the High Performance FICON (HPF) feature:\n"
		"  0: HPF is used when supported by the storage hardware\n"
		"  1: HPF is not used\n",
	.defval = "0",
	.activerem = 1,
	.defunset = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_tattr_eer_pages = {
	.name = "eer_pages",
	.title = "Modify buffer size for error records",
	.desc =
	"Control the number of 4 KB pages that are used for internal "
	"buffering\n"
	"of error records generated by the Extended Error Reporting (EER)\n"
	"feature.\n",
	.defval = "5",
	.activerem = 1,
	.nounload = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_NUM_GE(1)),
};

/*
 * DASD device attributes.
 */

static struct attrib dasd_attr_failfast = {
	.name = "failfast",
	.title = "Modify error recovery in no-path scenario",
	.desc =
	"Control I/O handling when all paths to a DASD have been lost:\n"
	"  0: Queue I/O until path becomes available\n"
	"  1: Fail I/O request immediately\n",
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_attr_readonly = {
	.name = "readonly",
	.title = "Inhibit write access to DASD",
	.desc = "Control the DASD driver read-only setting for a DASD:\n"
		"  0: Allow writing to the device\n"
		"  1: Deny writing to the device\n",
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_attr_erplog = {
	.name = "erplog",
	.title = "Enable logging of Error Recovery Processing",
	.desc = "Control logging of Error Recovery Processing (ERP), such as\n"
		"failing channel programs:\n"
		"  0: ERP logging is disabled\n"
		"  1: ERP logging is enabled\n",
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static int dasd_raw_diag_order_cmp(struct setting *, struct setting *);
static bool dasd_raw_diag_check(struct setting *, struct setting *, config_t);

static struct attrib dasd_attr_use_diag = {
	.name = "use_diag",
	.title = "Activate z/VM hypervisor assisted I/O processing",
	.desc =
	"Control I/O access mode for a DASD:\n"
	"  0: I/O is performed using standard channel programs\n"
	"  1: I/O is performed using the z/VM DIAGNOSE X'250' interface\n"
	"\n"
	"The DIAGNOSE X'250' access mode works only when:\n"
	"  - Running Linux as z/VM guest\n"
	"  - Using devices formatted with consistent block sizes, such as\n"
	"    ECKD DASDs with LDL or CMS format, or FBA devices\n",
	.order_cmp = dasd_raw_diag_order_cmp,
	.check = dasd_raw_diag_check,
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_attr_raw_track_access = {
	.name = "raw_track_access",
	.title = "Enable access to ECKD track metadata",
	.desc =
	"Control whether the DASD driver provides access to full ECKD tracks,\n"
	"including ECKD-specific meta-data:\n"
	"  0: Only access the data portion of ECKD tracks\n"
	"  1: Access the full track\n",
	.order_cmp = dasd_raw_diag_order_cmp,
	.check = dasd_raw_diag_check,
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

/* Ensure ordering if both use_diag and raw_track_access are modified. */
static int dasd_raw_diag_order_cmp(struct setting *a, struct setting *b)
{
	int a_val, b_val;

	if ((a->attrib == &dasd_attr_use_diag &&
	     b->attrib == &dasd_attr_raw_track_access) ||
	    (a->attrib == &dasd_attr_raw_track_access &&
	     b->attrib == &dasd_attr_use_diag)) {
		/* Define order to ensure that =0 is done before =1. */
		a_val = atoi(a->value);
		b_val = atoi(b->value);

		if (a_val == 1 && b_val == 0)
			return 1;
		if (a_val == 0 && b_val == 1)
			return -1;
	}

	return ccw_offline_only_order_cmp(a, b);
}

/* Conflict if both use_diag and raw_track_access are set. */
static bool dasd_raw_diag_check(struct setting *a, struct setting *b,
				config_t config)
{
	if ((a->attrib == &dasd_attr_use_diag &&
	     b->attrib == &dasd_attr_raw_track_access) ||
	    (a->attrib == &dasd_attr_raw_track_access &&
	     b->attrib == &dasd_attr_use_diag)) {
		if (atoi(a->value) == 1 && atoi(b->value) == 1)
			return false;
	}

	return ccw_offline_only_check(a, b, config);
}

static struct attrib dasd_attr_eer_enabled = {
	.name = "eer_enabled",
	.title = "Enable Extended Error Reporting",
	.desc =
	"Control the Extended Error Reporting (EER) feature for a DASD:\n"
	"  0: EER is disabled\n"
	"  1: EER is enabled\n",
	.order_cmp = ccw_online_only_order_cmp,
	.check = ccw_online_only_check,
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_attr_expires = {
	.name = "expires",
	.title = "Modify I/O operation timeout",
	.desc =
	"Specify the time in seconds that the DASD driver waits for the\n"
	"completion of a single I/O operation before considering that I/O\n"
	"operation to have failed.\n"
	"\n"
	"The default value depends on the DASD type.\n",
	.order_cmp = ccw_online_only_order_cmp,
	.check = ccw_online_only_check,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(1, 40000000)),
};

static struct attrib dasd_attr_retries = {
	.name = "retries",
	.title = "Modify I/O operation retry counter",
	.desc =
	"Specify the number of times that a failed I/O should be retried\n"
	"before reporting it as failed.\n"
	"\n"
	"The default value is dependent on the DASD type.\n",
	.order_cmp = ccw_online_only_order_cmp,
	.check = ccw_online_only_check,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 32768)),
};

static struct attrib dasd_attr_timeout = {
	.name = "timeout",
	.title = "Modify I/O request timeout",
	.desc =
	"Specify the total time in seconds that the Linux block device layer\n"
	"waits for the completion of an I/O request issued to the DASD driver\n"
	"before considering that I/O to have failed. Specify 0 to deactivate\n"
	"timeout handling.\n",
	.order_cmp = ccw_online_only_order_cmp,
	.check = ccw_online_only_check,
	.defval = "0",
	.accept = ACCEPT_ARRAY(ACCEPT_NUM_GE(0)),
};

static struct attrib dasd_attr_reservation_policy = {
	.name = "reservation_policy",
	.title = "Modify lost device reservation behavior",
	.desc =
	"Control the DASD driver behavior if an existing DASD reservation of\n"
	"this system for a DASD is lost:\n"
	"  ignore: I/O operations are blocked until the external reservation\n"
	"          is released (default)\n"
	"    fail: All I/O operations are considered to have failed\n",
	.defval = "ignore",
	.accept = ACCEPT_ARRAY(ACCEPT_STR("ignore"), ACCEPT_STR("fail")),
};

static struct attrib dasd_attr_last_known_reservation_state = {
	.name = "last_known_reservation_state",
	.title = "Display and reset driver device reservation view",
	.desc =
	"Display the DASD driver's view of device reservations held by this\n"
	"system:\n"
	"      none: No reservation held or no information available\n"
	"  reserved: Device is assumed to be reserved by this system\n"
	"      lost: A reservation held by this system was lost\n"
	"\n"
	"To reset the reservation state from 'lost' to 'none', set this\n"
	"attribute to 'reset'.\n",
	.unstable = 1,
	.accept = ACCEPT_ARRAY(ACCEPT_RANGE(0, 1)),
};

static struct attrib dasd_attr_safe_offline = {
	.name = "safe_offline",
	.title = "Deactivate DASD after processing pending I/Os",
	.desc = "Write an arbitrary value to this attribute to attempt to set\n"
		"the DASD offline after all outstanding I/O requests have\n"
		"been processed.\n",
	.activeonly = 1,
	.writeonly = 1,
};

/*
 * DASD subtype methods.
 */

/* Check if use_diag setting can be correctly applied. */
static exit_code_t check_use_diag(struct device *dev, config_t config)
{
	struct setting *u;
	int zvm = is_zvm();

	if (SCOPE_ACTIVE(config)) {
		u = setting_list_find(dev->active.settings,
				      dasd_attr_use_diag.name);
		if (u && u->modified) {
			if (u->value && atoi(u->value) == 1 && !zvm) {
				delayed_warn("Cannot set 'use_diag=1' on "
					     "non-z/VM system\n");
				return EXIT_INVALID_CONFIG;
			}
		}
	}
	if (SCOPE_PERSISTENT(config)) {
		u = setting_list_find(dev->persistent.settings,
				      dasd_attr_use_diag.name);
		if (u && u->modified) {
			if (u->value && atoi(u->value) == 1 && !zvm) {
				delayed_warn("Cannot set 'use_diag=1' on "
					     "non-z/VM system\n");
				return EXIT_INVALID_CONFIG;
			}
		}
	}

	return EXIT_OK;
}

static exit_code_t dasd_st_check_pre_configure(struct subtype *st,
					       struct device *dev,
					       int prereq, config_t config)
{
	exit_code_t rc;

	if (dev->active.deconfigured)
		return EXIT_OK;
	rc = check_use_diag(dev, config);
	if (rc)
		return rc;

	return EXIT_OK;
}

static void dasd_st_add_modules(struct subtype *st, struct device *dev,
				struct util_list *modules)
{
	int changed, aset, pset;

	/* Add main module. */
	st->super->add_modules(st, dev, modules);

	/* Add dasd_diag_mod if use_diag is set. */
	setting_list_get_bool_state(dev->active.settings,
				    dasd_attr_use_diag.name, &changed, &aset);
	setting_list_get_bool_state(dev->persistent.settings,
				    dasd_attr_use_diag.name, &changed, &pset);

	if (aset || pset)
		strlist_add_unique(modules, DASD_DIAG_MOD_NAME);
}

/*
 * DASD methods.
 */

/* Clean up all resources used by devtype object. */
static void dasd_devtype_exit(struct devtype *dt)
{
	setting_list_free(dt->active_settings);
	setting_list_free(dt->persistent_settings);
}

/* Split a dasd= module parameter string into a setting_list, based on
 * known attributes encoded in dasd= parameters. */
static void split_dasd(struct devtype *dt, const char *str,
		       struct setting_list *list)
{
	char *copy, *curr, *next, *dasd_str;
	struct attrib *a;
	struct util_list *dasd;

	dasd = strlist_new();
	copy = misc_strdup(str);
	next = copy;
	while ((curr = strsep(&next, ","))) {
		if (strcmp(curr, "(null)") == 0)
			continue;
		if (*curr == 0) {
			/* Handle dasd="" */
			continue;
		}
		/* Check for known attributes. */
		a = attrib_find(dt->type_attribs, curr);
		if (a) {
			/* Currently only boolean attributes implemented. */
			setting_list_apply_actual(list, a, curr, "1");
			continue;
		}
		/* An unknown setting or device spec - leave in dasd=. */
		strlist_add(dasd, "%s", curr);
	}
	free(copy);

	/* Add dasd= setting if necessary. */
	if (!util_list_is_empty(dasd)) {
		dasd_str = strlist_flatten(dasd, ",");
		setting_list_apply_actual(list, NULL, "dasd", dasd_str);
		free(dasd_str);
	}
	strlist_free(dasd);
}

/* Convert a module parameter list into a devtype settings list. */
static void convert_params_to_settings(struct devtype *dt,
				       struct setting_list *from,
				       struct setting_list *to)
{
	struct setting *s;

	util_list_iterate(&from->list, s) {
		if (strcmp(s->name, "dasd") == 0) {
			/* Special handling: split known attribs out. */
			split_dasd(dt, s->value, to);
			continue;
		}
		/* Just copy remaining attributes. */
		setting_list_apply_actual(to, s->attrib, s->name, s->value);
	}
}

/* Convert a devtype settings list to a module parameter list. */
static void convert_settings_to_params(struct devtype *dt,
				       struct setting_list *from,
				       struct setting_list **to)
{
	struct setting *s_from, *s_to;
	struct util_list *dasd;
	int dasd_mod;
	char *flat;

	*to = setting_list_new();
	dasd = strlist_new();
	dasd_mod = 0;

	util_list_iterate(&from->list, s_from) {
		if (s_from->removed)
			continue;
		if (s_from->derived && !s_from->modified)
			continue;
		if (strcmp(s_from->name, "eer_pages") == 0) {
			/* Copy stand-alone parameter. */
			setting_list_add(*to, setting_copy(s_from));
			continue;
		}
		/* All other settings need to go into dasd=. */
		if (strcmp(s_from->name, "dasd") == 0) {
			/* Just add explicit dasd= content. */
			strlist_add(dasd, "%s", s_from->value);
		} else if (strcmp(s_from->value, "0") == 0) {
			/* A flag in dasd= counts as set, so skip the unset
			 * ones. */
			continue;
		} else {
			/* Just add the name for set attributes. */
			strlist_add(dasd, "%s", s_from->name);
			dasd_mod += s_from->modified;
		}
	}

	flat = strlist_flatten(dasd, ",");
	s_to = setting_new(NULL, "dasd", flat);
	free(flat);
	if (dasd_mod > 0)
		s_to->modified = 1;
	if (util_list_is_empty(dasd))
		s_to->removed = 1;
	setting_list_add(*to, s_to);
	strlist_free(dasd);
}

static exit_code_t dasd_devtype_read_settings(struct devtype *dt,
					      config_t config)
{
	struct setting_list *list;
	char *path;
	exit_code_t rc = EXIT_OK;

	if (SCOPE_ACTIVE(config) && !dt->active_settings) {
		dt->active_exists = 0;
		rc = module_get_params(DASD_MOD_NAME, dt->type_attribs, &list);
		if (rc)
			return rc;
		dt->active_settings = setting_list_new();
		if (list) {
			convert_params_to_settings(dt, list,
						   dt->active_settings);
			setting_list_mark_default_derived(dt->active_settings);
			setting_list_apply_defaults(dt->active_settings,
						    dt->type_attribs, false);
			setting_list_free(list);
			dt->active_exists = 1;
		}
	}

	if (SCOPE_PERSISTENT(config) && !dt->persistent_settings) {
		dt->persistent_exists = 0;
		path = path_get_modprobe_conf(dt);
		rc = modprobe_read_settings(path, DASD_MOD_NAME,
					    dt->type_attribs, &list);
		free(path);
		if (rc)
			return rc;
		dt->persistent_settings = setting_list_new();
		if (list) {
			convert_params_to_settings(dt, list,
						   dt->persistent_settings);
			setting_list_apply_defaults(dt->persistent_settings,
						    dt->type_attribs, false);
			setting_list_free(list);
			dt->persistent_exists = 1;
		}
	}

	return rc;
}

static exit_code_t dasd_devtype_write_settings(struct devtype *dt,
					       config_t config)
{
	struct setting_list *list;
	char *path;
	exit_code_t rc = EXIT_OK;

	if (SCOPE_ACTIVE(config) && dt->active_settings) {
		/* Try setting parameters directly via Sysfs. */
		if (module_set_params(DASD_MOD_NAME, dt->active_settings))
			goto persistent;

		convert_settings_to_params(dt, dt->active_settings, &list);
		rc = module_load(DASD_MOD_NAME, dasd_mods, list,
				 err_delayed_print);
		setting_list_free(list);
		if (rc)
			return rc;
	}

persistent:
	if (SCOPE_PERSISTENT(config) && dt->persistent_settings) {
		path = path_get_modprobe_conf(dt);
		if (!rc) {
			convert_settings_to_params(dt, dt->persistent_settings,
						   &list);
			rc = modprobe_write_settings(path, DASD_MOD_NAME, list);
			setting_list_free(list);
		}
		free(path);
	}

	return rc;
}

/*
 * DASD device sub-types.
 */
static struct ccw_subtype_data dasd_eckd_data = {
	.ccwdrv		= "dasd-eckd",
	.mod		= "dasd_eckd_mod",
};

static struct subtype dasd_subtype_eckd = {
	.super		= &ccw_subtype,
	.devtype	= &dasd_devtype,
	.name		= "dasd-eckd",
	.title		= "Enhanced Count Key Data (ECKD) DASDs",
	.devname	= "ECKD DASD",
	.modules	= STRING_ARRAY(DASD_ECKD_MOD_NAME),
	.namespace	= &ccw_namespace,
	.data		= &dasd_eckd_data,

	.dev_attribs = ATTRIB_ARRAY(
		&ccw_attr_online_force,
		&ccw_attr_cmb_enable,
		&dasd_attr_failfast,
		&dasd_attr_readonly,
		&dasd_attr_erplog,
		&dasd_attr_use_diag,
		&dasd_attr_raw_track_access,
		&dasd_attr_eer_enabled,
		&dasd_attr_expires,
		&dasd_attr_retries,
		&dasd_attr_timeout,
		&dasd_attr_reservation_policy,
		&dasd_attr_last_known_reservation_state,
		&dasd_attr_safe_offline,
	),
	.unknown_dev_attribs	= 1,

	.check_pre_configure	= &dasd_st_check_pre_configure,
	.add_modules		= &dasd_st_add_modules,
};

static struct ccw_subtype_data dasd_fba_data = {
	.ccwdrv		= "dasd-fba",
	.mod		= "dasd_fba_mod",
};

static struct subtype dasd_subtype_fba = {
	.super		= &ccw_subtype,
	.devtype	= &dasd_devtype,
	.name		= "dasd-fba",
	.title		= "Fixed Block Architecture (FBA) DASDs",
	.devname	= "FBA DASD",
	.modules	= STRING_ARRAY(DASD_FBA_MOD_NAME),
	.namespace	= &ccw_namespace,
	.data		= &dasd_fba_data,

	.dev_attribs = ATTRIB_ARRAY(
		&ccw_attr_online_force,
		&ccw_attr_cmb_enable,
		&dasd_attr_failfast,
		&dasd_attr_readonly,
		&dasd_attr_erplog,
		&dasd_attr_use_diag,
		&dasd_attr_expires,
		&dasd_attr_retries,
		&dasd_attr_timeout,
		&dasd_attr_reservation_policy,
		&dasd_attr_last_known_reservation_state,
		&dasd_attr_safe_offline,
	),
	.unknown_dev_attribs	= 1,

	.check_pre_configure	= &dasd_st_check_pre_configure,
	.add_modules		= &dasd_st_add_modules,
};

/*
 * DASD device type.
 */

struct devtype dasd_devtype = {
	.name		= "dasd",
	.title		= "FICON-attached Direct Access Storage Devices "
			  "(DASDs)",
	.devname	= "DASD",
	.modules	= STRING_ARRAY(DASD_MOD_NAME),

	.subtypes = SUBTYPE_ARRAY(
		&dasd_subtype_eckd,
		&dasd_subtype_fba,
	),

	.type_attribs = ATTRIB_ARRAY(
		&dasd_tattr_autodetect,
		&dasd_tattr_probeonly,
		&dasd_tattr_nopav,
		&dasd_tattr_nofcx,
		&dasd_tattr_eer_pages,
	),

	.exit			= &dasd_devtype_exit,

	.read_settings		= &dasd_devtype_read_settings,
	.write_settings		= &dasd_devtype_write_settings,
};
