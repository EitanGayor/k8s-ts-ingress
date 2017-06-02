/* vim:set sw=8 ts=8 noet: */
/*
 * Copyright (c) 2016-2017 Torchbox Ltd.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<errno.h>
#include	<getopt.h>
#include	<regex.h>
#include	<ctype.h>
#include	<assert.h>

#include	<ts/ts.h>
#include	<ts/remap.h>
#include	<ts/experimental.h>

#include	<openssl/ssl.h>

#include	"watcher.h"
#include	"api.h"
#include	"config.h"
#include	"plugin.h"
#include	"synth.h"
#include	"base64.h"
#include	"ts_crypt.h"
#include	"auth.h"
#include	"strmatch.h"

void
rebuild_maps(struct state *state)
{
remap_db_t	*db;

	TSMutexLock(state->cluster_lock);
	if (!state->changed) {
		TSDebug("kubernetes", "rebuild_maps: no changes");
		TSMutexUnlock(state->cluster_lock);
		return;
	}

	TSDebug("kubernetes", "rebuild_maps: running");
	db = remap_db_from_cluster(state->config, state->cluster);
	state->changed = 0;
	TSMutexUnlock(state->cluster_lock);

	TSConfigSet(state->cfg_slot, db, (TSConfigDestroyFunc)remap_db_free);
}

/*
 * Copy the string s, which is exactly n bytes long.  Any nul characters in
 * s will be ignored.
 */
static char *
xstrndup(const char *s, size_t n)
{
char	*ret;
	if ((ret = malloc(n + 1)) == NULL)
		return NULL;
	bcopy(s, ret, n);
	ret[n] = '\0';
	return ret;
}

/*
 * Called when the DNS lookup for an ExternalName has finished.
 */
static int
external_lookup(TSCont contn, TSEvent event, void *data)
{
TSHttpTxn	txnp = TSContDataGet(contn);

	assert(event == TS_EVENT_HOST_LOOKUP);

	if (data)
		TSHttpTxnServerAddrSet(txnp, data);

	TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
	TSContDestroy(contn);
	return TS_SUCCESS;
}

/*
 * Build a remap_request to pass to remap_db from the TS request data.
 */
int
request_from_txn(TSHttpTxn txnp, remap_request_t *req)
{
TSMLoc		 hdrs, url;
TSMBuffer	 reqp;
const char	*cs;
char		*s;
int		 len;

	/* Fetch the request and the URL. */
	TSHttpTxnClientReqGet(txnp, &reqp, &hdrs);
	TSHttpHdrUrlGet(reqp, hdrs, &url);

	/* method */
	cs = TSHttpHdrMethodGet(reqp, hdrs, &len);
	req->rr_method = xstrndup(cs, len);

	/* scheme */
	if ((cs = TSUrlSchemeGet(reqp, url, &len)) != NULL) {
		req->rr_proto = xstrndup(cs, len);
		TSDebug("kubernetes", "request_from_txn: scheme is [%.*s]",
			len, cs);
	}

	/* host - if missing, ignore this request */
	if ((cs = TSHttpHdrHostGet(reqp, hdrs, &len)) != NULL) {
		req->rr_host = xstrndup(cs, len);

		/* remove port from host if present */
		if ((s = strchr(req->rr_host, ':')) != NULL)
			*s = '\0';
	}

	/* path */
	if ((cs = TSUrlPathGet(reqp, url, &len)) != NULL)
		req->rr_path = xstrndup(cs, len);

	/* query string */
	if ((cs = TSUrlHttpQueryGet(reqp, url, &len)) != NULL)
		req->rr_query = xstrndup(cs, len);

	/* client network address */
	req->rr_addr = TSHttpTxnClientAddrGet(txnp);

	/* request header fields */
	req->rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	for (int i = 0, end = TSMimeHdrFieldsCount(reqp, hdrs); i < end; ++i) {
	TSMLoc			 ts_field;
	remap_hdrfield_t	*remap_field;

		remap_field = calloc(1, sizeof(*remap_field));
		ts_field = TSMimeHdrFieldGet(reqp, hdrs, i);
		remap_field->rh_nvalues = TSMimeHdrFieldValuesCount(reqp, hdrs,
								    ts_field);
		remap_field->rh_values = calloc(sizeof(char *),
						remap_field->rh_nvalues);
		/* store each value */
		for (size_t j = 0; j < remap_field->rh_nvalues; ++j) {
			cs = TSMimeHdrFieldValueStringGet(reqp, hdrs, ts_field,
							  j, &len);
			remap_field->rh_values[j] = xstrndup(cs, len);
		}

		cs = TSMimeHdrFieldNameGet(reqp, hdrs, ts_field, &len);
		s = xstrndup(cs, len);
		for (int j = 0; j < len; ++j)
			s[j] = tolower(s[j]);
		hash_set(req->rr_hdrfields, s, remap_field);
		free(s);
	}

	TSHandleMLocRelease(reqp, hdrs, url);
	TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdrs);
	return 0;
}

