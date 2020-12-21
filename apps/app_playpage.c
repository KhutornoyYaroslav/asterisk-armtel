/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2006 Digium, Inc.  All rights reserved.
 *
 * Mark Spencer <markster@digium.com>
 *
 * This code is released under the GNU General Public License
 * version 2.0.  See LICENSE for more information.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief PlayPage() - Paging application
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>app_confbridge</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 410157 $")

#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/file.h"
#include "asterisk/app.h"
#include "asterisk/chanvars.h"
#include "asterisk/utils.h"
#include "asterisk/devicestate.h"
#include "asterisk/dial.h"
#include "aics.h"
/*** DOCUMENTATION
	<application name="PlayPage" language="en_US">
		<synopsis>
			Page series of phones
		</synopsis>
		<syntax>
			<parameter name="Technology/Resource" required="true" argsep="&amp;">
				<argument name="Technology/Resource" required="true">
					<para>Specification of the device(s) to dial. These must be in the format of
					<literal>Technology/Resource</literal>, where <replaceable>Technology</replaceable>
					represents a particular channel driver, and <replaceable>Resource</replaceable> represents a resource
					available to that particular channel driver.</para>
				</argument>
				<argument name="Technology2/Resource2" multiple="true">
					<para>Optional extra devices to dial in parallel</para>
					<para>If you need more than one, enter them as Technology2/Resource2&amp;
					Technology3/Resourse3&amp;.....</para>
				</argument>
			</parameter>
						<parameter name="filenames" required="true" argsep="&amp;">
				<argument name="filename" required="true" />
				<argument name="filename2" multiple="true" />
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="r">
						<para>Full duplex audio</para>
					</option>
					<option name="s">
						<para>Ignore attempts to forward the call</para>
					</option>
				</optionlist>
			</parameter>
			<parameter name="prio" >
			</parameter>
			<parameter name="repeat" >
			</parameter>
		</syntax>
		<description>
			<para>.</para>
		</description>
		<see-also>
			<ref type="application">ConfBridge</ref>
		</see-also>
	</application>
 ***/
static const char * const app_page= "PlayPage";

enum page_opt_flags {
	PAGE_SKIP = (1 << 0),
	PAGE_RESERVE= (1 << 1),
};



AST_APP_OPTIONS(page_opts, {
	AST_APP_OPTION('s', PAGE_SKIP),
	AST_APP_OPTION('r', PAGE_RESERVE),
});

/* We use this structure as a way to pass this to all dialed channels */
struct page_options {
	char *opts[2];
	struct ast_flags flags;
};


static void page_state_callback(struct ast_dial *dial)
{
	struct ast_channel *chan;
	struct page_options *options;
// ast_log(LOG_ERROR, "First my app callback[%d]\n",ast_dial_state(dial));
//if((chan = ast_dial_answered(dial))!=NULL)  ast_log(LOG_ERROR, "First my app callback[%s][%d]\n",ast_channel_name(chan),ast_dial_state(dial));
	if (ast_dial_state(dial) != AST_DIAL_RESULT_ANSWERED ||
	    !(chan = ast_dial_answered(dial)) ||
	    !(options = ast_dial_get_user_data(dial))) {


		return;
	}


}

static int playpage_exec(struct ast_channel *chan, const char *data)
{
	char *tech, *resource, *tmp;
	char playpageopts[128], originator[AST_CHANNEL_NAME];
	struct page_options options = { { 0, }, { 0, } };
//	unsigned int confid = ast_random();
	struct ast_app *app,*app1;//,*app2;
	int  pos = 0, i = 0;//res = 0;
	struct ast_dial **dial_list;
	unsigned int num_dials;
//	int timeout = 0;
	char *parse;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(devices);
		AST_APP_ARG(file);
		AST_APP_ARG(options);
		AST_APP_ARG(prio);
		AST_APP_ARG(repeat);
	);


	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "This application requires at least one argument (destination(s) to page)\n");
		return -1;
	}
	if (!(app1 = pbx_findapp("Hangup"))) {
		ast_log(LOG_WARNING, "There is no Hangup application available!\n");
		return -1;
	};
	if (!(app = pbx_findapp("SimplePlayback"))) {
		ast_log(LOG_WARNING, "There is no SimplePlayback application available!\n");
		return -1;
	};

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	ast_log(LOG_NOTICE, "dev=%s;opt=%s;file=%s,prio=%s,repeat=%s\n",args.devices,args.options,args.file,args.prio,args.repeat);
	struct aics_proxy_params *aipp;
	aipp = ast_channel_proxy(chan);
