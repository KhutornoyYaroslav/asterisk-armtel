#ifndef IACS_RADIOHYT_MSG_PARSER_H
#define IACS_RADIOHYT_MSG_PARSER_H

#include "asterisk/json.h"
#include "asterisk/linkedlists.h"

#define HYTERUS_MSG_MAX_SIZE 65536
#define HYTERUS_NAME_MAX_LEN 63 // remember about UTF8
#define HYTERUS_IDS_MAX_LEN 255

/* Number of callgroups per one subscriber */
#define HYTERUS_MAX_SUBSCRIBER_GROUPS 16
/* Number of channels(older - stations) per one callgroup */
#define HYTERUS_MAX_GROUP_STATIONS 16

/*
 * Hyterus DMR API Protocol Data Types
 */
enum hyterus_message_type
{
	/* Request from Hyterus to AICS */
	HYTERUS_REQUEST_AUTHENTICATE,

	/* Requests from AICS to Hyterus */
	HYTERUS_REQUEST_GET_CHANNELS,
	HYTERUS_REQUEST_GET_GROUPS,
	HYTERUS_REQUEST_GET_SUBSCRIBERS,
	HYTERUS_REQUEST_START_VOICE_CALL,
	HYTERUS_REQUEST_STOP_VOICE_CALL,
	HYTERUS_REQUEST_START_GROUP_CALL,
	HYTERUS_REQUEST_STOP_GROUP_CALL,
	HYTERUS_REQUEST_START_REMOTE_MONITOR,
	HYTERUS_REQUEST_CHECK_RADIO,
	HYTERUS_REQUEST_PING,

	/* Notifications from Hyterus to AICS */
	HYTERUS_NOTIFY_AUTH_STATUS,
	HYTERUS_NOTIFY_CHANNEL_STATUS,
	HYTERUS_NOTIFY_SUBSCRIBER_STATUS,
	HYTERUS_NOTIFY_CALL_STATE,

	HYTERUS_UNKNOWN_MESSAGE
};

extern const char *hyterus_msg_type_to_str(int type);

enum hyterus_auth_result_type
{
	SuccessAuth = 0,
	InvalidCredentials = 1,
	InvalidRtpPort = 2,
};

extern const char *hyterus_auth_result_type_to_str(int type);

enum hyterus_initiator_type
{
	Dispatcher = 1,
	Subscriber = 2,
};

enum hyterus_call_type
{
	Private = 0,
	Group = 1
};

enum hyterus_call_state
{
	CallInitiated = 1,
	CallInHangtime = 2,
	CallEnded = 3,
	NoAck = 4,
	CallInterrupted = 5
};

const char *radio_call_state_to_string(int type);

enum hyterus_radio_station_type
{
	Portable = 0,	// portable radiostation
	Mobile   = 1,	// car radiostation
};

const char *radio_station_type_to_string(int type);

enum hyterus_voice_call_command_result
{
	Success = 0,
	Failure = 1,
	InvalidParameter = 2,
	RadioBusy = 3,
	InvalidTargetAddress = 4,
};

/*
 * Radio
 */

/*! Radio subscriber */
typedef struct radiohyt_subscriber
{
	char ServerIP[16];

	int  Id;
	char Name[HYTERUS_NAME_MAX_LEN+1];
	int  RadioId;
	int  ChannelId;
	int  CallGroupId[HYTERUS_MAX_SUBSCRIBER_GROUPS];
	char CallGroupIdStr[HYTERUS_IDS_MAX_LEN+1];
	int  HasGpsSupport;
	int  HasTextMsgSupport;
	int  HasRemoteMonitorSupport;
	int  IsEnabled;
	int  IsOnline;
	int  RadioStationType;

	int  devstate;		// cache of ast_devstate

	AST_LIST_ENTRY(radiohyt_subscriber) entry;

}	RadioSubscriber;

/*! Radio call group */
typedef struct radiohyt_callgroup
{
	char ServerIP[16];

	int  Id;
	char Name[HYTERUS_NAME_MAX_LEN+1];
	int  RadioId;
	int  ChannelId[HYTERUS_MAX_GROUP_STATIONS];
	char ChannelIdStr[HYTERUS_IDS_MAX_LEN+1];

	// fixme: sip identifier linked with callgroup
	int  SipId;
	// fixme: control flag for rtp filtering function
	int  RtpFilter;
	// P.S. used temporary until better decision will happen

	int  devstate;		// cache of ast_devstate

	AST_LIST_ENTRY(radiohyt_callgroup) entry;

}	RadioCallGroup;

/*! Radio control station / channel */
typedef struct radiohyt_station
{
	char ServerIP[16];

	int  Id;
	int  Slot;
	char Name[HYTERUS_NAME_MAX_LEN+1];
	int  RadioId;
	int  IsAnalog;
	int  IsOnline;

	// fixme: sip identifier linked with station
	int  SipId;
	// fixme: control flag for rtp filtering function
	int  RtpFilter;
	// P.S. used temporary until better decision will happen

	AST_LIST_ENTRY(radiohyt_station) entry;

}	RadioControlStation;


/*! List of radio subscribers */
typedef AST_LIST_HEAD_NOLOCK(radiohyt_subscriber_list, radiohyt_subscriber) RadioSubscriberList;

/*! List of radio callgroups */
typedef AST_LIST_HEAD_NOLOCK(radiohyt_callgroup_list, radiohyt_callgroup) RadioGroupList;

/*! List of radio stations */
typedef AST_LIST_HEAD_NOLOCK(radiohyt_station_list, radiohyt_station) RadioControlStationList;


/*
 * Hyterus DMR API Protocol Message Parser
 */
typedef struct msg_parser_s {
	struct ast_str  *buf;
	struct ast_json *msg;
	struct ast_json_error err;

	int bracket;
	int is_msg_detected;

}	Msg_parser;

/*
 * Hyterus DMR API Protocol Message Parser Interface
 */
int init_parser(Msg_parser *p, unsigned len);
int eat_buffer(Msg_parser *p, const char *data, int len, int *eat_len);
struct ast_json *decoded_msg(Msg_parser *p);

#endif

