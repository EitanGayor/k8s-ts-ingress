/* vim:set sw=8 ts=8 noet: */
/*
 * Copyright (c) 2016-2017 Torchbox Ltd.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<stdlib.h>
#include	<string.h>
#include	<errno.h>
#include	<getopt.h>

#include	<ts/ts.h>
#include	<ts/apidefs.h>
#include	<ts/remap.h>

#include	<openssl/ssl.h>

#include	<json.h>

#include	"hash.h"
#include	"api.h"
#include	"watcher.h"
#include	"config.h"
#include	"plugin.h"

int
handle_tls(TSCont contn, TSEvent evt, void *edata)
{
TSVConn			 ssl_vc;
SSL			*ssl = NULL;
const char		*host = NULL, *version;
const remap_host_t	*rh;
int			 ret = 1;

	TSDebug("kubernetes", "handle_tls: starting");

	ssl_vc = edata;
	ssl = (SSL *)TSVConnSSLConnectionGet(ssl_vc);
	host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

	/* Host can sometimes be null; do nothing in that case. */
	if (!host) {
		TSDebug("kubernetes_tls", "handle_tls: no host name");
		return TS_SUCCESS;
	}

	TSDebug("kubernetes_tls", "handle_tls: doing SNI map for [%s]", host);

	/*
	 * Take a read lock on the cluster state so it doesn't change while
	 * we're using it.
	 */
	pthread_rwlock_rdlock(&state->lock);

	/* Not initialised yet? */
	if (!state->db) {
		pthread_rwlock_unlock(&state->lock);
		TSDebug("kubernetes", "handle_remap: no database");
		return TS_SUCCESS;
	}

	if ((rh = remap_db_get_host(state->db, host)) == NULL) {
		TSDebug("kubernetes", "[%s] handle_tls: host not found", host);
		goto cleanup;
	}

	if (!rh->rh_ctx) {
		TSDebug("kubernetes", "[%s] handle_tls: host found, but not ctx",
			host);
		goto cleanup;
	}

	version = SSL_get_version(ssl);
	TSDebug("kubernetes", "[%s] client TLS version %s, required %04x",
		host, version, rh->rh_tls_version);

	/*
	 * We would like to do this with SSL_set_min_proto_version or at least
	 * SSL_set_options(..., SSL_OP_NO_TLSxxx), but this hook is called too
	 * late and changing that has no effect.  Instead, just return without
	 * setting an SSL_CTX; since none is configured by default, this will
	 * return a TLS protocol error to the client.
	 */
	switch (rh->rh_tls_version) {
	case REMAP_TLS_1_2:
		if (strcmp(version, "TLSv1.1") == 0)
			goto cleanup;

	case REMAP_TLS_1_1:
		if (strcmp(version, "TLSv1") == 0)
			goto cleanup;

	case REMAP_TLS_1_0:
		if (strcmp(version, "SSLv3") == 0)
			goto cleanup;
		break;
	}

	SSL_set_SSL_CTX(ssl, rh->rh_ctx);
	TSDebug("kubernetes", "[%s] handle_tls: attached SSL context [%p]",
		host, rh->rh_ctx);

	/*
	 * Is HTTP/2 disabled on this Ingress?
	 */
	if (!rh->rh_http2) {
	TSAcceptor	acpt = TSAcceptorGet(ssl_vc);
	int		acptid = TSAcceptorIDGet(acpt);

		/* If yes, set the protocolset we saved earlier */
		TSRegisterProtocolSet(ssl_vc, state->protosets[acptid]);
	}

cleanup:
	TSDebug("kubernetes", "[%s] handle_tls: return %d", host, ret);
	TSVConnReenable(ssl_vc);
	pthread_rwlock_unlock(&state->lock);
	return 1;
}
