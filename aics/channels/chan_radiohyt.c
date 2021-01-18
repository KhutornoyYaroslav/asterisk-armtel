/*
 * AICS -- Armtel Intelligent Communication Server
 *
 * Asterisk's channel driver - Hyterus RPC Protocol implementation
 *
 * Copyright (C) 2016-2018 Armtel Limited
 *
 * Developer Aleksey Dorofeev <leshadorofeev@gmail.com>
 *
 */

/*! \file
 *
 * \brief Implementation of the Hytera RPC protocol
 *
 * \author Aleksey Dorofeev
 * \ingroup channel_drivers
 */

/*! \li \ref chan_radiohyt.c uses the configuration file \ref radiohyt.conf
 * \addtogroup configuration_file
 */

/*! \page radiohyt.conf radiohyt.conf
 * \verbinclude radiohyt.conf.sample
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/paths.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/rtp_engine.h"
#include "asterisk/netsock2.h"
#include "asterisk/callerid.h"
#include "asterisk/cli.h"
#include "asterisk/causes.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"
#include "asterisk/stringfields.h"
#include "asterisk/abstract_jb.h"
#include "asterisk/threadstorage.h"
#include "asterisk/devicestate.h"
#include "asterisk/linkedlists.h"
#include "asterisk/bridge.h"
#include "asterisk/parking.h"
#include "asterisk/translate.h"
#include "asterisk/unaligned.h"
#ifdef AST_NATIVE_VERSION
#include "asterisk/format_cache.h"
#endif
#include "asterisk/test.h"

#include "radiohyt/env_debug.h"
#include "radiohyt/env_error.h"
#include "radiohyt/chan_radiohyt_functions.h"
#include "radiohyt/monitor.h"

#include "../include/asterisk/astobj2.h"
#include "../include/asterisk/utils.h"
#include "../include/asterisk/devicestate.h"
#include "../include/asterisk/channel.h"
#include "../include/asterisk/lock.h"
#include "../include/asterisk/format_cap.h"
#include "../include/asterisk/frame.h"
#include "../include/asterisk/pbx.h"
#include "../include/asterisk/config.h"
#include "../include/asterisk/logger.h"

#include "../include/aics_rtp_engine.h"

#include <strings.h>

#define AST_MODULE "chan_radiohyt"

#define MAX_SERVERS 16
#define MAX_CHANNELS 128
#define MAX_STATIONS 100
#define MAX_CALLGROUPS 1000
#define MAX_SUBSCRIBERS 10000

#define BUFSIZE 512

#define KEEPALIVE_SEND_TIMEOUT 30000
#define GARBAGE_COLLECT_TIMEOUT 60000
#define LOADING_TIMEOUT_USEC 2000000

#define HYTERUS_PACKETIZATION_TIME 20 	// Packetization time in ms
#define HYTERUS_RTP_EXT_HEADER_LENGTH 16

#ifdef TEST_FRAMEWORK
// If enable, module will loading AB from configuration files, not from server
# define LOADING_AB_FROM_CONFIG
#endif

// Use its macros for testing purposes only!
// If enable, module will use rtp stream into duplex mode
//#define ENABLE_FULL_DUPLEX

/*
 * Channel callbacks from channel FSM
 * NB: Callbacks are called from monitor thread!
 */
static void radiohyt_on_changed_connection_state(void *this, int state);
static void radiohyt_on_receive_station_list(void *this, int rc, struct ast_json *result, int option);
static void radiohyt_on_receive_group_list(void *this, int rc, struct ast_json *result, int option);
static void radiohyt_on_receive_subscriber_list(void *this, int rc, struct ast_json *result, int option);
static  int radiohyt_on_incoming_call(void *this, const struct ast_sockaddr *addr, const char *caller_id, int receiver_radio_id, int chan_id, char *sip_id);
static void radiohyt_on_changed_call_state(void *this, const char *caller_id, int radio_id, int state);
static void radiohyt_on_changed_channel_state(void *this, int channel_id, int state);
static void radiohyt_on_changed_subscriber_state(void *this, int radio_id, int channel_id, int state);

/*
 * Channel interface functions, declared into structure ast_channel_tech.
 * NB: Callbacks are called from PBX thread (function pbx_thread())!
 */
static struct ast_channel *radiohyt_request(const char *type,
    struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids,
    const struct ast_channel *requestor, const char *dest, int *cause);
static int radiohyt_devicestate(const char *data);
static int radiohyt_call(struct ast_channel *chan, const char *dest, int timeout);
static int radiohyt_answer(struct ast_channel *chan);
static int radiohyt_hangup(struct ast_channel *chan);
static const char *radiohyt_get_callid(struct ast_channel *chan);
static int radiohyt_write(struct ast_channel *chan, struct ast_frame *f);
static struct ast_frame *radiohyt_read(struct ast_channel *chan);
#ifdef TEST_FRAMEWORK
static int radiohyt_get_devicestate(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
#endif

/*
 * Infrastructure functions
 */
static struct ast_channel *radiohyt_request_incoming(Channel *chan,
    const struct ast_sockaddr *addr, const char *caller_id,
    int receiver_radio_id, int radio_id, int rtp_filter, int *cause);
static void *radiohyt_channel_thread(void *data);
#ifdef TEST_FRAMEWORK
static void *radiohyt_fake_channel_thread(void *data);
#endif

static int radiohyt_get_call_params_by_dest(const struct ast_channel *caller,
    const char *dest, struct ast_sockaddr *addr, int *chan_id, int *slot_id,
    int *dest_id, int *call_type, Channel **chan);

static int chan_cmp_cb(void *obj, void *arg, int flags);
static int chan_hash_cb(const void *obj, const int flags);
static int chan_set_debug_cb(void *obj, void *arg, int flags);
static int chan_keepalive_cb(void *obj, void *arg, int flags);
static int chan_cleanup_cb(void *obj, void *arg, int flags);

static Channel *radiohyt_find_chan(const char *name, const char *exten,
    struct ast_sockaddr *addr);
static Channel *radiohyt_find_chan_by_name(const char *name);
static Channel *radiohyt_find_chan_by_exten(const char *exten);

// NB: radiohyt_mutex must be locked before use this function
static Channel *radiohyt_find_free_channel(const struct ast_sockaddr *addr,
    const char *str_addr, int *cause);
static Channel *radiohyt_find_free_dynamic_channel(int *cause);
static Channel *radiohyt_open_new_channel(int *cause);

static int keepalive_task_cb(const void *data);
static int garbage_collector_cb(const void *data);

static int srv_cmp_cb(void *obj, void *arg, int flags);
static int srv_hash_cb(const void *obj, const int flags);
static int srv_cleanup_cb(void *obj, void *arg, int flags);

static int stations_cmp_cb(void *obj, void *arg, int flags);
static int stations_hash_cb(const void *obj, const int flags);
static int stations_cleanup_cb(void *obj, void *arg, int flags);

static int callgroups_cmp_cb(void *obj, void *arg, int flags);
static int callgroups_hash_cb(const void *obj, const int flags);
static int callgroups_cleanup_cb(void *obj, void *arg, int flags);

static int subscribers_cmp_cb(void *obj, void *arg, int flags);
static int subscribers_hash_cb(const void *obj, const int flags);
static int subscribers_cleanup_cb(void *obj, void *arg, int flags);

enum {
	CMP_SERVER_IP = 12345,	// если флаг установлен, то при сравнении объектов:
				// station, callgroup, subscriber будет учтен параметр ServerIP
};

static int save_station_list_to_config(RadioControlStationList *plist,
	const char *subscribers_filename, const char *mode);
static int save_group_list_to_config(RadioGroupList *plist,
	const char *subscribers_filename, const char *mode);
static int save_subscriber_list_to_config(RadioSubscriberList *plist,
	const char *subscribers_filename, const char *mode);
static int save_extensions_to_config(RadioGroupList *pglist, RadioSubscriberList *pslist,
	const char *extensions_filename, const char *mode);

static RadioSubscriber *load_subscriber_config(struct ast_config *cfg, const char *cat);
static RadioCallGroup *load_callgroup_config(struct ast_config *cfg, const char *cat);
static RadioControlStation *load_station_config(struct ast_config *cfg, const char *cat);

static Channel *load_channel_config(struct ast_config *cfg, const char *cat);

// returns:
// 0 - load is OK
// 1 - config file is unchanged
// < 0 - error
static int load_config(enum channelreloadreason reason);
static void apply_config(void);

/* these functions are called from main thread - main()*/
static int load_module(void);
static int unload_module(void);
static int reload_module(void);

#define DESTRUCTOR_FN(OBJ) OBJ ## _destructor

#define OBJ_DESTRUCTOR_FN(OBJ) \
static void DESTRUCTOR_FN(OBJ) (void *data) \
{ \
	ast_debug(2, "deleting " #OBJ  ": %p\n", data); \
}

OBJ_DESTRUCTOR_FN(Server)
OBJ_DESTRUCTOR_FN(RadioControlStation)
OBJ_DESTRUCTOR_FN(RadioCallGroup)
OBJ_DESTRUCTOR_FN(RadioSubscriber)

static void Channel_destructor(void *data)
{
	Channel *this = (Channel*)data;
	RadiohytRequest *req;
	RadiohytSession *sess;
	RadiohytChannelState *pstate;
	int i;

	ENV_ASSERT(this->owner == NULL);
	ENV_ASSERT(this->rtpi == NULL);
	ENV_ASSERT(this->sched_conn_id == -1);
	ENV_ASSERT(this->session == NULL);

	for (i = 0; i < sizeof(this->states)/sizeof(this->states[0]); ++i) {
		pstate = &this->states[i];

		// reset state's timer
		if (pstate->timer_id != -1) {
			ast_debug(1, "RADIOHYT[%s]: reset state(%s) timer(%d)\n",
				this->name, channel_state_to_str(pstate->state), pstate->timer_id);
			AST_SCHED_DEL(monitor_sched_ctx(), pstate->timer_id);
		}
	}
	while ((req = AST_LIST_REMOVE_HEAD(&this->request_queue, next))) {
		ast_free(req);
	}
	ast_debug(2, "deleting Channel: %s(%p)\n", this->name, data);
}


/*
 * global static variables
 */

enum load_option {
	LOAD = 0,
	FETCH_ONLY = 1,
	TEST_OUTPUT = 2,
	UPDATE_CONFIG = 3
};

static const char *radiohyt_version = "1.4.0";

static const char radiohyt_config_filename[] = "radiohyt.conf";
static const char radiohyt_subscribers_filename[] = "radiohyt_subscribers.conf";
static const char radiohyt_extensions_filename[] = "radiohyt_extensions.conf";

// Sheduler task id
static int keepalive_task_id = -1;
// Sheduler task id
static int garbage_collector_id = -1;

// Dump all packets
static int radiohyt_debug = 0;
// Policy of channel creation
static int radiohyt_channel_policy = STATIC;
// Maximum number of channels
static int radiohyt_channel_limit = MAX_CHANNELS;
// Number for new dynamic channel
static int radiohyt_channel_num = 0;
// Context for calls
static char radiohyt_context[BUFSIZE] = "default";
// Common calls counter
static int radiohyt_calls_cnt = 0;
// Mandatory channel name for dynamic configuration
static const char first_channel_name[] = "channel0";

static struct ao2_container *radiohyt_channels = NULL;
static struct ao2_container *radiohyt_servers = NULL;
static struct ao2_container *radiohyt_stations = NULL;
static struct ao2_container *radiohyt_subscribers = NULL;
static struct ao2_container *radiohyt_callgroups = NULL;

AST_MUTEX_DEFINE_STATIC(radiohyt_mutex);

static struct ast_channel_tech radiohyt_tech = {
	.type = "HYT",
	.description = "Hytera DMR Channel Driver (HYT)",
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = radiohyt_request,			/* called with chan unlocked */
	.devicestate = radiohyt_devicestate,		/* called with chan unlocked (not chan-specific) */
	.call = radiohyt_call,				/* called with chan locked */
	.hangup = radiohyt_hangup,			/* called with chan locked */
	.answer = radiohyt_answer,			/* called with chan locked */
	.write = radiohyt_write,			/* called with chan locked */
	.read = radiohyt_read,				/* called with chan locked */
	.get_pvt_uniqueid = radiohyt_get_callid,
	//.early_bridge = ast_rtp_instance_early_bridge,//like sip
};

/*
 *
 */
static char *complete_radiohyt_channels(const char *line, const char *word, int pos, int state, int chan_state)
{
	int which = 0;
	char *res = NULL;
	int wordlen = strlen(word);
	struct ao2_iterator i;
	Channel *chan;

	i = ao2_iterator_init(radiohyt_channels, 0);
	while ((chan = ao2_iterator_next(&i))) {
		if (!strncasecmp(chan->name, word, wordlen)
		    && ++which > state
		    && (!chan_state || chan->state == chan_state)) {
			res = ast_strdup(chan->name);
			ao2_ref(chan, -1);
			break;
		}
		ao2_ref(chan, -1);
	}
	ao2_iterator_destroy(&i);

	return res;
}

static char *handle_cli_radiohyt_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int rc;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt reload";
		e->usage =
			"       Reload RADIOHYT configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	rc = load_config(CHANNEL_CLI_RELOAD);
	if (rc < 0) {
		return CLI_FAILURE;
	}
	if (rc == 0) {
		ast_log(LOG_NOTICE, "RADIOHYT: configuration is reloaded, applying changes..\n");
		apply_config();
		ast_log(LOG_NOTICE, "RADIOHYT: configuration is applyed successfully\n");
	}
	return CLI_SUCCESS;
}

static inline void show_subscriber_list(int fd, RadioSubscriberList list)
{
#define FORMAT  "%-15.15s %-4.4s %-7.7s %-8.8s %-3.3s %-3.3s %-3.3s %-6.6s %-3.3s %-10.10s %-32.32s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-8s %-3.3s %-3.3s %-3.3s %-6d %-3.3s %-10.10s %-32.32s\n"
	RadioSubscriber *item = NULL;

	ast_cli(fd, "Subscriber list:\n");
	ast_cli(fd, FORMAT, "ServerIP", "Id", "RadioId", "GroupIds", "Gps", "Msg", "Mon", "ChanId", "Ena", "Type", "Name");
	AST_LIST_TRAVERSE(&list, item, entry) {
		ast_cli(fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->CallGroupIdStr,
			item->HasGpsSupport ? "yes" : "no",
			item->HasTextMsgSupport ? "yes" : "no",
			item->HasRemoteMonitorSupport ? "yes" : "no",
			item->ChannelId,
			item->IsEnabled ? "yes" : "no",
			radio_station_type_to_string(item->RadioStationType),
			item->Name
		);
	}
	ast_cli(fd, "\n");
#undef FORMAT
#undef FORMAT2
}

static inline void show_group_list(int fd, RadioGroupList list)
{
#define FORMAT  "%-15.15s %-4.4s %-7.7s %-10.10s %-32.32s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-10s %-32.32s\n"
	RadioCallGroup *item = NULL;

	ast_cli(fd, "Group list:\n");
	ast_cli(fd, FORMAT, "ServerIP", "Id", "RadioId", "ChannelIds", "Name");
	AST_LIST_TRAVERSE(&list, item, entry) {
		ast_cli(fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->ChannelIdStr,
			item->Name
		);
	}
	ast_cli(fd, "\n");
#undef FORMAT
#undef FORMAT2
}

static inline void show_station_list(int fd, RadioControlStationList list)
{
#define FORMAT  "%-15.15s %-4.4s %-7.7s %-4.4s %-6.6s %-6.6s %-32.32s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-4d %-6.6s %-6.6s %-32.32s\n"
	RadioControlStation *item = NULL;

	ast_cli(fd, "Station list:\n");
	ast_cli(fd, FORMAT, "ServerIP", "Id", "RadioId", "Slot", "Analog", "Online", "Name");
	AST_LIST_TRAVERSE(&list, item, entry) {
		ast_cli(fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->Slot,
			item->IsAnalog ? "yes" : "no",
			item->IsOnline ? "yes" : "no",
			item->Name
		);
	}
	ast_cli(fd, "\n");
#undef FORMAT
#undef FORMAT2
}

static int show_config_file(const char *filename)
{
	FILE *f;
	RAII_VAR(char *, line, NULL, free);
	size_t len;
	ssize_t read;

	if (!(f = fopen(filename, "r"))) {
		return -1;
	}
	while ((read = getline(&line, &len, f)) != -1) {
		ast_verbose("%s", line);
	}
	fclose(f);

	return E_OK;
}

/*
 * CLI commands
 */
static char *handle_cli_radiohyt_load_subscribers(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int rc;
	int load_srv_count = 0;
	Channel *chan = NULL;
	struct ao2_iterator i;
	struct ast_sockaddr *srv;
	int option;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt load subscribers {fetch-only|test-output|update-config}";
		e->usage =
			"Usage: radiohyt load subscribers {fetch-only|test-output|update-config}\n"
			"       fetch-only - only show into console\n"
			"       test-output - generate configuration files and show its into console\n"
			"       update-config - update configuration files\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (!strcasecmp(a->argv[3], "fetch-only")) {
		option = FETCH_ONLY;

	} else if (!strcasecmp(a->argv[3], "test-output")) {
		option = TEST_OUTPUT;

	} else if (!strcasecmp(a->argv[3], "update-config")) {
		FILE *f;
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(struct ast_str *, epath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(struct ast_str *, spath_bak, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(struct ast_str *, epath_bak, ast_str_create(BUFSIZE), ast_free);

		ast_str_set(&spath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_subscribers_filename);
		ast_str_set(&epath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_extensions_filename);
		ast_str_set(&spath_bak, BUFSIZE, "%s/%s.bak", ast_config_AST_CONFIG_DIR, radiohyt_subscribers_filename);
		ast_str_set(&epath_bak, BUFSIZE, "%s/%s.bak", ast_config_AST_CONFIG_DIR, radiohyt_extensions_filename);

		unlink(ast_str_buffer(spath_bak));
		unlink(ast_str_buffer(epath_bak));
		if (rename(ast_str_buffer(spath), ast_str_buffer(spath_bak)) == 0)
			ast_log(LOG_VERBOSE, "RADIOHYT: create backup file for %s\n",
				ast_str_buffer(spath));
		if (rename(ast_str_buffer(epath), ast_str_buffer(epath_bak)) == 0)
			ast_log(LOG_VERBOSE, "RADIOHYT: create backup file for %s\n",
				ast_str_buffer(epath));

		f = fopen(ast_str_buffer(spath), "w");
		fclose(f);
		f = fopen(ast_str_buffer(epath), "w");
		fclose(f);

		option = UPDATE_CONFIG;

	} else {
		return NULL;
	}

	i = ao2_iterator_init(radiohyt_servers, AO2_ITERATOR_DONTLOCK);
	while ((srv = ao2_iterator_next(&i))) {
		ast_mutex_lock(&radiohyt_mutex);

		if (radiohyt_channel_policy == DYNAMIC) {
			chan = radiohyt_find_free_dynamic_channel(&rc);
		} else {
			chan = radiohyt_find_free_channel(srv, NULL, &rc);
		}
		if (chan == NULL) {
			ast_log(LOG_WARNING, "RADIOHYT: can't find free channel for server: %s\n",
				ast_sockaddr_stringify(srv));
			ast_mutex_unlock(&radiohyt_mutex);
			continue;
		}
		ao2_ref(chan, 1);

		ast_mutex_unlock(&radiohyt_mutex);

		rc = radiohyt_get_stations(chan, option);
		if (rc) {
			ast_log(LOG_ERROR, "RADIOHYT[%s]: can't load subscribers from server: %s\n",
				chan->name, ast_sockaddr_stringify(srv));
			goto cont;
		}
		//next we should get call of radiohyt_on_receive_station_list()

		++load_srv_count;
cont:
		ao2_ref(chan, -1);
		ao2_ref(srv, -1);
	}
	ao2_iterator_destroy(&i);

	return CLI_SUCCESS;
}

static char *handle_cli_radiohyt_set_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt set debug {on|off|channel}";
		e->usage =
			"Usage: radiohyt set debug {on|off|channel channelname}\n"
			"       Enables/Disables dumping of RADIOHYT packets for debugging purposes.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 4 && !strcasecmp(a->argv[3], "channel"))
			return complete_radiohyt_channels(a->line, a->word, a->pos, a->n, 0);
		return NULL;
	}

	if (a->argc < e->args  || a->argc > e->args + 1)
		return CLI_SHOWUSAGE;

	if (!strcasecmp(a->argv[3], "channel")) {
		Channel *chan;

		if (a->argc != e->args + 1)
			return CLI_SHOWUSAGE;

		chan = radiohyt_find_chan_by_name(a->argv[4]);
		if (!chan) {
			ast_cli(a->fd, "RADIOHYT: channel '%s' does not exist\n", a->argv[4]);
			return CLI_FAILURE;
		}
		chan->debug = 1;
		ast_cli(a->fd, "RADIOHYT: Debug output is enabled for channel '%s'\n", a->argv[4]);

		ao2_ref(chan, -1);

	} else if (!strncasecmp(a->argv[3], "on", 2)) {
		radiohyt_debug = 1;
		ao2_callback(radiohyt_channels, OBJ_NODATA | OBJ_MULTIPLE, chan_set_debug_cb, &radiohyt_debug);
		ast_cli(a->fd, "RADIOHYT: Debug output is enabled\n");

	} else {
		radiohyt_debug = 0;
		ao2_callback(radiohyt_channels, OBJ_NODATA | OBJ_MULTIPLE, chan_set_debug_cb, &radiohyt_debug);
		ast_cli(a->fd, "RADIOHYT: Debug output is disabled\n");
	}
	return CLI_SUCCESS;
}

#define str_datetime_sample "year,54w,7d,23:59.59"
#define str_datetime_bufsize 21

static void print_tvdiff(uint64_t sec, char *buf, int bufsize)
{
#define Km (60)
#define Kh (60 * Km)
#define Kd (24 * Kh)
#define Kw ( 7 * Kd)
#define Ky (365* Kd)
	int y,w,d,h,m,s;

	y = sec / Ky;
	sec -= y * Ky;
	w = sec / Kw;
	sec -= w * Kw;
	d = sec / Kd;
	sec -= d * Kd;
	h = sec / Kh;
	sec -= h * Kh;
	m = sec / Km;
	s = sec % Km;

	if (y) {
		snprintf(buf, bufsize, "year,%02dw,%1dd,%02d:%02d.%02d", w, d, h, m, s);
	} else if (w) {
		snprintf(buf, bufsize,      "%02dw,%1dd,%02d:%02d.%02d", w, d, h, m, s);
	} else if (d) {
		snprintf(buf, bufsize,            "%1dd,%02d:%02d.%02d", d, h, m, s);
	} else {
		snprintf(buf, bufsize,                 "%02d:%02d.%02d", h, m, s);
	}
}