/*
 * Build a Traffic Server URL as a TSMLoc from the given remap result.
 * Note that the URL doesn't have a host, because we use the Host: header
 * for that.
 */
TSMLoc
url_from_remap_result(TSHttpTxn txn, remap_request_t *req,
		      remap_result_t *res)
{
TSMBuffer	reqp;
TSMLoc		hdrs, newurl;

	TSHttpTxnClientReqGet(txn, &reqp, &hdrs);

	TSUrlCreate(reqp, &newurl);
	TSUrlSchemeSet(reqp, newurl, res->rz_proto, -1);
	//TSUrlPortSet(reqp, newurl, res->rz_target->rt_port);

	TSUrlPathSet(reqp, newurl, res->rz_urlpath, -1);

	if (res->rz_query)
		TSUrlHttpParamsSet(reqp, newurl, res->rz_query, -1);

	TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdrs);
	return newurl;
}

/*
 * Set or replace the Host header field on the request.
 */
void
set_host_field(TSHttpTxn txnp, const char *host)
{
TSMBuffer	reqp;
TSMLoc		hdrs, host_hdr;

	TSHttpTxnClientReqGet(txnp, &reqp, &hdrs);

	/*
	 * Set the host header.
	 */
	if ((host_hdr = TSMimeHdrFieldFind(reqp, hdrs, "Host", 4)) != NULL) {
		TSMimeHdrFieldRemove(reqp, hdrs, host_hdr);
		TSHandleMLocRelease(reqp, hdrs, host_hdr);
	}

	TSMimeHdrFieldCreateNamed(reqp, hdrs, "Host", 4, &host_hdr);
	TSMimeHdrFieldValueStringInsert(reqp, hdrs, host_hdr, 0, host, strlen(host));
	TSMimeHdrFieldAppend(reqp, hdrs, host_hdr);

	TSHandleMLocRelease(reqp, hdrs, host_hdr);
	TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdrs);
}

void
add_xfp(TSHttpTxn txn)
{
TSMBuffer	reqp;
TSMLoc		hdrs, xfp;

	TSHttpTxnClientReqGet(txn, &reqp, &hdrs);

	/* Remove any existing X-Forwarded-Proto header */
	xfp = TSMimeHdrFieldFind(reqp, hdrs,
		      REMAP_MIME_FIELD_X_FORWARDED_PROTO,
		      REMAP_MIME_FIELD_X_FORWARDED_PROTO_LEN);
	if (xfp != TS_NULL_MLOC) {
		TSMimeHdrFieldRemove(reqp, hdrs, xfp);
		TSHandleMLocRelease(reqp, hdrs, xfp);
	}

	TSMimeHdrFieldCreateNamed(reqp, hdrs,
				REMAP_MIME_FIELD_X_FORWARDED_PROTO,
				REMAP_MIME_FIELD_X_FORWARDED_PROTO_LEN,
				&xfp);

	if (TSHttpTxnClientProtocolStackContains(txn, "tls"))
		TSMimeHdrFieldValueStringInsert(reqp, hdrs, xfp, 0,
					     "https", 5);
	else
		TSMimeHdrFieldValueStringInsert(reqp, hdrs, xfp, 0,
					     "http", 4);

	TSMimeHdrFieldAppend(reqp, hdrs, xfp);

	TSHandleMLocRelease(reqp, hdrs, xfp);
	TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdrs);
}

