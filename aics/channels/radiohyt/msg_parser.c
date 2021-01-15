#include "asterisk.h"
#include "asterisk/logger.h"
#include "asterisk/strings.h"

#include "env_debug.h"
#include "env_error.h"
#include "msg_parser.h"

//#define PARSER_DEBUG

int init_parser(Msg_parser *this, unsigned init_len)
{
	this->bracket = 0;
	this->is_msg_detected = 0;
	this->msg = NULL;
	this->buf = ast_str_create(init_len);
	if (this->buf == NULL)
		return E_NO_MEM;
	return E_OK;
}

static int parser_eat_buffer(Msg_parser *this, const char *data, int len)
{
#ifdef PARSER_DEBUG
	ast_log(LOG_ERROR, "RADIOHYT: append str to JSON buffer: %s, len: %d bytes\n", data, len);
#endif
	ast_str_append_substr(&this->buf, ast_str_size((const struct ast_str *)&this->buf), data, len);

#ifdef PARSER_DEBUG
	ast_log(LOG_ERROR, "RADIOHYT: load JSON buffer: %s, len: %d bytes\n",
		ast_str_buffer((const struct ast_str *)this->buf),
		ast_str_strlen((const struct ast_str *)this->buf));
#endif
	this->msg = ast_json_load_buf(
		ast_str_buffer((const struct ast_str *)this->buf),
		ast_str_strlen((const struct ast_str *)this->buf),
		&this->err);

	ast_str_reset(this->buf);

	if (!this->msg) {
		ast_log(LOG_ERROR, "error when parsing JSON: %s\n", this->err.text);
		return E_FAIL;
	}
	return E_OK;
}

int eat_buffer(Msg_parser *this, const char *data, int len, int *eat_len)
{
	const char *p = data;
	unsigned i = 0;
#ifdef PARSER_DEBUG
	ast_log(LOG_ERROR, "RADIOHYT: parsing text: %s, len: %d bytes\n", data, len);
#endif
	for (i = 0; i < len; ++i) {
		switch (*p++) {
		case '{':
			if (++this->bracket == 1)
				this->is_msg_detected = 1;
			break;
		case '}':
			if (this->is_msg_detected && --this->bracket == 0) {
				parser_eat_buffer(this, data, i+1);
				*eat_len = i+1;
				this->is_msg_detected = 0;
				return E_OK;
			}
			break;
		}
	}

	ast_str_append_substr(&this->buf, ast_str_size((const struct ast_str *)&this->buf), data, len);
	*eat_len = len;
	return E_IN_PROGRESS;
}

struct ast_json *decoded_msg(Msg_parser *this)
{
    struct ast_json *msg = this->msg;
    this->msg = NULL;
    return msg;
}

/*
 *
 */
static const char *hyterus_auth_result_str[] =
{
	"success",
	"invalid credentials",
	"invalid rtp port",
};

const char *hyterus_auth_result_type_to_str(int type)
{
	if (type <= InvalidRtpPort) {
		return hyterus_auth_result_str[type];
	}
	ENV_ASSERT(0);
	return "unknown error";
}

static const char *hyterus_call_state_str[] =
{
	"unknown",
	"initiated",
	"in_hangtime",
	"ended",
	"no_answer",
	"interrupted",
};

const char *radio_call_state_to_string(int type)
{
	if (type <= CallInterrupted) {
		return hyterus_call_state_str[type];
	}
	ENV_ASSERT(0);
	return "unknown";
}

static const char *hyterus_station_type_str[] = 
{
	"portable",
	"mobile",
};

const char *radio_station_type_to_string(int type)
{
	if (type <= Mobile) {
		return hyterus_station_type_str[type];
	}
	return "unknown";
}

static const char *hyterus_msg_str[] = {
	"Authenticate",

	"GetChannels",
	"GetCallGroups",
	"GetCallGroupSubscribers",
	"StartVoiceCall",
	"StopVoiceCall",
	"StartMultiGroupCall",
	"StopVoiceCall",
	"StartRemoteMonitor",
	"CheckRadio",
	"Ping",

	"HandleAuthenticationResult",
	"HandleChannelRegistration",
	"HandleSubscriberRegistration",
	"HandleCallState",

	"UnknownMessage",
};

ENV_COMPILE_ASSERT(sizeof(hyterus_msg_str)/sizeof(*hyterus_msg_str) == HYTERUS_UNKNOWN_MESSAGE+1, hm_str_err);

const char *hyterus_msg_type_to_str(int type)
{
	if (type < HYTERUS_UNKNOWN_MESSAGE)
		return hyterus_msg_str[type];
	ENV_ASSERT(0);
	return "UnknownMessage";
}

