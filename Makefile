
TSDIR?=		/usr/local/trafficserver
CGO_CPPFLAGS?=	-I${TSDIR}/include

REMAP_SRC=	remap.go			\
		remap_controller.go

TLS_SRC=	tls.go tls_plugin.go

default: kubernetes_remap.so kubernetes_tls.so

kubernetes_remap.so: ${REMAP_SRC}
	CGO_CPPFLAGS="${CGO_CPPFLAGS}" go build -o kubernetes_remap.so -buildmode=c-shared ${REMAP_SRC}

kubernetes_tls.so: ${TLS_SRC}
	CGO_CPPFLAGS="${CGO_CPPFLAGS}" go build -o kubernetes_tls.so -buildmode=c-shared ${TLS_SRC}

build:
	#docker build --pull -t torchbox/trafficserver-ingress-controller:latest .

push:
	docker push torchbox/trafficserver-ingress-controller:latest