/*
 * Add an X-Cache-Status header to this response.
 */
int
add_cache_status(TSCont contn, TSEvent event, void *edata)
{
TSHttpTxn		txn = edata;
char			buf[64];
TSMgmtInt		cache_gen = 0;
int			status;
TSMBuffer		resp;
TSMLoc			hdr, field;
static const char *const status_names[] = {
	"miss", "hit-stale", "hit-fresh", "skipped", "unknown"
};

	TSHttpTxnConfigIntGet(txn, TS_CONFIG_HTTP_CACHE_GENERATION, &cache_gen);
	TSHttpTxnCacheLookupStatusGet(txn, &status);
	if (status < 0 || status > 4)
		status = 4;

	snprintf(buf, sizeof(buf), "%s (%ld)", status_names[status],
		(long) cache_gen);

	TSHttpTxnClientRespGet(txn, &resp, &hdr);
	field = TSMimeHdrFieldFind(resp, hdr, "X-Cache-Status", -1);
	if (field == TS_NULL_MLOC)
		TSMimeHdrFieldCreateNamed(resp, hdr,
				"X-Cache-Status", -1, &field);
	TSMimeHdrFieldValueStringInsert(resp, hdr, field, 0, buf, -1);
	TSMimeHdrFieldAppend(resp, hdr, field);
	TSHandleMLocRelease(resp, hdr, field);
	TSHandleMLocRelease(resp, TS_NULL_MLOC, hdr);

	TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
	return TS_SUCCESS;
}

/*
 * A continuation to set headers on the HTTP reponse.  It expects its
 * continuation data to be a hash_t of string pairs.  This must be hooked to
 * both TS_HTTP_SEND_RESPONSE_HDR and TS_HTTP_TXN_CLOSE_HOOK to ensure the
 * data is freed.
 *
 * This also sets the Server and Via headers.
 */