//	ast_log(LOG_NOTICE,"name=%s,prio=%d\n",ast_channel_name(chan),aipp->priority);
    if(strstr(ast_channel_name(chan),"Modbus"))
    {
    	aics_priority_set(aipp,args.prio);
    	aics_direction_set(aipp,"send");
//    	ast_log(LOG_NOTICE,"!!!!!! Set Modbus prio=%d\n",aipp->priority);
    }

	ast_copy_string(originator, ast_channel_name(chan), sizeof(originator));
	if ((tmp = strchr(originator, '-'))) {
		*tmp = '\0';
	}

	if (!ast_strlen_zero(args.options)) {
		ast_app_parse_options(page_opts, &options.flags, options.opts, args.options);
	}

	if (ast_strlen_zero(args.file)) {
		ast_log(LOG_ERROR, "No file name!\n");
       return -1;
     }

	snprintf(playpageopts, sizeof(playpageopts), "SimplePlayback,%s,%s,%s", args.file,args.repeat,"");
//	 ast_log(LOG_NOTICE,"-----------%s\n",playpageopts);

	/* Count number of extensions in list by number of ampersands + 1 */
	num_dials = 1;
	tmp = args.devices;
	while (*tmp) {
		if (*tmp == '&') {
			num_dials++;
		}
		tmp++;
	}

	if (!(dial_list = ast_calloc(num_dials, sizeof(struct ast_dial *)))) {
		ast_log(LOG_ERROR, "Can't allocate %ld bytes for dial list\n", (long)(sizeof(struct ast_dial *) * num_dials));
		return -1;
	}

	/* Go through parsing/calling each device */
	while ((tech = strsep(&args.devices, "&"))) {
//		int state = 0;
		struct ast_dial *dial = NULL;

		/* don't call the originating device */
		if (!strcasecmp(tech, originator))
			continue;

		/* If no resource is available, continue on */
		if (!(resource = strchr(tech, '/'))) {
			ast_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", tech);
			continue;
		}


		*resource++ = '\0';

		/* Create a dialing structure */
		if (!(dial = ast_dial_create())) {
			ast_log(LOG_WARNING, "Failed to create dialing structure.\n");
			continue;
		}

		/* Append technology and resource */
		if (ast_dial_append(dial, tech, resource, NULL) == -1) {
			ast_log(LOG_ERROR, "Failed to add %s to outbound dial\n", tech);
			ast_dial_destroy(dial);
			continue;
		}

		/* Set ANSWER_EXEC as global option */
		ast_dial_option_global_enable(dial, AST_DIAL_OPTION_ANSWER_EXEC, playpageopts);


		ast_dial_set_state_callback(dial, &page_state_callback);
//		ast_dial_set_user_data(dial, &options);

		/* Run this dial in async mode */
		ast_dial_run(dial, chan, 1);

		/* Put in our dialing array */
		dial_list[pos++] = dial;
	}

	if (ast_test_flag(&options.flags, PAGE_SKIP)) {
		pbx_exec(chan, app1, NULL);
	    return -1;
     }
	   snprintf(playpageopts, sizeof(playpageopts), "%s,%s,%s", args.file,args.repeat,"");

		pbx_exec(chan, app, playpageopts);
	//	pbx_exec(chan, app1, "50");


	/* Go through each dial attempt cancelling, joining, and destroying */

	for (i = 0; i < pos; i++) {
		struct ast_dial *dial = dial_list[i];
		/* We have to wait for the async thread to exit as it's possible ConfBridge won't throw them out immediately */
		ast_dial_join(dial);
		/* Hangup all channels */
		ast_dial_hangup(dial);
		/* Destroy dialing structure */
		ast_dial_destroy(dial);
	}

	ast_free(dial_list);

	return -1;
//	return 0;
}

static int unload_module(void)
{
	return ast_unregister_application(app_page);
}

static int load_module(void)
{
	return ast_register_application_xml(app_page, playpage_exec);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Page Play Phones");