static char *handle_cli_radiohyt_show_channels(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT  "%-10.10s %-22.22s %-5.5s %-22.22s %-5.5s %-10.10s %-20.20s %-32.32s %-9.9s %-13.13s\n"
#define FORMAT2 "%-10.10s %-22.22s %05d %-22.22s %05d %-10.10s %-20.20s %-32.32s %-9.9s %-13.13s\n"
	int numchans = 0;
	int numcalls = 0;
	uint64_t sec = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt show channels";
		e->usage =
			"Usage: radiohyt show channels\n"
			"       Lists all currently active RADIOHYT channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, FORMAT, "Channel", "LocalAddr", "Rtp", "SrvAddr", "Rtp", "State", "Duration", "CallId", "Format", "Peer");
	{
		Channel *c;
		char laddr[23];
		char saddr[23];
		char callid[AST_UUID_STR_LEN+1];
		char duration[str_datetime_bufsize];
		char format[10];
		char peer[14];
		struct timeval now;

		struct ao2_iterator i = ao2_iterator_init(radiohyt_channels, 0);
		gettimeofday(&now, 0);
		while ((c = ao2_iterator_next(&i))) {
			snprintf(callid, sizeof(callid), "(nothing)");
			snprintf(format, sizeof(format), "(nothing)");
			snprintf(peer,   sizeof(peer),   "(nothing)");

			switch (c->state) {
			case CHAN_RADIOHYT_ST_IDLE:
			case CHAN_RADIOHYT_ST_TRYC:
				sec = ast_tvdiff_sec(now, c->tport.stat.disconnection_ts);
				break;
			case CHAN_RADIOHYT_ST_CONN:
			case CHAN_RADIOHYT_ST_WFA:
			case CHAN_RADIOHYT_ST_READY:
			case CHAN_RADIOHYT_ST_DIAL:
			case CHAN_RADIOHYT_ST_RING:
			case CHAN_RADIOHYT_ST_CANCEL:
				sec = ast_tvdiff_sec(now, c->tport.stat.connection_ts);
				++numchans;
				break;
			case CHAN_RADIOHYT_ST_DETACH:
			case CHAN_RADIOHYT_ST_DETACH_2:
			case CHAN_RADIOHYT_ST_HANGTIME:
				ast_copy_string(callid, c->sid, sizeof(callid));
				sprintf(peer, "%d", c->peer_radio_id);
				//todo
				//sec = ast_tvdiff_sec(now, c->tport.stat.voice_request_answer_ts);
				++numchans;
				break;
			case CHAN_RADIOHYT_ST_BUSY:
			{
				struct ast_channel *chan;

				ao2_lock(c);
				chan = c->owner;
				if (chan) {
					ast_copy_string(callid, c->sid, sizeof(callid));

					ao2_ref(chan, 1);
					ao2_unlock(c);

					++numcalls;

					ast_channel_lock(chan);

					ast_getformatname_multiple(format, sizeof(format), ast_channel_nativeformats(chan));
					sprintf(peer, "%d", c->peer_radio_id);

					ast_channel_unlock(chan);
					ao2_ref(chan, -1);
				} else {
					ENV_ASSERT(0);
					ao2_unlock(c);
				}

				sec = ast_tvdiff_sec(now, c->tport.stat.voice_request_answer_ts);
				++numchans;
				break;
			}
			}

			ast_copy_string(laddr, ast_sockaddr_stringify(&c->addr),     sizeof(laddr));
			ast_copy_string(saddr, ast_sockaddr_stringify(&c->srv_addr), sizeof(saddr));
			print_tvdiff(sec, duration, str_datetime_bufsize);

			ast_cli(a->fd, FORMAT2, c->name, laddr, c->rtp_port, saddr, c->srv_rtp_port,
				channel_state_to_str(c->state), duration, callid, format, peer);

			ao2_ref(c, -1);
		}
		ao2_iterator_destroy(&i);
	}
	ast_cli(a->fd, "%d active RADIOHYT channel%s\n", numchans, (numchans != 1) ? "s" : "");
	ast_cli(a->fd, "%d active RADIOHYT call%s\n",    numcalls, (numcalls != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *handle_cli_radiohyt_show_statistics(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT  "%-10.10s %-20.20s %-9.9s %-9.9s %-9.9s %-9.9s %-26.26s\n"
#define FORMAT2 "%-10.10s %-20.20s %9d %9d %09ld %09ld %03d(min):%03d(max):%03d(ave)\n"
	int numchans = 0;
	uint64_t sec;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt show statistics";
		e->usage =
			"Usage: radiohyt show statistics\n"
			"       Show statistics per RADIOHYT channel.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, FORMAT, "Channel", "LifeTime", "ConnCount", "CallCount", "Recv(msg)", "Send(msg)", "AnswerTime(msec)");
	{
		Channel *c;
		char duration[str_datetime_bufsize];
		struct timeval now;

		struct ao2_iterator i = ao2_iterator_init(radiohyt_channels, 0);
		gettimeofday(&now, 0);
		while ((c = ao2_iterator_next(&i))) {
			sec = ast_tvdiff_sec(now, c->tport.stat.creation_ts);
			print_tvdiff(sec, duration, str_datetime_bufsize);

			switch (c->state) {
			case CHAN_RADIOHYT_ST_IDLE:
			case CHAN_RADIOHYT_ST_TRYC:
				break;
			case CHAN_RADIOHYT_ST_CONN:
			case CHAN_RADIOHYT_ST_WFA:
			case CHAN_RADIOHYT_ST_READY:
			case CHAN_RADIOHYT_ST_DIAL:
			case CHAN_RADIOHYT_ST_RING:
			case CHAN_RADIOHYT_ST_BUSY:
			case CHAN_RADIOHYT_ST_CANCEL:
			case CHAN_RADIOHYT_ST_DETACH:
			case CHAN_RADIOHYT_ST_DETACH_2:
			case CHAN_RADIOHYT_ST_HANGTIME:
				++numchans;
				break;
			}

			ast_cli(a->fd, FORMAT2,
				c->name,
				duration,
				c->tport.stat.connections_cnt,
				c->tport.stat.calls_cnt,
				c->tport.stat.recv_msg_cnt,
				c->tport.stat.send_msg_cnt,
				c->tport.stat.min_answer_time_ms,
				c->tport.stat.max_answer_time_ms,
				c->tport.stat.ave_answer_time_ms
			);

			ao2_ref(c, -1);
		}
		ao2_iterator_destroy(&i);
	}
	ast_cli(a->fd, "%d active RADIOHYT channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *handle_cli_radiohyt_show_subscribers(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int cnt = 0;
	struct ao2_iterator i;
	RadioSubscriber *item = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt show subscribers";
		e->usage =
			"Usage: radiohyt show subscribers\n"
			"       Show status of radio terminals.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

#define FORMAT  "%-15.15s %-4.4s %-7.7s %-8.8s %-3.3s %-3.3s %-3.3s %-6.6s %-3.3s %-10.10s %-32.32s %-11.11s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-8s %-3.3s %-3.3s %-3.3s %-6d %-3.3s %-10.10s %-32.32s %-11.11s\n"
	ast_cli(a->fd, FORMAT, "ServerIP", "Id", "RadioId", "GroupIds", "Gps", "Msg", "Mon", "ChanId", "Ena", "Type", "Name", "Status");
	i = ao2_iterator_init(radiohyt_subscribers, AO2_ITERATOR_DONTLOCK);
	while ((item = ao2_iterator_next(&i))) {
		++cnt;
		ast_cli(a->fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->CallGroupIdStr,
			item->HasGpsSupport ? "yes" : "no",
			item->HasTextMsgSupport ? "yes" : "no",
			item->HasRemoteMonitorSupport ? "yes" : "no",
			item->ChannelId,
			item->IsEnabled ? "yes" : "no",
			radio_station_type_to_string(item->RadioStationType),
			item->Name,
			ast_devstate2str(item->devstate)
		);
		ao2_ref(item, -1);
	}
	ao2_iterator_destroy(&i);

	ast_cli(a->fd, "%d RADIOHYT subscriber%s\n", cnt, (cnt != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *handle_cli_radiohyt_show_groups(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int cnt = 0;
	struct ao2_iterator i;
	RadioCallGroup *item = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt show groups";
		e->usage =
			"Usage: radiohyt show groups\n"
			"       Show status of radio callgroups.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

#define FORMAT  "%-15.15s %-4.4s %-7.7s %-10.10s %-32.32s %-11.11s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-10s %-32.32s %-11.11s\n"
	ast_cli(a->fd, FORMAT, "ServerIP", "Id", "RadioId", "ChannelIds", "Name", "Status");
	i = ao2_iterator_init(radiohyt_callgroups, AO2_ITERATOR_DONTLOCK);
	while ((item = ao2_iterator_next(&i))) {
		++cnt;
		ast_cli(a->fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->ChannelIdStr,
			item->Name,
			ast_devstate2str(item->devstate)
		);
		ao2_ref(item, -1);
	}
	ao2_iterator_destroy(&i);

	ast_cli(a->fd, "%d RADIOHYT callgroup%s\n", cnt, (cnt != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static char *handle_cli_radiohyt_show_stations(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int cnt = 0;
	struct ao2_iterator i;
	RadioControlStation *item = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "radiohyt show stations";
		e->usage =
			"Usage: radiohyt show stations\n"
			"       Show status of radio channels.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

#define FORMAT  "%-15.15s %-4.4s %-7.7s %-4.4s %-6.6s %-6.6s %-32.32s\n"
#define FORMAT2 "%-15.15s %-4d %-7d %-4d %-6.6s %-6.6s %-32.32s\n"
	ast_cli(a->fd, "Station list:\n");
	ast_cli(a->fd, FORMAT, "ServerIP", "Id", "RadioId", "Slot", "Analog", "Online", "Name");
	i = ao2_iterator_init(radiohyt_stations, AO2_ITERATOR_DONTLOCK);
	while ((item = ao2_iterator_next(&i))) {
		++cnt;
		ast_cli(a->fd, FORMAT2,
			item->ServerIP,
			item->Id,
			item->RadioId,
			item->Slot,
			item->IsAnalog ? "yes" : "no",
			item->IsOnline ? "yes" : "no",
			item->Name
		);
		ao2_ref(item, -1);
	}
	ao2_iterator_destroy(&i);

	ast_cli(a->fd, "%d RADIOHYT station%s\n", cnt, (cnt != 1) ? "s" : "");
	return CLI_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

/* this functions are called from main thread - main()*/
static struct ast_cli_entry cli_radiohyt[] = {
	AST_CLI_DEFINE(handle_cli_radiohyt_reload,              "Reload configuration"),
	AST_CLI_DEFINE(handle_cli_radiohyt_load_subscribers,    "Load stations, subscribers and callgroups"),
	AST_CLI_DEFINE(handle_cli_radiohyt_set_debug,           "Enable/Disable debugging"),
	AST_CLI_DEFINE(handle_cli_radiohyt_show_channels,       "Show list of active channels"),
	AST_CLI_DEFINE(handle_cli_radiohyt_show_statistics,     "Display channels statistics"),
	AST_CLI_DEFINE(handle_cli_radiohyt_show_subscribers,    "Show list of radio subscribers"),
	AST_CLI_DEFINE(handle_cli_radiohyt_show_groups,         "Show list of radio callgroups"),
	AST_CLI_DEFINE(handle_cli_radiohyt_show_stations,       "Show list of radio stations"),
};

/*
 *
 */
 
/* retcode:
 * 0 - found free channel
 * AST_CAUSE_USER_BUSY - not found free channel
 * AST_CAUSE_NORMAL_TEMPORARY_FAILURE - server is unavailable now
 */
static Channel *radiohyt_find_free_channel(const struct ast_sockaddr *addr, const char *str_addr, int *cause)
{
	Channel *chan;
	struct ao2_iterator i;

	i = ao2_iterator_init(radiohyt_channels, AO2_ITERATOR_DONTLOCK);
	while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
		if ((!(addr || str_addr)
		     || (addr && !ast_sockaddr_cmp_addr(addr, &chan->srv_addr))
		     || (str_addr && !strcmp(str_addr, chan->server_ip)))
		    && (chan->state == CHAN_RADIOHYT_ST_READY)
		    && (!chan->owner))
		{
			if (chan->state == CHAN_RADIOHYT_ST_READY) {
				ao2_ref(chan, -1);
				*cause = 0;
				return chan;
			} else if (chan->state == CHAN_RADIOHYT_ST_TRYC) {
				ao2_ref(chan, -1);
				*cause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
				return NULL;
			}
		}
		ao2_ref(chan, -1);
	}
	ao2_iterator_destroy(&i);

	*cause = AST_CAUSE_USER_BUSY;
	return NULL;
}

static Channel *radiohyt_find_free_dynamic_channel(int *cause)
{
	Channel *chan = radiohyt_find_chan_by_name(first_channel_name);
	if (chan->state < CHAN_RADIOHYT_ST_READY) {
		*cause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
		return NULL;
	}

	if (!(chan = radiohyt_find_free_channel(NULL, NULL, cause))) {
		if (*cause == AST_CAUSE_USER_BUSY)
			chan = radiohyt_open_new_channel(cause);
	}
	return chan;
}

static Channel *radiohyt_open_new_channel(int *cause)
{
	Channel *chan = NULL;
	int channels_count = ao2_container_count(radiohyt_channels);
	const char *config_file = radiohyt_config_filename;
	struct ast_config *cfg;
	struct ast_flags flags = { 0 };

	if (radiohyt_channel_limit == channels_count) {
		ast_log(LOG_WARNING, "RADIOHYT: Limit of opened RADIOHYT channels is reached\n");
		*cause = AST_CAUSE_USER_BUSY;
		return NULL;
	}

	cfg = ast_config_load(config_file, flags);
	if (!cfg) {
		ast_log(LOG_ERROR, "RADIOHYT: Can't create new channel - unable to load config %s\n", config_file);
		*cause = AST_CAUSE_FAILURE;
		return NULL;
	}
	chan = load_channel_config(cfg, first_channel_name);
	ast_config_destroy(cfg);
	if (!chan) {
		ast_log(LOG_ERROR, "RADIOHYT: Can't create new channel - unable to load mandatory section '[%s]'\n",
			first_channel_name);
		*cause = AST_CAUSE_FAILURE;
		return NULL;
	}
	snprintf(chan->name, sizeof(chan->name), "channel%d", ++radiohyt_channel_num);
	chan->debug = radiohyt_debug;
	chan->policy = radiohyt_channel_policy;

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: channel is successfully created\n", chan->name);

	// connect & authorize on the Hyterus server
	{
		int timeout = 2000000;
		int rc = radiohyt_chan_open(chan);
		if (rc) {
			ast_log(LOG_ERROR, "RADIOHYT[%s]: channel is not connected yet\n", chan->name);
			ao2_ref(chan, -1);
			*cause = AST_CAUSE_FAILURE;
			return NULL;
		}
		// wait for authentication into server
		do {
			usleep(10000);
			timeout -= 10000;
			if (chan->state == CHAN_RADIOHYT_ST_READY) {
				goto end;
			}
		} while (timeout);

		ast_log(LOG_ERROR, "RADIOHYT[%s]: channel is not authorized yet\n", chan->name);
		ao2_ref(chan, -1);
		*cause = AST_CAUSE_FAILURE;
		return NULL;
	}
end:
	ao2_t_link(radiohyt_channels, chan, "link channel into channels table");
	return chan;
}

/*
 * Callback functions
 */
static void radiohyt_on_changed_connection_state(void *arg, int state)
{
	Channel *this = (Channel *)arg;

	ast_debug(1, "RADIOHYT[%s]: state is changed to: %d, subscribers_loaded is %d, channel_policy is %d\n",
		this->name, this->state, this->is_subscribers_loaded, radiohyt_channel_policy);

	if (state == CHAN_RADIOHYT_ST_READY) {
		if (!this->is_subscribers_loaded
		    && ((radiohyt_channel_policy == STATIC)
			|| ((radiohyt_channel_policy == DYNAMIC) && !strcmp(this->name, first_channel_name))))
		{
			ast_log(LOG_NOTICE, "RADIOHYT[%s]: starting to load radio network configuration\n",
				this->name);

			int rc = radiohyt_get_stations(this, LOAD);
			if (rc) {
				ast_log(LOG_ERROR, "RADIOHYT[%s]: can't load station list from server: %s\n",
					this->name, ast_sockaddr_stringify(&this->srv_addr));
			}
			//next we should get call of radiohyt_on_receive_station_list()
		} else {
			ast_debug(1, "RADIOHYT[%s]: ignore this event\n", this->name);
		}

	} else if (state == CHAN_RADIOHYT_ST_TRYC) {
		if ((radiohyt_channel_policy == STATIC)
		    || ((radiohyt_channel_policy == DYNAMIC) && !strcmp(this->name, first_channel_name)))
		{
			RadioCallGroup *rg;
			RadioSubscriber *rs;
			struct ao2_iterator i;

			i = ao2_iterator_init(radiohyt_callgroups, 0);
			while ((rg = ao2_t_iterator_next(&i, "iterate thru callgroups table"))) {
				if (!strcmp(this->server_ip, rg->ServerIP)) {
					rg->devstate = AST_DEVICE_UNAVAILABLE;
					ast_devstate_changed(rg->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rg->RadioId);
					ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
						this->name, rg->RadioId, ast_devstate_str(rg->devstate));
				}
				ao2_ref(rg, -1);
			}
			ao2_iterator_restart(&i);

			i = ao2_iterator_init(radiohyt_subscribers, 0);
			while ((rs = ao2_t_iterator_next(&i, "iterate thru subscribers table"))) {
				if (!strcmp(this->server_ip, rs->ServerIP)) {
					rs->devstate = AST_DEVICE_UNAVAILABLE;
					ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
					ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
						this->name, rs->RadioId, ast_devstate_str(rs->devstate));
				}
				ao2_ref(rs, -1);
			}
			ao2_iterator_destroy(&i);
#ifndef LOADING_AB_FROM_CONFIG
			ast_log(LOG_NOTICE, "RADIOHYT[%s]: need to load radio network configuration after re-connect\n",
				this->name);
			this->is_subscribers_loaded = 0;
#endif
		} else {
			ast_debug(1, "RADIOHYT[%s]: ignore this event\n", this->name);
		}

	} else if (state == CHAN_RADIOHYT_ST_BUSY) {
		if (this->owner) {
			ast_setstate(this->owner, AST_STATE_UP);
		} else {
			ast_log(LOG_WARNING, "RADIOHYT[%s]: set busy state for channel without owner ast_channel!\n", 
				this->name);
		}

	} else {
		//all other states are ignored yet
		ast_debug(1, "RADIOHYT[%s]: ignore this event\n", this->name);
	}
}

static void radiohyt_on_receive_station_list(void *arg, int rc, struct ast_json *result, int option)
{
	Channel *this = (Channel *)arg;
	RadioControlStationList list;
	RadioControlStation *item;
	int i, cnt = ast_json_array_size(result);
	const char *config_file = radiohyt_config_filename;
	struct ast_config *cfg = NULL;
	struct ast_flags flags = { 0 };

	AST_LIST_HEAD_INIT_NOLOCK(&list);

	if (option == LOAD) {
		cfg = ast_config_load(config_file, flags);
	}

	for (i = 0; i < cnt; ++i) {
		struct ast_json *object = ast_json_array_get(result, i);
		RadioControlStation *item;

		if (option == LOAD) {
			if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioControlStation), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioControlStation struct")))
				break;
		} else {
			item = (RadioControlStation *)ast_calloc(1, sizeof(RadioControlStation));
		}

		snprintf(item->Name, sizeof(item->Name), "\"%s\"", ast_json_string_get(ast_json_object_get(object, "Name")));
		item->Id = ast_json_integer_get(ast_json_object_get(object, "Id"));
		item->RadioId = ast_json_integer_get(ast_json_object_get(object, "RadioId"));
		item->Slot = ast_json_integer_get(ast_json_object_get(object, "Slot"));
		item->IsAnalog = ast_json_is_true(ast_json_object_get(object, "IsAnalog"));
		item->IsOnline = ast_json_is_true(ast_json_object_get(object, "IsOnline"));
		ast_copy_string(item->ServerIP, ast_sockaddr_stringify_host(&this->srv_addr), sizeof(item->ServerIP));

		if (option == LOAD) {
			//fixme: load SipId from configuration file
			if (cfg) {
				RadioControlStation *cfg_item;
				char station[128];

				sprintf(station, "%d_%d", item->RadioId, item->Id);
				cfg_item = load_station_config(cfg, station);
				if (cfg_item) {
					item->SipId = cfg_item->SipId;
					item->RtpFilter = cfg_item->RtpFilter;
					ast_log(LOG_NOTICE, "RADIOHYT: found SipId %d for station %s(%d_%d)\n",
						item->SipId, item->Name, item->RadioId, item->Id);
					ao2_ref(cfg_item, -1);
				} else {
					ast_log(LOG_WARNING, "RADIOHYT: not found SipId for station %s(%d_%d)\n",
						item->Name, item->RadioId, item->Id);
				}
			}
			// store item
			RadioControlStation *old_item = ao2_t_find(radiohyt_stations, item, OBJ_POINTER, "find item into stations table");
			if (old_item) {
				ast_debug(2, "RADIOHYT: unlink station %d '%s'\n", old_item->RadioId, old_item->Name);
				ao2_t_unlink(radiohyt_stations, old_item, "unlink item from stations table");
			}
			ast_debug(2, "RADIOHYT: loaded new version of station %d '%s'\n", item->RadioId, item->Name);
			ao2_t_link(radiohyt_stations, item, "link item into stations table");
		} else {
			AST_LIST_INSERT_TAIL(&list, item, entry);
		}
	}

	switch (option) {
	case LOAD:
	{
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: successfully loaded %d stations\n",
			this->name, cnt);
		break;
	}
	case FETCH_ONLY:
	{
		show_station_list(1, list);
		break;
	}
	case TEST_OUTPUT:
	{
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(char *, tmp_fn, NULL, free);

		ast_str_set(&spath, BUFSIZE, "%s/asterisk/tmp", ast_config_AST_SPOOL_DIR);
		tmp_fn = tempnam(ast_str_buffer(spath), "radiohyt");

		if (save_station_list_to_config(&list, tmp_fn, "w") == E_OK)
			show_config_file(tmp_fn);

		unlink(tmp_fn);
		break;
	}
	case UPDATE_CONFIG:
	{
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);

		ast_str_set(&spath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_subscribers_filename);

		save_station_list_to_config(&list, ast_str_buffer(spath), "a");
		break;
	}
	}

	// free list
	while ((item = AST_LIST_REMOVE_HEAD(&list, entry)) != NULL)
		ast_free(item);

	if (cfg)
		ast_config_destroy(cfg);

	rc = radiohyt_get_groups(this, option);
	if (rc) {
		ast_log(LOG_ERROR, "RADIOHYT[%s]: can't load callgroup list from server: %s\n",
			this->name, ast_sockaddr_stringify(&this->srv_addr));
	}
	//next we should get call of radiohyt_on_receive_group_list()
}

static void radiohyt_on_receive_group_list(void *arg, int rc, struct ast_json *result, int option)
{
	Channel *this = (Channel *)arg;
	RadioGroupList list;
	RadioCallGroup *item;
	int i, cnt = ast_json_array_size(result);
	const char *config_file = radiohyt_config_filename;
	struct ast_config *cfg = NULL;
	struct ast_flags flags = { 0 };

	AST_LIST_HEAD_INIT_NOLOCK(&list);

	if (option == LOAD) {
		cfg = ast_config_load(config_file, flags);
	}

	for (i = 0; i < cnt; ++i) {
		struct ast_json *object = ast_json_array_get(result, i);
		struct ast_json *chans = ast_json_object_get(object, "ChannelIds");
		int j, gcnt = ast_json_array_size(chans);

		if (option == LOAD) {
			if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioCallGroup), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioCallGroup struct")))
				break;
		} else {
			item = (RadioCallGroup *)ast_calloc(1, sizeof(RadioCallGroup));
		}

		snprintf(item->Name, sizeof(item->Name), "\"%s\"", ast_json_string_get(ast_json_object_get(object, "Name")));
		item->Id = ast_json_integer_get(ast_json_object_get(object, "Id"));
		item->RadioId = ast_json_integer_get(ast_json_object_get(object, "RadioId"));
		item->ChannelIdStr[0] = '\0';
		for (j = 0; j < gcnt && j < HYTERUS_MAX_GROUP_STATIONS; ++j) {
			item->ChannelId[j] = ast_json_integer_get(ast_json_array_get(chans, j));
			snprintf(&item->ChannelIdStr[strlen(item->ChannelIdStr)], HYTERUS_IDS_MAX_LEN, "%d,", item->ChannelId[j]);
		}
		ast_copy_string(item->ServerIP, ast_sockaddr_stringify_host(&this->srv_addr), sizeof(item->ServerIP));

		if (option == LOAD) {
			//fixme: load SipId from configuration file
			if (cfg) {
				RadioCallGroup *cfg_item;
				char group[128];

				sprintf(group, "%d", item->RadioId);
				cfg_item = load_callgroup_config(cfg, group);
				if (cfg_item) {
					item->SipId = cfg_item->SipId;
					item->RtpFilter = cfg_item->RtpFilter;
					ast_log(LOG_NOTICE, "RADIOHYT: found SipId %d for group %s(%d)\n",
						item->SipId, item->Name, item->RadioId);
					ao2_ref(cfg_item, -1);
				} else {
					ast_log(LOG_WARNING, "RADIOHYT: not found SipId for group %s(%d)\n",
						item->Name, item->RadioId);
				}
			}

			// update group list
			RadioCallGroup *old_item = ao2_t_find(radiohyt_callgroups, item, OBJ_POINTER, "find item into callgroups table");
			if (old_item) {
				ast_debug(2, "RADIOHYT: unlink group %d '%s'\n", old_item->RadioId, old_item->Name);
				ao2_t_unlink(radiohyt_callgroups, old_item, "unlink item from callgroups table");
			}
			ast_debug(2, "RADIOHYT: loaded new version of station %d '%s'\n", item->RadioId, item->Name);
			ao2_t_link(radiohyt_callgroups, item, "link item into callgroups table");

			// set status
			RadioControlStation rc_item = { .Id = item->ChannelId[0] };
			RadioControlStation *rc = ao2_t_find(radiohyt_stations, &rc_item, OBJ_POINTER, "find item into stations table");
			if (rc) {
				item->devstate = rc->IsOnline ? AST_DEVICE_NOT_INUSE : AST_DEVICE_UNAVAILABLE;
				ast_devstate_changed(item->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", item->RadioId);
				ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
					this->name, item->RadioId, ast_devstate_str(item->devstate));
			} else {
				ast_log(LOG_WARNING, "RADIOHYT[%s]: misconfigured group %s(%d)!\n",
					this->name, item->Name, item->RadioId);
			}

		} else {
			AST_LIST_INSERT_TAIL(&list, item, entry);
		}
	}

	switch (option) {
	case LOAD:
	{
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: successfully loaded %d callgroups\n",
			this->name, cnt);
		break;
	}
	case FETCH_ONLY:
	{
		show_group_list(1, list);
		break;
	}
	case TEST_OUTPUT:
	{
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(char *, tmp_fn, NULL, free);

		ast_str_set(&spath, BUFSIZE, "%s/asterisk/tmp", ast_config_AST_SPOOL_DIR);
		tmp_fn = tempnam(ast_str_buffer(spath), "radiohyt");

		if (save_group_list_to_config(&list, tmp_fn, "w") == E_OK)
			rc = show_config_file(tmp_fn);

		if (save_extensions_to_config(&list, NULL, tmp_fn, "a") == E_OK)
			rc = show_config_file(tmp_fn);

		unlink(tmp_fn);
		break;
	}
	case UPDATE_CONFIG:
	{
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(struct ast_str *, epath, ast_str_create(BUFSIZE), ast_free);

		ast_str_set(&spath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_subscribers_filename);
		ast_str_set(&epath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_extensions_filename);

		save_group_list_to_config(&list, ast_str_buffer(spath), "a");
		save_extensions_to_config(&list, NULL, ast_str_buffer(epath), "a");
		break;
	}
	}

	// free list
	while ((item = AST_LIST_REMOVE_HEAD(&list, entry)) != NULL)
		ast_free(item);

	rc = radiohyt_get_subscribers(this, option);
	if (rc) {
		ast_log(LOG_ERROR, "RADIOHYT[%s]: can't load subscriber's list from server: %s\n",
			this->name, ast_sockaddr_stringify(&this->srv_addr));
	}
	//next we should get call of radiohyt_on_receive_subscriber_list()
}

static void radiohyt_on_receive_subscriber_list(void *arg, int rc, struct ast_json *result, int option)
{
	Channel *this = (Channel *)arg;
	RadioSubscriberList list;
	RadioSubscriber *item;
	int i, cnt = ast_json_array_size(result);

	AST_LIST_HEAD_INIT_NOLOCK(&list);

	for (i = 0; i < cnt; ++i) {
		struct ast_json *object = ast_json_array_get(result, i);
		struct ast_json *groups = ast_json_object_get(object, "CallGroupIds");
		int j, gcnt = ast_json_array_size(groups);

		if (option == LOAD) {
			if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioSubscriber), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioSubscriber struct")))
				break;
		} else {
			item = (RadioSubscriber *)ast_calloc(1, sizeof(RadioSubscriber));
		}

		snprintf(item->Name, sizeof(item->Name), "\"%s\"", ast_json_string_get(ast_json_object_get(object, "Name")));
		item->Id = ast_json_integer_get(ast_json_object_get(object, "Id"));
		item->RadioId = ast_json_integer_get(ast_json_object_get(object, "RadioId"));
		item->ChannelId = ast_json_integer_get(ast_json_object_get(object, "ChannelId"));
		item->CallGroupIdStr[0] = '\0';
		for (j = 0; j < gcnt && j < HYTERUS_MAX_SUBSCRIBER_GROUPS; ++j) {
			item->CallGroupId[j] = ast_json_integer_get(ast_json_array_get(groups, j));
			snprintf(&item->CallGroupIdStr[strlen(item->CallGroupIdStr)], HYTERUS_IDS_MAX_LEN, "%d,", item->CallGroupId[j]);
		}
		item->HasGpsSupport = ast_json_is_true(ast_json_object_get(object, "HasGpsSupport"));
		item->HasTextMsgSupport = ast_json_is_true(ast_json_object_get(object, "HasTextMsgSupport"));
		item->HasRemoteMonitorSupport = ast_json_is_true(ast_json_object_get(object, "HasRemoteMonitorSupport"));
		item->RadioStationType = ast_json_integer_get(ast_json_object_get(object, "RadioStationType"));
		item->IsOnline = ast_json_is_true(ast_json_object_get(object, "IsOnline"));
		item->IsEnabled = ast_json_is_true(ast_json_object_get(object, "IsEnabled"));

		ast_copy_string(item->ServerIP, ast_sockaddr_stringify_host(&this->srv_addr), sizeof(item->ServerIP));

		if (option == LOAD) {
			RadioSubscriber *old_item = ao2_t_find(radiohyt_subscribers, item, OBJ_POINTER, "find item into subscribers table");
			if (old_item) {
				ast_debug(2, "RADIOHYT: unlink subscriber %d '%s'\n", old_item->RadioId, old_item->Name);
				ao2_t_unlink(radiohyt_subscribers, old_item, "unlink item from subscribers table");
			}
			ast_debug(2, "RADIOHYT: loaded new version of subscriber %d '%s'\n", item->RadioId, item->Name);
			ao2_t_link(radiohyt_subscribers, item, "link item into subscribers table");

			RadioControlStation rc_item = { .Id = item->ChannelId };
			RadioControlStation *rc = ao2_t_find(radiohyt_stations, &rc_item, OBJ_POINTER, "find item into stations table");
			if (rc) {
				item->devstate = item->IsOnline ? AST_DEVICE_NOT_INUSE : AST_DEVICE_UNAVAILABLE;
				ast_devstate_changed(item->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", item->RadioId);
				ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
					this->name, item->RadioId, ast_devstate_str(item->devstate));
			} else {
				ast_log(LOG_WARNING, "RADIOHYT[%s]: misconfigured subscriber %s(%d)!\n",
					this->name, item->Name, item->RadioId);
			}

		} else {
			AST_LIST_INSERT_TAIL(&list, item, entry);
		}
	}

	switch (option) {
	case LOAD:
	{
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: successfully loaded %d subscribers\n",
			this->name, cnt);
		break;
	}
	case FETCH_ONLY:
	{
		show_subscriber_list(1, list);
		break;
	}
	case TEST_OUTPUT:
	{
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(char *, tmp_fn, NULL, free);

		ast_str_set(&spath, BUFSIZE, "%s/asterisk/tmp", ast_config_AST_SPOOL_DIR);
		tmp_fn = tempnam(ast_str_buffer(spath), "radiohyt");

		if (save_subscriber_list_to_config(&list, tmp_fn, "w") == E_OK)
			rc = show_config_file(tmp_fn);

		if (save_extensions_to_config(NULL, &list, tmp_fn, "a") == E_OK)
			rc = show_config_file(tmp_fn);

		unlink(tmp_fn);
		break;
	}
	case UPDATE_CONFIG:
	{
		int rc1, rc2;
		RAII_VAR(struct ast_str *, spath, ast_str_create(BUFSIZE), ast_free);
		RAII_VAR(struct ast_str *, epath, ast_str_create(BUFSIZE), ast_free);

		ast_str_set(&spath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_subscribers_filename);
		ast_str_set(&epath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, radiohyt_extensions_filename);

		rc1 = save_subscriber_list_to_config(&list, ast_str_buffer(spath), "a");
		rc2 = save_extensions_to_config(NULL, &list, ast_str_buffer(epath), "a");
		if (rc1 == E_OK && rc2 == E_OK) {
			FILE *f;
			char sbuf[BUFSIZE];
			int flag = 0;

			ast_log(LOG_VERBOSE, "Configuration files are successfully updated:\n");
			ast_log(LOG_VERBOSE, "\t- %s\n", ast_str_buffer(spath));
			ast_log(LOG_VERBOSE, "\t- %s\n", ast_str_buffer(epath));

			ast_str_set(&spath, BUFSIZE, "%s/%s", ast_config_AST_CONFIG_DIR, "extensions.conf");
			f = fopen(ast_str_buffer(spath), "a+");
			if (!f) {
				ast_log(LOG_ERROR, "Can't open configuration file for edit: \"%s\"!\n",
					ast_str_buffer(spath));
			}
			while (fgets(sbuf, BUFSIZE, f)) {
				if (strstr(sbuf, "#include \"radiohyt_extensions.conf\"\n")) {
					flag = 1;
					break;
				}
			}
			if (!flag) {
				fprintf(f, "\n#include \"radiohyt_extensions.conf\"\n");
				ast_log(LOG_VERBOSE, "\t- %s\n", ast_str_buffer(spath));
			}
			fclose(f);
		} else {
			ast_log(LOG_WARNING, "Something is wrong!\n");
		}
		break;
	}
	}

	// free list
	while ((item = AST_LIST_REMOVE_HEAD(&list, entry)) != NULL)
		ast_free(item);

	this->is_subscribers_loaded = 1;

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: radio network configuration has successfully loaded\n",
		this->name);
}

