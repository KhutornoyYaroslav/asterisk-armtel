/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2012 - 2013, Digium, Inc.
 *
 * David M. Lee, II <dlee@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * !!!!!                               DO NOT EDIT                        !!!!!
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * This file is generated by a mustache template. Please see the original
 * template in rest-api-templates/res_ari_resource.c.mustache
 */

/*! \file
 *
 * \brief Asterisk resources
 *
 * \author David M. Lee, II <dlee@digium.com>
 */

/*** MODULEINFO
	<depend type="module">res_ari</depend>
	<depend type="module">res_stasis</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 406003 $")

#include "asterisk/app.h"
#include "asterisk/module.h"
#include "asterisk/stasis_app.h"
#include "ari/resource_asterisk.h"
#if defined(AST_DEVMODE)
#include "ari/ari_model_validators.h"
#endif

#define MAX_VALS 128

int ast_ari_asterisk_get_info_parse_body(
	struct ast_json *body,
	struct ast_ari_asterisk_get_info_args *args)
{
	struct ast_json *field;
	/* Parse query parameters out of it */
	field = ast_json_object_get(body, "only");
	if (field) {
		/* If they were silly enough to both pass in a query param and a
		 * JSON body, free up the query value.
		 */
		ast_free(args->only);
		if (ast_json_typeof(field) == AST_JSON_ARRAY) {
			/* Multiple param passed as array */
			size_t i;
			args->only_count = ast_json_array_size(field);
			args->only = ast_malloc(sizeof(*args->only) * args->only_count);

			if (!args->only) {
				return -1;
			}

			for (i = 0; i < args->only_count; ++i) {
				args->only[i] = ast_json_string_get(ast_json_array_get(field, i));
			}
		} else {
			/* Multiple param passed as single value */
			args->only_count = 1;
			args->only = ast_malloc(sizeof(*args->only) * args->only_count);
			if (!args->only) {
				return -1;
			}
			args->only[0] = ast_json_string_get(field);
		}
	}
	return 0;
}

/*!
 * \brief Parameter parsing callback for /asterisk/info.
 * \param get_params GET parameters in the HTTP request.
 * \param path_vars Path variables extracted from the request.
 * \param headers HTTP headers.
 * \param[out] response Response to the HTTP request.
 */
static void ast_ari_asterisk_get_info_cb(
	struct ast_tcptls_session_instance *ser,
	struct ast_variable *get_params, struct ast_variable *path_vars,
	struct ast_variable *headers, struct ast_ari_response *response)
{
	struct ast_ari_asterisk_get_info_args args = {};
	struct ast_variable *i;
	RAII_VAR(struct ast_json *, body, NULL, ast_json_unref);
#if defined(AST_DEVMODE)
	int is_valid;
	int code;
#endif /* AST_DEVMODE */

	for (i = get_params; i; i = i->next) {
		if (strcmp(i->name, "only") == 0) {
			/* Parse comma separated list */
			char *vals[MAX_VALS];
			size_t j;

			args.only_parse = ast_strdup(i->value);
			if (!args.only_parse) {
				ast_ari_response_alloc_failed(response);
				goto fin;
			}

			if (strlen(args.only_parse) == 0) {
				/* ast_app_separate_args can't handle "" */
				args.only_count = 1;
				vals[0] = args.only_parse;
			} else {
				args.only_count = ast_app_separate_args(
					args.only_parse, ',', vals,
					ARRAY_LEN(vals));
			}

			if (args.only_count == 0) {
				ast_ari_response_alloc_failed(response);
				goto fin;
			}

			if (args.only_count >= MAX_VALS) {
				ast_ari_response_error(response, 400,
					"Bad Request",
					"Too many values for only");
				goto fin;
			}

			args.only = ast_malloc(sizeof(*args.only) * args.only_count);
			if (!args.only) {
				ast_ari_response_alloc_failed(response);
				goto fin;
			}

			for (j = 0; j < args.only_count; ++j) {
				args.only[j] = (vals[j]);
			}
		} else
		{}
	}
	/* Look for a JSON request entity */
	body = ast_http_get_json(ser, headers);
	if (!body) {
		switch (errno) {
		case EFBIG:
			ast_ari_response_error(response, 413, "Request Entity Too Large", "Request body too large");
			goto fin;
		case ENOMEM:
			ast_ari_response_error(response, 500, "Internal Server Error", "Error processing request");
			goto fin;
		case EIO:
			ast_ari_response_error(response, 400, "Bad Request", "Error parsing request body");
			goto fin;
		}
	}
	if (ast_ari_asterisk_get_info_parse_body(body, &args)) {
		ast_ari_response_alloc_failed(response);
		goto fin;
	}
	ast_ari_asterisk_get_info(headers, &args, response);
#if defined(AST_DEVMODE)
	code = response->response_code;

