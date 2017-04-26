/* vim:set sw=8 ts=8 noet: */
/*
 * Copyright (c) 2016-2017 Torchbox Ltd.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<errno.h>
#include	<ctype.h>

#include	<ts/ts.h>

#include	"config.h"

void
k8s_config_free(k8s_config_t *cfg)
{
	if (!cfg)
		return;

	free(cfg->co_host);
	free(cfg->co_tls_certfile);
	free(cfg->co_tls_keyfile);
	free(cfg->co_tls_cafile);
}

k8s_config_t *
k8s_config_load(const char *file)
{
char		 line[1024];
FILE		*f = NULL;
k8s_config_t	*ret = NULL;
int		 lineno = 0;

	if ((ret = calloc(1, sizeof(*ret))) == NULL) {
		TSError("calloc: %s", strerror(errno));
		goto error;
	}

	ret->co_tls = 1;
	ret->co_remap = 1;
	ret->co_port = 443;

	/*
	 * Check for in-cluster service account config.
	 */
	if ((f = fopen(SA_TOKEN_FILE, "r")) != NULL) {
		if (fgets(line, sizeof(line), f) != NULL) {
		char	*e;
			while (strchr("\r\n", line[strlen(line) - 1]))
				line[strlen(line) - 1] = '\0';

			ret->co_token = strdup(line);
			if ((e = getenv("KUBERNETES_SERVICE_HOST")) != NULL)
				ret->co_host = strdup(e);
			else
				ret->co_host = strdup("kubernetes.default.svc");
			if ((e = getenv("KUBERNETES_SERVICE_PORT")) != NULL)
				ret->co_port = atoi(e);
		}

		fclose(f);
		f = NULL;
	}

	if (file == NULL)
		return ret;

	if ((f = fopen(file, "r")) == NULL) {
		TSError("%s: %s", file, strerror(errno));
		goto error;
	}

	while (fgets(line, sizeof(line), f)) {
	char	*opt = line, *value = NULL, *s = line + strlen(line);

		++lineno;

		while (s >= line && strchr("\r\n", *s))
			*s-- = '\0';

		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = '\0';

		while (isspace(*opt))
			opt++;

		if (*opt == '#' || *opt == '\0')
			continue;

		if ((value = strchr(opt, ':')) != NULL) {
			*value++ = '\0';
			while (isspace(*value))
				value++;
		}

		if (strcmp(opt, "server") == 0) {
		char	*port;
			free(ret->co_host);
			ret->co_host = strdup(value);
			if ((port = strchr(ret->co_host, ':')) != NULL) {
				*port++ = '\0';
				ret->co_port = atoi(port);
			} else
				ret->co_port = 443;
		} else if (strcmp(opt, "token") == 0) {
			free(ret->co_token);
			ret->co_token = strdup(value);
		} else if (strcmp(opt, "certfile") == 0) {
			free(ret->co_tls_certfile);
			ret->co_tls_certfile = strdup(value);
		} else if (strcmp(opt, "keyfile") == 0) {
			free(ret->co_tls_keyfile);
			ret->co_tls_keyfile = strdup(value);
		} else if (strcmp(opt, "cafile") == 0) {
			free(ret->co_tls_cafile);
			ret->co_tls_cafile = strdup(value);
		} else if (strcmp(opt, "tls") == 0) {
			if (strcmp(value, "true") == 0)
				ret->co_tls = 1;
			else if (strcmp(value, "false") == 0)
				ret->co_tls = 0;
			else {
				TSError("%s:%d: expected \"true\" or \"false\"",
					file, lineno);
				goto error;
			}
		} else if (strcmp(opt, "remap") == 0) {
			if (strcmp(value, "true") == 0)
				ret->co_remap = 1;
			else if (strcmp(value, "false") == 0)
				ret->co_remap = 0;
			else {
				TSError("%s:%d: expected \"true\" or \"false\"",
					file, lineno);
				goto error;
			}
		} else {
			TSError("%s:%d: unknown option \"%s\"",
				file, lineno, opt);
			goto error;
		}
	}

	if (ret->co_tls_keyfile)
		free(ret->co_token);

	if (ret->co_tls_keyfile && !ret->co_tls_certfile) {
		TSError("%s: must specify certfile with keyfile", file);
		goto error;
	}

	if (ret->co_tls_certfile && !ret->co_tls_keyfile) {
		TSError("%s: must specify keyfile with certfile", file);
		goto error;
	}

	if (!ret->co_token && !ret->co_tls_keyfile) {
		TSError("%s: must specify either (keyfile, certfile) or token",
			file);
		goto error;
	}

	if (!ret->co_host) {
		TSError("%s: must specify server", file);
		goto error;
	}

	return ret;
error:
	if (f)
		fclose(f);
	k8s_config_free(ret);
	return NULL;
}