static void radiohyt_on_changed_call_state(void *this, const char *caller_id, int radio_id, int call_state)
{
	Channel *chan = (Channel *)this;
	RadioSubscriber rs_item;
	RadioSubscriber *rs;
	RadioCallGroup rg_item;
	RadioCallGroup *rg;
	int devstate = AST_DEVICE_UNAVAILABLE;

	switch(call_state) //enum hyterus_call_state
	{
	case CallInitiated:
		devstate = AST_DEVICE_INUSE;
		break;
	case CallInHangtime:
		devstate = AST_DEVICE_NOT_INUSE;
		break;
	case CallEnded:
	case NoAck:
	case CallInterrupted:
		devstate = AST_DEVICE_NOT_INUSE;
		break;
	default:
		ast_assert(0);
	};

	//todo: should use mutex?!

	// set state for A - finding subscriber
	rs_item.RadioId = atoi(caller_id);
	rs = ao2_t_find(radiohyt_subscribers, &rs_item, OBJ_POINTER, "find item into subscribers table");
	if (rs) {
		rs->devstate = devstate;
		ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
			chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
	}

	// set state for B - finding subscriber
	rs_item.RadioId = radio_id;
	rs = ao2_t_find(radiohyt_subscribers, &rs_item, OBJ_POINTER, "find item into subscribers table");
	if (rs) {
		rs->devstate = devstate;
		ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
			chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
	}

	// set state for B - finding callgroup
	rg_item.RadioId = radio_id;
	rg = ao2_t_find(radiohyt_callgroups, &rg_item, OBJ_POINTER, "find item into callgroups table");
	if (rg) {
		rg->devstate = devstate;
		ast_devstate_changed(rg->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rg->RadioId);
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
			chan->name, rg->RadioId, ast_devstate_str(rg->devstate));

		{
			struct ao2_iterator i = ao2_iterator_init(radiohyt_subscribers, 0);
			while ((rs = ao2_t_iterator_next(&i, "iterate thru subscribers table"))) {
				int subscriber_in_group = 0;
				int n;
				for (n=0; n<HYTERUS_MAX_SUBSCRIBER_GROUPS; ++n) {
					if (rs->CallGroupId[n] == rg->Id && rs->ChannelId == rg->ChannelId[0]) {
						ast_debug(2, "RADIOHYT[%s]: subscriber with RadioId %d is in group with RadioId %d\n",
							chan->name, rs->RadioId, rg->RadioId);
						subscriber_in_group = 1;
						break;
					}
				}
				if (!subscriber_in_group)
					continue;

				if (call_state == CallInitiated && rs->devstate == AST_DEVICE_NOT_INUSE) {
					rs->devstate = AST_DEVICE_INUSE;
					ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
					ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
						chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
				}
				if (call_state >= CallInHangtime && rs->devstate != AST_DEVICE_UNAVAILABLE) {
					rs->devstate = AST_DEVICE_NOT_INUSE;
					ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
					ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
						chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
				}
				ao2_ref(rs, -1);
			}
			ao2_iterator_destroy(&i);
		}
	}
}

static void radiohyt_on_changed_channel_state(void *this, int chan_id, int online)
{
	Channel *chan = (Channel *)this;
	RadioControlStation rc_item = { .Id = chan_id };
	RadioControlStation *rc;

	rc = ao2_t_find(radiohyt_stations, &rc_item, OBJ_POINTER, "find item into stations table");
	if (rc == NULL) {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: receive status for unknown station: %d, ignored!\n",
			chan->name, chan_id);
		return;
	}

	rc->IsOnline = online;

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: station %d is %s\n",
		chan->name, rc->Id, rc->IsOnline ? "online" : "offline");

	{
		RadioCallGroup *rg;
		RadioSubscriber *rs;
		struct ao2_iterator i;

		i = ao2_iterator_init(radiohyt_callgroups, 0);
		while ((rg = ao2_t_iterator_next(&i, "iterate thru callgroups table"))) {
			if (!strcmp(chan->server_ip, rg->ServerIP) && rc->Id == rg->ChannelId[0]) {
				if (rc->IsOnline) {
					if (rg->devstate == AST_DEVICE_INUSE) {
						ast_debug(1, "RADIOHYT[%s]: group %d is busy, skip notification",
							chan->name, rg->RadioId);
						continue;
					}
					rg->devstate = AST_DEVICE_NOT_INUSE;
				} else {
					rg->devstate = AST_DEVICE_UNAVAILABLE;
				}
				ast_devstate_changed(rg->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rg->RadioId);
				ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
					chan->name, rg->RadioId, ast_devstate_str(rg->devstate));
			}
			ao2_ref(rg, -1);
		}
		ao2_iterator_restart(&i);

		i = ao2_iterator_init(radiohyt_subscribers, 0);
		while ((rs = ao2_t_iterator_next(&i, "iterate thru subscribers table"))) {
			if (!rc->IsOnline && !strcmp(chan->server_ip, rs->ServerIP) && rc->Id == rs->ChannelId) {
				rs->devstate = AST_DEVICE_UNAVAILABLE;
				ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
				ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
					chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
			}
			ao2_ref(rs, -1);
		}
		ao2_iterator_destroy(&i);
	}
}

static void radiohyt_on_changed_subscriber_state(void *this, int radio_id, int chan_id, int online)
{
	Channel *chan = (Channel *)this;
	RadioSubscriber rs_item = { .RadioId = radio_id };
	RadioSubscriber *rs;

	rs = ao2_t_find(radiohyt_subscribers, &rs_item, OBJ_POINTER, "find item into subscribers table");
	if (rs) {
		rs->ChannelId = chan_id;
		rs->IsOnline = online;
		rs->devstate = online ? AST_DEVICE_NOT_INUSE : AST_DEVICE_UNAVAILABLE;

		if (rs->IsOnline)
			ast_log(LOG_NOTICE, "RADIOHYT[%s]: subscriber %d is registered into channel %d\n",
				chan->name, rs->RadioId, rs->ChannelId);
		else
			ast_log(LOG_NOTICE, "RADIOHYT[%s]: subscriber %d is unregistered\n",
				chan->name, rs->RadioId);

		ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: set device HYT/%d state as %s\n",
			chan->name, rs->RadioId, ast_devstate_str(rs->devstate));
	} else {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: receive subscriber status for unknown radio id: %d, ignored!\n",
			chan->name, radio_id);
	}
}

static void *radiohyt_channel_thread(void *data)
{
	struct ast_channel *ast_chan = data;
	Channel *chan;
	int rc;

	chan = ast_channel_tech_pvt(ast_chan);
	ENV_ASSERT(chan);

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: thread is running\n", chan->name);

	// retcode has type enum ast_pbx_result
	rc = ast_pbx_run(ast_chan);
	switch (rc) {
	case AST_PBX_SUCCESS:
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: incoming connection is gracefully closed\n",
			chan->name);
		break;
	case AST_PBX_FAILED:
	case AST_PBX_CALL_LIMIT:
		ast_log(LOG_WARNING, "RADIOHYT[%s]: incoming connection is rejected\n",
			chan->name);
		break;
	}

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: thread is stopped\n", chan->name);
	return NULL;
}

#ifdef TEST_FRAMEWORK
void *radiohyt_fake_channel_thread(void *data)
{
	ast_log(LOG_NOTICE, "RADIOHYT[%s]: fake thread is running\n", (const char *)data);
	usleep(1000000);
	ast_log(LOG_NOTICE, "RADIOHYT[%s]: fake thread is stopped\n", (const char *)data);
	return NULL;
}
#endif

//todo: called from monitor thread!!!
int radiohyt_on_incoming_call(
    void *this,
    const struct ast_sockaddr *addr,
    const char *caller_id,
    int receiver_radio_id,
    int chan_id,
    char *sip_id)
{
	Channel *chan = (Channel *)this;
	struct ast_channel *ast_chan = NULL;
	pthread_t threadid;
	int cause = E_OK;
	int dest_id = 0;
	int find_route = 0;
	int rtp_filter = 0;

	do {
		if (sip_id && sip_id[0] != 0) {
			dest_id = atoi(sip_id);
			find_route = 1;
			break;
		}

		struct ao2_iterator i;
		RadioControlStation *rc = NULL;
		RadioCallGroup *rg = NULL;

		i = ao2_iterator_init(radiohyt_stations, AO2_ITERATOR_DONTLOCK);
		while ((rc = ao2_iterator_next(&i))) {
			if (!strcmp(chan->server_ip, rc->ServerIP) && rc->RadioId == receiver_radio_id && rc->Id == chan_id) {
				find_route = 1;

				//fixme: temporary decision to route incoming call into SIP abonent
				dest_id = rc->SipId;
				sprintf(sip_id, "%d", dest_id);
				//fixme: temporary decision to manage for rtp filtering function
				rtp_filter = rc->RtpFilter;

				ast_debug(1, "RADIOHYT[%s]: route incoming call to station with sip_id %d\n",
					chan->name, dest_id);

				ao2_ref(rc, -1);
				break;
			}
			ao2_ref(rc, -1);
		}
		if (find_route)
			break;

		ao2_iterator_restart(&i);
		i = ao2_iterator_init(radiohyt_callgroups, AO2_ITERATOR_DONTLOCK);
		while ((rg = ao2_t_iterator_next(&i, "iterate thru callgroups table"))) {
			if (!strcmp(chan->server_ip, rg->ServerIP) && rg->RadioId == receiver_radio_id && rg->ChannelId[0] == chan_id) {
				find_route = 1;

				//fixme: temporary decision to route incoming call into SIP abonent
				dest_id = rg->SipId;
				sprintf(sip_id, "%d", dest_id);
				//fixme: temporary decision to manage for rtp filtering function
				rtp_filter = rg->RtpFilter;

				ast_debug(1, "RADIOHYT[%s]: route incoming call to group with sip_id %d\n",
					chan->name, dest_id);

				ao2_ref(rg, -1);
				break;
			}
			ao2_ref(rg, -1);
		}
		ao2_iterator_destroy(&i);
	} while (0);

	if (!find_route) {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: no find route for incoming call to radio_id %d\n",
			chan->name, receiver_radio_id);
		return E_FAIL;
	}

	ast_uuid_generate_str(chan->sid, AST_UUID_STR_LEN);
	chan->direction = incoming;
	chan->peer_radio_id = atoi(caller_id);
	chan->peer_channel_id = chan_id;

	if (!dest_id) {
		ast_log(LOG_NOTICE, "RADIOHYT[%s]: no set sip_id for incoming call, ignore empty route\n",
			chan->name);
		return cause;
	}

	ast_log(LOG_NOTICE, "RADIOHYT[%s]: route incoming call to sip_id: %d\n",
		chan->name, dest_id);

#ifdef TEST_FRAMEWORK
	if (ast_pthread_create_detached(&threadid, NULL, radiohyt_fake_channel_thread, chan->name)) {
		ast_log(LOG_ERROR, "RADIOHYT: Unable to start channel thread\n");
		return E_FAIL;
	}
#else
	if ((ast_chan = radiohyt_request_incoming(chan, addr, caller_id, receiver_radio_id, dest_id, rtp_filter, &cause))) {
		if (ast_pthread_create_detached(&threadid, NULL, radiohyt_channel_thread, ast_chan)) {
			ast_log(LOG_ERROR, "RADIOHYT: Unable to start channel thread\n");
			ast_hangup(ast_chan);
			return E_FAIL;
		}
		return E_OK;
	}
#endif

	return cause;
}