int
set_headers(TSCont contp, TSEvent event, void *edata)
{
hash_t		hdrset = TSContDataGet(contp);
TSHttpTxn	txn = edata;
TSMBuffer	resp;
TSMLoc		hdrs;
TSMLoc		hdr;
const char	*h, *v;
size_t		 hlen;

	if (event == TS_EVENT_HTTP_TXN_CLOSE) {
		TSDebug("kubernetes", "set_headers: closing");
		hash_free(hdrset);
		TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
		return TS_SUCCESS;
	}

	assert(event == TS_EVENT_HTTP_SEND_RESPONSE_HDR);
	TSDebug("kubernetes", "set_headers: running");

	TSHttpTxnClientRespGet(txn, &resp, &hdrs);

	/*
	 * Set any header fields in the txn's hdrset; typically these come from
	 * the remap response.
	 */
	if (hdrset) hash_foreach(hdrset, &h, &hlen, &v) {
		TSDebug("kubernetes", "set_headers: [%.*s] = [%s]",
			(int) hlen, h, v);
		TSMimeHdrFieldCreateNamed(resp, hdrs, h, hlen, &hdr);
		TSMimeHdrFieldValueStringInsert(resp, hdrs, hdr, 0,
						v, strlen(v));
		TSMimeHdrFieldAppend(resp, hdrs, hdr);
		TSHandleMLocRelease(resp, hdrs, hdr);
	}

	/*
	 * Set Via and Server fields.
	 */
	{
	char		 via[256];
	const char	*stack[10];
	int		 stacksz;

		TSHttpTxnClientProtocolStackGet(txn, 10, &stack[0], &stacksz);
		if (stacksz > 10)
			stacksz = 10;
		snprintf(via, sizeof(via), "%s %s (%s)",
			 stacksz ? stack[0] : "http/1.0", myhostname, via_name);

		if ((hdr = TSMimeHdrFieldFind(resp, hdrs, "Via", 3)) == TS_NULL_MLOC) {
			TSMimeHdrFieldCreateNamed(resp, hdrs, "Via", 3, &hdr);
			TSMimeHdrFieldAppend(resp, hdrs, hdr);
		}
		TSMimeHdrFieldValueStringInsert(resp, hdrs, hdr, 0, via, -1);
		TSHandleMLocRelease(resp, hdrs, hdr);
	}
#if 1
	if ((hdr = TSMimeHdrFieldFind(resp, hdrs, "Server", 6)) == TS_NULL_MLOC) {
		TSMimeHdrFieldCreateNamed(resp, hdrs, "Server", 6, &hdr);
		TSMimeHdrFieldAppend(resp, hdrs, hdr);
	} else
		TSMimeHdrFieldValuesClear(resp, hdrs, hdr);
	TSMimeHdrFieldValueStringSet(resp, hdrs, hdr, -1, via_name, via_name_len);
	TSHandleMLocRelease(resp, hdrs, hdr);

#else	/* doesn't seem to work */
	TSHttpTxnConfigIntSet(txn, TS_CONFIG_HTTP_RESPONSE_SERVER_ENABLED, 1);
	TSHttpTxnConfigStringSet(txn, TS_CONFIG_HTTP_RESPONSE_SERVER_STR,
			      via_name, via_name_len);
#endif
	TSHandleMLocRelease(resp, TS_NULL_MLOC, hdrs);
	TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
	return TS_SUCCESS;
}


	/*
	 * Do HTTP/2 server push.
	 */
