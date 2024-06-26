/*
 * Copyright (C) 2017 OpenSIPS Project
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * History:
 * ---------
 *  2017-01-24  created (razvanc)
 */

#include "../../mem/shm_mem.h"
#include "../../sr_module.h"
#include "../../db/db_id.h"
#include "../../lib/list.h"
#include "../../mod_fix.h"
#include "../../dprint.h"
#include "../../ut.h"

#include "rmq_servers.h"

static int mod_init(void);
static int child_init(int);
static void mod_destroy(void);

static int fixup_check_avp(void** param);
static int rmq_publish(struct sip_msg *msg, struct rmq_server *srv, str *srkey,
			str *sbody, str *sctype, pv_spec_t *hnames, pv_spec_t *hvals);

int use_tls;
struct openssl_binds openssl_api;
struct tls_mgm_binds tls_api;
static int rmq_connect_timeout = RMQ_DEFAULT_CONNECT_TIMEOUT;
static int rmq_timeout = 0;

struct timeval conn_timeout_tv;
#if defined AMQP_VERSION && AMQP_VERSION >= 0x00090000
struct timeval rpc_timeout_tv;
#endif

#if AMQP_VERSION < AMQP_VERSION_CODE(0, 10, 0, 0)
gen_lock_t *ssl_lock;
#endif

static const param_export_t params[]={
	{ "server_id",			STR_PARAM|USE_FUNC_PARAM,
		(void *)rmq_server_add},
	{"use_tls", INT_PARAM, &use_tls},
	{"connect_timeout", INT_PARAM, &rmq_connect_timeout},
	{"timeout",         INT_PARAM, &rmq_timeout},
	{0,0,0}
};

static module_dependency_t *get_deps_use_tls_mgm(const param_export_t *param)
{
	if (*(int *)param->param_pointer == 0)
		return NULL;

	return alloc_module_dep(MOD_TYPE_DEFAULT, "tls_mgm", DEP_ABORT);
}
static module_dependency_t *get_deps_use_tls_openssl(const param_export_t *param)
{
	if (*(int *)param->param_pointer == 0)
		return NULL;

	return alloc_module_dep(MOD_TYPE_DEFAULT, "tls_openssl", DEP_ABORT);
}

/* modules dependencies */
static const dep_export_t deps = {
	{ /* OpenSIPS module dependencies */
		{ MOD_TYPE_NULL, NULL, 0 },
	},
	{ /* modparam dependencies */
		{ "use_tls", get_deps_use_tls_mgm },
		{ "use_tls", get_deps_use_tls_openssl },
		{ NULL, NULL },
	},
};

/* exported commands */
static const cmd_export_t cmds[] = {
	{"rabbitmq_publish",(cmd_function)rmq_publish, {
		{CMD_PARAM_STR, fixup_rmq_server, 0},
		{CMD_PARAM_STR, 0, 0},
		{CMD_PARAM_STR, 0, 0},
		{CMD_PARAM_STR | CMD_PARAM_OPT, 0, 0},
		{CMD_PARAM_VAR | CMD_PARAM_OPT, fixup_check_avp, 0},
		{CMD_PARAM_VAR | CMD_PARAM_OPT, fixup_check_avp, 0}, {0,0,0}},
		ALL_ROUTES},
	{0,0,{{0,0,0}},0}
};

/* module exports */
struct module_exports exports= {
	"rabbitmq",						/* module name */
	MOD_TYPE_DEFAULT,				/* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,				/* dlopen flags */
	0,								/* load function */
	&deps,						    /* OpenSIPS module dependencies */
	cmds,							/* exported functions */
	0,								/* exported async functions */
	params,							/* exported parameters */
	0,								/* exported statistics */
	0,								/* exported MI functions */
	0,								/* exported pseudo-variables */
	0,								/* exported transformations */
	0,								/* extra processes */
	0,								/* module pre-initialization function */
	mod_init,						/* module initialization function */
	(response_function) 0,			/* response handling function */
	(destroy_function)mod_destroy,	/* destroy function */
	child_init,						/* per-child init function */
	0								/* reload confirm function */
};

/**
 * init module function
 */
static int mod_init(void)
{
	LM_NOTICE("initializing RabbitMQ module ...\n");

	if (use_tls) {
		#ifndef AMQP_VERSION_v04
		LM_ERR("TLS not supported for librabbitmq version lower than 0.4.0\n");
		return -1;
		#endif

		if (load_tls_openssl_api(&openssl_api)) {
			LM_DBG("Failed to load openssl API\n");
			return -1;
		}

		if (load_tls_mgm_api(&tls_api) != 0) {
			LM_ERR("failed to load tls_mgm API!\n");
			return -1;
		}

		#if AMQP_VERSION < AMQP_VERSION_CODE(0, 10, 0, 0)
		ssl_lock = lock_alloc();
		if (!ssl_lock) {
			LM_ERR("No more shm memory\n");
			return -1;
		}
		if (!lock_init(ssl_lock)) {
			LM_ERR("Failed to init lock\n");
			return -1;
		}
		#endif

		amqp_set_initialize_ssl_library(0);
	}

	conn_timeout_tv.tv_sec = rmq_connect_timeout/1000;
	conn_timeout_tv.tv_usec = (rmq_connect_timeout%1000)*1000;

#if defined AMQP_VERSION && AMQP_VERSION >= 0x00090000
	if (rmq_timeout < 0) {
		LM_WARN("invalid value for 'timeout' %d; fallback to blocking mode\n", rmq_timeout);
		rmq_timeout = 0;
	}
	rpc_timeout_tv.tv_sec = rmq_timeout/1000;
	rpc_timeout_tv.tv_usec = (rmq_timeout%1000)*1000;
#else
	if (rmq_timeout != 0)
		LM_WARN("setting the timeout without support for it; fallback to blocking mode\n");
#endif

	return 0;
}

/*
 * function called when a child process starts
 */
static int child_init(int rank)
{
	rmq_connect_servers();
	return 0;
}

/*
 * function called after OpenSIPS has been stopped to cleanup resources
 */
static void mod_destroy(void)
{
	LM_NOTICE("destroying RabbitMQ module ...\n");

	#if AMQP_VERSION < AMQP_VERSION_CODE(0, 10, 0, 0)
	lock_destroy(ssl_lock);
	lock_dealloc(ssl_lock);
	#endif
}

static int fixup_check_avp(void** param)
{
	if (((pv_spec_t *)*param)->type!=PVT_AVP) {
		LM_ERR("return parameter must be an AVP\n");
		return E_SCRIPT;
	}

	return 0;
}

/*
 * function that simply prints the parameters passed
 */
static int rmq_publish(struct sip_msg *msg, struct rmq_server *srv, str *srkey,
			str *sbody, str *sctype, pv_spec_t *hnames, pv_spec_t *hvals)
{
	int aname, avals;
	unsigned short type;

	if (hnames && !hvals) {
		LM_ERR("header names without values!\n");
		return -1;
	}
	if (!hnames && hvals) {
		LM_ERR("header values without names!\n");
		return -1;
	}

	if (hnames &&
			pv_get_avp_name(msg, &hnames->pvp, &aname, &type) < 0) {
		LM_ERR("cannot resolve names AVP\n");
		return -1;
	}

	if (hvals &&
			pv_get_avp_name(msg, &hvals->pvp, &avals, &type) < 0) {
		LM_ERR("cannot resolve values AVP\n");
		return -1;
	}

	/* resolve the AVP */
	return rmq_send(srv, srkey, sbody, sctype,
			(hnames ? &aname : NULL),
			(hvals ? &avals : NULL)) == 0 ? 1: -1;
}