struct ast_channel *radiohyt_request_incoming(
    Channel *chan,
    const struct ast_sockaddr *from_addr,
    const char *caller_id,
    int receiver_radio_id,
    int sip_id,
    int rtp_filter,
    int *cause)
{
	struct ast_channel *ast_chan = NULL;
	struct ast_format tmpfmt;
	struct ast_format_cap *caps;

	struct ast_sockaddr addr;
	int chan_id = -1;
	int radio_id = -1;
	int slot_id = -1;
	int call_type = -1;

	ast_mutex_lock(&radiohyt_mutex);

	radiohyt_get_call_params_by_dest(NULL, caller_id, &addr, &chan_id, &slot_id, &radio_id, &call_type, NULL);

	ast_assert(!ast_sockaddr_isnull(from_addr));
	ast_assert(ast_sockaddr_cmp(from_addr, &addr));

	if (!chan) {
		if (radiohyt_channel_policy == DYNAMIC) {
			chan = radiohyt_find_free_dynamic_channel(cause);
		} else {
			chan = radiohyt_find_free_channel(from_addr, NULL, cause);
		}
		if (!chan) {
			*cause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
			ast_mutex_unlock(&radiohyt_mutex);
			return NULL;
		}
	}

	sprintf(chan->exten, "%d", sip_id);

	if (chan->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: incoming request to alloc channel: from caller_id %s to exten %s\n",
			chan->name, caller_id, chan->exten);

	ast_chan = ast_channel_alloc(1, AST_STATE_UP,
		caller_id, // cid_num
		"RADIOHYT", // cid_name
		NULL, // accountcode
		chan->exten,
		chan->context,
		NULL, // assignedids
		NULL, // requestor
		0, // amaflags
		"HYT/%s-%04d",
		caller_id,
		++radiohyt_calls_cnt);

	if (!ast_chan) {
		ast_mutex_unlock(&radiohyt_mutex);
		ast_log(LOG_ERROR, "Unable to allocate channel structure\n");
		return NULL;
	}

	chan->owner = ast_chan;
	ast_mutex_unlock(&radiohyt_mutex);

	chan->peer = 0;

	ast_channel_tech_set(ast_chan, &radiohyt_tech);
	ast_channel_tech_pvt_set(ast_chan, chan);

	ast_channel_rings_set(ast_chan, 1);
	ast_channel_exten_set(ast_chan, chan->exten);
	ast_channel_context_set(ast_chan, chan->context);
	ast_channel_priority_set(ast_chan, 1);

	// begin of codec negotiation
	caps = ast_format_cap_alloc(0);
	ast_format_cap_add(caps, ast_format_set(&tmpfmt, AST_FORMAT_ULAW, 0));
	ast_format_cap_add(caps, ast_format_set(&tmpfmt, AST_FORMAT_ALAW, 0));
	ast_channel_nativeformats_set(ast_chan, caps);

	ast_format_copy(ast_channel_writeformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_rawwriteformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_readformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_rawreadformat(ast_chan), &tmpfmt);

	if (chan->rtpi) {
		ast_rtp_instance_set_read_format(chan->rtpi, &tmpfmt);
		ast_rtp_instance_set_write_format(chan->rtpi, &tmpfmt);

		ast_rtp_instance_set_channel_id(chan->rtpi, ast_channel_uniqueid(ast_chan));

		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_NAT,  0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_DTMF, 0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_RTCP, 0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_STUN, 0);

		if (rtp_filter) {
			struct hyterus_rtp_hdr_ex_t *rtp_ext_header = (struct hyterus_rtp_hdr_ex_t *)
				ast_rtp_instance_get_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER);
			if (rtp_ext_header == NULL) {
				ast_assert(sizeof(*rtp_ext_header) == HYTERUS_RTP_EXT_HEADER_LENGTH);
				rtp_ext_header = (struct hyterus_rtp_hdr_ex_t *) ast_calloc(0, sizeof(*rtp_ext_header));
			}
			rtp_ext_header->profile_id = htons(0x15);
			rtp_ext_header->length = htons(3);
			rtp_ext_header->source_id = htonl((((slot_id << 1) & 0xfe) << 24) | (chan->peer_radio_id & 0x00ffffff));
			rtp_ext_header->receiver_id = htonl((receiver_radio_id << 8) | (call_type & 0xff));
			ast_rtp_instance_set_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER, rtp_ext_header);
			ast_debug(1, "set rtp filter for incoming packets: [Src]: 0x%08x, [Rcv]: 0x%08x\n",
				ntohl(rtp_ext_header->source_id), ntohl(rtp_ext_header->receiver_id));
		} else {
			ast_rtp_instance_set_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER, NULL);
			ast_debug(1, "reset rtp filter for incoming packets\n");
		}

		if (chan->rtp_timeout) {
			ast_rtp_instance_set_timeout(chan->rtpi, chan->rtp_timeout);
			ast_debug(1, "set rtp timeout: %d\n", chan->rtp_timeout);
		}

		// set ptime
		{
			char buf[BUFSIZE];
			long int ptime = HYTERUS_PACKETIZATION_TIME;
			struct ast_rtp_codecs *codecs = ast_rtp_instance_get_codecs(chan->rtpi);
			struct ast_codec_pref *pref = &codecs->pref;
			int codec_n;

			for (codec_n = 0; codec_n < AST_RTP_MAX_PT; codec_n++) {
				struct ast_rtp_payload_type format = ast_rtp_codecs_payload_lookup(codecs, codec_n);
				if (!format.asterisk_format || !(codec_n == 0 || codec_n == 8)) {
					continue;
				}
				ast_debug(1, "set pt: %3d, codec: %10s: ptime: %ld\n",
					codec_n, ast_getformatname(&format.format), ptime);
				ast_codec_pref_append(pref, &format.format);
				ast_codec_pref_setsize(pref, &format.format, ptime);
			}
			ast_codec_pref_string(pref, buf, sizeof(buf));
			ast_debug(1, "set codecs pref: %s\n", buf);

			ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(chan->rtpi), chan->rtpi, pref);
		}
		ast_channel_set_fd(ast_chan, 0, ast_rtp_instance_fd(chan->rtpi, 0));
#ifdef ENABLE_FULL_DUPLEX
		ast_channel_set_fd(ast_chan, 1, ast_rtp_instance_fd(chan->rtpi, 1));
#endif
		// Tell Asterisk to apply changes
		ast_queue_frame(ast_chan, &ast_null_frame);
	}
	// end of codec negotiation

	ast_channel_unlock(ast_chan);

	return ast_chan;
}

/*! \brief PBX interface function - build RADIOHYT pvt structure
 *	RADIOHYT (DMR) calls initiated by the PBX arrive here.
 *
 * \verbatim
 *	Dial string syntax: "HYT/RadioId"
 * \endverbatim
 */
static struct ast_channel *radiohyt_request(
	const char *type,
	struct ast_format_cap *cap,
	const struct ast_assigned_ids *assignedids,
	const struct ast_channel *requestor,
	const char *dest,
	int *cause)
{
	Channel *chan = NULL;
	struct ast_channel *ast_chan = NULL;
	struct ast_format tmpfmt;
	struct ast_format_cap *caps, *joint_caps;
	const char *call_dest = NULL;
	char str_addr[16];

	struct ast_sockaddr addr;
	int chan_id = -1;
	int radio_id = -1;
	int slot_id = -1;
	int call_type = -1;

	ast_debug(1, "RADIOHYT: request(type=%s, dest=%s) enter\n", type, dest);

	if ((call_dest = strchr(dest, '/')))
		++call_dest;
	else
		call_dest = dest;

	ast_mutex_lock(&radiohyt_mutex);

	if (radiohyt_get_call_params_by_dest(requestor, call_dest, &addr, &chan_id, &slot_id, &radio_id, &call_type, &chan) != E_OK) {
		*cause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
		ast_mutex_unlock(&radiohyt_mutex);
		return NULL;
	}
	ast_copy_string(str_addr, ast_sockaddr_stringify_host(&addr), sizeof(str_addr));

	if (chan) {
		switch (chan->state) {
		case CHAN_RADIOHYT_ST_DIAL:
		case CHAN_RADIOHYT_ST_RING:
		case CHAN_RADIOHYT_ST_BUSY:
		case CHAN_RADIOHYT_ST_CANCEL:
			// radio channel is being already used for communication
			*cause = AST_CAUSE_USER_BUSY;
			ast_mutex_unlock(&radiohyt_mutex);
			return NULL;
		case CHAN_RADIOHYT_ST_DETACH:
		case CHAN_RADIOHYT_ST_DETACH_2:
		case CHAN_RADIOHYT_ST_HANGTIME:
			// that's good, use this channel to communication
			break;
		default:
			// forget clean appropriate variable into radiohyt_channel?
			chan = 0;
			ast_assert(0);
			break;
		}
	} else {
		if (radiohyt_channel_policy == DYNAMIC) {
			chan = radiohyt_find_free_dynamic_channel(cause);
		} else {
			chan = radiohyt_find_free_channel(NULL, str_addr, cause);
		}
	}

	if (!chan) {
		ast_mutex_unlock(&radiohyt_mutex);
		return NULL;
	}

	ast_uuid_generate_str(chan->sid, AST_UUID_STR_LEN);
	chan->direction = outgoing;
	chan->peer_radio_id = atoi(call_dest);
	chan->peer_channel_id = chan_id;

	strncpy(chan->exten, call_dest, AST_MAX_EXTENSION);

	if (chan->debug)
		ast_log(LOG_VERBOSE, "RADIOHYT[%s]: outgoing request to call_dest %s\n",
			chan->name, call_dest);

	ast_chan = ast_channel_alloc(1, AST_STATE_RING,
		call_dest, // cid_num
		"RADIOHYT", // cid_name
		NULL, // account_code
		chan->exten,
		chan->context,
		assignedids,
		requestor,
		0, // amaflags
		"HYT/%s-%04d",
		call_dest,
		++radiohyt_calls_cnt);

	if (!ast_chan) {
		ast_log(LOG_ERROR, "RADIOHYT: Unable to allocate channel structure\n");
		ast_mutex_unlock(&radiohyt_mutex);
		*cause = AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
		return NULL;
	}

	gettimeofday(&chan->tport.stat.voice_request_ts, 0);

	chan->owner = ast_chan;
	ast_mutex_unlock(&radiohyt_mutex);

	// todo: is operation correct and safe?
	chan->peer = (struct ast_channel *)requestor;

	ast_channel_tech_set(ast_chan, &radiohyt_tech);
	ast_channel_tech_pvt_set(ast_chan, chan);

	ast_channel_rings_set(ast_chan, 1);
	ast_channel_context_set(ast_chan, chan->context);
	ast_channel_exten_set(ast_chan, chan->exten);
#ifndef TEST_FRAMEWORK
	// begin of codec negotiation
	caps = ast_format_cap_alloc(0);
	ast_format_cap_copy(caps, chan->caps);
	ast_channel_nativeformats_set(ast_chan, caps);

	joint_caps = ast_format_cap_joint(ast_channel_nativeformats(ast_chan), ast_channel_nativeformats(chan->peer));
	if (joint_caps) {
		char buf1[BUFSIZE], buf2[BUFSIZE], buf3[BUFSIZE];

		ast_log(LOG_NOTICE, "RADIOHYT[%s]: Capabilities: peer - %s, us - %s, combined - %s on %s\n",
			chan->name,
			ast_getformatname_multiple(buf1, BUFSIZE, ast_channel_nativeformats(chan->peer)),
			ast_getformatname_multiple(buf2, BUFSIZE, ast_channel_nativeformats(ast_chan)),
			ast_getformatname_multiple(buf3, BUFSIZE, joint_caps),
			ast_channel_name(ast_chan));

		ast_format_cap_copy(caps, joint_caps);
		ast_channel_nativeformats_set(ast_chan, caps);

		joint_caps = ast_format_cap_destroy(joint_caps);
	} else {
		char buf1[BUFSIZE], buf2[BUFSIZE];
		struct ast_format best_fmt_dst;
		struct ast_format best_fmt_src;

		if (ast_translator_best_choice(ast_channel_nativeformats(ast_chan), ast_channel_nativeformats(chan->peer), &best_fmt_dst, &best_fmt_src) < 0) {
			ast_log(LOG_WARNING, "Unable to create translator path from %s to %s on %s\n",
				ast_getformatname_multiple(buf1, BUFSIZE, ast_channel_nativeformats(chan->peer)),
				ast_getformatname_multiple(buf2, BUFSIZE, ast_channel_nativeformats(ast_chan)),
				ast_channel_name(ast_chan));

			// TODO: free objects
			*cause = AST_CAUSE_BEARERCAPABILITY_NOTAVAIL;
			return NULL;
		}

		ast_format_cap_set(ast_channel_nativeformats(ast_chan), &best_fmt_dst);

		ast_log(LOG_NOTICE, "RADIOHYT[%s]: Translator path is found from %s to %s on %s\n",
			chan->name,
			ast_getformatname_multiple(buf1, BUFSIZE, ast_channel_nativeformats(chan->peer)),
			ast_getformatname_multiple(buf2, BUFSIZE, ast_channel_nativeformats(ast_chan)),
			ast_channel_name(ast_chan));
	}

	ast_best_codec(ast_channel_nativeformats(ast_chan), &tmpfmt);

	ast_format_copy(ast_channel_writeformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_rawwriteformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_readformat(ast_chan), &tmpfmt);
	ast_format_copy(ast_channel_rawreadformat(ast_chan), &tmpfmt);

	if (chan->rtpi) {
		struct hyterus_rtp_hdr_ex_t *rtp_ext_header = NULL;

		ast_rtp_instance_set_read_format(chan->rtpi, &tmpfmt);
		ast_rtp_instance_set_write_format(chan->rtpi, &tmpfmt);

		ast_rtp_instance_set_channel_id(chan->rtpi, ast_channel_uniqueid(ast_chan));

		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_NAT,  0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_DTMF, 0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_RTCP, 0);
		ast_rtp_instance_set_prop(chan->rtpi, AST_RTP_PROPERTY_STUN, 0);

		rtp_ext_header = (struct hyterus_rtp_hdr_ex_t *)
				ast_rtp_instance_get_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER);
		if (rtp_ext_header == NULL) {
			ast_assert(sizeof(*rtp_ext_header) == HYTERUS_RTP_EXT_HEADER_LENGTH);
			rtp_ext_header = (struct hyterus_rtp_hdr_ex_t *)ast_calloc(0, sizeof(*rtp_ext_header));
		}
		
		rtp_ext_header->profile_id = htons(0x15);
		rtp_ext_header->length = htons(3);
		rtp_ext_header->source_id = htonl((((slot_id << 1) & 0xfe) << 24) & 0xffffff00);
		rtp_ext_header->receiver_id = htonl((radio_id << 8) | (call_type & 0xff));
		ast_rtp_instance_set_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER, rtp_ext_header);
		ast_debug(1, "set rtp params for outgoing packets: [Src]: 0x%08x, [Rcv]: 0x%08x\n",
			ntohl(rtp_ext_header->source_id), ntohl(rtp_ext_header->receiver_id));

		if (chan->rtp_timeout)
			ast_rtp_instance_set_timeout(chan->rtpi, chan->rtp_timeout);

		// set ptime
		{
			char buf[BUFSIZE];
			long int ptime = HYTERUS_PACKETIZATION_TIME;
			struct ast_rtp_codecs *codecs = ast_rtp_instance_get_codecs(chan->rtpi);
			struct ast_codec_pref *pref = &codecs->pref;
			int codec_n;

			for (codec_n = 0; codec_n < AST_RTP_MAX_PT; codec_n++) {
				struct ast_rtp_payload_type format = ast_rtp_codecs_payload_lookup(codecs, codec_n);
				if (!format.asterisk_format || !(codec_n == 0 || codec_n == 8)) {
					continue;
				}
				ast_log(LOG_NOTICE, "pt: %3d, codec: %10s: ptime: %ld\n", codec_n, ast_getformatname(&format.format), ptime);
				ast_codec_pref_append(pref, &format.format);
				ast_codec_pref_setsize(pref, &format.format, ptime);
			}
			ast_codec_pref_string(pref, buf, sizeof(buf));
			ast_debug(1, "rtp: codecs pref: %s\n", buf);

			ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(chan->rtpi), chan->rtpi, pref);
		}
# ifdef ENABLE_FULL_DUPLEX
		ast_channel_set_fd(ast_chan, 0, ast_rtp_instance_fd(chan->rtpi, 0));
# endif
		ast_channel_set_fd(ast_chan, 1, ast_rtp_instance_fd(chan->rtpi, 1));
		// Tell Asterisk to apply changes
		ast_queue_frame(ast_chan, &ast_null_frame);
	}
	// end of codec negotiation
#endif
	ast_channel_unlock(ast_chan);

	ast_debug(1, "RADIOHYT: request(type=%s, dest=%s) exit\n", type, dest);

	return ast_chan;
}

#ifdef TEST_FRAMEWORK
static int radiohyt_get_devicestate(const char *fmt, ...)
{
	char buf[AST_MAX_EXTENSION];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return radiohyt_devicestate(buf);
}
#endif

static int radiohyt_devicestate(const char *dest)
{
	const char *tmp;
	int devstate = AST_DEVICE_UNAVAILABLE;
	int radio_id = -1;

	if (!dest) {
		ast_log(LOG_WARNING, "RADIOHYT: request for devicestate of NULL: %s\n",
			ast_devstate_str(devstate));
		return devstate;
	}
	if ((tmp = strchr(dest, '/')))
		++tmp;
	else
		tmp = dest;

	radio_id = atoi(tmp);

	ast_mutex_lock(&radiohyt_mutex);

	//find subscriber
	{
		RadioSubscriber rs_item = { .RadioId = radio_id };
		RadioSubscriber *rs;

		rs = ao2_t_find(radiohyt_subscribers, &rs_item, OBJ_POINTER, "find item into subscribers table");
		if (rs) {
			ast_log(LOG_VERBOSE, "RADIOHYT: request for devicestate of subscriber %s(%d): %s\n",
				dest, rs->RadioId, ast_devstate_str(rs->devstate));
			ast_mutex_unlock(&radiohyt_mutex);
			return rs->devstate;
		}
	}

	//find callgroup
	{
		RadioCallGroup rg_item = { .RadioId = radio_id };
		RadioCallGroup *rg;

		rg = ao2_t_find(radiohyt_callgroups, &rg_item, OBJ_POINTER, "find item into callgroups table");
		if (rg) {
			ast_log(LOG_VERBOSE, "RADIOHYT: request for devicestate of callgroup %s(%d): %s\n",
					dest, rg->RadioId, ast_devstate_str(rg->devstate));
			ast_mutex_unlock(&radiohyt_mutex);
			return rg->devstate;
		}
	}

	ast_mutex_unlock(&radiohyt_mutex);

	ast_log(LOG_WARNING, "RADIOHYT: request for devicestate of unknown device %s: %s\n",
		dest, ast_devstate_str(devstate));

	return devstate;
}

static int radiohyt_get_call_params_by_dest(const struct ast_channel *caller,
    const char *dest, struct ast_sockaddr *addr, int *chan_id, int *slot_id,
    int *radio_id, int *call_type, Channel **out_chan)
{
	int rc = E_OK;
	const char *tmp;

	// format of destination: "HYT/RadioId-CallNo" or simply "RadioId"
	if (!dest)
		return E_FAIL;
	if ((tmp = strchr(dest, '/')))
		++tmp;
	else
		tmp = dest;

	*chan_id = -1;
	*slot_id = -1;
	*radio_id = atoi(tmp);

	ast_debug(1, "RADIOHYT: trying to find destination %s, RadioId %d\n",
		dest, *radio_id);
	do {
		RadioSubscriber rs_item = { .RadioId = *radio_id };
		RadioCallGroup rg_item = { .RadioId = *radio_id };
		RadioSubscriber *rs;
		RadioCallGroup *rg;

		rs = ao2_t_find(radiohyt_subscribers, &rs_item, OBJ_POINTER, "find item into subscribers table");
		if (rs) {
			if (!rs->IsOnline) {
				ast_log(LOG_WARNING, "RADIOHYT: can't make a call - subscriber %d is unavailable\n",
					rs->RadioId);
				return E_FAIL;
			}
			*chan_id = rs->ChannelId;
			*call_type = Private;
			ast_debug(1, "RADIOHYT: found subscriber %s at channel %d\n", rs->Name, rs->ChannelId);
			break;
		}

		rg = ao2_t_find(radiohyt_callgroups, &rg_item, OBJ_POINTER, "find item into callgroups table");
		if (rg) {
			*chan_id = rg->ChannelId[0];
			*call_type = Group;
			ast_debug(1, "RADIOHYT: found group %s at channel %d\n", rg->Name, rg->ChannelId[0]);
			break;
		}

		ast_log(LOG_ERROR, "RADIOHYT: can't find subscriber nor group by dest: %s\n", dest);
		return E_FAIL;
	} while(0);

	do {
		RadioControlStation rc_item = { .Id = *chan_id };
		RadioControlStation *rc;

		rc = ao2_t_find(radiohyt_stations, &rc_item, OBJ_POINTER, "find item into stations table");
		if (rc) {
			if (!rc->IsOnline) {
				ast_log(LOG_WARNING, "RADIOHYT: can't make a call - channel %d is offline\n", rc->Id);
				return E_FAIL;
			}
			*slot_id = rc->Slot;
			ast_sockaddr_parse(addr, rc->ServerIP, PARSE_PORT_IGNORE);
			ast_debug(1, "RADIOHYT: found channel %d, slot %d at station %s@%s\n",
				rc->Id, rc->Slot, rc->Name, rc->ServerIP);
			break;
		}

		ast_log(LOG_ERROR, "RADIOHYT: can't find channel %d\n", *chan_id);
		return E_FAIL;
	} while(0);

	// find radiohyt channel, already using radio channel
	if (out_chan) {
		Channel *chan;
		struct ao2_iterator i;

		*out_chan = NULL;

		i = ao2_iterator_init(radiohyt_channels, 0);
		while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
			if (chan->peer_channel_id == *chan_id) {
				// check if caller_id equals caller_id from previous call
				do {
					if (chan->session == NULL) {
						ast_assert(chan->session);
						break;
					}
					if (caller == NULL) {
						ast_assert(caller);
						break;
					}
					if (ast_channel_caller(caller)->id.number.str == NULL) {
						ast_assert(ast_channel_caller(caller)->id.number.str);
						break;
					}
					if (!strcmp(ast_channel_caller(caller)->id.number.str, chan->session->caller_id)) {
						break;
					}
					if (atoi(chan->session->caller_id) == 0) {
						ast_debug(1, "RADIOHYT[%s]: previous call to channel_id %d has no route, use it\n",
							chan->name, *chan_id);
						strcpy(ast_channel_caller(caller)->id.number.str, chan->session->caller_id);
						break;
					}
					ast_log(LOG_WARNING, "RADIOHYT[%s]: is already used channel_id %d for sip_id %s\n",
						chan->name, *chan_id, chan->session->caller_id);
					rc = E_FAIL;
					goto exit;
				} while (0);

				*out_chan = chan;
				ast_debug(1, "RADIOHYT[%s]: is already used channel_id %d\n",
					chan->name, *chan_id);
exit:
				ao2_ref(chan, -1);
				break;
			}
			ao2_ref(chan, -1);
		}
		ao2_iterator_destroy(&i);
	}

	return rc;
}

int radiohyt_call(struct ast_channel *ast_chan, const char *dest, int timeout)
{
	struct ast_sockaddr addr;
	int chan_id = -1;
	int slot_id = -1;
	int radio_id = -1;
	int call_type = Private;

	Channel *chan = ast_channel_tech_pvt(ast_chan);
	ENV_ASSERT(chan);

	radiohyt_get_call_params_by_dest(NULL, dest, &addr, &chan_id, &slot_id, &radio_id, &call_type, NULL);
	return radiohyt_make_call(chan, chan_id, slot_id, radio_id, call_type, 0);
}

int radiohyt_hangup(struct ast_channel *ast_chan)
{
	struct ast_sockaddr addr;
	int chan_id = -1;
	int slot_id = -1;
	int radio_id = -1;
	int call_type = Private;
	Channel *chan;

	ast_mutex_lock(&radiohyt_mutex);

	chan = ast_channel_tech_pvt(ast_chan);
	ENV_ASSERT(chan);
	if (chan) {
		ENV_ASSERT(chan->exten);

		ast_debug(1, "RADIOHYT: hangup(exten=%s)\n", chan->exten);

		//todo: check it!
		//if (chan->direction == outgoing) {
		radiohyt_get_call_params_by_dest(NULL, chan->exten, &addr, &chan_id, &slot_id, &radio_id, &call_type, NULL);

		ast_channel_tech_pvt_set(ast_chan, NULL);

		radiohyt_drop_call(chan, chan_id, slot_id, radio_id, call_type, 0);
		//}

		// set mark of final voice packet
		if (chan->rtpi) {
			struct hyterus_rtp_hdr_ex_t *rtp_ext_header = 
				(struct hyterus_rtp_hdr_ex_t *)ast_rtp_instance_get_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER);
			if (rtp_ext_header) {
				rtp_ext_header->source_id |= 0x01000000;
				ast_rtp_instance_set_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER, rtp_ext_header);
			}
		}

		ao2_lock(chan);
		chan->owner = NULL;
		ao2_unlock(chan);
	}

	ast_mutex_unlock(&radiohyt_mutex);

	return E_OK;
}

/*! \brief Function called by core when we should answer a HYT session */
static int radiohyt_answer(struct ast_channel *ast_chan)
{
	if (ast_channel_state(ast_chan) != AST_STATE_UP) {
		Channel *chan = ast_channel_tech_pvt(ast_chan);
		ast_assert(chan);

		ast_setstate(ast_chan, AST_STATE_UP);

		ast_debug(1, "RADIOHYT[%s]: answering channel: %s\n", chan->name, ast_channel_name(ast_chan));
		ast_rtp_instance_update_source(chan->rtpi);
	}

	return 0;
}

/*! \brief Deliver RADIOHYT call ID for the call */
static const char *radiohyt_get_callid(struct ast_channel *ast_chan)
{
	// TODO: non-safe call
	return ast_channel_tech_pvt(ast_chan) ? ((Channel *)ast_channel_tech_pvt(ast_chan))->sid : "";
}

struct ast_frame *radiohyt_read(struct ast_channel *ast_chan)
{
	struct ast_frame *f;
	Channel *chan = ast_channel_tech_pvt(ast_chan);
	ENV_ASSERT(chan);

	ao2_lock(chan);
	ENV_ASSERT(chan->rtpi);
	if (chan->rtpi) {
		f = ast_rtp_instance_read(chan->rtpi, 0);
	} else {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: can't read from channel - no rtp found\n",
			chan->name);
		f = &ast_null_frame;
	}
	ao2_unlock(chan);

	return f;
}

