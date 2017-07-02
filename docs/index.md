# Traffic Server ingress controller

*Note*: This software is still in development.  While we are using it internally,
we do not recommend deploying it in a production cluster at this time.

[Traffic Server](https://trafficserver.apache.org/) is a high-performance,
extensible HTTP proxy server with a rich feature set, including TLS termination,
caching, and edge-side includes (ESI).  This plugin allows Traffic Server to
act as an [Ingress](https://github.com/kubernetes/ingress) controller for
[Kubernetes](https://kubernetes.io) clusters, providing the reverse proxy that
allows HTTP requests from the Internet to reach Kubernetes pods.

The controller is provided as C source code and as a pre-built Docker image.
If you want to add Kubernetes support to an existing instance of TS, you should
build the plugin from source.  If you want to deploy TS inside a Kubernetes
cluster, you can use the pre-built Docker image.

## Quick start

To deploy the image on an existing Kubernetes 1.6 (or later) cluster:

```sh
$ curl -L https://raw.githubusercontent.com/torchbox/k8s-ts-ingress/master/example-rbac.yaml | kubectl apply -f -
$ curl -L https://raw.githubusercontent.com/torchbox/k8s-ts-ingress/master/example-deployment.yaml | kubectl apply -f -
```

(If you're using 1.5 or earlier, you can still use `example-deployment.yaml`,
but if you need RBAC support you will need to convert `example-rbac.yaml` to use
the old alpha RBAC API.)

This will start two copies of Traffic Server, each with an emptyDir volume for
cache storage, listening on node ports 30080 (http) and 30443 (https).   You can
configure an external load balancer of some sort to route incoming traffic to
those ports, or use Kubernetes'
[keepalived-vip](https://github.com/kubernetes/contrib/tree/master/keepalived-vip)
to manage a virtual IP address on your cluster.

For more detailed installation instructions, see the documentation for
[building from source](source.md) or [Deploying on Kubernetes](docker.md).

## Known bugs

* A Traffic Server bug
  ([GH #2076](https://github.com/apache/trafficserver/issues/2076)) causes
  incorrect processing of HTTP/2 PATCH requests which use chunked transfer 
  encoding.  Requests of this type are very unusual, but `docker push` has been
  observed to use them.  As a result, pushing to a private registry hosted
  behind the TS Ingress controller will fail with 503 Internal Server Error.
  Workaround: set the annotation `ingress.kubernetes.io/http2-enable: "false"`
  to disable HTTP/2 on the affected Ingress, or set
  `$PROXY_CONFIG_HTTP2_ENABLED` to `"0"` to disable HTTP/2 globally.

## Release history

* 1.0.0-alpha10 (unreleased):
    * Bug fix: if a domain was specified in `domain-access-list`, a later match
        for `*` or the same domain would be ignored.

* 1.0.0-alpha9:
    * Incompatible change: the Docker image now listens on ports 80 and 443 by
        default, instead of 7080 and 7443.
    * Feature: the `tls-minimum-version` annotation was implemented.
    * Feature: the global ConfigMap was implemented.
    * Feature: the `tls-certificates` ConfigMap option was implemented.
    * Feature: built-in TS healthchecks were implemented.
    * Feature: the `domain-access-list` ConfigMap option was implemented.
    * Improvement: the Docker image now sends log output to stdout, as
        expected in Docker.
    * Bug fix: a request for `/` on an Ingress which had `rewrite-target` set on
        its default path would crash.
    * Bug fix: annotations which are host-specific rather than path-specific
        (such as `hsts-max-age`) could be set wrongly.
    * Bug fix: if TS received an HTTP request before it had finished syncing the
        initial cluster state, it would leak memory.
    * Improvement: The plugin was added to Coverity SCAN.  As a result, several
        minor bugs were fixed and code quality was improved.

* 1.0.0-alpha8:
    * Incompatible change: The CORS configuration was changed to be both more
        clear and more flexible.
    * Feature: HTTP/2 Server Push was implemented.
    * Feature: the `cache-whitelist-cookies` annotation was implemented.
    * Feature: the `debug-log` annotation was implemented.
    * Feature: the `http2-enable` annotation was implemented.
    * Improvement: Two unnecessary plugins were removed from the Docker image
        configuration (header_rewrite and xdebug).
    * Improvement: End-to-end test coverage was improved; as a result, several
        bugs were fixed:
        * A 401 Unauthorised response did not include a WWW-Authenticate header.
        * A response including both `Set-Cookie` and `Cache-Control` header
          fields would be cached even though responses containing `Set-Cookie`
          fields should not be cached.
        * The `cache-ignore-query-params` and `cache-whitelist-query-params`
          annotations did not work correctly.
    * Improvement: The Kubernetes API code was rewritten and is now more
        reliable, more efficient, and faster to respond to changes in the
        cluster.
    * Bug fix: Several memory leaks were fixed.
    * Bug fix: TS would crash if the connection to the API server failed.
    * Bug fix: Path handling code could crash with non-default paths.
    * Bug fix: If no configuration file was specified, environment-based
        configuration would not be loaded either.
    * Bug fix: Compilation would fail if the C compiler did not enable
        C99 by default.

* 1.0.0-alpha7:
    * Improvement: The Traffic Server version in the Docker image has been
        upgraded from 7.0.0 to 7.1.x (prerelease).
    * Improvement: The hash tree implementation has been replaced with a radix
        tree, reducing memory use for small clusters and providing better (and
        more predictable) performance for large clusters.
    * Bug fix: A synthetic response could cause a crash.
    * Bug fix: An incorrect Cookie header could be sent to the origin.
    * Bug fix: An incorrect Content-Encoding header could be sent to the client
        if the client supported compression but the response object was not
        compressed.

* 1.0.0-alpha6:
    * Incompatible change: The behaviour of the `app-root` annotation was
        changed to match the behaviour of other Ingress controllers.
    * Incompatible change: Several annotations were moved from
        `ingress.torchbox.com` to `ingress.kubernetes.io` to improve
         compatibility among Ingress controllers.
    * Feature: The `ingress.kubernetes.io/read-response-timeout` annotation
        was implemented.
    * Feature: CORS annotations were implemented.
    * Feature: HTTP compression was implemented.
    * Feature: For Ingress resources with caching enabled, an `X-Cache-Status`
        header is returned in the response, indicating whether the request was
        cached and the current cache generation.
    * Feature: The `cache-ignore-cookies` annotation was implemented.
    * Bug fix: With certain combinations of OpenSSL and Traffic Server versions,
        a TLS request for an unknown host could hang indefinitely instead of
        returning an error.

* 1.0.0-alpha5:
    * Incompatible change: The `ingress.torchbox.com/auth-address-list`
        annotation was renamed to `ingress.torchbox.com/whitelist-source-range`,
        and is now comma-delimited, for compatibility with other Ingress
        controllers.
    * Feature: Support Ingress classes.
    * Feature: The X-Forwarded-Proto header is now (optionally) sent to the
        backend.
    * Feature: The `cache-whitelist-params` and `cache-ignore-params`
        annotations were implemented.
    * Feature: The `tls_verify` configuration option was added.
    * Improvement: The API server connection code was reimplemented using cURL,
        making it more reliable and featureful.
    * Bug fix: TLS redirects with an empty URL path could crash.
    * Bug fix: TLS secret handling could leak memory.
    * Bug fix: with some combinations of Traffic Server and OpenSSL versions,
        TLS certificates might not be loaded correctly.  Use the new
        TS_SSL_CERT_HOOK hook to ensure this works properly in all cases.
    * Bug fix: An Endpoints with more than one port or address could be parsed
        incorrectly or cause a crash.

* 1.0.0-alpha4:
    * Do not return a client error if the requested host or path was not
      found, to allow use with other plugins like healthchecks.

* 1.0.0-alpha3:
    * Greatly improved unit test coverage.
    * Several minor bugs fixed.
    * Support configuration via environment variables.

* 1.0.0-alpha2: Implement IP address authentication.
* 1.0.0-alpha1: Initial release.

## License and credits

This plugin was developed by Felicity Tarnell (ft@le-Fay.ORG) for
[Torchbox Ltd.](https://torchbox.com).  Copyright (c) 2016-2017 Torchbox Ltd.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely. This software is provided 'as-is', without any express or implied
warranty.

----

`crypt_bf.c` was written by Solar Designer, and is released into the public
domain.

`crypt_des.c` is copyright (c) 1989, 1993 The Regents of the University of
California, based on code written by Tom Truscott.

`crypt_md5.c` was written by Poul-Henning Kamp, and is released under the
"beer-ware" license.

`crypt_sha256.c` and `crypt_sha512.c` were written by Ulrich Drepper, and are
released into the public domain.

`strmatch.c` is copyright (c) 1989, 1993, 1994 The Regents of the University
of California, based on code written by Guido van Rossum.

`contrib/rax` is Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>

`contrib/brotli` is Copyright 2013 Google Inc.

`base64.c` is copyright (c) 2011-2017 Felicity Tarnell.