	switch (code) {
	case 0: /* Implementation is still a stub, or the code wasn't set */
		is_valid = response->message == NULL;
		break;
	case 500: /* Internal Server Error */
	case 501: /* Not Implemented */
		is_valid = 1;
		break;
	default:
		if (200 <= code && code <= 299) {
			is_valid = ast_ari_validate_asterisk_info(
				response->message);
		} else {
			ast_log(LOG_ERROR, "Invalid error response %d for /asterisk/info\n", code);
			is_valid = 0;
		}
	}

	if (!is_valid) {
		ast_log(LOG_ERROR, "Response validation failed for /asterisk/info\n");
		ast_ari_response_error(response, 500,
			"Internal Server Error", "Response validation failed");
	}
#endif /* AST_DEVMODE */

fin: __attribute__((unused))
	ast_free(args.only_parse);
	ast_free(args.only);
	return;
}
int ast_ari_asterisk_get_global_var_parse_body(
	struct ast_json *body,
	struct ast_ari_asterisk_get_global_var_args *args)
{
	struct ast_json *field;
	/* Parse query parameters out of it */
	field = ast_json_object_get(body, "variable");
	if (field) {
		args->variable = ast_json_string_get(field);
	}
	return 0;
}

/*!
 * \brief Parameter parsing callback for /asterisk/variable.
 * \param get_params GET parameters in the HTTP request.
 * \param path_vars Path variables extracted from the request.
 * \param headers HTTP headers.
 * \param[out] response Response to the HTTP request.
 */
static void ast_ari_asterisk_get_global_var_cb(
	struct ast_tcptls_session_instance *ser,
	struct ast_variable *get_params, struct ast_variable *path_vars,
	struct ast_variable *headers, struct ast_ari_response *response)
{
	struct ast_ari_asterisk_get_global_var_args args = {};
	struct ast_variable *i;
	RAII_VAR(struct ast_json *, body, NULL, ast_json_unref);
#if defined(AST_DEVMODE)
	int is_valid;
	int code;
#endif /* AST_DEVMODE */

	for (i = get_params; i; i = i->next) {
		if (strcmp(i->name, "variable") == 0) {
			args.variable = (i->value);
		} else
		{}
	}
	/* Look for a JSON request entity */
	body = ast_http_get_json(ser, headers);
	if (!body) {
		switch (errno) {
		case EFBIG:
			ast_ari_response_error(response, 413, "Request Entity Too Large", "Request body too large");
			goto fin;
		case ENOMEM:
			ast_ari_response_error(response, 500, "Internal Server Error", "Error processing request");
			goto fin;
		case EIO:
			ast_ari_response_error(response, 400, "Bad Request", "Error parsing request body");
			goto fin;
		}
	}
	if (ast_ari_asterisk_get_global_var_parse_body(body, &args)) {
		ast_ari_response_alloc_failed(response);
		goto fin;
	}
	ast_ari_asterisk_get_global_var(headers, &args, response);
#if defined(AST_DEVMODE)
	code = response->response_code;

	switch (code) {
	case 0: /* Implementation is still a stub, or the code wasn't set */
		is_valid = response->message == NULL;
		break;
	case 500: /* Internal Server Error */
	case 501: /* Not Implemented */
	case 400: /* Missing variable parameter. */
		is_valid = 1;
		break;
	default:
		if (200 <= code && code <= 299) {
			is_valid = ast_ari_validate_variable(
				response->message);
		} else {
			ast_log(LOG_ERROR, "Invalid error response %d for /asterisk/variable\n", code);
			is_valid = 0;
		}
	}

	if (!is_valid) {
		ast_log(LOG_ERROR, "Response validation failed for /asterisk/variable\n");
		ast_ari_response_error(response, 500,
			"Internal Server Error", "Response validation failed");
	}
#endif /* AST_DEVMODE */

fin: __attribute__((unused))
	return;
}
int ast_ari_asterisk_set_global_var_parse_body(
	struct ast_json *body,
	struct ast_ari_asterisk_set_global_var_args *args)
{
	struct ast_json *field;
	/* Parse query parameters out of it */
	field = ast_json_object_get(body, "variable");
	if (field) {
		args->variable = ast_json_string_get(field);
	}
	field = ast_json_object_get(body, "value");
	if (field) {
		args->value = ast_json_string_get(field);
	}
	return 0;
}

