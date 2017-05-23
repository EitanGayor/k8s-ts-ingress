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
 * API object tests.  This tests JSON parsing and construction of API objects,
 * as well as deletion of objects if run under ASAN.  It does not test any of
 * the network code, including watchers.
 */

#include	<string>
#include	<cstdio>
#include	<iostream>
#include	<fstream>
#include	<iterator>
#include	<map>
#include	<cstdarg>

#include	<json.h>

#include	"gtest/gtest.h"

#include	"tests/test.h"
#include	"api.h"

using std::string;
using std::ifstream;
using std::istreambuf_iterator;
using std::size_t;
using std::map;

TEST(API, Ingress) {
	ts_api_errors = 0;

	json_object *obj = test_load_json("tests/ingress.json");
	ASSERT_TRUE(obj != NULL);

	ingress_t *ing = ingress_make(obj);
	ASSERT_TRUE(ing != NULL);
	
	json_object_put(obj);
	obj = NULL;

	EXPECT_STREQ("default", ing->in_namespace);
	EXPECT_STREQ("echoheaders", ing->in_name);

	map<string, string> actual_annotations, expected_annotations{
		{ "ingress.kubernetes.io/auth-realm",		"test auth" },
		{ "ingress.kubernetes.io/auth-secret",		"authtest" },
		{ "ingress.kubernetes.io/auth-type",		"basic" },
		{ "ingress.kubernetes.io/rewrite-target",	"/dst" },
	};

	const char *key, *value;
	hash_foreach(ing->in_annotations, &key, &value)
		actual_annotations[key] = value;

	EXPECT_EQ(expected_annotations, actual_annotations);

	EXPECT_EQ(0u, ing->in_ntls);
	ASSERT_EQ(1u, ing->in_nrules);
	
	ingress_rule_t *rule = &ing->in_rules[0];
	EXPECT_STREQ(rule->ir_host, "echoheaders.gce.t6x.uk");
	ASSERT_EQ(1u, rule->ir_npaths);

	ingress_path_t *path = &rule->ir_paths[0];
	EXPECT_STREQ("/src", path->ip_path);
	EXPECT_STREQ("echoheaders", path->ip_service_name);
	EXPECT_STREQ("http", path->ip_service_port);

	ingress_free(ing);
	EXPECT_EQ(0, ts_api_errors);
}

TEST(API, Service) {
	ts_api_errors = 0;

	json_object *obj = test_load_json("tests/service.json");
	ASSERT_TRUE(obj != NULL);

	service_t *svc = service_make(obj);
	ASSERT_TRUE(svc != NULL);

	json_object_put(obj);
	obj = NULL;

	EXPECT_STREQ("echoheaders", svc->sv_name);
	EXPECT_STREQ("default", svc->sv_namespace);
	EXPECT_STREQ("ClusterIP", svc->sv_type);
	EXPECT_STREQ("10.3.19.27", svc->sv_cluster_ip);
	EXPECT_STREQ("None", svc->sv_session_affinity);
	EXPECT_TRUE(svc->sv_external_name == NULL);
	
	map<string, string> actual_selectors, expected_selectors{
		{ "app", "echoheaders" },
	};

	const char *key, *value;
	hash_foreach(svc->sv_selector, &key, &value)
		actual_selectors[key] = value;

	EXPECT_EQ(expected_selectors, actual_selectors);

	service_port_t *port = service_find_port(svc, "http", SV_P_TCP);
	ASSERT_TRUE(port != NULL);

	EXPECT_STREQ(port->sp_name, "http");
	EXPECT_EQ(port->sp_port, 80);
	EXPECT_EQ(port->sp_protocol, SV_P_TCP);
	EXPECT_EQ(port->sp_target_port, 8080);

	service_free(svc);
	EXPECT_EQ(0, ts_api_errors);
}

TEST(API, ExternalService) {
	ts_api_errors = 0;

	json_object *obj = test_load_json("tests/service_external.json");
	ASSERT_TRUE(obj != NULL);

	service_t *svc = service_make(obj);
	ASSERT_TRUE(svc != NULL);

	json_object_put(obj);
	obj = NULL;

	EXPECT_STREQ("external-service", svc->sv_name);
	EXPECT_STREQ("default", svc->sv_namespace);
	EXPECT_STREQ("ExternalName", svc->sv_type);
	EXPECT_STREQ("None", svc->sv_session_affinity);
	EXPECT_STREQ("echoheaders.gce.t6x.uk", svc->sv_external_name);
	EXPECT_TRUE(svc->sv_cluster_ip == NULL);
	
	map<string, string> actual_selectors, expected_selectors;

	const char *label, *value;
	hash_foreach(svc->sv_selector, &label, &value)
		actual_selectors[label] = value;

	EXPECT_EQ(expected_selectors, actual_selectors);

	service_free(svc);
	EXPECT_EQ(0, ts_api_errors);
}

TEST(API, Secret) {
	ts_api_errors = 0;

	json_object *obj = test_load_json("tests/secret.json");
	ASSERT_TRUE(obj != NULL);

	secret_t *srt = secret_make(obj);
	ASSERT_TRUE(srt != NULL);
	json_object_put(obj);

	EXPECT_STREQ("default", srt->se_namespace);
	EXPECT_STREQ("testsecret", srt->se_name);
	EXPECT_STREQ("Opaque", srt->se_type);
	
	map<string, string> actual_data, expected_data{
		{ "key1", "c29tZSB2YWx1ZQ==" },
		{ "key2", "b3RoZXIgdmFsdWU=" },
	};

	const char *key, *value;
	hash_foreach(srt->se_data, &key, &value)
		actual_data[key] = value;

	EXPECT_EQ(expected_data, actual_data);

	secret_free(srt);
	EXPECT_EQ(0, ts_api_errors);
}
