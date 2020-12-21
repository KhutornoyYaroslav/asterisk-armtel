/*!
 * \file
 * \brief AICSOriginate application
 *
 * \author Roberto Casas <roberto.casas@diaple.com>
 * \author Russell Bryant <russell@digium.com>
 *
 * \ingroup applications
 *
 * \todo Make a way to be able to set variables (and functions) on the outbound
 *       channel, similar to the Variable headers for the AMI AICSOriginate, and the
 *       Set options for call files.
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"
#include "aics.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 412581 $")

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/app.h"

static const char app_aicsoriginate[] = "AICSOriginate";

/*** DOCUMENTATION
	<application name="AICSOriginate" language="en_US">
		<synopsis>
			Originate a call using caller parameters.
		</synopsis>
		<syntax>
			<parameter name="tech_data" required="true">
				<para>Channel technology and data for creating the outbound channel.
                      For example, SIP/1234.</para>
			</parameter>
			<parameter name="caller_data" required="true">
				<para>Channel technology and data for identification the incoming channel.
                      For example, SIP/4321.</para>
            </parameter>
			<parameter name="context" required="true">
				<para>This is the context that the channel will be sent to.</para>
			</parameter>
			<parameter name="extension" required="false">
				<para>This is the extension that the channel will be sent to.</para>
			</parameter>
			<parameter name="priority" required="false">
				<para>This is the priority that the channel is sent to.</para>
			</parameter>
			<parameter name="scenario" required="false">
				<para>This is the ICS scenario name.</para>
			</parameter>
			<parameter name="timeout" required="false">
				<para>Timeout in seconds. Default is 30 seconds.</para>
			</parameter>
		</syntax>
		<description>
		<para>This application aicsoriginates an outbound call and connects it to a specified extension or application.  This application will block until the outgoing call fails or gets answered.  At that point, this application will exit with the status variable set and dialplan processing will continue.</para>

		<para>This application sets the following channel variable before exiting:</para>
		<variablelist>
			<variable name="AICSORIGINATE_STATUS">
				<para>This indicates the result of the call origination.</para>
				<value name="FAILED"/>
				<value name="SUCCESS"/>
				<value name="BUSY"/>
				<value name="CONGESTION"/>
				<value name="HANGUP"/>
				<value name="RINGING"/>
				<value name="UNKNOWN">
				In practice, you should never see this value.  Please report it to the issue tracker if you ever see it.
				</value>
			</variable>
			<variable name="AICSORIGINATE_CHANNEL">
				<para>This indicates the originated channel name.</para>
			</variable>
		</variablelist>
		</description>
	</application>
 ***/

static int aicsoriginate_exec(struct ast_channel *chan, const char *data)
{
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(tech_data);
		AST_APP_ARG(caller_data);
		AST_APP_ARG(context);
		AST_APP_ARG(extension);
		AST_APP_ARG(priority);
		AST_APP_ARG(scenario);
		AST_APP_ARG(timeout);
	);
	char *parse;
	char *chantech, *chandata;
	char *callertech, *callerdata;
	char *ggs_scenario = NULL;
	char *spawned_chan_name = NULL;
	int res = -1;
	int outgoing_status = 0;
	unsigned int timeout = 30;
	static const char default_exten[] = "s";
	struct ast_format tmpfmt;
	struct ast_format_cap *cap_slin = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_NOLOCK);

	ast_autoservice_start(chan);
	if (!cap_slin) {
		goto return_cleanup;
	}
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR12, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR16, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR24, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR32, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR44, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR48, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR96, 0));
	ast_format_cap_add(cap_slin, ast_format_set(&tmpfmt, AST_FORMAT_SLINEAR192, 0));

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "AICSOriginate() requires arguments\n");
		goto return_cleanup;
	}

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (args.argc < 3) {
		ast_log(LOG_ERROR, "Incorrect number of arguments\n");
		goto return_cleanup;
	}

	if (!ast_strlen_zero(args.timeout)) {
		if(sscanf(args.timeout, "%u", &timeout) != 1) {
			ast_log(LOG_NOTICE, "Invalid timeout: '%s'. Setting timeout to 30 seconds\n", args.timeout);
			timeout = 30;
		}
	}

	chandata = ast_strdupa(args.tech_data);
	chantech = strsep(&chandata, "/");

	if (ast_strlen_zero(chandata) || ast_strlen_zero(chantech)) {
		ast_log(LOG_ERROR, "Channel Tech/Data invalid: '%s'\n", args.tech_data);
		goto return_cleanup;
	}

	callerdata = ast_strdupa(args.caller_data);
	callertech = strsep(&callerdata, "/");

	if (ast_strlen_zero(callerdata) || ast_strlen_zero(callertech)) {
		ast_log(LOG_ERROR, "Caller Tech/Data invalid: '%s'\n", args.caller_data);
		goto return_cleanup;
	}


	int priority = 1; /* Initialized in case priority not specified */
	const char *exten = args.extension;

	if (args.argc >= 5) {
		/* Context/Exten/Priority all specified */
		if (sscanf(args.priority, "%30d", &priority) != 1) {
			ast_log(LOG_ERROR, "Invalid priority: '%s'\n", args.priority);
			goto return_cleanup;
		}
	} else if (args.argc == 3) {
		/* Exten not specified */
		exten = default_exten;
	}

	ggs_scenario = ast_strdup(args.scenario);

	ast_debug(1, "AICSOriginating call to '%s/%s' from '%s/%s' and connecting them to extension %s,%s,%d in scenario '%s'\n",
			chantech, chandata, callertech, callerdata, args.context, exten, priority, ggs_scenario);

	struct aics_proxy_params* params = ast_channel_proxy(chan);
	params->scenario = aics_scenario_from_str(ggs_scenario);
	//ast_channel_ggs_scenario_set(chan, ggs_scenario);
	//spawned_chan_name = ast_alloca(128);
	res = aics_pbx_outgoing_exten(chan, chantech, cap_slin, chandata, timeout * 1000, args.context,
			exten, priority, &outgoing_status, 1, callerdata, &spawned_chan_name);

	ast_log(LOG_NOTICE, "spawned [%s] res %d\n", spawned_chan_name, res);

return_cleanup:
	pbx_builtin_setvar_helper(chan, "AICSORIGINATE_CHANNEL", "");
	if (res) {
		pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "FAILED");
	} else {
		switch (outgoing_status) {
		case 0:
		case AST_CONTROL_ANSWER:
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "SUCCESS");
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_CHANNEL", spawned_chan_name);
			break;
		case AST_CONTROL_BUSY:
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "BUSY");
			break;
		case AST_CONTROL_CONGESTION:
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "CONGESTION");
			break;
		case AST_CONTROL_HANGUP:
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "HANGUP");
			break;
		case AST_CONTROL_RINGING:
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "RINGING");
			break;
		default:
			ast_log(LOG_WARNING, "Unknown aicsoriginate status result of '%d'\n",
					outgoing_status);
			pbx_builtin_setvar_helper(chan, "AICSORIGINATE_STATUS", "UNKNOWN");
			break;
		}
	}
	cap_slin = ast_format_cap_destroy(cap_slin);
	ast_autoservice_stop(chan);
	if (spawned_chan_name)
		ast_free(spawned_chan_name);
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app_aicsoriginate);
}

static int load_module(void)
{
	int res;

	res = ast_register_application_xml(app_aicsoriginate, aicsoriginate_exec);

	return res ? AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "AICSOriginate call");
