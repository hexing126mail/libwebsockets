/*
 * lws-minimal-secure-streams-smd
 *
 * Written in 2010-2020 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 *
 * This demonstrates a minimal http client using secure streams to access the
 * SMD api.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

static int interrupted, bad = 1, count_p1, count_p2, count_tx;
static lws_sorted_usec_list_t sul_timeout;

/*
 * If the -proxy app is fulfilling our connection, then we don't need to have
 * the policy in the client.
 *
 * When we build with LWS_SS_USE_SSPC, the apis hook up to a proxy process over
 * a Unix Domain Socket.  To test that, you need to separately run the
 * ./lws-minimal-secure-streams-proxy test app on the same machine.
 */

#if !defined(LWS_SS_USE_SSPC)
static const char * const default_ss_policy =
	"{"
		"\"schema-version\":1"
	"}"
;

#endif

typedef struct myss {
	struct lws_ss_handle 		*ss;
	void				*opaque_data;
	/* ... application specific state ... */
	lws_sorted_usec_list_t		sul;
} myss_t;


/* secure streams payload interface */

static int
myss_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
//	myss_t *m = (myss_t *)userobj;

	lwsl_info("%s: len %d, flags: %d\n", __func__, (int)len, flags);
	lwsl_hexdump_info(buf, len);

	count_p1++;

	return 0;
}

static void
sul_tx_periodic_cb(lws_sorted_usec_list_t *sul)
{
	myss_t *m = lws_container_of(sul, myss_t, sul);

	lwsl_notice("%s: requesting TX\n", __func__);
	lws_ss_request_tx(m->ss);
}

static int
myss_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	int *flags)
{
	myss_t *m = (myss_t *)userobj;

	lwsl_notice("%s: sending SS smd\n", __func__);

	lws_ser_wu64be(buf, LWSSMDCL_NETWORK);
	*len = LWS_SMD_SS_RX_HEADER_LEN +
		lws_snprintf((char *)buf + LWS_SMD_SS_RX_HEADER_LEN, *len,
				"{\"somthing\":\"notseenbyssrx\"}");

	*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

	count_tx++;

	lws_sul_schedule(lws_ss_get_context(m->ss), 0, &m->sul,
			 sul_tx_periodic_cb, 500 * LWS_US_PER_MS);

	return 0;
}

static int
myss_state(void *userobj, void *h_src, lws_ss_constate_t state,
	   lws_ss_tx_ordinal_t ack)
{
	myss_t *m = (myss_t *)userobj;

	if (state == LWSSSCS_DESTROYING) {
		lws_sul_cancel(&m->sul);
		return 0;
	}

	if (state != LWSSSCS_CREATING)
		return 0;

	lws_sul_schedule(lws_ss_get_context(m->ss), 0, &m->sul,
			 sul_tx_periodic_cb, 1);

	return 0;
}

static const lws_ss_info_t ssi_lws_smd = {
	.handle_offset		  = offsetof(myss_t, ss),
	.opaque_user_data_offset  = offsetof(myss_t, opaque_data),
	.rx			  = myss_rx,
	.tx			  = myss_tx,
	.state			  = myss_state,
	.user_alloc		  = sizeof(myss_t),
	.streamtype		  = LWS_SMD_STREAMTYPENAME,
	.manual_initial_tx_credit = LWSSMDCL_SYSTEM_STATE |
				    LWSSMDCL_INTERACTION,
};

/* for comparison, this is a non-SS lws_smd participant */

static int
direct_smd_cb(void *opaque, lws_smd_class_t _class, lws_usec_t timestamp,
	      void *buf, size_t len)
{
	struct lws_context **pctx = (struct lws_context **)opaque;
	const char *cp;
	size_t al;

	lwsl_debug("%s: class: 0x%x, ts: %llu\n", __func__, _class,
		  (unsigned long long)timestamp);
	lwsl_hexdump_debug(buf, len);

	count_p2++;

	if (_class != LWSSMDCL_SYSTEM_STATE)
		return 0;

	cp = lws_json_simple_find(buf, len, "\"state\":", &al);
	if (cp && !strncmp(cp, "OPERATIONAL", al)) {

		/* create the SS link to lws_smd */

		if (lws_ss_create(*pctx, 0, &ssi_lws_smd, NULL, NULL, NULL, NULL)) {
			lwsl_err("%s: failed to create secure stream\n",
				 __func__);

			return -1;
		}
	}

	return 0;
}


static void
sul_timeout_cb(lws_sorted_usec_list_t *sul)
{
	interrupted = 1;
}


static void
sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	struct lws_context *context;

	signal(SIGINT, sigint_handler);

	memset(&info, 0, sizeof info);
	lws_cmdline_option_handle_builtin(argc, argv, &info);

	lwsl_user("LWS Secure Streams SMD test client [-d<verb>]\n");

	info.fd_limit_per_thread	= 1 + 6 + 1;
	info.port			= CONTEXT_PORT_NO_LISTEN;
#if !defined(LWS_SS_USE_SSPC)
	info.pss_policies_json		= default_ss_policy;
#else
	info.protocols			= lws_sspc_protocols;
#endif
	info.options			= LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
					  LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

	info.early_smd_cb		= direct_smd_cb;
	info.early_smd_class_filter	= 0xffffffff;
	info.early_smd_opaque		= &context;

	/* create the context */

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

#if defined(LWS_SS_USE_SSPC)
	if (!lws_create_vhost(context, &info)) {
		lwsl_err("%s: failed to create default vhost\n", __func__);
		goto bail;
	}
#endif

	/* set up the test timeout */

	lws_sul_schedule(context, 0, &sul_timeout, sul_timeout_cb,
			 3 * LWS_US_PER_SEC);

	/* the event loop */

	while (lws_service(context, 0) >= 0 && !interrupted)
		;

	/* compare what happened with what we expect */

	if (!count_p1 && count_p2 >= 12 && count_tx >= 1)
		bad = 0;

	lwsl_info("%d %d %d\n", count_p1, count_p2, count_tx);

#if defined(LWS_SS_USE_SSPC)
bail:
#endif
	lws_context_destroy(context);

	lwsl_user("Completed: %s\n", bad ? "failed" : "OK");

	return bad;
}
