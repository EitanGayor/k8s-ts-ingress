/* vim:set sw=8 ts=8 noet: */
/*
 * Copyright (c) 2016-2017 Torchbox Ltd.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

/*
 * Tests for remap_db.c: the remap database.
 */

#include	<sys/types.h>
#include	<sys/socket.h>
#include	<netinet/in.h>
#include	<arpa/inet.h>

#include	<string>
#include	<vector>
#include	<map>
#include	<cstring>

#include	"remap.h"

#include	"gtest/gtest.h"
#include	"tests/test.h"

using std::vector;
using std::map;
using std::string;
using std::pair;

namespace {
	cluster_t *load_test_ingress(std::string const &fname) {
		json_object *obj;

		cluster_t *cluster = cluster_make();
		namespace_t *ns = cluster_get_namespace(cluster, "default");

		obj = test_load_json("tests/endpoints.json");
		namespace_put_endpoints(ns, endpoints_make(obj));
		json_object_put(obj);

		obj = test_load_json("tests/service.json");
		namespace_put_service(ns, service_make(obj));
		json_object_put(obj);

		obj = test_load_json("tests/secret-htauth.json");
		namespace_put_secret(ns, secret_make(obj));
		json_object_put(obj);

		obj = test_load_json(fname);
		namespace_put_ingress(ns, ingress_make(obj));
		json_object_put(obj);

		return cluster;
	}

	remap_hdrfield_t *
	make_hdr_field(const char *value)
	{
		remap_hdrfield_t *ret;
		ret = (remap_hdrfield_t *)calloc(1, sizeof(*ret));
		ret->rh_nvalues = 1;
		ret->rh_values = (char **)calloc(sizeof(char *), 1);
		ret->rh_values[0] = strdup(value);
		return ret;
	}

} // anonymous namespace

TEST(RemapDB, PathLookup)
{
	vector<string> paths{
		"/foo",
		"/bar/.*/baz",
	};

	vector<pair<string, string>> path_tests{
		/* Note that when looking up paths, there is no leading '/' */

		/* path			should match */
		{ "foo",		"/foo"		},
		{ "bar",		"<default>",	},
		{ "bar/foo",		"<default>"	},
		{ "bar/foo/baz",	"/bar/.*/baz"	},
	};

	remap_host_t *host = remap_host_new();
	scoped_c_ptr<remap_host_t *> host_(host, remap_host_free);

	/*
	 * Add some paths to the database.  We (mis)use rp_app_root to record what
	 * the actual path is.
	 */
	for (string path: paths) {
		remap_path_t *rp = remap_host_new_path(host, path.c_str());
		ASSERT_TRUE(rp != NULL)
			<< "creating path [" << path << "]";

		rp->rp_app_root = strdup(path.c_str());
	}

	/* Mark the default path */
	remap_path_t *defpath = remap_host_get_default_path(host);
	defpath->rp_app_root = strdup("<default>");

	/*
	 * For each test vector, ensure it does or doesn't match.
	 */
	for (auto test: path_tests) {
		remap_path_t *rp;

		rp = remap_host_find_path(host, test.first.c_str(), nullptr);
		EXPECT_STREQ(test.second.c_str(), rp->rp_app_root);
	}
}

TEST(RemapDB, HostLookup)
{
	vector<pair<string, remap_host_t *>> hosts{
		{ "example.com",			nullptr },
		{ "foo.example.com",			nullptr },
		{ "bar.foo.example.com",		nullptr },
		{ "example.com.otherdomain.com",	nullptr },
	};

	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_new(cfg);

	for (auto &host: hosts)
		host.second = remap_db_get_or_create_host(db, host.first.c_str());

	for (auto &host: hosts)
		EXPECT_EQ(host.second, remap_db_get_host(db, host.first.c_str()));

	remap_db_free(db);
	k8s_config_free(cfg);
}

TEST(RemapDB, Basic)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-basic.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, EmptyPath)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-basic.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = NULL;

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, EmptyHost)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-no-host.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.whatever");
	req.rr_path = NULL;

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_ERR_NO_HOST, ret);

#if 0	/* Until null host is supported */
	/* Make sure pick_target returns the right host */
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
#endif
}

TEST(RemapDB, IngressClass1)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-class1.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, IngressClass2)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-class2.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_NO_HOST, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, IngressClass3)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-class2.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);
	cfg_set_ingress_classes(cfg, "nginx");

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, ForceTLSRedirect)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-force-tls.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_SYNTHETIC, ret);
	const char *s = (const char *)hash_get(res.rz_headers, "Location");
	EXPECT_STREQ("https://echoheaders.gce.t6x.uk/what/ever", s);
	EXPECT_EQ(301, res.rz_status);
}

TEST(RemapDB, ForceTLSRedirectEmptyPath)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-force-tls.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = NULL;

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_SYNTHETIC, ret);
	const char *s = (const char *)hash_get(res.rz_headers, "Location");
	EXPECT_STREQ("https://echoheaders.gce.t6x.uk/", s);
	EXPECT_EQ(301, res.rz_status);
}

TEST(RemapDB, AppRoot1)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-app-root.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_OK, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AppRoot2)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-app-root.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = NULL;

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_SYNTHETIC, ret);
	const char *s = (const char *)hash_get(res.rz_headers, "Location");
	EXPECT_STREQ("/app/", s);
	EXPECT_EQ(301, res.rz_status);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AppRoot3)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-app-root.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("app/foo");

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_OK, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, RewriteTarget)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-rewrite-target.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("app/bar", res.rz_urlpath);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, SecureBackends)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-secure-backends.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);

	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("https", res.rz_proto);
}

