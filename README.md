Apache Traffic Server ingress controller for Kubernetes
=======================================================

**WARNING: This is alpha code, do not use it in production.**

This is a Kubernetes ingress controller plugin for
[Apache Traffic Server](https://trafficserver.apache.org/).  It allows Traffic
Server to act as an ingress controller for Kubernetes clusters, routing incoming
requests to pods while providing TLS termination, caching, ESI and other standard
Traffic Server features.

The plugin is only supported with Traffic Server 7.0+.  It may work with 6.x
versions, but has not been tested.  It definitely won't work with 5.x or older
versions without modifications, as the plugin API has changed.

Depending on Traffic Server version, the following protocols are supported:

* HTTP/1.0, HTTP/1.1, HTTP/2
* WebSockets (ws and wss)
* TLSv1, TLSv1.1, TLSv1.2

Web socket support is transparent; an incoming websocket request will be routed
directly to the backend.

Advanced HTTP/2 features like server push are not currently supported.

Why Traffic Server?
-------------------

The primary reason to use TS as an Ingress controller is caching.  TS provides
highly configurable, fast and well-tested memory- and disk-based caching of HTTP
requests.  Using caching allows an application or cluster to serve significantly
higher request load than it could if the application had to respond to every
request itself.

This is true even if the application has its own caching layer; with a sufficiently
high cache hit ratio, a 12-CPU single machine running Traffic Server can serve
requests at upwards of 40Gbps.  Traffic Server was developed by Yahoo! for use
in high traffic environments, and is used by other large sites such as Akamai,
LinkedIn, and Comcast.

A secondary reason to use TS is its plugin support; it has a stable and
well-developed plugin API allowing plugins (like this one) to extend its
functionality.

Building
--------

Requirements:

* A working C compiler and `make` utility.
* json-c library
* OpenSSL (or a compatible TLS library, e.g. LibreSSL)

Build and install the plugin:

```
$ ./configure [--with-tsxs=/path/to/trafficserver/bin/tsxs]
$ make
# make install
```

Configuration
-------------

Run `make install` to install the plugin, or copy `kubernetes.so` to the
Traffic Server plugin directory (e.g.  `/usr/local/libexec/trafficserver`).
Edit `plugin.config` to load the plugin.

If Traffic Server is running inside the cluster, no configuration is required;
it will pick up its service account details automatically.

Otherwise, copy `kubernetes.config.example` to the Traffic Server configuration
directory as `kubernetes.config` and edit it for your site.

Debugging
---------

To debug problems with the plugin, enable the debug tags `kubernetes` (for the
plugin itself) or `watcher` (for the Kubernetes API code).

Docker configuration
--------------------

If you're using the pre-built Docker image, you can set the following environment
variables to configure Traffic Server:

* `TS_CACHE_SIZE`: Size of the on-disk cache file to create, in megabytes.

In addition, any TS configuration (records.config) setting can be
overridden in the environment:

https://docs.trafficserver.apache.org/en/latest/admin-guide/files/records.config.en.html#environment-overrides

For persistent cache storage, mount a volume on `/var/lib/trafficserver`.
This can be a persistent volume or an emptyDir; any missing necessary files,
including the cache store, will be created at startup.

Ingress annotations
-------------------

The behaviour of an Ingress can be configured by setting annotations on the
resource.  Annotations beginning with `ingress.kubernetes.io` are standard
annotations supported by most Ingress controllers; those beginning with
`ingress.torchbox.com` are specific to the Traffic Server Ingress controller.

* `ingress.kubernetes.io/rewrite-target`: if set to a string, the portion of the
  request path matched by the Ingress `path` attribute will be replaced with
  this string.  This has no effect on an Ingress without a `path` set.

* `ingress.kubernetes.io/app-root`: if set to a path prefix, and the request URI
  does not begin with that prefix, then a redirect will be returned to this
  path.  This can be used for applications which sit in a subdirectory rather
  than at the root.

* `ingress.torchbox.com/follow-redirects`: if `"true"`, Traffic Server will
  follow 3xx redirect responses and serve the final response to the client.
  If the redirect destination is cached, it will be cached with the cache key
  of the original request.  Redirects will only be followed to other Ingress
  resources, not to arbitrary destinations (but see below about proxying to
  external resources).

* `ingress.torchbox.com/preserve-host`: if `"false"`, set the `Host` header
  in the request to the backend name (e.g., the pod name), instead of the
  original request host.

TLS
---

TLS keys and certificates are taken from Kubernetes Secret resources according
to `tls` attribute of each Ingress resource.  TLS Server Name Indication support
is required for this to work; clients without SNI support will receive a TLS
negotiation error.

If you don't want to use Kubernetes for TLS, set `tls: false` in
`kubernetes.config`.  You will need to provide TLS configuration some other way,
like `ssl_multicert.config` or the `ssl-cert-loader` plugin, or else terminate
TLS before traffic reaches Traffic Server.

By default, non-TLS HTTP requests to an Ingress host with TLS configured will
be 301 redirected to HTTPS.  To disable this behaviour, set the
`ingress.kubernetes.io/ssl-redirect` annotation to `false`.

To force a redirect to HTTPS even when TLS is not configured on the Ingress, set
the `ingress.kubernetes.io/force-ssl-redirect` annotation to `true`.  This will
not work unless you are offloading TLS termination in front of Traffic Server.

Usually, communication between Traffic Server and backends (e.g. pods) is via
non-TLS HTTP, even if the request was made over HTTPS.  To use HTTPS to
communicate with the backend, set the `ingress.kubernetes.io/secure-backends`
annotation to `true`.  This is not very useful when the backend is a pod,
because TS connects to the pod by its IP address, and it's extremely unlikely
the pod will have a TLS certificate for that IP address.  However, this can be
useful when using external proxying (described below).

A better method to secure traffic between Traffic Server and pods is to use a
network CNI plugin that supports encryption, such as Weave Net.

To enable HTTP Strict Transport Security (HSTS), set the 
`ingress.torchbox.com/hsts-max-age` annotation on the Ingress to the HSTS
max-age time in seconds.  To be useful, this should be set to at least six
months (15768000 seconds), but you should start with a lower value and gradually
increase it.  Do not set it to a large value without testing it first, because,
by design, it cannot be turned off for browsers that already saw the HSTS
header until the max-age expires.

HSTS headers are per-hostname, not per-host.  Therefore, `hsts-max-age` can only
be set on the Ingress that includes the root path for a particular hostname
(i.e., where the Ingress rule has no `path` attribute).

To apply HSTS to subdomains as well, set the
`ingress.torchbox.com/hsts-include-subdomains` annotation.

Caching
-------

Traffic Server will cache HTTP responses according to their `Cache-Control`
and/or `Expires` headers.  `Cache-Control` is the recommended method of
configuring caching, since it's much more flexible than `Expires`.

Ingress annotations can be used to configure caching.  To disable caching
entirely, set the `ingress.torchbox.com/cache-enable` annotation to `false`.

You can purge individual URLs from the cache by sending an HTTP `PURGE` request
to Traffic Server.  To make this easy to do from pods, create a Service for the
TS pod.  The PURGE request should look like this:

```
PURGE http://www.mysite.com/mypage.html HTTP/1.0
```

Unfortunately, this doesn't work very well when multiple copies of TS are
running, since there's no simple way for an application to retrieve the list of
TS instances.  We plan to release our internal purge multiplexor service called
"multipurger" which solves this problem.

Occasionally, you might want to clear the cache for an entire domain, for
example if some boilerplate HTML has changed that affects all pages.  To do this,
set the `ingress.torchbox.com/cache-generation` annotation to a non-zero
integer.  This changes the cache generation for the Ingress; any objects cached
with a different generation are no longer visible, and have been effectively
removed from the cache.  Typically the cache generation would be set to the
current UNIX timestamp, although any non-zero integer will work.

Authentication
--------------

To enable password authentication, set the `ingress.kubernetes.io/auth-type`
annotation on the Ingress to `basic`, and `ingress.kubernetes.io/auth-secret`
to the name of a secret which contains an htpasswd file as the `auth` key.  You
can create such a secret with `kubectl`:

```
$ kubectl create secret generic my-auth --from-file=auth=my-htpasswd
```

Optionally, set `ingress.kubernetes.io/auth-realm` to the basic authentication
realm, which is displayed in the password prompt by most browsers.

Most common password hash schemes are supported, including DES, MD5 (`$1$` and
`$apr1$`), bcrypt (`$2[abxy]$`), SHA-256 (`$5$`) and SHA-512 (`$6$`), and four
RFC2307-style hashes: `{PLAIN}`, `{SHA}`, `{SSHA}` and `{CRYPT}` (the first
three of which are also supported by nginx; `{CRYPT}` is supported by OpenLDAP,
but is somewhat redundant since it's handled by simply removing the `{CRYPT}`
prefix and treating it as a normal crypt hash).

Security-wise, although the MD5 schemes are extremely weak as password hashes,
they are probably fine for any situation where htpasswd-based authentication is
in use.  The primary security improvement in newer algorithms (e.g. bcrypt and
SHA-2) is they are slower, which increases the time required to perform an
offline brute force attack; however, this also increases the time required to
_check_ the password, which leads to unacceptable delays on typical HTML page
loads.

For example, if you use a bcrypt configuration that takes 200ms to check one
hash, and you load an HTML page with 20 assets, then you will spend 4 seconds
doing nothing but waiting for authentication.  If multiple users are loading
pages at the same time, then things will be even slower once you run out of
CPUs.

If you need stronger password security than MD5, you should stop using HTTP
basic authentication and use another authentication method (like Cookie-based
authentication) instead.

External proxying
-----------------

Sometimes, you might want to proxy traffic to a service that doesn't run as a
Kubernetes pod.  This can be used to expose external services via an Ingress,
and to allow the `follow-redirects` annotation to access external resources.

### External proxying via IP address

To proxy requests to a particular IP address or a set of IP address, create a
`Service` resource without a selector, and create its associated `Endpoints`
resource:

```
kind: Service
apiVersion: v1
metadata:
  name: external-service
spec:
  ports:
  - protocol: TCP
    port: 80

---

kind: Endpoints
apiVersion: v1
metadata:
  name: external-service
subsets:
- addresses:
  - ip: 1.2.3.4
  ports:
  - port: 80
```

You can now define an Ingress to route traffic to this service:

```
apiVersion: extensions/v1beta1
kind: Ingress
metadata:
  name: external-ingress
spec:
  rules:
  - host: external.example.com
    http:
      paths:
      - backend:
          serviceName: external-service
          servicePort: 80
```

Traffic Server will now route requests for http://external.example.com/ to your
external service at IP address 1.2.3.4.

### External proxying via hostname

To proxy to an external hostname, create a `Service` resource of type
`ExternalName`:

```
kind: Service
apiVersion: v1
metadata:
  name: external-service
spec:
  type: ExternalName
  externalName: my-external-backend.example.com
```

Do not configure an `Endpoints` resource.  Create an Ingress for this Service:

```
apiVersion: extensions/v1beta1
kind: Ingress
metadata:
  name: external-ingress
  annotations:
    ingress.torchbox.com/preserve-host: "false"
spec:
  rules:
  - host: external.example.com
    http:
      paths:
      - backend:
          serviceName: external-service
          servicePort: 80
```

Now requests for http://external.example.com will be proxied to
http://my-external-backend.example.com.

When using an ExternalName Service, the `servicePort` must be an integer;
named ports are not supported.

In most cases, you will want to set the `preserve-host` annotation to `"false"`
so that the external service sees the hostname it's expecting, rather than the
hostname in the client request.

### External proxying and TLS

By default, even if a request uses TLS, it will be proxied to the external
backend via HTTP.  To use TLS for the backend, set an annotation on the Ingress:
`ingress.kubernetes.io/secure-backends: "true"`.  This is not very useful for
external IP addresses, because it's unlikely the backend will have a TLS
certificate for its IP address, but it will work well with `ExternalName`
services.

For TLS to work, remember to set `servicePort` to `443` (or some other suitable
value).

Support
-------

Please open a [Github issue](https://github.com/torchbox/k8s-ts-ingress/issues)
for questions or support, or to report bugs.


License
-------

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

`hash.c` contains code written by Landon Curt Noll, which is released into the
public domain.

`base64.c` is copyright (c) 2011-2017 Felicity Tarnell.