int radiohyt_write(struct ast_channel *ast_chan, struct ast_frame *f)
{
	int rc;
	Channel *chan = ast_channel_tech_pvt(ast_chan);
	ENV_ASSERT(chan);

	if (f->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: asked to send unsupported frame type: %d\n",
			chan->name, f->frametype);
		return -1;
	}
	if (!(ast_format_cap_iscompatible(ast_channel_nativeformats(ast_chan), &f->subclass.format))) {
		char buf[256];
		ast_log(LOG_ERROR,
			"RADIOHYT[%s]: asked to transmit frame type %s, while native formats is %s (read/write = %s/%s)\n",
			chan->name,
			ast_getformatname(&f->subclass.format),
			ast_getformatname_multiple(buf, sizeof(buf), ast_channel_nativeformats(ast_chan)),
			ast_getformatname(ast_channel_readformat(ast_chan)),
			ast_getformatname(ast_channel_writeformat(ast_chan)));
		return -1;
	}

	ao2_lock(chan);
	ENV_ASSERT(chan->rtpi);
	if (chan->rtpi) {
		rc = ast_rtp_instance_write(chan->rtpi, f);
	} else {
		ast_log(LOG_WARNING, "RADIOHYT[%s]: can't write to channel - no rtp found\n",
			chan->name);
		rc = -1;
	}
	ao2_unlock(chan);

	return rc;
}

/*
 * infrastructure functions
 */

static int chan_cmp_cb(void *obj, void *arg, int flags)
{
	Channel *chan = obj, *chan2 = arg;
	return !strcasecmp(chan->name, chan2->name) ? CMP_MATCH | CMP_STOP : 0;
}

static int chan_hash_cb(const void *obj, const int flags)
{
	const Channel *chan = obj;
	ENV_ASSERT(chan->name);
	return ast_str_case_hash(chan->name);
}

static int chan_set_debug_cb(void *obj, void *arg, int flags)
{
	Channel *chan = obj;
	chan->debug = *(int *)arg;
	return CMP_MATCH;
}

typedef struct {
	int chan_id;
	int radio_id;
} chan_keepalive_arg_t;

static int chan_keepalive_cb(void *obj, void *arg, int flags)
{
	Channel *chan = obj;
	chan_keepalive_arg_t which = *(chan_keepalive_arg_t *)arg;

	if (chan->state == CHAN_RADIOHYT_ST_READY) {
		radiohyt_check_radio(chan, which.chan_id, which.radio_id, 100);
	}
	return 0;
}

static void chan_cleanup(Channel *chan)
{
	radiohyt_chan_close(chan);

	if (chan->rtpi) {
		unsigned char *rtp_ext_header = (unsigned char *)ast_rtp_instance_get_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER);
		if (rtp_ext_header) {
			ast_rtp_instance_set_extended_prop(chan->rtpi, AST_RTP_EXTENSION_HEADER, NULL);
			ast_free(rtp_ext_header);
		}
		ast_rtp_instance_destroy(chan->rtpi);
		chan->rtpi = 0;
	}
}

typedef enum {
	CHANNELS_DYNAMIC,
	CHANNELS_ALL,
} chan_unlink_arg_t;

static int chan_cleanup_cb(void *obj, void *arg, int flags)
{
	Channel *chan = obj;
	chan_unlink_arg_t which = *(chan_unlink_arg_t *)arg;

	if ( which == CHANNELS_ALL ||
	    (which == CHANNELS_DYNAMIC
	    && strcmp(chan->name, first_channel_name)
	    && !chan->owner
	    && chan->state > CHAN_RADIOHYT_ST_IDLE
	    && chan->state <= CHAN_RADIOHYT_ST_READY) ) {
		int refc = ao2_ref(chan, 1);
		if (refc == 2) {
			chan_cleanup(chan);
			ao2_ref(chan, -2);
			return CMP_MATCH;
		}
		ao2_ref(chan, -1);
	}
	return 0;
}

/* Function to assist finding channel by name only */
static int radiohyt_chan_find_by_name(void *obj, void *arg, void *data, int flags)
{
	Channel *search = obj, *match = arg;

	if (strcmp(search->name, match->name)) {
		return 0;
	}
	return CMP_MATCH | CMP_STOP;
}

/* Function to assist finding channel by one of key parameter */
static Channel *radiohyt_find_chan(const char *name, const char *exten, struct ast_sockaddr *addr)
{
	Channel *c = NULL;
	Channel tmp_chan;

	if (name) {
		ast_copy_string(tmp_chan.name, name, sizeof(tmp_chan.name));
		c = ao2_t_callback_data(radiohyt_channels, OBJ_POINTER,
			radiohyt_chan_find_by_name, &tmp_chan, NULL, "find item into channels table");
	} else if (exten) {
	        //todo
	} else if (addr) {
		ast_sockaddr_copy(&tmp_chan.srv_addr, addr);
		//todo
//		c = ao2_t_callback_data(radiohyt_channels_by_addr, OBJ_POINTER,
//			radiohyt_chan_find_by_addr, &tmp_chan, NULL, "find item into channels_by_addr table");
	}
	return c;
}

static inline Channel *radiohyt_find_chan_by_name(const char *name)
{
    return radiohyt_find_chan(name, NULL, NULL);
}

static inline Channel *radiohyt_find_chan_by_exten(const char *exten)
{
    return radiohyt_find_chan(NULL, exten, NULL);
}

static inline Channel *radiohyt_find_chan_by_addr(struct ast_sockaddr *addr)
{
    return radiohyt_find_chan(NULL, NULL, addr);
}

static int srv_cmp_cb(void *obj, void *arg, int flags)
{
	if (ast_sockaddr_cmp_addr((const struct ast_sockaddr *)obj, (const struct ast_sockaddr *)arg)) {
		return 0;
	}
	return CMP_MATCH | CMP_STOP;
}

static int srv_hash_cb(const void *obj, const int flags)
{
	return ast_str_case_hash(ast_sockaddr_stringify_addr((const struct ast_sockaddr *)obj));
}

static int srv_cleanup_cb(void *obj, void *arg, int flags)
{
	struct ast_sockaddr *srv = obj;

	ao2_ref(srv, -1);
	return CMP_MATCH;
}

static int stations_cmp_cb(void *obj, void *arg, int flags)
{
	RadioControlStation const *rc = obj, *rc2 = arg;
	return rc->Id == rc2->Id ? CMP_MATCH | CMP_STOP : 0;
}

static int stations_hash_cb(const void *obj, const int flags)
{
	return ((const RadioControlStation *)obj)->Id;
}

static int stations_cleanup_cb(void *obj, void *arg, int flags)
{
	RadioControlStation *rc = obj;

	ao2_ref(rc, -1);
	return CMP_MATCH;
}

static int callgroups_cmp_cb(void *obj, void *arg, int flags)
{
	RadioCallGroup const *rg = obj, *rg2 = arg;
	return rg->RadioId == rg2->RadioId ? CMP_MATCH | CMP_STOP : 0;
}

static int callgroups_hash_cb(const void *obj, const int flags)
{
	return ((const RadioCallGroup *)obj)->RadioId;
}

static int callgroups_cleanup_cb(void *obj, void *arg, int flags)
{
	RadioCallGroup *rg = obj;

	rg->devstate = AST_DEVICE_UNAVAILABLE;
	ast_devstate_changed(rg->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rg->RadioId);
	ast_log(LOG_VERBOSE, "RADIOHYT: set device HYT/%d state as %s\n",
		rg->RadioId, ast_devstate_str(rg->devstate));

	ao2_ref(rg, -1);
	return CMP_MATCH;
}

static int subscribers_cmp_cb(void *obj, void *arg, int flags)
{
	RadioSubscriber const *rs = obj, *rs2 = arg;
	return rs->RadioId == rs2->RadioId ? CMP_MATCH | CMP_STOP : 0;
}

static int subscribers_hash_cb(const void *obj, const int flags)
{
	return ((const RadioSubscriber *)obj)->RadioId;
}

static int subscribers_cleanup_cb(void *obj, void *arg, int flags)
{
	RadioSubscriber *rs = obj;

	rs->devstate = AST_DEVICE_UNAVAILABLE;
	ast_devstate_changed(rs->devstate, AST_DEVSTATE_CACHABLE, "HYT/%d", rs->RadioId);
	ast_log(LOG_VERBOSE, "RADIOHYT: set device HYT/%d state as %s\n",
		rs->RadioId, ast_devstate_str(rs->devstate));

	ao2_ref(rs, -1);
	return CMP_MATCH;
}

/*
 *
 */

static int save_station_list_to_config(
	RadioControlStationList *plist,
	const char *subscribers_filename,
	const char *mode)
{
	FILE *f = NULL;
	RadioControlStation *item = NULL;

	f = fopen(subscribers_filename, mode);
	if (!f) {
		ast_log(LOG_VERBOSE, "Can't open %s file for writing, error - %s\n",
			subscribers_filename, strerror(errno));
		return -1;
	}

	AST_LIST_TRAVERSE(plist, item, entry) {
		fprintf(f, "[%d_%d]\n", item->RadioId, item->Id);
		fprintf(f, "type=%s\n", "station");
		fprintf(f, "Id=%d\n", item->Id);
		fprintf(f, "Name=%s\n", item->Name);
		fprintf(f, "RadioId=%d\n", item->RadioId);
		fprintf(f, "Slot=%d\n", item->Slot);
		fprintf(f, "SipId=%d\n", item->SipId);
		fprintf(f, "RtpFilter=%d\n", item->RtpFilter);
		fprintf(f, "IsAnalog=%s\n", item->IsAnalog ? "yes" : "no");
		fprintf(f, "IsOnline=%s\n", item->IsOnline ? "yes" : "no");
		fprintf(f, "ServerIP=%s\n", item->ServerIP);
		fprintf(f, "\n");
	}

	fclose(f);
	return E_OK;
}

static int save_group_list_to_config(
	RadioGroupList *plist,
	const char *subscribers_filename,
	const char *mode)
{
	FILE *f = NULL;
	RadioCallGroup *item = NULL;

	f = fopen(subscribers_filename, mode);
	if (!f) {
		ast_log(LOG_VERBOSE, "Can't open %s file for writing, error - %s\n",
			subscribers_filename, strerror(errno));
		return -1;
	}

	fprintf(f, "[template_group](!)\n");
	fprintf(f, "context=%s\n", radiohyt_context);
	fprintf(f, "nat=%s\n", "no");
	fprintf(f, "busylevel=%s\n", "0");
	fprintf(f, "type=%s\n\n", "group");

	AST_LIST_TRAVERSE(plist, item, entry) {
		fprintf(f, "[%d](template_group)\n", item->RadioId);
		fprintf(f, "Id=%d\n", item->Id);
		fprintf(f, "Name=%s\n", item->Name);
		fprintf(f, "RadioId=%d\n", item->RadioId);
		fprintf(f, "ChannelIds=%s\n", item->ChannelIdStr);
		fprintf(f, "SipId=%d\n", item->SipId);
		fprintf(f, "RtpFilter=%d\n", item->RtpFilter);
		fprintf(f, "ServerIP=%s\n", item->ServerIP);
		fprintf(f, "\n");
	}

	fclose(f);
	return E_OK;
}

static int save_subscriber_list_to_config(
	RadioSubscriberList *plist,
	const char *subscribers_filename,
	const char *mode)
{
	FILE *f = NULL;
	RadioSubscriber *item = NULL;

	f = fopen(subscribers_filename, mode);
	if (!f) {
		ast_log(LOG_VERBOSE, "Can't open %s file for writing, error - %s\n",
			subscribers_filename, strerror(errno));
		return -1;
	}

	fprintf(f, "[template_s](!)\n");
	fprintf(f, "context=%s\n", radiohyt_context);
	fprintf(f, "nat=%s\n", "no");
	fprintf(f, "busylevel=%s\n", "0");
	fprintf(f, "type=%s\n\n", "subscriber");

	AST_LIST_TRAVERSE(plist, item, entry) {
		fprintf(f, "[%d](template_s)\n", item->RadioId);
		fprintf(f, "Id=%d\n", item->Id);
		fprintf(f, "Name=%s\n", item->Name);
		fprintf(f, "RadioId=%d\n", item->RadioId);
		fprintf(f, "ChannelId=%d\n", item->ChannelId);
		fprintf(f, "CallGroupIds=%s\n", item->CallGroupIdStr);
		fprintf(f, "HasGpsSupport=%s\n", item->HasGpsSupport ? "yes" : "no");
		fprintf(f, "HasTextMsgSupport=%s\n", item->HasTextMsgSupport ? "yes" : "no");
		fprintf(f, "HasRemoteMonitorSupport=%s\n", item->HasRemoteMonitorSupport ? "yes" : "no");
		fprintf(f, "IsEnabled=%s\n", item->IsEnabled ? "yes" : "no");
		fprintf(f, "IsOnline=%s\n", item->IsOnline ? "yes" : "no");
		fprintf(f, "RadioStationType=%d\n", item->RadioStationType);
		fprintf(f, "ServerIP=%s\n", item->ServerIP);
		fprintf(f, "\n");
	}

	fclose(f);
	return E_OK;
}

static int save_extensions_to_config(
    RadioGroupList *pglist,
    RadioSubscriberList *pslist,
    const char *extensions_filename,
    const char *mode)
{
	FILE *f = NULL;

	f = fopen(extensions_filename, mode);
	if (!f) {
		ast_log(LOG_VERBOSE, "Can't open %s file for writing, error - %s\n", extensions_filename, strerror(errno));
		return -1;
	}

	fprintf(f, "[%s]\n", radiohyt_context);

	if (pslist) {
		RadioSubscriber *item = NULL;

		fprintf(f, "; HYTERUS Subscribers\n");
		AST_LIST_TRAVERSE(pslist, item, entry) {
			fprintf(f, "exten => %d,hint,HYT/%d\n", item->RadioId, /*radiohyt_tech.type, */item->RadioId);
		}
	}
	if (pglist) {
		RadioCallGroup *item = NULL;

		fprintf(f, "; HYTERUS Callgroups\n");
		AST_LIST_TRAVERSE(pglist, item, entry) {
			fprintf(f, "exten => %d,hint,HYT/%d\n", item->RadioId, /*radiohyt_tech.type, */item->RadioId);
		}
	}

	fprintf(f, "\n");

	fclose(f);
	return E_OK;
}

static RadioControlStation *load_station_config(struct ast_config *cfg, const char *cat)
{
	struct ast_variable *v;
	RadioControlStation *item = NULL;

	if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioControlStation), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioControlStation struct"))) {
		return NULL;
	}

	for (v = ast_variable_browse(cfg, cat); v; v = v->next) {
		if (!strcasecmp(v->name, "Name")) {
			ast_copy_string(item->Name, v->value, sizeof(item->Name));
		} else if (!strcasecmp(v->name, "Id")) {
			item->Id = atoi(v->value);
		} else if (!strcasecmp(v->name, "RadioId")) {
			item->RadioId = atoi(v->value);
		} else if (!strcasecmp(v->name, "Slot")) {
			item->Slot = atoi(v->value);
		} else if (!strcasecmp(v->name, "IsAnalog")) {
			item->IsAnalog = ast_true(v->value);
		} else if (!strcasecmp(v->name, "IsOnline")) {
			item->IsOnline = ast_true(v->value);
		} else if (!strcasecmp(v->name, "SipId")) {
			item->SipId = atoi(v->value);
		} else if (!strcasecmp(v->name, "RtpFilter")) {
			item->RtpFilter = atoi(v->value);
		} else if (!strcasecmp(v->name, "ServerIP")) {
			ast_copy_string(item->ServerIP, v->value, sizeof(item->ServerIP));
		} else if (!strcasecmp(v->name, "type")) {
			// skip it
		} else {
			ast_debug(2, "Unknown parameter: [%s]:%s, skip\n", cat, v->name);
		}
	}

	return item;
}

static RadioCallGroup *load_callgroup_config(struct ast_config *cfg, const char *cat)
{
	struct ast_variable *v;
	RadioCallGroup *item = NULL;

	if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioCallGroup), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioCallGroup struct"))) {
		return NULL;
	}

	item->Id = atoi(cat);

	for (v = ast_variable_browse(cfg, cat); v; v = v->next) {
		if (!strcasecmp(v->name, "Name")) {
			ast_copy_string(item->Name, v->value, sizeof(item->Name));
		} else if (!strcasecmp(v->name, "Id")) {
			item->Id = atoi(v->value);
		} else if (!strcasecmp(v->name, "RadioId")) {
			item->RadioId = atoi(v->value);
		} else if (!strcasecmp(v->name, "ChannelIds")) {
			ast_copy_string(item->ChannelIdStr, v->value, sizeof(item->ChannelIdStr));
			//todo: parse all values into string
			item->ChannelId[0] = atoi(v->value);
		} else if (!strcasecmp(v->name, "SipId")) {
			item->SipId = atoi(v->value);
		} else if (!strcasecmp(v->name, "RtpFilter")) {
			item->RtpFilter = atoi(v->value);
		} else if (!strcasecmp(v->name, "ServerIP")) {
			ast_copy_string(item->ServerIP, v->value, sizeof(item->ServerIP));
		} else if (!strcasecmp(v->name, "type")) {
			// skip it
		} else {
			ast_debug(2, "Unknown parameter: [%s]:%s, skip\n", cat, v->name);
		}
	}

	return item;
}