TEST(RemapDB, AuthAddressPermit)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-address.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, AuthAddressDeny)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-address.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "10.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_FORBIDDEN, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AuthBasicPermit)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-basic.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);
	
	hash_set(req.rr_hdrfields, "authorization",
		 make_hdr_field("Basic cGxhaW50ZXN0OnBsYWludGVzdA=="));

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, AuthBasicDenyNoCredentials)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-basic.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "10.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_UNAUTHORIZED, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AuthBasicDenyInvalidCredentials)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-basic.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "10.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);
	hash_set(req.rr_hdrfields, "authorization",
		 make_hdr_field("Basic cGxhaW50ZXN0OnBsYWlueHRlc3Q="));

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_UNAUTHORIZED, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AuthAllPermit)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-all.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);
	hash_set(req.rr_hdrfields, "authorization",
	         make_hdr_field("Basic cGxhaW50ZXN0OnBsYWludGVzdA=="));

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);
	EXPECT_STREQ("172.28.35.130", res.rz_target->rt_host);
	EXPECT_EQ(8080, res.rz_target->rt_port);
	EXPECT_STREQ("http", res.rz_proto);
}

TEST(RemapDB, AuthAllDenyNoCredentials)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-all.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_UNAUTHORIZED, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AuthAllDenyInvalidCredentials)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-all.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);
	hash_set(req.rr_hdrfields, "authorization",
	         make_hdr_field("Basic cGxhaW50ZXN0OnBsYWlueHRlc3Q="));

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_UNAUTHORIZED, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, AuthAllDenyInvalidAddress)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-auth-all.json");
	k8s_config_t *cfg = k8s_config_new();
	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);

	/* Build a request */
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	inet_pton(AF_INET, "10.0.0.1", &sin.sin_addr);

	remap_request_t req;
	memset(&req, 0, sizeof(req));
	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("foo/bar");
	req.rr_addr = reinterpret_cast<struct sockaddr *>(&sin);
	hash_set(req.rr_hdrfields, "authorization",
	        make_hdr_field("Basic cGxhaW50ZXN0OnBsYWludGVzdA=="));

	remap_result_t res;
	memset(&res, 0, sizeof(res));

	int ret = remap_run(db, &req, &res);
	EXPECT_EQ(RR_ERR_FORBIDDEN, ret);

	remap_request_free(&req);
	remap_result_free(&res);
	remap_db_free(db);
	k8s_config_free(cfg);
	cluster_free(cluster);
}

TEST(RemapDB, QueryString)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-basic.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");
	req.rr_query = strdup("a=1&b=2&c=3");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ(res.rz_query, "a=1&b=2&c=3");
}

TEST(RemapDB, QueryStringIgnore)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-ignore-params.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");
	req.rr_query = strdup("foo=1&bar=2&baz=3&quux=4&xyzzy=5");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ(res.rz_query, "quux=4&xyzzy=5");
}

TEST(RemapDB, QueryStringWhitelist)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-whitelist-params.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");
	req.rr_query = strdup("foo=x&bar=2&baz=3&quux=4&xyzzy=5");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ(res.rz_query, "quux=4&xyzzy=5");
}

TEST(RemapDB, QueryStringIgnoreAndWhitelist)
{
	cluster_t *cluster = load_test_ingress("tests/ingress-ignore-whitelist-params.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");
	req.rr_query = strdup("fox=x&bar=2&bax=3&quux=4&xyzzy=5");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ(res.rz_query, "quux=4&xyzzy=5");
}

TEST(RemapDB, CacheKey)
{
	cluster_t *cluster = load_test_ingress(
			"tests/ingress-ignore-whitelist-params.json");
	scoped_c_ptr<cluster_t *> cluster_(cluster, cluster_free);

	k8s_config_t *cfg = k8s_config_new();
	scoped_c_ptr<k8s_config_t *> cfg_(cfg, k8s_config_free);

	remap_db_t *db = remap_db_from_cluster(cfg, cluster);
	ASSERT_TRUE(db != nullptr);
	scoped_c_ptr<remap_db_t *> db_(db, remap_db_free);

	/* Build a request */
	remap_request_t req;
	memset(&req, 0, sizeof(req));
	scoped_c_ptr<remap_request_t *> req_(&req, remap_request_free);

	req.rr_hdrfields = hash_new(127, (hash_free_fn)remap_hdrfield_free);
	req.rr_proto = strdup("http");
	req.rr_host = strdup("echoheaders.gce.t6x.uk");
	req.rr_path = strdup("what/ever");
	req.rr_query = strdup("fox=x&bar=2&bax=3&quux=4&xyzzy=5");

	remap_result_t res;
	memset(&res, 0, sizeof(res));
	scoped_c_ptr<remap_result_t *> res_(&res, remap_result_free);

	int ret = remap_run(db, &req, &res);
	ASSERT_EQ(RR_OK, ret);

	/* Make sure pick_target returns the right host */
	EXPECT_STREQ(res.rz_query, "quux=4&xyzzy=5");

	char *cachekey;
	size_t keysize;
	string expected = "/BGh0dHAWZWNob2hlYWRlcnMuZ2NlLnQ2eC51awkAd2hhdC9ldm"
		"VyDgBxdXV4PTQmeHl6enk9NQ==";
	remap_make_cache_key(&req, &res, &cachekey, &keysize);
	string actual = string(cachekey, keysize);
	EXPECT_EQ(expected.size(), keysize);
	EXPECT_EQ(expected, actual);
	free(cachekey);
}