int
server_push(TSCont contp, TSEvent event, void *edata)
{
TSHttpTxn	txn = edata;
TSMBuffer	resp = NULL;
TSMLoc		hdr, field;

	TSDebug("kubernetes", "server_push: running");

	/*
	 * We can be called from both TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE and
	 * TS_EVENT_HTTP_READ_RESPONSE_HDR.  Pick the correct response to
	 * process (server response or cached response) to process based on the
	 * event type.
	 *
	 * In the case of a cache lookup, only process cache hits; misses
	 * will go to origin and we'll be called again later for the server
	 * response.
	 */
	if (event == TS_EVENT_HTTP_READ_RESPONSE_HDR) {
		if (TSHttpTxnServerRespGet(txn, &resp, &hdr) != TS_SUCCESS) {
			TSDebug("kubernetes", "server_push: cannot get "
				"server resp?!");
			TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
			return TS_SUCCESS;
		}
	} else {
	int	cache_status = 0;
		TSHttpTxnCacheLookupStatusGet(txn, &cache_status);

		if (cache_status != TS_CACHE_LOOKUP_HIT_FRESH) {
			TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
			return TS_SUCCESS;
		}

		if (TSHttpTxnCachedRespGet(txn, &resp, &hdr) != TS_SUCCESS) {
			TSDebug("kubernetes", "server_push: cannot get "
				"cached resp?!");
			TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
			return TS_SUCCESS;
		}
	}

	for (int i = 0, end = TSMimeHdrFieldsCount(resp, hdr); i < end; ++i) {
	const char	*cs;
	int		 len, n;
	TSMBuffer	 req;
	TSMLoc		 reqhdr;
	TSMLoc		 requrl;


		field = TSMimeHdrFieldGet(resp, hdr, i);
		cs = TSMimeHdrFieldNameGet(resp, hdr, field, &len);
		if (len != 4 || memcmp(cs, "Link", 4)) {
			TSHandleMLocRelease(resp, hdr, field);
			continue;
		}

		n = TSMimeHdrFieldValuesCount(resp, hdr, field);
		TSDebug("kubernetes", "server_push: will push %d URLs", n);

		TSHttpTxnClientReqGet(txn, &req, &reqhdr);
		TSHttpHdrUrlGet(req, reqhdr, &requrl);

		for (int i = 0; i < n; ++i) {
		char		*s, *t, *r, *url = NULL;
		TSMLoc		 pushurl;
		int		 nopush = 0, relpreload = 0;

			
			/* Fetch the Link field */
			cs = TSMimeHdrFieldValueStringGet(resp, hdr, field, i, &len);
			s = strndup(cs, len);

			for (t = strtok_r(s, "; ", &r); t != NULL;
			     t = strtok_r(NULL, "; ", &r)) {
			size_t	len;

				if (strcmp(t, "nopush") == 0)
					nopush = 1;
				else if (strcmp(t, "rel=preload") == 0)
					relpreload = 1;
				else {
					len = strlen(t);

					if (len >= 3 && t[0] == '<' &&
					    t[1] == '/' && t[len-1] == '>')
						url = strndup(t + 2, len - 3);
				}
			}
			free(s);

			if (nopush || !relpreload || !url) {
				free(url);
				continue;
			}

			/*
			 * The Link URL is relative; construct a new URL with
			 * the absolute URL based on the request URL.
			 */
			TSUrlClone(resp, req, requrl, &pushurl);
			TSUrlPathSet(resp, pushurl, url, strlen(url));
			TSDebug("kubernetes", "push header is [%.*s]", len, cs);

			/*
			 * We might have changed the request URL scheme from
			 * https to http.  Put it back to https if the client
			 * request was https.
			 */
			if (TSHttpTxnClientProtocolStackContains(txn, "tls"))
				TSUrlSchemeSet(resp, pushurl, "https", 5);

			cs = TSUrlStringGet(resp, pushurl, &len);
			TSDebug("kubernetes", "push URL is [%.*s]", len, cs);
			TSHttpTxnServerPush(txn, cs, len);

			TSHandleMLocRelease(resp, hdr, pushurl);
		}

		TSHandleMLocRelease(resp, hdr, field);
		TSHandleMLocRelease(req, reqhdr, requrl);
		TSHandleMLocRelease(req, TS_NULL_MLOC, reqhdr);
	}

	TSHandleMLocRelease(resp, TS_NULL_MLOC, hdr);
	TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
	return TS_SUCCESS;
}

int
should_ignore_cookie(hash_t globs, const char *cookie)
{
const char	*k, *p = strchr(cookie, '=');
size_t		 klen;

	if (!p)
		return 0;

	hash_foreach(globs, &k, &klen, NULL)
		if (strmatch(cookie, p, k, k + klen))
			return 1;

	return 0;
}

static void
check_cookies(TSHttpTxn txn, remap_request_t *req, remap_result_t *res,
	      int *can_cache)
{
TSMBuffer	 reqp;
TSMLoc		 hdr;
TSMLoc		 field;
char		*s, *r, *t;
const char	*cs;
int		 len;
char		*newhdr;

	TSHttpTxnClientReqGet(txn, &reqp, &hdr);
	field = TSMimeHdrFieldFind(reqp, hdr, "Cookie", 6);
	if (field == TS_NULL_MLOC) {
		/* No cookies, we can cache this */
		*can_cache = 1;
		goto cleanup;
	}

	/* For now, we cannot cache; this may change later */
	*can_cache = 0;

	cs = TSMimeHdrFieldValueStringGet(reqp, hdr, field, 0, &len);
	s = malloc(len + 1);
	bcopy(cs, s, len);
	s[len] = 0;

	newhdr = malloc(1);
	newhdr[0] = '\0';

	for (r = strtok_r(s, " ;", &t); r; r = strtok_r(NULL, " ;", &t)) {
		if (should_ignore_cookie(res->rz_path->rp_ignore_cookies, r))
			continue;
		TSDebug("kubernetes", "check_cookies: preserving this cookie [%s]",
			r);
		newhdr = realloc(newhdr, strlen(newhdr) + strlen(r) + 3);
		if (*newhdr)
			strcat(newhdr, "; ");
		strcat(newhdr, r);
	}

	free(s);

	if (*newhdr) {
		/* Set the new cookie header */
		TSMimeHdrFieldValuesClear(reqp, hdr, field);
		TSMimeHdrFieldValueStringSet(reqp, hdr, field, -1, newhdr, -1);
	} else {
		/* No cookies left; remove the header */
		TSMimeHdrFieldRemove(reqp, hdr, field);
		*can_cache = 1;
	}

	free(newhdr);

cleanup:
	if (field != TS_NULL_MLOC)
		TSHandleMLocRelease(reqp, hdr, field);
	TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdr);
}