static RadioSubscriber *load_subscriber_config(struct ast_config *cfg, const char *cat)
{
	struct ast_variable *v;
	RadioSubscriber *item = NULL;

	if (!(item = ao2_t_alloc_options(sizeof(*item), DESTRUCTOR_FN(RadioSubscriber), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate a RadioSubscriber struct"))) {
		return NULL;
	}

	item->Id = atoi(cat);

	for (v = ast_variable_browse(cfg, cat); v; v = v->next) {
		if (!strcasecmp(v->name, "Name")) {
			ast_copy_string(item->Name, v->value, sizeof(item->Name));
		} else if (!strcasecmp(v->name, "Id")) {
			item->Id = atoi(v->value);
		} else if (!strcasecmp(v->name, "RadioId")) {
			item->RadioId = atoi(v->value);
		} else if (!strcasecmp(v->name, "ChannelId")) {
			item->ChannelId = atoi(v->value);
		} else if (!strcasecmp(v->name, "CallGroupIds")) {
			ast_copy_string(item->CallGroupIdStr, v->value, sizeof(item->CallGroupIdStr));
			//todo: parse all values into string
			item->CallGroupId[0] = atoi(v->value);
		} else if (!strcasecmp(v->name, "HasGpsSupport")) {
			item->HasGpsSupport = ast_true(v->value);
		} else if (!strcasecmp(v->name, "HasTextMsgSupport")) {
			item->HasTextMsgSupport = ast_true(v->value);
		} else if (!strcasecmp(v->name, "HasRemoteMonitorSupport")) {
			item->HasRemoteMonitorSupport = ast_true(v->value);
		} else if (!strcasecmp(v->name, "IsEnabled")) {
			item->IsEnabled = ast_true(v->value);
		} else if (!strcasecmp(v->name, "IsOnline")) {
			item->IsOnline = ast_true(v->value);
		} else if (!strcasecmp(v->name, "RadioStationType")) {
			item->RadioStationType = atoi(v->value);
			switch (item->RadioStationType) {
			case Portable:
			case Mobile:
				break;
			default:
				ast_log(LOG_ERROR, "Incorrect value of parameter: [%s]:%s = %s\n", cat, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "ServerIP")) {
			ast_copy_string(item->ServerIP, v->value, sizeof(item->ServerIP));
		} else if (!strcasecmp(v->name, "type")) {
			// skip it
		} else {
			ast_debug(2, "Unknown parameter: [%s]:%s, skip\n", cat, v->name);
		}
	}

	return item;
}

static Channel *load_channel_config(struct ast_config *cfg, const char *cat)
{
#define CHANNEL_DISALLOW_DEFAULT "all"
#define CHANNEL_ALLOW_DEFAULT "ulaw,alaw"

#define CHANNEL_CONNECT_TIMEOUT_DEFAULT 10
#define CHANNEL_REQUEST_TIMEOUT_DEFAULT 2

#define CHANNEL_ST_IDLE_TIMEOUT    120		// channel may waits for starting for a restricted time
#define CHANNEL_ST_TRYC_TIMEOUT    600		// channel may connects to server for a long time
#define CHANNEL_ST_CONN_TIMEOUT     10		// channel may waits for authentication request for a short time
#define CHANNEL_ST_WFA_TIMEOUT      10		// channel may waits for authentication response for a short time
#define CHANNEL_ST_READY_TIMEOUT     0		// channel may stay connected and ready for call infinitely
#define CHANNEL_ST_DIAL_TIMEOUT      5		// state should be very short
#define CHANNEL_ST_RING_TIMEOUT     10		// state should be very short
#define CHANNEL_ST_BUSY_TIMEOUT      0		// channel may be busy infinitely
#define CHANNEL_ST_DETACH_TIMEOUT    5		// state should be very short
#define CHANNEL_ST_HANGTIME_TIMEOUT 10		// state should be very short
#define CHANNEL_ST_CANCEL_TIMEOUT   10		// state should be very short

	struct ast_variable *v;
	Channel *chan;
	RadiohytChannelState template_state = { .state = CHAN_RADIOHYT_ST_IDLE, .timer_id = -1 };

	gettimeofday(&template_state.timestamp, 0);

	if (!(chan = ao2_t_alloc(sizeof(*chan), DESTRUCTOR_FN(Channel), "allocate a Channel struct"))) {
		return NULL;
	}
	if (!(chan->caps = ast_format_cap_alloc(0))) {
		ao2_ref(chan, -1);
		return NULL;
	}

	chan->on_changed_connection_state_cb = &radiohyt_on_changed_connection_state;
	chan->on_receive_station_list_cb = &radiohyt_on_receive_station_list;
	chan->on_receive_group_list_cb = &radiohyt_on_receive_group_list;
	chan->on_receive_subscriber_list_cb = &radiohyt_on_receive_subscriber_list;
	chan->on_incoming_call_cb = &radiohyt_on_incoming_call;
	chan->on_changed_call_state_cb = &radiohyt_on_changed_call_state;
	chan->on_changed_channel_state_cb = &radiohyt_on_changed_channel_state;
	chan->on_changed_subscriber_state_cb = &radiohyt_on_changed_subscriber_state;

	ast_copy_string(chan->name, cat, sizeof(chan->name));
	ast_copy_string(chan->context, radiohyt_context, sizeof(chan->context));

	ast_parse_allow_disallow(NULL, chan->caps, CHANNEL_DISALLOW_DEFAULT, 0);
	ast_parse_allow_disallow(NULL, chan->caps, CHANNEL_ALLOW_DEFAULT, 1);

	//todo: use state timers instead
	chan->connect_timeout = CHANNEL_CONNECT_TIMEOUT_DEFAULT;
	chan->request_timeout = CHANNEL_REQUEST_TIMEOUT_DEFAULT;

	// init timeout's default values and timers for all states
	chan->states[CHAN_RADIOHYT_ST_IDLE] = template_state;
	chan->states[CHAN_RADIOHYT_ST_IDLE].timeout = CHANNEL_ST_IDLE_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_TRYC] = template_state;
	chan->states[CHAN_RADIOHYT_ST_TRYC].timeout = CHANNEL_ST_TRYC_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_CONN] = template_state;
	chan->states[CHAN_RADIOHYT_ST_CONN].timeout = CHANNEL_ST_CONN_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_WFA] = template_state;
	chan->states[CHAN_RADIOHYT_ST_WFA].timeout = CHANNEL_ST_WFA_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_READY] = template_state;
	chan->states[CHAN_RADIOHYT_ST_READY].timeout = CHANNEL_ST_READY_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DIAL] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DIAL].timeout = CHANNEL_ST_DIAL_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_RING] = template_state;
	chan->states[CHAN_RADIOHYT_ST_RING].timeout = CHANNEL_ST_RING_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_BUSY] = template_state;
	chan->states[CHAN_RADIOHYT_ST_BUSY].timeout = CHANNEL_ST_BUSY_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DETACH] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DETACH].timeout = CHANNEL_ST_DETACH_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DETACH_2] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DETACH_2].timeout = CHANNEL_ST_DETACH_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_HANGTIME] = template_state;
	chan->states[CHAN_RADIOHYT_ST_HANGTIME].timeout = CHANNEL_ST_HANGTIME_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_CANCEL] = template_state;
	chan->states[CHAN_RADIOHYT_ST_CANCEL].timeout = CHANNEL_ST_CANCEL_TIMEOUT;

	// load channel's settings
	for (v = ast_variable_browse(cfg, cat); v; v = v->next) {
		if (!strcasecmp(v->name, "login")) {
			ast_copy_string(chan->login, v->value, sizeof(chan->login));
		} else if (!strcasecmp(v->name, "secret")) {
			ast_md5_hash(chan->md5secret, v->value);
		} else if (!strcasecmp(v->name, "md5secret")) {
			ast_copy_string(chan->md5secret, v->value, sizeof(chan->md5secret));
		} else if (!strcasecmp(v->name, "server_address")) {
			struct ast_sockaddr addr;
			if (ast_sockaddr_parse(&addr, v->value, PARSE_PORT_REQUIRE)) {
				ast_sockaddr_copy(&chan->srv_addr, &addr);
				ast_copy_string(chan->server_ip, ast_sockaddr_stringify_host(&addr), sizeof(chan->server_ip));
			} else {
				ast_log(LOG_ERROR, "Incorrect value in line %d: %s = %s\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "address")) {
			struct ast_sockaddr addr;
			if (radiohyt_channel_policy == DYNAMIC) {
				continue;
			}
			if (ast_sockaddr_parse(&addr, v->value, PARSE_PORT_REQUIRE)) {
				ast_sockaddr_copy(&chan->addr, &addr);
			} else {
				ast_log(LOG_ERROR, "Incorrect value in line %d: %s = %s, skip\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "rtp_port")) {
			int port = atoi(v->value);
			if (radiohyt_channel_policy == DYNAMIC) {
				continue;
			}
			if (port > 1024 || port < 65535) {
				chan->rtp_port = (short)port;
			} else {
				ast_log(LOG_ERROR, "Incorrect value in line %d: %s = %s, skip\n", v->lineno, v->name, v->value);
				chan->rtp_port = 0;
			}
		} else if (!strcasecmp(v->name, "rtp_timeout")) {
			chan->rtp_timeout = atoi(v->value);
		} else if (!strcasecmp(v->name, "connect_timeout")) {
			int timeout = atoi(v->value);
			if (timeout && timeout <= 60) {
				chan->connect_timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "request_timeout")) {
			int timeout = atoi(v->value);
			if (timeout && timeout <= 60) {
				chan->request_timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "idle_timeout")) {
			int timeout = atoi(v->value);
			if (timeout == 0 || timeout > 60) {
				chan->states[CHAN_RADIOHYT_ST_IDLE].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "tryc_timeout")) {
			int timeout = atoi(v->value);
			if (timeout >= 0) {
				chan->states[CHAN_RADIOHYT_ST_TRYC].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "conn_timeout")) {
			int timeout = atoi(v->value);
			if (timeout && timeout <= 60) {
				chan->states[CHAN_RADIOHYT_ST_CONN].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "wfa_timeout")) {
			int timeout = atoi(v->value);
			if (timeout && timeout <= 60) {
				chan->states[CHAN_RADIOHYT_ST_WFA].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "ready_timeout")) {
			int timeout = atoi(v->value);
			if (timeout == 0 || timeout > 60) {
				chan->states[CHAN_RADIOHYT_ST_READY].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "ring_timeout")) {
			int timeout = atoi(v->value);
			if (timeout && timeout <= 60) {
				chan->states[CHAN_RADIOHYT_ST_RING].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "busy_timeout")) {
			int timeout = atoi(v->value);
			if (timeout >= 0) {
				chan->states[CHAN_RADIOHYT_ST_BUSY].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "hangtime_timeout")) {
			int timeout = atoi(v->value);
			if (timeout >= 0) {
				chan->states[CHAN_RADIOHYT_ST_HANGTIME].timeout = timeout;
			} else {
				ast_log(LOG_WARNING, "Incorrect value in line %d: %s = %s, use default\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "disallow")) {
			if (ast_parse_allow_disallow(NULL, chan->caps, v->value, 0)) {
				ast_log(LOG_WARNING, "Configuration error found in line %d: %s = %s\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "allow")) {
			if (ast_parse_allow_disallow(NULL, chan->caps, v->value, 1)) {
				ast_log(LOG_WARNING, "Configuration error found in line %d: %s = %s\n", v->lineno, v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "type")) {
			;
		} else {
			ast_log(LOG_WARNING, "Unknown parameter found in line %d: %s = %s\n", v->lineno, v->name, v->value);
		}
	}

	if (!strlen(chan->login)) {
		ast_log(LOG_ERROR, "Can't find mandatory parameter '%s'\n", "login");
		ao2_ref(chan, -1);
		return NULL;
	}
	if (!strlen(chan->md5secret)) {
		ast_log(LOG_ERROR, "Can't find mandatory parameter '%s' nor '%s'\n", "secret", "md5secret");
		ao2_ref(chan, -1);
		return NULL;
	}
	if (ast_sockaddr_isnull(&chan->srv_addr)) {
		ast_log(LOG_ERROR, "Can't find mandatory parameter '%s'\n", "server_address");
		ao2_ref(chan, -1);
		return NULL;
	}

	// allocate RTP instance
	{
		struct ast_sockaddr rtp_addr;

		ast_sockaddr_parse(&rtp_addr, "0.0.0.0", PARSE_PORT_IGNORE);
		ast_sockaddr_set_port(&rtp_addr, chan->rtp_port);

		chan->rtpi = ast_rtp_instance_new("asterisk", monitor_sched_ctx(), &rtp_addr, NULL);
		if (!chan->rtpi) {
			ast_log(LOG_ERROR, "Unable to allocate rtp instance\n");
			ao2_ref(chan, -1);
			return NULL;
		}
		ast_rtp_instance_get_local_address(chan->rtpi, &rtp_addr);
		chan->rtp_port = ast_sockaddr_port(&rtp_addr);
	}

	// init request's queue
	AST_LIST_HEAD_INIT_NOLOCK(&chan->request_queue);

	// set creation time
	gettimeofday(&chan->tport.stat.creation_ts, 0);

	return chan;
}

static int load_config(enum channelreloadreason reason)
{
	struct ast_config *cfg = NULL;
	struct ast_flags flags = { reason == CHANNEL_MODULE_LOAD ? 0 : CONFIG_FLAG_FILEUNCHANGED };
	const char *value;
	char *cat = "general";
	int is_reload = ((reason == CHANNEL_MODULE_RELOAD) || (reason == CHANNEL_CLI_RELOAD));
	int prev_channel_policy = radiohyt_channel_policy;

	if (is_reload) {
		int rc = 0;
		Channel *chan;
		struct ao2_iterator i;

		i = ao2_iterator_init(radiohyt_channels, 0);
		while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
			if (chan->state > CHAN_RADIOHYT_ST_READY) {
				rc++;
			}
			ao2_ref(chan, -1);
		}
		ao2_iterator_destroy(&i);

		if (rc) {
			ast_log(LOG_WARNING, "RADIOHYT: Unable to reload configuration, because there are busy channels\n");
			return -1;
		}
	}

	cfg = ast_config_load(radiohyt_config_filename, flags);
	if (!cfg) {
		ast_log(LOG_ERROR, "RADIOHYT: Unable to load config file %s\n", radiohyt_config_filename);
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		ast_log(LOG_NOTICE, "RADIOHYT: Config file %s is unchanged, skipped\n", radiohyt_config_filename);
		return 1;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "RADIOHYT: Content of %s is invalid and cannot be parsed\n", radiohyt_config_filename);
		return -1;
	}

	if (ast_category_root(cfg, cat)) {
		value = ast_variable_retrieve(cfg, cat, "context");
		if (value) {
			ast_copy_string(radiohyt_context, value, sizeof(radiohyt_context));
		} else {
			sprintf(radiohyt_context, "default");
		}

		value = ast_variable_retrieve(cfg, cat, "channel_policy");
		if (!value) {
			ast_log(LOG_WARNING, "RADIOHYT: Can't find mandatory parameter '[%s]:%s', use default value\n", cat, "channel_policy");
		} else if (!strcasecmp(value, "static")) {
			radiohyt_channel_policy = STATIC;
			ast_log(LOG_NOTICE, "RADIOHYT: Creation channel policy is static\n");
		} else if (!strcasecmp(value, "dynamic")) {
			radiohyt_channel_policy = DYNAMIC;
			ast_log(LOG_NOTICE, "RADIOHYT: Creation channel policy is dynamic\n");
		} else {
			ast_log(LOG_WARNING, "RADIOHYT: Incorrect value of parameter '[%s]:%s', use default value\n", cat, "channel_policy");
		}

		value = ast_variable_retrieve(cfg, cat, "channel_limit");
		if (value) {
			int limit = atoi(value);
			if (limit && limit <= MAX_CHANNELS) {
				radiohyt_channel_limit = limit;
			} else {
				ast_log(LOG_WARNING, "RADIOHYT: Incorrect value of parameter '[%s]:%s', use default\n", cat, "channel_limit");
			}
		}

		value = ast_variable_retrieve(cfg, cat, "debug");
		if (value) {
			radiohyt_debug = ast_true(value) == -1 ? 1 : 0;
		}
	} else {
		ast_log(LOG_WARNING, "RADIOHYT: Can't find mandatory section [%s], use default values\n", cat);
	}

	if (is_reload) {
		ao2_t_callback(radiohyt_stations, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
			stations_cleanup_cb, NULL, "initiating callback to clean stations table");

		ao2_t_callback(radiohyt_callgroups, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
			callgroups_cleanup_cb, NULL, "initiating callback to clean callgroups table");

		ao2_t_callback(radiohyt_subscribers, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
			subscribers_cleanup_cb, NULL, "initiating callback to clean subscribers table");

		if (prev_channel_policy != radiohyt_channel_policy) {
			// destroying all channels
			int arg = CHANNELS_ALL;

			ao2_t_callback(radiohyt_channels, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
				chan_cleanup_cb, &arg, "initiating callback to clean channels table");
		}
	}

	while ((cat = ast_category_browse(cfg, cat))) {
		const char *value = ast_variable_retrieve(cfg, cat, "type");
		if (!value) {
			ast_log(LOG_WARNING, "RADIOHYT: Not specified type for section [%s], skip it\n", cat);
			continue;
		}
		if (!strcasecmp(value, "channel")) {
			Channel *chan, *chan_old;

			if (radiohyt_channel_policy == DYNAMIC && strcmp(cat, first_channel_name)) {
				ast_log(LOG_WARNING, "RADIOHYT: Skip settings of \"%s\"\n - policy is dynamic!\n", cat);
				continue;
			}
			if ((chan_old = radiohyt_find_chan_by_name(cat))) {
				ao2_ref(chan_old, -1);
			}
			chan = load_channel_config(cfg, cat);
			if (chan) {
				char buf[BUFSIZE];
				
				if (!chan_old) {
					ast_log(LOG_NOTICE, "RADIOHYT: New logical channel \"%s\" is successfully created\n",
						chan->name);
				} else if (chan_old->state > CHAN_RADIOHYT_ST_READY) {
					ast_log(LOG_WARNING, "RADIOHYT: Can't apply new settings for \"%s\" - channel is busy!\n",
						chan->name);
					continue;
				} else {
					ast_log(LOG_WARNING, "RADIOHYT: Apply new settings for \"%s\" - channel is reloaded\n",
						chan->name);
					chan_cleanup(chan_old);
					ao2_t_unlink(radiohyt_channels, chan_old, "unlink item into channels table");
					ao2_ref(chan_old, -1);
				}
				ast_log(LOG_NOTICE, "RADIOHYT: Channel \"%s\" capabilities: %s\n",
					chan->name, ast_getformatname_multiple(buf, BUFSIZE, chan->caps));
#ifdef LOADING_AB_FROM_CONFIG
				chan->is_subscribers_loaded = 1;
#endif
				ao2_t_link(radiohyt_channels, chan, "link item into channels table");

				if (!ao2_t_find(radiohyt_servers, &chan->srv_addr, OBJ_POINTER, "find item into servers table")) {
					struct ast_sockaddr *addr = ao2_t_alloc_options(sizeof(*addr), DESTRUCTOR_FN(Server), AO2_ALLOC_OPT_LOCK_NOLOCK, "allocate an sockaddr struct");
					if (addr) {
						ast_sockaddr_copy(addr, &chan->srv_addr);
						ao2_t_link(radiohyt_servers, addr, "link item into servers table");
					}
				}

			} else {
				ast_log(LOG_ERROR, "RADIOHYT: Can't create new channel - unable to load settings \"%s\"\n",
					cat);
			}

		} else if (!strcasecmp(value, "group")) {
#ifdef LOADING_AB_FROM_CONFIG
			RadioCallGroup *rg  = load_callgroup_config(cfg, cat);
			RadioCallGroup *rg2 = ao2_t_find(radiohyt_callgroups, rg, OBJ_POINTER, "find item into callgroups table");
			if (rg2) {
				if (rg)
					ast_copy_string(rg->ServerIP, rg2->ServerIP, sizeof(rg->ServerIP));
				if (is_reload)
					ast_debug(2, "RADIOHYT: unlink callgroup %d '%s'\n",
						rg2->RadioId, rg2->Name);
				ao2_t_unlink(radiohyt_callgroups, rg2, "unlink item from callgroups table");
			}
			if (rg) {
				if (is_reload)
					ast_debug(2, "RADIOHYT: loaded config of callgroup %d '%s'\n",
						rg->RadioId, rg->Name);
				ao2_t_link(radiohyt_callgroups, rg, "link item into callgroups table");
			}
#endif
		} else if (!strcasecmp(value, "subscriber")) {
#ifdef LOADING_AB_FROM_CONFIG
			RadioSubscriber *rs  = load_subscriber_config(cfg, cat);
			RadioSubscriber *rs2 = ao2_t_find(radiohyt_subscribers, rs, OBJ_POINTER, "find item into subscribers table");
			if (rs2) {
				if (rs)
					ast_copy_string(rs->ServerIP, rs2->ServerIP, sizeof(rs->ServerIP));
				if (is_reload)
					ast_debug(2, "RADIOHYT: unlink subscriber %d '%s'\n",
						rs2->RadioId, rs2->Name);
				ao2_t_unlink(radiohyt_subscribers, rs2, "unlink item from subscribers table");
			}
			if (rs) {
				if (is_reload)
					ast_debug(2, "RADIOHYT: loaded config of subscriber %d '%s'\n",
						rs->RadioId, rs->Name);
				ao2_t_link(radiohyt_subscribers, rs, "link item into subscribers table");
			}
#endif
		} else if (!strcasecmp(value, "station")) {
#ifdef LOADING_AB_FROM_CONFIG
			RadioControlStation *rc  = load_station_config(cfg, cat);
			RadioControlStation *rc2 = ao2_t_find(radiohyt_stations, rc, OBJ_POINTER, "find item into stations table");
			if (rc2) {
				if (rc)
					ast_copy_string(rc->ServerIP, rc2->ServerIP, sizeof(rc->ServerIP));
				if (is_reload)
					ast_debug(2, "RADIOHYT: unlink station %d '%s'\n",
						rc2->RadioId, rc2->Name);
				ao2_t_unlink(radiohyt_stations, rc2, "unlink item from stations table");
			}
			if (rc) {
				if (is_reload)
					ast_debug(2, "RADIOHYT: loaded config of station %d '%s'\n",
						rc->RadioId, rc->Name);
				ao2_t_link(radiohyt_stations, rc, "link item into stations table");
			}
#endif
		} else {
			ast_log(LOG_WARNING, "RADIOHYT: Unknown type of settings: '%s', skip\n", cat);
		}
	}

	ast_config_destroy(cfg);

	if (!ao2_container_count(radiohyt_channels)) {
		if (radiohyt_channel_policy == DYNAMIC) {
			ast_log(LOG_ERROR, "RADIOHYT: Can't start - mandatory %s has not been created\n",
				first_channel_name);
		} else {
			ast_log(LOG_ERROR, "RADIOHYT: Can't start - no one channel has been created\n");
		}
		return -1;
	}
	return 0;
}

static void apply_config(void)
{
	Channel *chan;
	struct ao2_iterator i;

	// apply "debug" value
	ao2_callback(radiohyt_channels, OBJ_NODATA | OBJ_MULTIPLE, chan_set_debug_cb, &radiohyt_debug);

	// open channels
	i = ao2_iterator_init(radiohyt_channels, 0);
	while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
		if (chan->state == CHAN_RADIOHYT_ST_IDLE) {
			radiohyt_chan_open(chan);
		}
		ao2_ref(chan, -1);
	}
	ao2_iterator_destroy(&i);
}

static int keepalive_task_cb(const void *data)
{
	ast_debug(2, "RADIOHYT: keepalive task run...\n");
	if (ast_mutex_trylock(&radiohyt_mutex) == 0) {
		RadioSubscriber *rs;
		struct ao2_iterator i = ao2_iterator_init(radiohyt_subscribers, 0);
		while ((rs = ao2_t_iterator_next(&i, "iterate thru subscribers table"))) {
			chan_keepalive_arg_t arg = { rs->ChannelId, rs->RadioId };
			ao2_t_callback(radiohyt_channels, OBJ_NODATA | OBJ_MULTIPLE,
				chan_keepalive_cb, &arg, "initiating callback to send keepalive");
			ao2_ref(rs, -1);
			break;
		}
		ao2_iterator_destroy(&i);

		ast_mutex_unlock(&radiohyt_mutex);
	}
	return 1;
}

static int garbage_collector_cb(const void *data)
{
	if (radiohyt_channel_policy == STATIC)
		return 0;

	ast_debug(2, "RADIOHYT: garbage collector run checking...\n");
	if (ast_mutex_trylock(&radiohyt_mutex) == 0) {
		int arg = CHANNELS_DYNAMIC;

		ao2_t_callback(radiohyt_channels, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
			chan_cleanup_cb, &arg, "initiating callback to clean channels table");

		ast_mutex_unlock(&radiohyt_mutex);
	}
	ast_debug(2, "RADIOHYT: garbage collector stop\n");

	return 1;
}

#ifdef TEST_FRAMEWORK

AST_TEST_DEFINE(radiohyt_test_case_50)
{
	int rc;
	int res = AST_TEST_FAIL;
	struct ast_channel *ast_chan0 = NULL;
	Channel *chan0 = NULL;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_50";
		info->category = "/channels/radiohyt/channel_allocations/";
		info->summary = "Check channel allocation algorithm for dynamic trunk";
		info->description = "Make call, switch to hangtime and continuation call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	do {
		char channel_name[255];
		const char *dest0 = "HYT/100";
		int cause;

		int id = 1;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		if (radiohyt_channel_policy != DYNAMIC) break;

		// 1. AICS: sending StartVoiceCall
		ast_chan0 = radiohyt_request("HYT", NULL, NULL, ast_dummy_channel_alloc(), dest0, &cause);
		if (ast_chan0 == NULL) break;

		chan0 = ast_channel_tech_pvt(ast_chan0);
		if (chan0 == NULL) break;
		strcpy(channel_name, chan0->name);
		//!!!
		chan0->owner = NULL;
		chan0->peer = NULL;
		rc = radiohyt_call(ast_chan0, dest0, 10000);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT: sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan0->state != CHAN_RADIOHYT_ST_RING) break;

		// 3. RHYT: sending Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall at c0
		rc = radiohyt_drop_call(chan0, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Hangtime Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// release ast_channel
		ast_queue_hangup_with_cause(ast_chan0, AST_CAUSE_NORMAL_CLEARING);
		ast_channel_unref(ast_chan0);

		// 7. AICS: sending StartVoiceCall at the same destination
		ast_chan0 = radiohyt_request("HYT", NULL, NULL, ast_dummy_channel_alloc(), dest0, &cause);
		if (ast_chan0 == NULL) break;

		chan0 = ast_channel_tech_pvt(ast_chan0);
		if (chan0 == NULL || strcmp(chan0->name, channel_name)) break;
		//!!!
		chan0->owner = NULL;
		chan0->peer = NULL;
		rc = radiohyt_call(ast_chan0, dest0, 10000);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 8. RHYT: sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan0->state != CHAN_RADIOHYT_ST_RING) break;

		// 9: RHYT sending BeginCall Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 10: AICS sending StopVoiceCall at c0
		rc = radiohyt_drop_call(chan0, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 11: RHYT sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);

		// 12: RHYT sending Hangtime Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 13: RHYT sending EndCall Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_READY) break;

		ast_queue_hangup_with_cause(ast_chan0, AST_CAUSE_NORMAL_CLEARING);

		res = AST_TEST_PASS;
	} while(0);

	if (ast_chan0)
		ast_channel_unref(ast_chan0);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_51)
{
	int rc;
	int res = AST_TEST_FAIL;
	struct ast_channel *ast_chan0 = NULL, *ast_chan1 = NULL;
	Channel *chan0 = NULL, *chan1 = NULL;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_51";
		info->category = "/channels/radiohyt/channel_allocations/";
		info->summary = "Check channel allocation algorithm for dynamic trunk";
		info->description = "Make first call, switch to hangtime and make second call, then continuation first call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	do {
		const char *dest0 = "HYT/100";
		const char *dest1 = "HYT/101";
		int cause;

		int id = 1;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		if (radiohyt_channel_policy != DYNAMIC) break;

		// 1. AICS: sending StartVoiceCall
		ast_chan0 = radiohyt_request("HYT", NULL, NULL, ast_dummy_channel_alloc(), dest0, &cause);
		if (ast_chan0 == NULL) break;

		chan0 = ast_channel_tech_pvt(ast_chan0);
		if (chan0 == NULL) break;
		//!!!
		chan0->owner = NULL;
		chan0->peer = NULL;

		rc = radiohyt_call(ast_chan0, dest0, 10000);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT: sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan0->state != CHAN_RADIOHYT_ST_RING) break;

		// 3. RHYT: sending Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall at c0
		rc = radiohyt_drop_call(chan0, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Hangtime Notification at c0
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7. AICS: sending StartVoiceCall
		ast_chan1 = radiohyt_request("HYT", NULL, NULL, ast_dummy_channel_alloc(), dest1, &cause);
		if (ast_chan1 == NULL) break;

		chan1 = ast_channel_tech_pvt(ast_chan1);
		if (chan1 == NULL) break;
		if (chan1 == chan0) break;
		//!!!
		chan1->owner = NULL;
		chan1->peer = NULL;

		rc = radiohyt_call(ast_chan1, dest1, 10000);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 8. RHYT: sending Response at c1
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// 9. RHYT: sending Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 1000);
		if (chan0->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// release ast_channel
		ast_queue_hangup_with_cause(ast_chan0, AST_CAUSE_NORMAL_CLEARING);
		ast_channel_unref(ast_chan0);

		// 10. AICS: sending StartVoiceCall at the same destination
		ast_chan0 = radiohyt_request("HYT", NULL, NULL, ast_dummy_channel_alloc(), dest0, &cause);
		if (ast_chan0 == NULL) break;

		chan0 = ast_channel_tech_pvt(ast_chan0);
		if (chan0 == NULL || strcmp(chan0->name, first_channel_name)) break;
		//!!!
		chan0->owner = NULL;
		chan0->peer = NULL;
		rc = radiohyt_call(ast_chan0, dest0, 10000);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 11. RHYT: sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan0->state != CHAN_RADIOHYT_ST_RING) break;

		// 12: RHYT sending BeginCall Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 13: AICS sending StopVoiceCall at c0
		rc = radiohyt_drop_call(chan0, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan0->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 14: RHYT sending Response at c0
		send_msg = chan0->last_send_msg;
		chan0->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		ast_json_unref(send_msg);

		// 15: RHYT sending Hangtime Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 16: RHYT sending EndCall Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 17: AICS sending StopVoiceCall at c1
		rc = radiohyt_drop_call(chan1, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 18: RHYT sending Response at c1
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// 19: RHYT sending Hangtime Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 20: RHYT sending EndCall Notification at both c0, c1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan0, recv_msg, 100);
		if (chan0->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	if (ast_chan0) {
		ast_queue_hangup_with_cause(ast_chan0, AST_CAUSE_NORMAL_CLEARING);
		ast_channel_unref(ast_chan0);
	}
	if (ast_chan1) {
		ast_queue_hangup_with_cause(ast_chan1, AST_CAUSE_NORMAL_CLEARING);
		ast_channel_unref(ast_chan1);
	}
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

static Channel* radiohyt_alloc_channel(const char *name, int policy)
{
	RadiohytChannelState template_state = {
	    .state = CHAN_RADIOHYT_ST_IDLE,
	    .timer_id = -1,
	};

	Channel *chan = ast_malloc(sizeof(Channel));

	sprintf(chan->server_ip, "127.0.0.1");
	ast_sockaddr_parse(&chan->srv_addr, chan->server_ip, PARSE_PORT_IGNORE);

	chan->on_changed_connection_state_cb = &radiohyt_on_changed_connection_state;
	chan->on_receive_station_list_cb = &radiohyt_on_receive_station_list;
	chan->on_receive_group_list_cb = &radiohyt_on_receive_group_list;
	chan->on_receive_subscriber_list_cb = &radiohyt_on_receive_subscriber_list;
	chan->on_incoming_call_cb = &radiohyt_on_incoming_call;
	chan->on_changed_call_state_cb = &radiohyt_on_changed_call_state;
	chan->on_changed_channel_state_cb = &radiohyt_on_changed_channel_state;
	chan->on_changed_subscriber_state_cb = &radiohyt_on_changed_subscriber_state;

	chan->policy = policy;
	ast_copy_string(chan->name, name, sizeof(chan->name));
	ast_copy_string(chan->context, "AICS", sizeof(chan->context));
	chan->debug = radiohyt_debug;

	chan->caps = ast_format_cap_alloc(0);
	ast_parse_allow_disallow(NULL, chan->caps, CHANNEL_DISALLOW_DEFAULT, 0);
	ast_parse_allow_disallow(NULL, chan->caps, CHANNEL_ALLOW_DEFAULT, 1);

	chan->connect_timeout = CHANNEL_CONNECT_TIMEOUT_DEFAULT;
	chan->request_timeout = CHANNEL_REQUEST_TIMEOUT_DEFAULT;

	// init timeout's default values and timers for all states
	chan->states[CHAN_RADIOHYT_ST_IDLE] = template_state;
	chan->states[CHAN_RADIOHYT_ST_IDLE].timeout = 10;//CHANNEL_ST_IDLE_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_TRYC] = template_state;
	chan->states[CHAN_RADIOHYT_ST_TRYC].timeout = CHANNEL_ST_TRYC_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_CONN] = template_state;
	chan->states[CHAN_RADIOHYT_ST_CONN].timeout = CHANNEL_ST_CONN_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_WFA] = template_state;
	chan->states[CHAN_RADIOHYT_ST_WFA].timeout = CHANNEL_ST_WFA_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_READY] = template_state;
	chan->states[CHAN_RADIOHYT_ST_READY].timeout = CHANNEL_ST_READY_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DIAL] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DIAL].timeout = CHANNEL_ST_DIAL_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_RING] = template_state;
	chan->states[CHAN_RADIOHYT_ST_RING].timeout = CHANNEL_ST_RING_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_BUSY] = template_state;
	chan->states[CHAN_RADIOHYT_ST_BUSY].timeout = CHANNEL_ST_BUSY_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DETACH] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DETACH].timeout = CHANNEL_ST_DETACH_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_DETACH_2] = template_state;
	chan->states[CHAN_RADIOHYT_ST_DETACH_2].timeout = CHANNEL_ST_DETACH_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_HANGTIME] = template_state;
	chan->states[CHAN_RADIOHYT_ST_HANGTIME].timeout = CHANNEL_ST_HANGTIME_TIMEOUT;

	++template_state.state;
	chan->states[CHAN_RADIOHYT_ST_CANCEL] = template_state;
	chan->states[CHAN_RADIOHYT_ST_CANCEL].timeout = CHANNEL_ST_CANCEL_TIMEOUT;

	ast_copy_string(chan->login, "alvid", sizeof(chan->login));
	ast_md5_hash(chan->md5secret, "alvid");

	// init request's queue
	AST_LIST_HEAD_INIT_NOLOCK(&chan->request_queue);

	chan->last_send_msg = NULL;
	chan->rtpi = NULL;
	chan->owner = NULL;
	chan->peer = NULL;
	chan->session = NULL;
	chan->last_transaction_id = 0;
	chan->sched_conn_id = 0;
	chan->state = CHAN_RADIOHYT_ST_IDLE;
	memset(&chan->tport.stat, 0, sizeof(chan->tport.stat));

	// set creation time
	gettimeofday(&chan->tport.stat.creation_ts, 0);

	return chan;
}