/*!
 * \brief Parameter parsing callback for /asterisk/variable.
 * \param get_params GET parameters in the HTTP request.
 * \param path_vars Path variables extracted from the request.
 * \param headers HTTP headers.
 * \param[out] response Response to the HTTP request.
 */
static void ast_ari_asterisk_set_global_var_cb(
	struct ast_tcptls_session_instance *ser,
	struct ast_variable *get_params, struct ast_variable *path_vars,
	struct ast_variable *headers, struct ast_ari_response *response)
{
	struct ast_ari_asterisk_set_global_var_args args = {};
	struct ast_variable *i;
	RAII_VAR(struct ast_json *, body, NULL, ast_json_unref);
#if defined(AST_DEVMODE)
	int is_valid;
	int code;
#endif /* AST_DEVMODE */

	for (i = get_params; i; i = i->next) {
		if (strcmp(i->name, "variable") == 0) {
			args.variable = (i->value);
		} else
		if (strcmp(i->name, "value") == 0) {
			args.value = (i->value);
		} else
		{}
	}
	/* Look for a JSON request entity */
	body = ast_http_get_json(ser, headers);
	if (!body) {
		switch (errno) {
		case EFBIG:
			ast_ari_response_error(response, 413, "Request Entity Too Large", "Request body too large");
			goto fin;
		case ENOMEM:
			ast_ari_response_error(response, 500, "Internal Server Error", "Error processing request");
			goto fin;
		case EIO:
			ast_ari_response_error(response, 400, "Bad Request", "Error parsing request body");
			goto fin;
		}
	}
	if (ast_ari_asterisk_set_global_var_parse_body(body, &args)) {
		ast_ari_response_alloc_failed(response);
		goto fin;
	}
	ast_ari_asterisk_set_global_var(headers, &args, response);
#if defined(AST_DEVMODE)
	code = response->response_code;

	switch (code) {
	case 0: /* Implementation is still a stub, or the code wasn't set */
		is_valid = response->message == NULL;
		break;
	case 500: /* Internal Server Error */
	case 501: /* Not Implemented */
	case 400: /* Missing variable parameter. */
		is_valid = 1;
		break;
	default:
		if (200 <= code && code <= 299) {
			is_valid = ast_ari_validate_void(
				response->message);
		} else {
			ast_log(LOG_ERROR, "Invalid error response %d for /asterisk/variable\n", code);
			is_valid = 0;
		}
	}

	if (!is_valid) {
		ast_log(LOG_ERROR, "Response validation failed for /asterisk/variable\n");
		ast_ari_response_error(response, 500,
			"Internal Server Error", "Response validation failed");
	}
#endif /* AST_DEVMODE */

fin: __attribute__((unused))
	return;
}

/*! \brief REST handler for /api-docs/asterisk.{format} */
static struct stasis_rest_handlers asterisk_info = {
	.path_segment = "info",
	.callbacks = {
		[AST_HTTP_GET] = ast_ari_asterisk_get_info_cb,
	},
	.num_children = 0,
	.children = {  }
};
/*! \brief REST handler for /api-docs/asterisk.{format} */
static struct stasis_rest_handlers asterisk_variable = {
	.path_segment = "variable",
	.callbacks = {
		[AST_HTTP_GET] = ast_ari_asterisk_get_global_var_cb,
		[AST_HTTP_POST] = ast_ari_asterisk_set_global_var_cb,
	},
	.num_children = 0,
	.children = {  }
};
/*! \brief REST handler for /api-docs/asterisk.{format} */
static struct stasis_rest_handlers asterisk = {
	.path_segment = "asterisk",
	.callbacks = {
	},
	.num_children = 2,
	.children = { &asterisk_info,&asterisk_variable, }
};

static int load_module(void)
{
	int res = 0;
	stasis_app_ref();
	res |= ast_ari_add_handler(&asterisk);
	return res;
}

static int unload_module(void)
{
	ast_ari_remove_handler(&asterisk);
	stasis_app_unref();
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "RESTful API module - Asterisk resources",
	.load = load_module,
	.unload = unload_module,
	.nonoptreq = "res_ari,res_stasis",
	);