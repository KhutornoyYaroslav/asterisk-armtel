/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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

/*! \file
 *
 * \brief Trivial application to playback a sound file
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 396309 $")

#include "asterisk/file.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/app.h"
/* This file provides config-file based 'say' functions, and implenents
 * some CLI commands.
 */
#include "asterisk/say.h"	/*!< provides config-file based 'say' functions */
#include "asterisk/cli.h"

/*** DOCUMENTATION
	<application name="SimplePlayback" language="en_US">
		<synopsis>
			Play a file.
		</synopsis>
		<syntax>
			<parameter name="filenames" required="true" argsep="&amp;">
				<argument name="filename" required="true" />
				<argument name="filename2" multiple="true" />
			</parameter>
			<parameter name="options">
				<para>Comma separated list of options</para>
				<optionlist>
					<option name="skip">
						<para>Do not play if not answered</para>
					</option>
					<option name="noanswer">
						<para>Playback without answering, otherwise the channel will
						be answered before the sound is played.</para>
						<note><para>Not all channel types support playing messages while still on hook.</para></note>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Plays back given filenames (do not put extension of wav/alaw etc).
			The playback command answer the channel if no options are specified.
			If the file is non-existant it will fail</para>
			<para>This application sets the following channel variable upon completion:</para>
			<variablelist>
				<variable name="PLAYBACKSTATUS">
					<para>The status of the playback attempt as a text string.</para>
					<value name="SUCCESS"/>
					<value name="FAILED"/>
				</variable>
			</variablelist>
			<para>See Also: Background (application) -- for playing sound files that are interruptible</para>
			<para>WaitExten (application) -- wait for digits from caller, optionally play music on hold</para>
		</description>
		<see-also>
			<ref type="application">Background</ref>
			<ref type="application">WaitExten</ref>
			<ref type="application">ControlPlayback</ref>
			<ref type="agi">stream file</ref>
			<ref type="agi">control stream file</ref>
			<ref type="manager">ControlPlayback</ref>
		</see-also>
	</application>
 ***/

static char *app = "SimplePlayback";
#define SPLAYBASK_VERSION          "1.1"


static int sp_delete(char *file)
{
	char *txt;
	int txtsize = 0;

	txtsize = (strlen(file) + 5)*sizeof(char);
	txt = ast_alloca(txtsize);
	/* Sprintf here would safe because we alloca'd exactly the right length,
	 * but trying to eliminate all sprintf's anyhow
	 */
	if (ast_check_realtime("voicemail_data")) {
		ast_destroy_realtime("voicemail_data", "filename", file, SENTINEL);
	}
	snprintf(txt, txtsize, "%s.txt", file);
	unlink(txt);
	return ast_filedelete(file, NULL);
}


static int simpleplayback_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	int mres = 0;
	char *tmp,tmp1[1024];
	int option_skip=0;
	int option_noanswer = 0;
	int option_queue = 0;
    int repeat=1;
    int infinitely =0;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(filenames);
		AST_APP_ARG(repeat);
		AST_APP_ARG(options);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "SimplePlayback requires an argument (filename)\n");
		return -1;
	}

	tmp = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, tmp);
//	AST_NONSTANDARD_APP_ARGS(args, tmp,'|');
	ast_log(LOG_NOTICE, "SimplePlayback(ver=%s) argument filename=%s;repeat=%s;opt=%s \n",SPLAYBASK_VERSION,args.filenames,args.repeat,args.options);
	if(args.repeat){
		repeat = atoi(args.repeat);
		if(repeat == 0){
		   infinitely=1;
		   ast_log(LOG_NOTICE, "infinitely=%d\n",repeat);
		}
	}


	if (args.options) {
		if (strcasestr(args.options, "skip"))
			option_skip = 1;
		if (strcasestr(args.options, "noanswer"))
			option_noanswer = 1;
		if (strcasestr(args.options, "queue"))
			option_queue = 1;
	}
	if (ast_channel_state(chan) != AST_STATE_UP) {
		if (option_skip) {
			/* At the user's option, skip if the line is not up */
			goto done;
		} else if (!option_noanswer) {
			/* Otherwise answer unless we're supposed to send this while on-hook */
			res = ast_answer(chan);
		}
	}
	if (!res) {
		strcpy(tmp1,args.filenames);
		char *back = tmp1;
		char *front;

		ast_stopstream(chan);
        while(repeat || infinitely ){
    	  strcpy(tmp1,args.filenames);
          back = tmp1;
          if(mres) break;
		  while (!res && (front = strsep(&back, "&"))) {
			res = ast_streamfile(chan, front, ast_channel_language(chan));

			if (!res) {
				 res = ast_waitstream(chan, "");
				 ast_stopstream(chan);
				 if(option_queue){
                      sp_delete(front);
				 }
			}
			if (res) {
				ast_log(LOG_WARNING, "SimplePlayback failed on %s for %s\n", ast_channel_name(chan), (char *)data);
				res = 0;
				mres = 1;
				if(option_queue){
                      sp_delete(front);
				}
			}
		  }
		  repeat--;
        }
	}
done:
	pbx_builtin_setvar_helper(chan, "SIMPLEPLAYBACKSTATUS", mres ? "FAILED" : "SUCCESS");
	return res;
}


static int unload_module(void)
{
	int res;

	res = ast_unregister_application(app);


	return res;
}

static int load_module(void)
{

	return ast_register_application_xml(app, simpleplayback_exec);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Sound File SimplePlayback Application",
		.load = load_module,
		.unload = unload_module,
	       );