AST_TEST_DEFINE(radiohyt_test_case_30)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_30";
		info->category = "/channels/radiohyt/unexpected_end_call";
		info->summary = "Group of tests to check correct behaviour of channel receiving unexpected call event";
		info->description = "Catch event into BUSY state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_31)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_31";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into DIAL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_32)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_32";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into CANCEL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_33)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_33";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into RING state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_34)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_34";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into BUSY state for outgoing call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_35)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_35";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into BUSY state for incoming call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id
		const char *source1 = "101";
		const char *source2 = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc || this->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT sending Incoming Call Notification (from RadioId 100)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;
		rc = radiohyt_session_get(this, chan_id, &session);
		if (!(rc == E_OK && session.owner == this)) break;

		// RHYT sending Incoming Call Notification (from RadioId 101)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", source1, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		rc = radiohyt_session_get(this, chan_id+1, &session);
		if (rc != E_OK) break;

		// RHYT sending Incoming Call Notification (from RadioId 102)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source2, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		rc = radiohyt_session_get(this, chan_id+2, &session);
		if (rc != E_OK) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%s", source) != AST_DEVICE_UNAVAILABLE) break;
		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;
		
		if (radiohyt_get_devicestate("HYT/%s", source1) != AST_DEVICE_UNAVAILABLE) break;
		rc = radiohyt_session_get(this, chan_id+1, &session);
		if (rc == E_OK) break;

		if (radiohyt_get_devicestate("HYT/%s", source2) != AST_DEVICE_UNAVAILABLE) break;
		rc = radiohyt_session_get(this, chan_id+2, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_36)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_36";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into DETACH state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_37)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_37";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into DETACH_2 state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_38)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_38";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving break event";
		info->description = "Catch break event into HANGTIME state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_39)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_39";
		info->category = "/channels/radiohyt/break_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Catch break event into BUSY state for incoming call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id
		const char *source1 = "101";
		const char *source2 = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc || this->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT sending Incoming Call Notification (from RadioId 100)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;
		rc = radiohyt_session_get(this, chan_id, &session);
		if (!(rc == E_OK && session.owner == this)) break;

		// RHYT sending Incoming Call Notification (from RadioId 101)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", source1, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		rc = radiohyt_session_get(this, chan_id+1, &session);
		if (rc != E_OK) break;

		// RHYT sending Incoming Call Notification (from RadioId 102)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source2, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		rc = radiohyt_session_get(this, chan_id+2, &session);
		if (rc != E_OK) break;

		radiohyt_fake_on_break_event(this);
		if (this->state != CHAN_RADIOHYT_ST_TRYC) break;

		if (radiohyt_get_devicestate("HYT/%s", source) != AST_DEVICE_NOT_INUSE) break;
		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (radiohyt_get_devicestate("HYT/%s", source1) != AST_DEVICE_NOT_INUSE) break;
		rc = radiohyt_session_get(this, chan_id+1, &session);
		if (rc == E_OK) break;

		if (radiohyt_get_devicestate("HYT/%s", source2) != AST_DEVICE_NOT_INUSE) break;
		rc = radiohyt_session_get(this, chan_id+2, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_41)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_41";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into DIAL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_42)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_42";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into CANCEL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_43)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_43";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into RING state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_44)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_44";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into BUSY state for outgoing call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_45)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_45";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into BUSY state for incoming call";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc || this->state != CHAN_RADIOHYT_ST_READY) break;

		// 1: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_46)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_46";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into DETACH state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_47)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_47";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into DETACH_2 state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_48)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_48";
		info->category = "/channels/radiohyt/stop_event";
		info->summary = "Group of tests to check correct behaviour of channel receiving stop event";
		info->description = "Catch stop event into HANGTIME state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_chan_close(this);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_61)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_61";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into DIAL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 1000);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		usleep(CHANNEL_ST_DIAL_TIMEOUT*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_62)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_62";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into CANCEL state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		usleep((CHANNEL_ST_CANCEL_TIMEOUT+1)*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_63)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_63";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into RING state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		usleep((CHANNEL_ST_RING_TIMEOUT+1)*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_64)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_64";
		info->category = "/channels/radiohyt/fail_event";
		info->summary = "Group of tests to check correct behaviour of channel receives session fail event";
		info->description = "Handle call session fail event into RING state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type,
				"CallState", 4, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_66)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_66";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into DETACH state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		usleep((CHANNEL_ST_DETACH_TIMEOUT+1)*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_67)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_67";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into DETACH_2 state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		usleep((CHANNEL_ST_DETACH_TIMEOUT+1)*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_68)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "radiohyt_test_case_68";
		info->category = "/channels/radiohyt/timeout_event";
		info->summary = "Group of tests to check correct behaviour of channel handles timeout";
		info->description = "Handle timeout into HANGTIME state";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_DETACH_2) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		usleep((CHANNEL_ST_HANGTIME_TIMEOUT+1)*1e6);
		if (this->state != CHAN_RADIOHYT_ST_IDLE) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		if (this->session) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	Channel_destructor(this);
	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

////////////////////////////////////////////////////////////////////////////////

AST_TEST_DEFINE(radiohyt_test_case_01)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_01";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Take erroneous response on outgoing call request";
		info->description = "This is a test for emergency outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}


	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response on StartVoiceCall
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", req_id, "result", "Result", -1);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_02)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_02";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Cancel outgoing call to RadioId before take erroneous response";
		info->description = "This is a test for emergency outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		// RHYT: sending Response on StartVoiceCall
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", req_id, "result", "Result", -1);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->last_send_msg != NULL) break;
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_03)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_03";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Cancel outgoing call to RadioId before answer and erroneous response";
		info->description = "This is a test for normal outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		// RHYT: sending Response on StartVoiceCall
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", req_id, "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_CANCEL) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type,
				"CallState", 4, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->last_send_msg != NULL) break;
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_04)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_04";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Cancel outgoing call to RadioId before answer";
		info->description = "This is a test for normal outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int req_id;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		ast_json_unref(send_msg);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;
		if (this->last_send_msg != NULL) break;

		// RHYT: sending Response on StartVoiceCall
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", req_id, "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_CANCEL) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Response on StopVoiceCall
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		if (send_msg == NULL) break;
		req_id = ast_json_integer_get(ast_json_object_get(send_msg, "id"));
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", req_id, "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_05)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_05";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Outgoing call to unavailable radio subscriber";
		info->description = "This is a test for unsuccessfull outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type,
				"CallState", 4, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_06)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_06";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Cancel outgoing call to RadioId before answer";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (rc || this->state != CHAN_RADIOHYT_ST_RING) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_CANCEL) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_07)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_07";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal outgoing call to RadioId";
		info->description = "This is a test for normal outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		usleep(1000000);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_08)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_08";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal outgoing call to RadioId with continuation into hangtime";
		info->description = "This is a test for outgoing call scenario with continuation";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 8: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 9: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 10: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 11: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 12: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 13: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_09)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_09";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal outgoing call to RadioId with continuation into hangtime with incoming call";
		info->description = "This is a test for outgoing call scenario with continuation";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 8: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 9: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_10)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_10";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal incoming call from RadioId";
		info->description = "This is a test for incoming call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 3: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_11)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_11";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal incoming call from RadioId with trying to drop";
		info->description = "This is a test for incoming call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: AICS trying to send StopVoiceCall, but can't
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 3: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 4: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_12)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_12";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal incoming call from RadioId with continuation by incoming call too";
		info->description = "This is a test for incoming call scenario with continuation";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 3: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 5: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_13)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_13";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal incoming call from RadioId with continuation by outgoing call";
		info->description = "This is a test for incoming call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 3: AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 4: RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 5: RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// 6: AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 7: RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 8: RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 9: RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_14)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_14";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, normal outgoing call to RadioId";
		info->description = "This is a test for normal outgoing call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel1", STATIC);
	chan2 = radiohyt_alloc_channel("channel2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_15)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_15";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, normal incoming call from RadioId";
		info->description = "Receive incoming call notification for each of two static channels";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel1", STATIC);
	chan2 = radiohyt_alloc_channel("channel2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "100";//dest_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;
		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Incoming Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// 3: RHYT sending Hangtime Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 4: RHYT sending Hangtime Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// 5: RHYT sending End Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		// 6: RHYT sending End Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_19)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_19";
		info->category = "/channels/radiohyt/hangtime";
		info->summary = "2x Dynamic channels, first outgoing call, switch to hangtime and second outgoing call";
		info->description = "This is a test for hangtime scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		// Here: channel2 is busy

		// RHYT[1]: sending EndCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// AICS[2]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_20)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;
	RadiohytSession session;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_20";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, 2x outgoing calls to RadioId";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel1", STATIC);
	chan2 = radiohyt_alloc_channel("channel2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		// Here: channel2 is busy

		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc || session.owner != chan1) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		// AICS[2]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan2, chan_id+1, slot_id, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (rc || session.owner != chan2) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_21)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_21";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, 2x incoming call from RadioId";
		info->description = "Receive incoming call notification for two static channels";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", STATIC);
	chan2 = radiohyt_alloc_channel("channel1", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int call_type = Private;
		int station_id = 10;
		int dest_id = 100;
		const char *source1 = "100";
		const char *source2 = "101";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;
		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// RHYT[1]: sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source1, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc != E_OK) break;
		if (!(session.owner == chan1 && session.status_owner == chan1)) break;

		// RHYT[2]: sending Incoming Call Notification for chan2
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", source2, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (rc != E_OK) break;
		if (!(session.owner == chan2 && session.status_owner == chan2)) break;

		// HERE: chan1 & chan2 are busy

		// RHYT[2]: sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", source2, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", source2, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT[1]: sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source1, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);

		// RHYT[1]: sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id, "Source", source1, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan1, chan_id+1, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_22)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_22";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, 2x outgoing calls to RadioId and new incoming call";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel1", STATIC);
	chan2 = radiohyt_alloc_channel("channel2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (!(rc == E_OK && session.owner == chan1 && session.status_owner == chan1)) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (!(rc == E_OK && session.owner == chan2 && session.status_owner == chan2)) break;
		// Here: channel2 is busy

		// RHYT: sending Incoming Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%s", source) != AST_DEVICE_INUSE) break;

		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (rc == E_OK) break;

		// RHYT: sending Hangtime Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%s", source) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending End Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		rc = radiohyt_session_get(chan1, chan_id+2, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan2, chan_id+2, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_23)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_23";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Static channels, 2x outgoing calls to RadioId and new incoming call";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel1", STATIC);
	chan2 = radiohyt_alloc_channel("channel2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		// Here: channel2 is busy


		// RHYT: sending Incoming Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate(source) != AST_DEVICE_INUSE) break;

		// RHYT: sending Hangtime Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate(source) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending End Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate(source) != AST_DEVICE_NOT_INUSE) break;


		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_24)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_24";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x Dynamic channels, 2x outgoing calls to RadioId";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;

		// Here: channel2 is busy

		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_25)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_25";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x dynamic channels, 2x outgoing calls to RadioId and new incoming call";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		// Here: channel2 is busy

		// RHYT: sending Incoming Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate(source) != AST_DEVICE_INUSE) break;

		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT: sending Hangtime Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		if (radiohyt_get_devicestate(source) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending End Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_26)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_26";
		info->category = "/channels/radiohyt/common/";
		info->summary = "2x dynamic channels, 2x outgoing calls to RadioId and new incoming call";
		info->description = "This is a test for normal outgoing call scenario";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;
		int station_id = 10;
		const char *source = "102";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS[1]: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		// Here: channel1 is busy

		// AICS[2]: sending StartVoiceCall
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id+1, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT[2]: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_INUSE) break;
		// Here: channel2 is busy


		// RHYT: sending Incoming Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate(source) != AST_DEVICE_INUSE) break;

		// RHYT: sending Hangtime Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate(source) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending End Call[3] Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 2, "CallType", call_type,
				"ChannelId", chan_id+2, "Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate(source) != AST_DEVICE_NOT_INUSE) break;


		// AICS[1]: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT[1]: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[1]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;
		radiohyt_fake_recv_event(chan2, recv_msg, 100);

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id+1) != AST_DEVICE_NOT_INUSE) break;

		// RHYT[2]: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id+1, "Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_27)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_27";
		info->category = "/channels/radiohyt/common/";
		info->summary = "Normal outgoing call to Group RadioId with continuation into hangtime with incoming call";
		info->description = "This is a test for outgoing call scenario with continuation";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;
		int station_id = 10;
		const char *source = "100";//radio_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 8: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		// 9: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_28)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_28";
		info->category = "/channels/radiohyt/hangtime";
		info->summary = "Continuation call into hangtime with reception end of call notification into DIAL state and accept of new call";
		info->description = "This is a test for outgoing call scenario with continuation";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending BeginCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Hangtime Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 8: RHYT sending EndCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 9: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 10: RHYT sending BeginCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 11: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 12: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 13: RHYT sending Hangtime Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 14: RHYT sending EndCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_29)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_29";
		info->category = "/channels/radiohyt/hangtime";
		info->summary = "Continuation call into hangtime with reception end of call notification into DIAL state and reject of new call";
		info->description = "This is a test for outgoing call scenario with continuation";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 100;
		int call_type = Private;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending BeginCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 1,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Hangtime Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 2,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 8: RHYT sending EndCall Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"CallState", 3,
				"InitiatorType", 1, "CallType", call_type,
				"ChannelId", chan_id, "Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 9: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", -1);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

////////////////////////////////////////////////////////////////////////////////

AST_TEST_DEFINE(radiohyt_test_case_70)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_70";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Make subscribers & callgroups as unavailable for call if channel set offline";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id_1 = 10;
		int chan_id_2 = 11;
		int group_id_1 = 50;
		int group_id_2 = 51;
		int dest_id_1 = 100;
		int dest_id_2 = 101;
		int online = 1;
		int offline = 0;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(this);
		if (rc) break;