/*
 * Check if the response can be cached, and disable caching if not.
 */
int
check_response_cache(TSCont contn, TSEvent event, void *edata)
{
TSHttpTxn	txn = edata;
TSMBuffer	resp;
TSMLoc		hdr, field;

	if (TSHttpTxnServerRespGet(txn, &resp, &hdr) != TS_SUCCESS)
		return TS_SUCCESS;

	field = TSMimeHdrFieldFind(resp, hdr, "Set-Cookie", -1);
	if (field) {
		/* Response has cookies - do not cache it */
		TSHttpTxnConfigIntSet(txn, TS_CONFIG_HTTP_CACHE_HTTP, 0);
		TSHandleMLocRelease(resp, hdr, field);
	}

	TSHandleMLocRelease(resp, TS_NULL_MLOC, hdr);
	TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
	return TS_SUCCESS;
}

/*
 * handle_remap: called in READ_REQUEST_HDR_HOOK.  Match the incoming request
 * to an Ingress path (remap_path), apply any configurations from annotations,
 * and either set the host to proxy the request to the backend, or return
 * our own error or redirect response.
 */
int
handle_remap(TSCont contn, TSEvent event, void *edata)
{
TSMLoc			 newurl;
TSHttpTxn		 txnp = (TSHttpTxn) edata;
TSConfig		 map_cfg = NULL;
const remap_db_t	*db;
struct state		*state = TSContDataGet(contn);
remap_request_t		 req;
remap_result_t		 res;
synth_t			*sy;
struct sockaddr_in	 addr;
int			 reenable = 1;
TSCont			 c, set_headers_cont;

	bzero(&req, sizeof(req));
	bzero(&res, sizeof(res));

	set_headers_cont = TSContCreate(set_headers, TSMutexCreate());
	TSContDataSet(set_headers_cont, NULL);
	TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, set_headers_cont);
	TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, set_headers_cont);

	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_NORMALIZE_AE_GZIP, 0);
	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_INSERT_RESPONSE_VIA_STR, 0);
	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_INSERT_REQUEST_VIA_STR, 1);
	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_RESPONSE_SERVER_ENABLED, 0);

	map_cfg = TSConfigGet(state->cfg_slot);
	db = TSConfigDataGet(map_cfg);

	/* Not initialised yet? */
	if (!db) {
		TSDebug("kubernetes", "handle_remap: no database");
		goto cleanup;
	}

	/* Create a remap_request from the TS request */
	if (request_from_txn(txnp, &req) != 0)
		goto cleanup;

	/* Do the remap */
	switch (remap_run(db, &req, &res)) {
	case RR_SYNTHETIC: {
		TSContDataSet(set_headers_cont, res.rz_headers);
		res.rz_headers = NULL;

		sy = synth_new(res.rz_status, res.rz_status_text);
		synth_add_header(sy, "Content-Type", "text/plain;charset=UTF-8");
		synth_set_body(sy, res.rz_body);
		synth_intercept(sy, txnp);
		goto cleanup;
	}

		/* client errors */
	case RR_ERR_INVALID_HOST:
	case RR_ERR_INVALID_PROTOCOL:
		sy = synth_new(400, "Bad request");
		synth_add_header(sy, "Content-Type", "text/plain;charset=UTF-8");
		synth_set_body(sy, "The server could not undestand this request.\r\n");
		synth_intercept(sy, txnp);
		goto cleanup;

		/* not found */
	case RR_ERR_NO_HOST:
	case RR_ERR_NO_PATH:
		/*
		 * Don't return an error here; let other plugins (like
		 * healthchecks) handle the request.
		 */
		goto cleanup;

		/* no backend */
	case RR_ERR_NO_BACKEND:
		sy = synth_new(503, "Service unavailable");
		synth_add_header(sy, "Content-Type", "text/plain;charset=UTF-8");
		synth_set_body(sy, "No backend is available to service this"
				   " request.\r\n");
		synth_intercept(sy, txnp);
		goto cleanup;

	case RR_ERR_FORBIDDEN:
		sy = synth_new(403, "Forbidden");
		synth_add_header(sy, "Content-Type", "text/plain;charset=UTF-8");
		synth_set_body(sy, "Access denied.\r\n");
		synth_intercept(sy, txnp);
		goto cleanup;

	case RR_ERR_UNAUTHORIZED:
		sy = synth_new(401, "Unauthorized");
		synth_add_header(sy, "Content-Type", "text/plain;charset=UTF-8");
		synth_set_body(sy, "Unauthorized.\r\n");
		synth_intercept(sy, txnp);
		goto cleanup;

	case RR_OK:
		break;

	default:
		goto cleanup;
	}

	/*
	 * The remap succeeded, so we need to set the new backend
	 * protocol, host:port and other request configuration.
	 */

	if ((newurl = url_from_remap_result(txnp, &req, &res)) == NULL)
		goto cleanup;
	else {
	TSMBuffer	reqp;
	TSMLoc		hdrs;

		TSHttpTxnClientReqGet(txnp, &reqp, &hdrs);
		TSHttpHdrUrlSet(reqp, hdrs, newurl);
		TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdrs);
	}

	/* Do HTTP/2 server push */
	if (res.rz_path->rp_server_push) {
		c = TSContCreate(server_push, TSMutexCreate());
		TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, c);
		TSHttpTxnHookAdd(txnp, TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, c);
	}

	/* follow redirects if configured on the Ingress.  */
	if (res.rz_path->rp_follow_redirects) {
		TSHttpTxnConfigIntSet(txnp,
				TS_CONFIG_HTTP_ENABLE_REDIRECTION, 1);
		TSHttpTxnConfigIntSet(txnp,
				TS_CONFIG_HTTP_REDIRECT_USE_ORIG_CACHE_KEY, 1);
	}

	if (res.rz_path->rp_cache) {
		c = TSContCreate(add_cache_status, TSMutexCreate());
		TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, c);
	}

	/*
	 * Send HSTS headers.
	 */
	if (res.rz_host->rh_hsts_max_age)
		TSHttpTxnConfigIntSet(txnp, TS_CONFIG_SSL_HSTS_MAX_AGE,
				      res.rz_host->rh_hsts_max_age);
	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_SSL_HSTS_INCLUDE_SUBDOMAINS,
			      res.rz_host->rh_hsts_subdomains);

	/* Set timeouts */
	if (res.rz_path->rp_read_timeout) {
		TSHttpTxnConfigIntSet(txnp,
			TS_CONFIG_HTTP_CONNECT_ATTEMPTS_TIMEOUT,
			res.rz_path->rp_read_timeout);
		TSHttpTxnConfigIntSet(txnp,
			TS_CONFIG_HTTP_POST_CONNECT_ATTEMPTS_TIMEOUT,
			res.rz_path->rp_read_timeout);
	}

	/*
	 * Usually, we want to preserve the request host header so the backend
	 * can use it.  If preserve-host is set to false on the Ingress, then
	 * we instead replace any host header in the request with the backend
	 * host.
	 */
	if (res.rz_path->rp_preserve_host)
		set_host_field(txnp, req.rr_host);
	else
		set_host_field(txnp, res.rz_target->rt_host);

	/* Add an X-Forwarded-Proto header, if configured */
	if (state->config->co_xfp)
		add_xfp(txnp);

	/* By default, do not cache */
	TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_CACHE_HTTP, 0);

	/*
	 * If caching is enabled on this path, see if we can actually cache
	 * this request.
	 */
	if (res.rz_path->rp_cache) {
	char	*cacheurl;
	size_t	 urllen;
	int	 can_cache;

		/*
		 * Removes any cookies from the request that we don't want,
		 * and removes the entire Cookie header if that leaves it
		 * empty; if so, it sets can_cache to 1.  If can_cache is
		 * 0, we should not cache this request.
		 */
		check_cookies(txnp, &req, &res, &can_cache);

		if (can_cache) {
			/*
			 * Set cache generation if it's set on the Ingress.  If it's
			 * not set, just set it to zero.
			 */
			TSHttpTxnConfigIntSet(txnp,
					TS_CONFIG_HTTP_CACHE_GENERATION,
					res.rz_path->rp_cache_gen);
			TSHttpTxnConfigIntSet(txnp, TS_CONFIG_HTTP_CACHE_HTTP, 1);

			/* Set the cache URL */
			remap_make_cache_key(&req, &res, &cacheurl, &urllen);
			TSCacheUrlSet(txnp, cacheurl, urllen);
			free(cacheurl);

			/* Check if the response should be cached */
			c = TSContCreate(check_response_cache, TSMutexCreate());
			TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, c);
		}
	}

	/*
	 * We already remapped this request, so skip any further remapping.
	 * This also prevents TS from failing the request if remap_required
	 * is set.
	 */
	TSSkipRemappingSet(txnp, 1);

	/*
	 * Set any extra response headers we need, e.g. for CORS.
	 */
	if (res.rz_headers) {
		TSContDataSet(set_headers_cont, res.rz_headers);
		/* prevent the hash being freed later */
		res.rz_headers = NULL;
	}

	/*
	 * Compress the response if enabled.  Do this here so we don't waste
	 * time compressing any of the tiny error responses that might have
	 * been generated above.
	 */
	if (res.rz_path->rp_compress)
		tsi_compress(res.rz_path, txnp);

	/*
	 * If the target is an IP address (the usual case) we can pass it
	 * to TS directly.
	 */
	bzero(&addr, sizeof(addr));
	if (inet_pton(AF_INET, res.rz_target->rt_host, &addr.sin_addr) == 1) {
		addr.sin_family = AF_INET;
		addr.sin_port = htons(res.rz_target->rt_port);
		TSHttpTxnServerAddrSet(txnp, (struct sockaddr *) &addr);
	} else {
		/*
		 * We have a DNS name, so we need to do a host lookup to get the
		 * IP address.
		 */
		if ((c = TSContCreate(external_lookup, TSMutexCreate())) == NULL) {
			/* Well, that's unfortunate. */
			TSDebug("kubernetes", "[%s] cannot create continuation",
				res.rz_target->rt_host);
			goto cleanup;
		}

		TSContDataSet(c, txnp);
		TSHostLookup(c, res.rz_target->rt_host,
			     strlen(res.rz_target->rt_host));
		TSDebug("kubernetes", "[%s]: starting external name lookup",
			res.rz_target->rt_host);
		reenable = 0;
	}

cleanup:
	TSConfigRelease(state->cfg_slot, map_cfg);

	remap_request_free(&req);
	remap_result_free(&res);

	if (reenable)
		TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);

	return TS_SUCCESS;
}