init:
		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_1, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_1, "RadioId", dest_id_1, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_1, "RadioId", dest_id_1+2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending HandleChannelRegistration(11, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_2, "RadioId", dest_id_2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_2, "RadioId", dest_id_2+2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_2) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2+2) != AST_DEVICE_NOT_INUSE) break;

		if (res == AST_TEST_PASS) break;

		// RHYT: sending HandleChannelRegistration(10, offline)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_1, "Online", offline));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_UNAVAILABLE) break;

		// RHYT: sending HandleChannelRegistration(11, offline)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_2, "Online", offline));
		ast_json_ref(recv_msg);

		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2+2) != AST_DEVICE_UNAVAILABLE) break;

		res = AST_TEST_PASS;
		goto init;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_71)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_71";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Check subscribers statuses for outgoing call to group";
		
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int dest_id_1 = 100;
		int call_type = Group;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_INUSE) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		usleep(1e6);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_72)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_72";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Check subscribers statuses for incoming call to group";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int dest_id_1 = 100;
		int call_type = Group;
		const char *source = "100";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_INUSE) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// 3: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_73)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;
	
	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_73";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Check groups/subscribers statuses due continuation of group's outgoing call by incoming";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int station_id = 10;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int dest_id_1 = 100;
		int call_type = Group;
		const char *source = "100";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		// RHYT: sending HandleSubscriberRegistration(dest_id_1, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		// RHYT: sending HandleSubscriberRegistration(dest_id_1+2, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// 1: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 2. RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);
		if (this->state != CHAN_RADIOHYT_ST_RING) break;

		// 3: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_INUSE) break;

		// 4: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 5: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 6: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 7: RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		//if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_INUSE) break;

		// 8: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		//if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 9: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		//if (this->state != CHAN_RADIOHYT_ST_READY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_74)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_74";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Check subscribers statuses for outgoing call to group when abonent migrates to another channel";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelX", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int dest_id_1 = 100;
		int call_type = Group;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_INUSE) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		usleep(1e6);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT: RadioId=102 leave chan_id
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id_1+2, "Online", 0));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 500);

		// RHYT: RadioId=102 arrive on chan_id+1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id+1, "RadioId", dest_id_1+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 500);

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		usleep(1e6);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_75)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_75";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Mark subscribers & callgroups as unavailable for call if connection with server is broken";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id_1 = 10;
		int chan_id_2 = 11;
		int group_id_1 = 50;
		int group_id_2 = 51;
		int dest_id_1 = 100;
		int dest_id_2 = 101;
		int online = 1;
		int offline = 0;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_1, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_1, "RadioId", dest_id_1, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_1, "RadioId", dest_id_1+2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: sending HandleChannelRegistration(11, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id_2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_2, "RadioId", dest_id_2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id_2, "RadioId", dest_id_2+2, "Online", online));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", group_id_2) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT: connection is broken
		radiohyt_fake_on_break_event(this);

		usleep(1e6);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", group_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2+2) != AST_DEVICE_UNAVAILABLE) break;

		// RHYT: connection is established
		radiohyt_fake_on_connect_event(this);

		usleep(1e6);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_1+2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", group_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2) != AST_DEVICE_UNAVAILABLE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id_2+2) != AST_DEVICE_UNAVAILABLE) break;

		this->is_subscribers_loaded = 1;

		// Load station's list
		rc = radiohyt_get_stations(this, LOAD);
		if (rc != E_OK) break;

		// Recv answer
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: o}",
			"jsonrpc", "2.0",
			"id", ast_json_integer_get(ast_json_object_get(send_msg, "id")),
			"result", ast_json_pack("{s: o}",
				"Channels", ast_json_pack("[{s: i, s: i, s: b, s: b, s: i, s: s}]",
					"RadioId", chan_id_1, "Slot", 0,
					"IsAnalog", 0, "IsOnline", 1,
					"Id", chan_id_1, "Name", "station")));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// Recv answer
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: o}",
			"jsonrpc", "2.0",
			"id", ast_json_integer_get(ast_json_object_get(send_msg, "id")),
			"result", ast_json_pack("{s: o}",
				"CallGroups", ast_json_pack("[{s: i, s: i, s: s, s: [i]}]",
					"RadioId", group_id_1, "Id", group_id_1, "Name", "group",
					"ChannelIds", chan_id_1)));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		if (radiohyt_get_devicestate("HYT/%d", group_id_1) != AST_DEVICE_NOT_INUSE) break;

		// Recv answer
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: o}",
			"jsonrpc", "2.0",
			"id", ast_json_integer_get(ast_json_object_get(send_msg, "id")),
			"result", ast_json_pack("{s: o}",
				"Subscribers", ast_json_pack("[{s: i, s: i, s: [i], s: b, s: b, s: i, s: s}]",
					"RadioId", dest_id_1, "ChannelId", chan_id_1,
					"CallGroupIds", group_id_1,
					"IsEnabled", 1, "IsOnline", 1,
					"Id", dest_id_1, "Name", "subscriber")));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		if (radiohyt_get_devicestate("HYT/%d", dest_id_1) != AST_DEVICE_NOT_INUSE) break;

		this->is_subscribers_loaded = 0;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);
	
	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_76)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_76";
		info->category = "/channels/radiohyt/status/";
		info->summary = "Checks for radio statuses";
		info->description = "Check subscribers statuses for incoming call to radio";

		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int radio_id = 100;
		int dest_id = 101;
		int call_type = Group;
		const char *source = "100";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", radio_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", dest_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;

		// 3: RHYT sending End Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_77)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_77";
		info->category = "/channels/radiohyt/group/";
		info->summary = "Check on dynamic channel for group call";
		info->description = "Incoming group call and continuation by outgoing";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channel0", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int radio_id = 100;
		int call_type = Group;
		const char *source = "100";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", radio_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", radio_id+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_INUSE) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		// 3: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, dest_id);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 4: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 5: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_INUSE) break;

		// 6: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 7: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 8: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		usleep(1e6);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_78)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *this;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_78";
		info->category = "/channels/radiohyt/group/";
		info->summary = "Check on static channel for group call";
		info->description = "Incoming group call and continuation by outgoing";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	this = radiohyt_alloc_channel("channelS", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int radio_id = 100;
		int call_type = Group;
		const char *source = "100";

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(this);
		if (rc) break;

		// RHYT: sending HandleChannelRegistration(10, online)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleChannelRegistration",
			"params", ast_json_pack("{s: i, s: b}",
				"ChannelId", chan_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", radio_id, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleSubscriberRegistration",
			"params", ast_json_pack("{s: i, s: i, s: b}",
				"ChannelId", chan_id, "RadioId", radio_id+2, "Online", 1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(this, recv_msg, 100);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		// RHYT sending Incoming Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_INUSE) break;

		// 2: RHYT sending Hangtime Call Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		// 3: AICS sending StartVoiceCall
		rc = radiohyt_make_call(this, chan_id, slot_id, dest_id, call_type, dest_id);
		if (rc || this->state != CHAN_RADIOHYT_ST_DIAL) break;

		// 4: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 5: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 1000);
		if (this->state != CHAN_RADIOHYT_ST_BUSY) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_INUSE) break;

		// 6: AICS sending StopVoiceCall
		rc = radiohyt_drop_call(this, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || this->state != CHAN_RADIOHYT_ST_DETACH) break;

		// 7: RHYT sending Response
		send_msg = this->last_send_msg;
		this->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(this, recv_msg, 100);
		ast_json_unref(send_msg);

		// 8: RHYT sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		if (radiohyt_get_devicestate("HYT/%d", dest_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id) != AST_DEVICE_NOT_INUSE) break;
		if (radiohyt_get_devicestate("HYT/%d", radio_id+2) != AST_DEVICE_NOT_INUSE) break;

		usleep(1e6);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id, "InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		radiohyt_fake_recv_event(this, recv_msg, 100);
		if (this->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(this, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(this);
	if (this->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(this);

	ast_free(this);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_79)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_79";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Static channels, normal outgoing call to group";
		info->description = "This is a test for normal outgoing group call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channelS1", STATIC);
	chan2 = radiohyt_alloc_channel("channelS2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_80)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_80";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Static channels, normal incoming call from RadioId to Group";
		info->description = "Receive incoming call notification for each of two static channels";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channelS1", STATIC);
	chan2 = radiohyt_alloc_channel("channelS2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;
		int station_id = 10;
		const char *source = "100";//src_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;
		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Incoming Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// 3: RHYT sending Hangtime Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 4: RHYT sending Hangtime Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// 5: RHYT sending End Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		// 6: RHYT sending End Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_81)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_81";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Dynamic channels, normal outgoing call to group";
		info->description = "This is a test for normal outgoing group call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DETACH) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_82)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_82";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Dynamic channels, normal incoming call from RadioId to Group";
		info->description = "Receive incoming call notification for each of two static channels";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channel0", DYNAMIC);
	chan2 = radiohyt_alloc_channel("channel1", DYNAMIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;
		int station_id = 10;
		const char *source = "100";//src_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;
		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		// 2: RHYT sending Incoming Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 3: RHYT sending Hangtime Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		// 4: RHYT sending Hangtime Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 5: RHYT sending End Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", station_id));
		ast_json_ref(recv_msg);
		// 6: RHYT sending End Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_83)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_83";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Static channels, 2x normal outgoing call to group";
		info->description = "2x normal outgoing group call scenario";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channelS1", STATIC);
	chan2 = radiohyt_alloc_channel("channelS2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;

		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// AICS: sending StartVoiceCall
		rc = radiohyt_make_call(chan1, chan_id, slot_id, dest_id, call_type, 100);
		if (rc || chan1->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan1->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// AICS: sending StartVoiceCall(2)
		rc = radiohyt_make_call(chan2, chan_id+1, slot_id, dest_id+1, call_type, 100);
		if (rc || chan2->state != CHAN_RADIOHYT_ST_DIAL) break;

		// RHYT: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);
		if (chan2->state != CHAN_RADIOHYT_ST_RING) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 1, "CallType", call_type, "CallState", 1,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		// AICS: sending StopVoiceCall
		rc = radiohyt_drop_call(chan1, chan_id, slot_id, dest_id, call_type, 100);

		// RHYT: sending Response
		send_msg = chan1->last_send_msg;
		chan1->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		// AICS: sending StopVoiceCall(2)
		rc = radiohyt_drop_call(chan2, chan_id+1, slot_id, dest_id+1, call_type, 100);

		// RHYT: sending Response
		send_msg = chan2->last_send_msg;
		chan2->last_send_msg = NULL;
		recv_msg = ast_json_pack("{s: s, s: i, s: {s: i}}",
			"jsonrpc", "2.0", "id", ast_json_integer_get(ast_json_object_get(send_msg, "id")), "result", "Result", 0);
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		ast_json_unref(send_msg);

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 1, "CallType", call_type, "CallState", 2,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Notification
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// RHYT: sending Notification(2)
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 1, "CallType", call_type, "CallState", 3,
				"Source", "alvid", "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan1, chan_id+1, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan2, chan_id, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan2, chan_id+2, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

AST_TEST_DEFINE(radiohyt_test_case_84)
{
	int rc;
	int res = AST_TEST_FAIL;
	Channel *chan1;
	Channel *chan2;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_case_84";
		info->category = "/channels/radiohyt/group/";
		info->summary = "2x Static channels, 2x normal incoming call from RadioId to Group";
		info->description = "2x incoming call notification for each of two static channels";
		return AST_TEST_NOT_RUN;
	
	case TEST_EXECUTE:
		break;
	}

	chan1 = radiohyt_alloc_channel("channelS1", STATIC);
	chan2 = radiohyt_alloc_channel("channelS2", STATIC);

	do {
		int id = 0;
		int chan_id = 10;
		int slot_id = 1;
		int dest_id = 50;
		int call_type = Group;
		const char *source = "100";//src_id

		struct ast_json *send_msg;
		struct ast_json *recv_msg;

		RadiohytSession session;

		rc = radiohyt_chan_open(chan1);
		if (rc) break;
		rc = radiohyt_chan_open(chan2);
		if (rc) break;

		// 1: RHYT sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 2: RHYT sending Incoming Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		// 3: RHYT sending Incoming Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 2, "CallType", call_type, "CallState", 1,
				"Source", source, "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 4: RHYT sending Incoming Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 5: RHYT sending Hangtime Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 6: RHYT sending Hangtime Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_BUSY) break;

		// 7: RHYT sending Hangtime Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 2, "CallType", call_type, "CallState", 2,
				"Source", source, "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 1000);
		if (chan1->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 8: RHYT sending Hangtime Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 1000);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 9: RHYT sending End Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id,
				"InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", dest_id));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		// 10: RHYT sending End Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_HANGTIME) break;

		// 11: RHYT sending End Call Notification for chan1
		recv_msg = ast_json_pack("{s: s, s: i, s: s, s: o}",
			"jsonrpc", "2.0", "id", ++id, "method", "HandleCallState",
			"params", ast_json_pack("{s: i, s: i, s: i, s: i, s: s, s: i}",
				"ChannelId", chan_id+1,
				"InitiatorType", 2, "CallType", call_type, "CallState", 3,
				"Source", source, "ReceiverRadioId", dest_id+1));
		ast_json_ref(recv_msg);
		radiohyt_fake_recv_event(chan1, recv_msg, 100);
		if (chan1->state != CHAN_RADIOHYT_ST_READY) break;

		// 12: RHYT sending End Call Notification for chan2
		radiohyt_fake_recv_event(chan2, recv_msg, 100);
		if (chan2->state != CHAN_RADIOHYT_ST_READY) break;

		rc = radiohyt_session_get(chan1, chan_id, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan1, chan_id+1, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan2, chan_id, &session);
		if (rc == E_OK) break;
		rc = radiohyt_session_get(chan2, chan_id+1, &session);
		if (rc == E_OK) break;

		res = AST_TEST_PASS;
	} while(0);

	radiohyt_chan_close(chan1);
	if (chan1->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan1);

	ast_free(chan1);

	radiohyt_chan_close(chan2);
	if (chan2->state != CHAN_RADIOHYT_ST_IDLE)
		res = AST_TEST_FAIL;
	Channel_destructor(chan2);

	ast_free(chan2);

	if (res == AST_TEST_FAIL)
		ast_test_status_update(test, "something wrong\n");

	return res;
}

#endif // TEST_FRAMEWORK

static int load_module(void)
{
	int rc;

	radiohyt_channels = ao2_t_container_alloc(MAX_CHANNELS, chan_hash_cb, chan_cmp_cb, "allocate radiohyt channels table");
	radiohyt_servers = ao2_t_container_alloc(MAX_SERVERS, srv_hash_cb, srv_cmp_cb, "allocate radiohyt servers table");
	radiohyt_stations = ao2_t_container_alloc(MAX_STATIONS, stations_hash_cb, stations_cmp_cb, "allocate radiohyt stations table");
	radiohyt_callgroups = ao2_t_container_alloc(MAX_CALLGROUPS, callgroups_hash_cb, callgroups_cmp_cb, "allocate radiohyt callgroups table");
	radiohyt_subscribers = ao2_t_container_alloc(MAX_SUBSCRIBERS, subscribers_hash_cb, subscribers_cmp_cb, "allocate radiohyt subscribers table");
	if (!radiohyt_channels
	    || !radiohyt_servers
	    || !radiohyt_stations
	    || !radiohyt_callgroups
	    || !radiohyt_subscribers) {
		return AST_MODULE_LOAD_FAILURE;
	}

	//todo: not used
	if (!(radiohyt_tech.capabilities = ast_format_cap_alloc(0))) {
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_format_cap_add_all_by_type(radiohyt_tech.capabilities, AST_FORMAT_TYPE_AUDIO);
	//ast_format_cap_add(radiohyt_tech.capabilities, ast_format_set(&tmpfmt, AST_FORMAT_ULAW, 0));
	//ast_format_cap_add(radiohyt_tech.capabilities, ast_format_set(&tmpfmt, AST_FORMAT_ALAW, 0));

	if (ast_channel_register(&radiohyt_tech)) {
		unload_module();
		ast_log(LOG_ERROR, "RADIOHYT: Unable to register channel class\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_cli_register_multiple(cli_radiohyt, ARRAY_LEN(cli_radiohyt));

	rc = monitor_start();
	if (rc) {
		unload_module();
		ast_log(LOG_ERROR, "RADIOHYT: unable to start monitor thread\n");
		return AST_MODULE_LOAD_FAILURE;
	}
	radiohyt_session_storage_start();

	rc = load_config(CHANNEL_MODULE_LOAD);
	if (rc < 0) {
		unload_module();
		ast_log(LOG_ERROR, "RADIOHYT: unable to load configuration\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	apply_config();

	keepalive_task_id = ast_sched_add(monitor_sched_ctx(),
		KEEPALIVE_SEND_TIMEOUT, keepalive_task_cb, NULL);
	ast_debug(2, "RADIOHYT: keepalive task is started\n");

	garbage_collector_id = ast_sched_add(monitor_sched_ctx(),
		GARBAGE_COLLECT_TIMEOUT, garbage_collector_cb, NULL);
	ast_debug(2, "RADIOHYT: garbage collector task is started\n");

#ifdef TEST_FRAMEWORK
	AST_TEST_REGISTER(radiohyt_test_case_01);
	AST_TEST_REGISTER(radiohyt_test_case_02);
	AST_TEST_REGISTER(radiohyt_test_case_03);
	AST_TEST_REGISTER(radiohyt_test_case_04);
	AST_TEST_REGISTER(radiohyt_test_case_05);
	AST_TEST_REGISTER(radiohyt_test_case_06);
	AST_TEST_REGISTER(radiohyt_test_case_07);
	AST_TEST_REGISTER(radiohyt_test_case_08);
	AST_TEST_REGISTER(radiohyt_test_case_09);
	AST_TEST_REGISTER(radiohyt_test_case_10);
	AST_TEST_REGISTER(radiohyt_test_case_11);
	AST_TEST_REGISTER(radiohyt_test_case_12);
	AST_TEST_REGISTER(radiohyt_test_case_13);
	AST_TEST_REGISTER(radiohyt_test_case_14);
	AST_TEST_REGISTER(radiohyt_test_case_15);
	AST_TEST_REGISTER(radiohyt_test_case_19);
	AST_TEST_REGISTER(radiohyt_test_case_20);
	AST_TEST_REGISTER(radiohyt_test_case_21);
	AST_TEST_REGISTER(radiohyt_test_case_22);
	AST_TEST_REGISTER(radiohyt_test_case_23);
	AST_TEST_REGISTER(radiohyt_test_case_24);
	AST_TEST_REGISTER(radiohyt_test_case_25);
	AST_TEST_REGISTER(radiohyt_test_case_26);
	AST_TEST_REGISTER(radiohyt_test_case_27);
	AST_TEST_REGISTER(radiohyt_test_case_28);
	AST_TEST_REGISTER(radiohyt_test_case_29);
	AST_TEST_REGISTER(radiohyt_test_case_30);
	AST_TEST_REGISTER(radiohyt_test_case_31);
	AST_TEST_REGISTER(radiohyt_test_case_32);
	AST_TEST_REGISTER(radiohyt_test_case_33);
	AST_TEST_REGISTER(radiohyt_test_case_34);
	AST_TEST_REGISTER(radiohyt_test_case_35);
	AST_TEST_REGISTER(radiohyt_test_case_36);
	AST_TEST_REGISTER(radiohyt_test_case_37);
	AST_TEST_REGISTER(radiohyt_test_case_38);
	AST_TEST_REGISTER(radiohyt_test_case_39);
	AST_TEST_REGISTER(radiohyt_test_case_41);
	AST_TEST_REGISTER(radiohyt_test_case_42);
	AST_TEST_REGISTER(radiohyt_test_case_43);
	AST_TEST_REGISTER(radiohyt_test_case_44);
	AST_TEST_REGISTER(radiohyt_test_case_45);
	AST_TEST_REGISTER(radiohyt_test_case_46);
	AST_TEST_REGISTER(radiohyt_test_case_47);
	AST_TEST_REGISTER(radiohyt_test_case_48);
	AST_TEST_REGISTER(radiohyt_test_case_50);
	AST_TEST_REGISTER(radiohyt_test_case_51);
	AST_TEST_REGISTER(radiohyt_test_case_61);
	AST_TEST_REGISTER(radiohyt_test_case_62);
	AST_TEST_REGISTER(radiohyt_test_case_63);
	AST_TEST_REGISTER(radiohyt_test_case_64);
	AST_TEST_REGISTER(radiohyt_test_case_66);
	AST_TEST_REGISTER(radiohyt_test_case_67);
	AST_TEST_REGISTER(radiohyt_test_case_68);
	AST_TEST_REGISTER(radiohyt_test_case_70);
	AST_TEST_REGISTER(radiohyt_test_case_71);
	AST_TEST_REGISTER(radiohyt_test_case_72);
	AST_TEST_REGISTER(radiohyt_test_case_73);
	AST_TEST_REGISTER(radiohyt_test_case_74);
	AST_TEST_REGISTER(radiohyt_test_case_75);
	AST_TEST_REGISTER(radiohyt_test_case_76);
	AST_TEST_REGISTER(radiohyt_test_case_77);
	AST_TEST_REGISTER(radiohyt_test_case_78);
	AST_TEST_REGISTER(radiohyt_test_case_79);
	AST_TEST_REGISTER(radiohyt_test_case_80);
	AST_TEST_REGISTER(radiohyt_test_case_81);
	AST_TEST_REGISTER(radiohyt_test_case_82);
	AST_TEST_REGISTER(radiohyt_test_case_83);
	AST_TEST_REGISTER(radiohyt_test_case_84);
#endif

	ast_debug(1, "RADIOHYT: module v%s is successfully started\n", radiohyt_version);
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	Channel *chan;
	struct ao2_iterator i;

#ifdef TEST_FRAMEWORK
	AST_TEST_UNREGISTER(radiohyt_test_case_01);
	AST_TEST_UNREGISTER(radiohyt_test_case_02);
	AST_TEST_UNREGISTER(radiohyt_test_case_03);
	AST_TEST_UNREGISTER(radiohyt_test_case_04);
	AST_TEST_UNREGISTER(radiohyt_test_case_05);
	AST_TEST_UNREGISTER(radiohyt_test_case_06);
	AST_TEST_UNREGISTER(radiohyt_test_case_07);
	AST_TEST_UNREGISTER(radiohyt_test_case_08);
	AST_TEST_UNREGISTER(radiohyt_test_case_09);
	AST_TEST_UNREGISTER(radiohyt_test_case_10);
	AST_TEST_UNREGISTER(radiohyt_test_case_11);
	AST_TEST_UNREGISTER(radiohyt_test_case_12);
	AST_TEST_UNREGISTER(radiohyt_test_case_13);
	AST_TEST_UNREGISTER(radiohyt_test_case_14);
	AST_TEST_UNREGISTER(radiohyt_test_case_15);
	AST_TEST_UNREGISTER(radiohyt_test_case_19);
	AST_TEST_UNREGISTER(radiohyt_test_case_20);
	AST_TEST_UNREGISTER(radiohyt_test_case_21);
	AST_TEST_UNREGISTER(radiohyt_test_case_22);
	AST_TEST_UNREGISTER(radiohyt_test_case_23);
	AST_TEST_UNREGISTER(radiohyt_test_case_24);
	AST_TEST_UNREGISTER(radiohyt_test_case_25);
	AST_TEST_UNREGISTER(radiohyt_test_case_26);
	AST_TEST_UNREGISTER(radiohyt_test_case_27);
	AST_TEST_UNREGISTER(radiohyt_test_case_28);
	AST_TEST_UNREGISTER(radiohyt_test_case_29);
	AST_TEST_UNREGISTER(radiohyt_test_case_30);
	AST_TEST_UNREGISTER(radiohyt_test_case_31);
	AST_TEST_UNREGISTER(radiohyt_test_case_32);
	AST_TEST_UNREGISTER(radiohyt_test_case_33);
	AST_TEST_UNREGISTER(radiohyt_test_case_34);
	AST_TEST_UNREGISTER(radiohyt_test_case_35);
	AST_TEST_UNREGISTER(radiohyt_test_case_36);
	AST_TEST_UNREGISTER(radiohyt_test_case_37);
	AST_TEST_UNREGISTER(radiohyt_test_case_38);
	AST_TEST_UNREGISTER(radiohyt_test_case_39);
	AST_TEST_UNREGISTER(radiohyt_test_case_41);
	AST_TEST_UNREGISTER(radiohyt_test_case_42);
	AST_TEST_UNREGISTER(radiohyt_test_case_43);
	AST_TEST_UNREGISTER(radiohyt_test_case_44);
	AST_TEST_UNREGISTER(radiohyt_test_case_45);
	AST_TEST_UNREGISTER(radiohyt_test_case_46);
	AST_TEST_UNREGISTER(radiohyt_test_case_47);
	AST_TEST_UNREGISTER(radiohyt_test_case_48);
	AST_TEST_UNREGISTER(radiohyt_test_case_50);
	AST_TEST_UNREGISTER(radiohyt_test_case_51);
	AST_TEST_UNREGISTER(radiohyt_test_case_61);
	AST_TEST_UNREGISTER(radiohyt_test_case_62);
	AST_TEST_UNREGISTER(radiohyt_test_case_63);
	AST_TEST_UNREGISTER(radiohyt_test_case_64);
	AST_TEST_UNREGISTER(radiohyt_test_case_66);
	AST_TEST_UNREGISTER(radiohyt_test_case_67);
	AST_TEST_UNREGISTER(radiohyt_test_case_68);
	AST_TEST_UNREGISTER(radiohyt_test_case_70);
	AST_TEST_UNREGISTER(radiohyt_test_case_71);
	AST_TEST_UNREGISTER(radiohyt_test_case_72);
	AST_TEST_UNREGISTER(radiohyt_test_case_73);
	AST_TEST_UNREGISTER(radiohyt_test_case_74);
	AST_TEST_UNREGISTER(radiohyt_test_case_75);
	AST_TEST_UNREGISTER(radiohyt_test_case_76);
	AST_TEST_UNREGISTER(radiohyt_test_case_77);
	AST_TEST_UNREGISTER(radiohyt_test_case_78);
	AST_TEST_UNREGISTER(radiohyt_test_case_79);
	AST_TEST_UNREGISTER(radiohyt_test_case_80);
	AST_TEST_UNREGISTER(radiohyt_test_case_81);
	AST_TEST_UNREGISTER(radiohyt_test_case_82);
	AST_TEST_UNREGISTER(radiohyt_test_case_83);
	AST_TEST_UNREGISTER(radiohyt_test_case_84);
#endif

	// stop keepalive task
	if (keepalive_task_id != -1) {
		AST_SCHED_DEL(monitor_sched_ctx(), keepalive_task_id);
		keepalive_task_id = -1;
	}

	// stop garbage collector
	if (garbage_collector_id != -1) {
		AST_SCHED_DEL(monitor_sched_ctx(), garbage_collector_id);
		garbage_collector_id = -1;
	}

	// break active connections
	i = ao2_iterator_init(radiohyt_channels, 0);
	while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
		if (chan->state > CHAN_RADIOHYT_ST_READY) {
			ast_debug(1, "RADIOHYT[%s]: emergency break call %s\n", chan->name, chan->sid);
			if (chan->owner)
				ast_queue_hangup_with_cause(chan->owner, AST_CAUSE_NORMAL_TEMPORARY_FAILURE);
		}
		ao2_ref(chan, -1);
	}
	ao2_iterator_destroy(&i);

	usleep(2000000UL);

	ast_mutex_lock(&radiohyt_mutex);

	ao2_t_callback(radiohyt_servers, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
	    srv_cleanup_cb, NULL, "initiating callback to clean servers table");

	ao2_t_callback(radiohyt_stations, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
	    stations_cleanup_cb, NULL, "initiating callback to clean stations table");

	ao2_t_callback(radiohyt_callgroups, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
	    callgroups_cleanup_cb, NULL, "initiating callback to clean callgroups table");

	ao2_t_callback(radiohyt_subscribers, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
	    subscribers_cleanup_cb, NULL, "initiating callback to clean subscribers table");

	// finally destroying all channels
	{
		int arg = CHANNELS_ALL;

		ao2_t_callback(radiohyt_channels, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
		    chan_cleanup_cb, &arg, "initiating callback to clean channels table");

		i = ao2_iterator_init(radiohyt_channels, 0);
		while ((chan = ao2_t_iterator_next(&i, "iterate thru channels table"))) {
			int refc = ao2_ref(chan, -1);
			ast_debug(2, "rest channel: name: %s, state: %d, refc = %d\n", chan->name, chan->state, refc);
		}
		ao2_iterator_destroy(&i);
	}

	ENV_ASSERT(ao2_container_count(radiohyt_channels) == 0);
	ENV_ASSERT(ao2_container_count(radiohyt_stations) == 0);
	ENV_ASSERT(ao2_container_count(radiohyt_servers) == 0);
	ENV_ASSERT(ao2_container_count(radiohyt_callgroups) == 0);
	ENV_ASSERT(ao2_container_count(radiohyt_subscribers) == 0);

	ast_mutex_unlock(&radiohyt_mutex);

	monitor_stop();

	radiohyt_session_storage_stop();

	ast_cli_unregister_multiple(cli_radiohyt, ARRAY_LEN(cli_radiohyt));

	ast_channel_unregister(&radiohyt_tech);

	radiohyt_tech.capabilities = ast_format_cap_destroy(radiohyt_tech.capabilities);

	return 0;
}

#ifndef AST_MODULE_RELOAD_SUCCESS
# define AST_MODULE_RELOAD_SUCCESS (0)
#endif
#ifndef AST_MODULE_RELOAD_ERROR
# define AST_MODULE_RELOAD_ERROR (1)
#endif

static int reload_module(void)
{
	if (unload_module())
		return AST_MODULE_RELOAD_ERROR;
	if (load_module())
		return AST_MODULE_RELOAD_ERROR;
	return AST_MODULE_RELOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Hytera DMR Channel Driver (HYT)",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
	.nonoptreq = "res_rtp_asterisk",
);
