/*
 * aics.c
 */
#include <math.h>

#include "asterisk.h"
#include "aics.h"

//ASTERISK_FILE_VERSION(__FILE__, "$Revision: 413588 $")
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/stasis_channels.h"

// prototypes
//void aics_list_channel_snapshot(void);
//void aics_list_chanvars(const char *channame);
//char *aics_itoa(const int n);


static char aics_version[] = "XX.XX.XX";


static const struct aics_command_armtel {
    enum aics_cmd_armtel code;
    char * const text;
} aics_cmd_armtel_table[] = {
  { AICS_CMD_NONE,"NONE"},
  { AICS_ARMTEL_ASK_WORD,"ASK_WORD"},
  { AICS_ARMTEL_REMOVE_WORD,"REMOVE_WORD"},
  { AICS_ARMTEL_INTERRUPT_WORD,"NTERRUPT_WORD"},
  { AICS_ARMTEL_DIRECTION,"DIRECTION"},
  { AICS_ARMTEL_DISKRET3,"DISKRET3"},
  { AICS_ARMTEL_CHANGE_STATE_CONF,"CHANGE_STATE_CONF"},
};

const char* aics_get_name_cmd(unsigned char code)
{

    if (code >= 0 && code < ARRAY_LEN(aics_cmd_armtel_table)) {
        return aics_cmd_armtel_table[code].text;
    }
    return aics_cmd_armtel_table[0].text;
}


void aics_num_from_name(char*num,const char* name)
{
char*p1,*p2;
   if(name){
      *num=0;
      if(p1=strrchr(name,'/')){
        if(p2=strchr(p1,'-' )){
          strncpy(num,p1+1,p2-p1-1);
          num[p2-p1-1]='\0';
        }
        else{
          strcpy(num,p1+1);
        }
      }
   }
}


void aics_send_armtel_sign(struct ast_channel* ast,unsigned char cmd,unsigned char prio,unsigned char context)
{
struct aics_proxy_params *lp;
  if(ast){
	lp=ast_channel_proxy(ast);
    if(lp){
	  if((lp->scenario == AICS_SCENARIO_SELECTOR)||(lp->scenario == AICS_SCENARIO_CIRCULAR)){
	      ast_channel_lock(ast);
          aics_num_from_name(lp->sig_armtel.number,ast_channel_name(ast));
		  lp->sig_armtel.cmd=cmd;
		  lp->sig_armtel.context=context;
		  lp->sig_armtel.prio=prio;
		  ast_channel_unlock(ast);
	  }
    }
  }
}

//char *aics_itoa(const int n)
//{
//	char * outbuf;
//	ast_asprintf(&outbuf, "%d", n);
//	return outbuf;
//}

char *aics_get_version(void)
{
	char *outbuf = aics_version;
	if (aics_version[0] == 'X')
	{
		int build = (int)AICS_VER;
		int major = build / 10000;
		build = build % 10000;
		int minor = build / 100;
		build = build % 100;
		sprintf(outbuf, "%02d.%02d.%02d", major, minor, build);
	}
	return outbuf;
}

// aics_priority stuff

static const struct aics_prioritys {
    char * const text;
    int value;
} aics_priority_table[] = {
    { "non-urgent",     0 },
    { "normal",         1 },
    { "urgent",         2 },
    { "emergency",      256 },
};

int aics_priority_from_str(const char *text)
{
	char *rest = NULL;
    int value = aics_priority_table[0].value;
    int i = ARRAY_LEN(aics_priority_table);
    //ast_log(LOG_NOTICE, "priority str [%s]\n", text);
    if (!ast_strlen_zero(text)) {
        while (--i >= 0) {
            if (!strcasecmp(text, aics_priority_table[i].text)) {
                return aics_priority_table[i].value; // spaghetti code :)
            }
        }
		int tmp = (int)strtol(text, &rest, 10);
		if (*rest == '\0') {
			tmp = (tmp > 255 ? 255 : tmp);
			value = (tmp > value ? tmp : value);
		} else
			value = -1;
    } else
		value = -1;
   //ast_log(LOG_NOTICE, "priority value %d\n", value);
    return value;
}

const char *aics_priority_to_str(const int code)
{
    static char unsafe_buf[4];
    int tmp = (code > 255 ? 255 : code);
    tmp = (tmp < 0 ? 0 : tmp);
    sprintf(unsafe_buf, "%d", tmp);
    return unsafe_buf;
}

// enum aics_direction stuff

static const struct aics_directions {
    enum aics_direction code;
    char * const text;
} aics_direction_table[] = {
    { AICS_DIRECTION_DUPLEX,        "duplex" },
    { AICS_DIRECTION_SEND,          "send" },
    { AICS_DIRECTION_RECV,          "recv" },
    { AICS_DIRECTION_MUTE,          "mute" },
};

enum aics_direction aics_direction_from_str(const char *text)
{
    enum aics_direction code = aics_direction_table[0].code;
    int i = ARRAY_LEN(aics_direction_table);
    if (!ast_strlen_zero(text)) {
    	//ast_log(LOG_NOTICE, "Direction fromstr %s\n", text);
        while (--i >= 0) {
            if (!strcasecmp(text, aics_direction_table[i].text)) {
                code = aics_direction_table[i].code;
                break;
            }
        }
    }
    return code;
}

const char *aics_direction_to_str(const int code)
{
    if (code >= 0 && code < ARRAY_LEN(aics_direction_table)) {
        return aics_direction_table[code].text;
    }
    return aics_direction_table[0].text;
}

// enum aics_scenario stuff

static const struct aics_scenarios {
    enum aics_scenario code;
    char * const text;
} aics_scenario_table[] = {
    { AICS_SCENARIO_NONE,         	"none" },
    { AICS_SCENARIO_GROUP,          "group" },
    { AICS_SCENARIO_CONFERENCE,     "conference" },
    { AICS_SCENARIO_SELECTOR,       "selector" },
    { AICS_SCENARIO_CIRCULAR,       "circular" },
};

enum aics_scenario aics_scenario_from_str(const char *text)
{
    enum aics_scenario code = aics_scenario_table[0].code;
    int i = ARRAY_LEN(aics_scenario_table);
    if (!ast_strlen_zero(text)) {
    	//ast_log(LOG_NOTICE, "Scenario fromstr %s\n", text);
        while (--i >= 0) {
            if (!strcasecmp(text, aics_scenario_table[i].text)) {
                code = aics_scenario_table[i].code;
                break;
            }
        }
    }
    return code;
}

const char *aics_scenario_to_str(const int code)
{
    if (code >= 0 && code < ARRAY_LEN(aics_scenario_table)) {
        return aics_scenario_table[code].text;
    }
    return aics_scenario_table[0].text;
}

// enum aics_priority_handle stuff

static const struct aics_priority_handles {
    enum aics_priority_handle code;
    char * const text;
} aics_priority_handle_table[] = {
    { AICS_PRIORITY_HANDLE_NONE,            "none" },
    { AICS_PRIORITY_HANDLE_HANGUP,          "hangup" },
    { AICS_PRIORITY_HANDLE_HOLD,            "hold" },
    { AICS_PRIORITY_HANDLE_HANGUP_NOTIFY,   "hangup-notify" },
    { AICS_PRIORITY_HANDLE_QUEUE,           "queue" },
};

enum aics_priority_handle aics_priority_handle_from_str(const char *text)
{
    enum aics_priority_handle code = aics_priority_handle_table[0].code;
    int i = ARRAY_LEN(aics_priority_handle_table);
    if (!ast_strlen_zero(text)) {
        while (--i >= 0) {
            if (!strcasecmp(text, aics_priority_handle_table[i].text)) {
                code = aics_priority_handle_table[i].code;
                break;
            }
        }
    }
    return code;
}

const char *aics_priority_handle_to_str(const int code)
{
    if (code >= 0 && code < ARRAY_LEN(aics_priority_handle_table)) {
        return aics_priority_handle_table[code].text;
    }
    return aics_priority_handle_table[0].text;
}

// enum aics_event_status stuff

static const struct aics_event_statuses {
    enum aics_event_status code;
    char * const text;
} aics_event_status_table[] = {
    { AICS_EVENT_OFF,           "off" },
    { AICS_EVENT_ON,          	"on" },
    { AICS_EVENT_DROP,          "drop" },
};

enum aics_event_status aics_event_status_from_str(const char *text)
{
    enum aics_event_status code = aics_event_status_table[0].code;
    int i = ARRAY_LEN(aics_event_status_table);
    if (!ast_strlen_zero(text)) {
        while (--i >= 0) {
            if (!strcasecmp(text, aics_event_status_table[i].text)) {
                code = aics_event_status_table[i].code;
                break;
            }
        }
    }
    return code;
}

const char *aics_event_status_to_str(const int code)
{
    if (code >= 0 && code < ARRAY_LEN(aics_event_status_table)) {
        return aics_event_status_table[code].text;
    }
    return aics_event_status_table[0].text;
}


// struct aics_proxy_params stuff

void aics_proxy_params_init(struct aics_proxy_params *params)
{
	if (!params)
		return;
	params->validity.as_int = 0;
	params->priority = aics_priority_from_str(DEFAULT_CALLPRIORITY);
	params->direction = AICS_DIRECTION_DUPLEX;
	params->scenario = AICS_SCENARIO_NONE;
	memset(&params->relay, 0, sizeof(params->relay));
	memset(&params->event, 0, sizeof(params->event));
	AST_LIST_HEAD_INIT_NOLOCK(&params->playlist.playlist);
	params->sig_armtel.number[0]=0;
	params->sig_armtel.cmd=AICS_CMD_NONE;
	params->sig_armtel.context=0;
	params->sig_armtel.prio=0;
	params->sig_armtel.param=0;
	params->sig_armtel.send=0;
	params->is_member=0;
	params->state_member=AICS_STATE_IDLE;
	params->prio_member=-1;
//    params->armtel_dial=0;
	AST_LIST_HEAD_INIT_NOLOCK(&params->armtel_dial.armtel_dial);

}

void aics_proxy_params_copy(struct aics_proxy_params *dst, struct aics_proxy_params *src)
{
	if (!dst || !src || dst == src)
		return;
	//ast_log(LOG_NOTICE, "ProxyParamsCopy src p%d d%d s%d v%d\n", src->priority, src->direction, src->scenario, src->validity.as_int);
	//ast_log(LOG_NOTICE, "ProxyParamsCopy dst p%d d%d s%d v%d\n", dst->priority, dst->direction, dst->scenario, dst->validity.as_int);
	aics_proxy_params_free(dst);
	dst->validity.as_int = src->validity.as_int;
	dst->priority = src->priority;
	dst->direction = src->direction;
	dst->scenario = src->scenario;
	int l = sizeof(src->relay)/sizeof(src->relay[0]);
	while (--l >= 0) {
		//ast_log(LOG_NOTICE, "ProxyParamsCopy src i%d s%d t%d t%d\n", l, src->relay[l].status, src->relay[l].tm_on, src->relay[l].tm_off);
		//ast_log(LOG_NOTICE, "ProxyParamsCopy dst i%d s%d t%d t%d\n", l, dst->relay[l].status, dst->relay[l].tm_on, dst->relay[l].tm_off);
		dst->relay[l].status = src->relay[l].status;
		dst->relay[l].tm_on = src->relay[l].tm_on;
		dst->relay[l].tm_off = src->relay[l].tm_off;
	}
	l = sizeof(src->event)/sizeof(src->event[0]);
	while (--l >= 0) {
		dst->event[l].status = src->event[l].status;
	}
	if (!AST_LIST_EMPTY(&src->playlist.playlist)) {
		struct aics_playlist_item *element;
		AST_LIST_TRAVERSE(&src->playlist.playlist, element, next) {
			//ast_log(LOG_NOTICE, "element %c %s %d %d \n", element->function, element->filename, element->repeat, element->wait);
			AST_LIST_INSERT_TAIL(&dst->playlist.playlist, element, next);
		}
	}

}

void aics_proxy_params_free(struct aics_proxy_params *params)
{
	if (!params)
		return;
	params->validity.as_int = 0;
	params->priority = aics_priority_from_str(DEFAULT_CALLPRIORITY);
	params->direction = AICS_DIRECTION_DUPLEX;
	params->scenario = AICS_SCENARIO_NONE;
	memset(&params->relay, 0, sizeof(params->relay));
	memset(&params->event, 0, sizeof(params->event));
	if (!AST_LIST_EMPTY(&params->playlist.playlist)) {
		struct aics_playlist_item *element;
		while ( (element = AST_LIST_REMOVE_HEAD(&params->playlist.playlist, next)) ) {
			//ast_free(element);
		}
	}
	params->sig_armtel.number[0]=0;
    params->sig_armtel.cmd=AICS_CMD_NONE;
	params->sig_armtel.context=0;
	params->sig_armtel.prio=0;
	params->sig_armtel.param=0;
	params->sig_armtel.send=0;
	params->is_member=0;
	params->state_member=AICS_STATE_IDLE;
	params->prio_member=-1;
//    params->armtel_dial=0;
	if (!AST_LIST_EMPTY(&params->armtel_dial.armtel_dial)) {
		struct aics_dial_item *element;
		while ( (element = AST_LIST_REMOVE_HEAD(&params->armtel_dial.armtel_dial, next)) ) {
		}
	}

}

// struct aics_local_params stuff

void aics_local_params_init(struct aics_local_params *params)
{
	if (!params)
		return;

	params->priority_handle = aics_priority_handle_from_str(DEFAULT_PRIORITYHANDLE);
	params->spy = 0;
	params->pendingupdate = 0;
	params->hangup_anounce = ast_strdup(DEFAULT_HANGUPANOUNCE);
}

void aics_local_params_copy(struct aics_local_params *dst, struct aics_local_params *src)
{
	if (!dst || !src || dst == src)
		return;
	//ast_log(LOG_NOTICE, "LocalParamsCopy\n");
	aics_local_params_free(dst);
	dst->priority_handle = src->priority_handle;
	dst->spy = src->spy;
	dst->pendingupdate = src->pendingupdate;
	dst->hangup_anounce = ast_strdup(src->hangup_anounce);
}

void aics_local_params_free(struct aics_local_params *params)
{
	if (!params)
		return;

	if (params->hangup_anounce) {
		ast_free(params->hangup_anounce);
		params->hangup_anounce = NULL;
	}

}

int aics_playlist_from_str(struct aics_playlist *head, const char *text)
{
	struct aics_playlist_item *element;
	char *s_cur = NULL, *s_rst = NULL;
	char *token = NULL, *rest = NULL;
	int res = 1;

	if (!head || ast_strlen_zero(text))
		return res;
	//ast_log(LOG_NOTICE, "Playlist fromstr %s\n", text);
	if (!AST_LIST_EMPTY(&head->playlist)) {
		while ( (element = AST_LIST_REMOVE_HEAD(&head->playlist, next)) ) {
			ast_free(element);
		}
	}

	s_rst = ast_strdupa(text);
	while ((s_cur = strsep(&s_rst, "&")) ) {
		//ast_log(LOG_NOTICE, "cur %s rst %s\n", s_cur, s_rst);

		element = NULL;
		if (!(element = ast_calloc(1, sizeof(*element))))
			break;

		element->function = '?';
		element->filename[0] = '\0';
		element->repeat = 1;

//		if (sscanf(s_cur, "%c\"%s[^:]:R%30u:W%30u\"", &c, &element->filename[0], &element->repeat, &element->wait) == 4) {
//			if ((c == 'A') || (c == 'T')) {
//				element->function = c;
//			}
//		}
//		if  ((element->function == '?') && (sscanf(s_cur, "%c\"%s[^:]:R%30u\"", &c, &element->filename[0], &element->repeat) == 3)) {
//			if ((c == 'A') || (c == 'T')) {
//				element->function = c;
//			}
//		}
//		if  ((element->function == '?') && (sscanf(s_cur, "%c\"%s[^:]:W%30u\"", &c, &element->filename[0], &element->wait) == 3)) {
//			if ((c == 'A') || (c == 'T')) {
//				element->function = c;
//			}
//		}
		token = NULL;
		rest = s_cur;
		while ((token = strtok_r(rest, "\":", &rest))) {
			//ast_log(LOG_NOTICE, "token %s rest %s\n", token, rest);
			if (ast_strlen_zero(token))
				break;
			if  (element->function == '?') {
				if ((*token != 'A') && (*token != 'T'))
					break;
				element->function = *token;
			} else if (ast_strlen_zero(element->filename)) {
				strncat(element->filename, token, 79);
			} else {
				if (sscanf(token, "W%30u", &element->wait) != 1) {

				}
				if (sscanf(token, "R%30u", &element->repeat) != 1) {

				}

			}
		}
		if  ((element->function == '?') && (sscanf(s_cur, "W%30u", &element->wait) == 1)) {
			element->function = 'W';
		}
//		if  ((element->function == '?') && (sscanf(s_cur, "%c\"%s[^\"]\"", &c, &element->filename[0]) == 2)) {
//			if ((c == 'A') || (c == 'T')) {
//				element->function = c;
//			}
//		}

		if (element->function == '?') {
			ast_free(element);
			element = NULL;
		}

		if (element) {
			//ast_log(LOG_NOTICE, "element %c %s %d %d \n", element->function, element->filename, element->repeat, element->wait);
			AST_LIST_INSERT_TAIL(&head->playlist, element, next);
			res = 0;
		}
	} //while
	return res;
}

const char *aics_playlist_to_str(struct aics_playlist *head)
{
	static char unsafe_buf255[255];
	int shift = 0;
	char * delim = "";

	unsafe_buf255[0] = '\0';
	if (head) {
		if (!AST_LIST_EMPTY(&head->playlist)) {
			struct aics_playlist_item *element;
			AST_LIST_TRAVERSE(&head->playlist, element, next) {
				//ast_log(LOG_NOTICE, "element %c %s %d %d \n", element->function, element->filename, element->repeat, element->wait);
				if (shift)
					delim = "&";
				if (element->function == 'W') {
					shift += snprintf(&unsafe_buf255[shift], sizeof(unsafe_buf255) - shift, "%s%c%d", delim, element->function, element->wait);
				} else {
					shift += snprintf(&unsafe_buf255[shift], sizeof(unsafe_buf255) - shift, "%s%c\"%s:R%d:W%d\"", delim, element->function, element->filename, element->repeat, element->wait);
				}
			}
		}
	}
	//ast_log(LOG_NOTICE, "Playlist tostr %s\n", unsafe_buf255);
	return unsafe_buf255;
}

int aics_relay_from_str(struct aics_relay *array, const int size, const char *text)
{
	int i = 0, tm1 = 0, tm2 = 0, scan = 0, k = 0;
	char *cpy, *ptr;
	struct aics_relay *head = array;
	int res = 1;

	if (!array || ast_strlen_zero(text))
		return res;
	//ast_log(LOG_NOTICE, "Relay fromstr %s\n", text);
	cpy = ast_strdupa(text);
	memset(&array, 0, size);
	ptr = cpy;
	while ((ptr) && (k < size)) {
		k++;
		scan = sscanf(ptr, "%d:%d:%d", &i, &tm1, &tm2);
		//ast_log(LOG_NOTICE, "[%s] %d %d %d %d %d\n", ptr, scan, i, tm1, tm2, res);
		if (scan == 3) {//if (scan > 0) {
			head[k-1].status = i;
			head[k-1].tm_on = tm1;
			head[k-1].tm_off = tm2;
			res = 0;
//			if (i <= size) {
//				head[i-1].status = 1;
//				if (scan == 3) {
//					head[i-1].tm_on = tm1;
//					head[i-1].tm_off = tm2;
//				}
//			}
		} else {
			res = 2;
			break;
		}
		cpy = ptr;
		ptr = strchr(cpy, '/');
		if (ptr)
			*ptr++ = '\0';
	};
	//ast_free(cpy);
	//ast_log(LOG_NOTICE, "%d\n",  res);
	return res;
}

const char *aics_relay_to_str(struct aics_relay *array, const int size)
{
	static char unsafe_buf80[80];
	struct aics_relay *head = array;
	int i, shift = 0;
	char * delim = "";

	unsafe_buf80[0] = '\0';
    if (head) {
    	for (i = 1; i <= size; i++) {
    		//ast_log(LOG_NOTICE, "%d head-status %d\n", i, head->status);
    		if (shift)
    			delim = "/";
    		shift += snprintf(&unsafe_buf80[shift], sizeof(unsafe_buf80) - shift, "%s%d:%d:%d", delim, head->status, head->tm_on, head->tm_off);
//			if (head->status) {
//				if (shift)
//					delim = "/";
//				if (head->tm_on + head->tm_off > 0) {
//					shift += snprintf(&unsafe_buf80[shift], sizeof(unsafe_buf80) - shift, "%s%d:%d:%d", delim, i, head->tm_on, head->tm_off);
//				} else {
//					shift += snprintf(&unsafe_buf80[shift], sizeof(unsafe_buf80) - shift, "%s%d", delim, i);
//				}
//			}
			head++;
    	}
    }
    //ast_log(LOG_NOTICE, "Relay tostr %s shift %d\n", unsafe_buf80, shift);
    return unsafe_buf80;
}

int aics_event_from_str(struct aics_event *array, const int size, const char *text)
{
	int i = 0, scan = 0;
	char *cpy, *ptr;
	char buf[20];
	struct aics_event *head = array;
	int res = 1;

	if (!array || ast_strlen_zero(text))
		return res;
	//ast_log(LOG_NOTICE, "Event fromstr %s\n", text);
	cpy = ast_strdupa(text);
	memset(&array, 0, size);
	ptr = cpy;
	while (ptr) {
		scan = sscanf(ptr, "event%d:%19[^&]", &i, buf);
		//ast_log(LOG_NOTICE, "[%s] %d %d %s\n", ptr, scan, i, buf);
		if (scan > 0) {
			if (i <= size) {
				head[i-1].status = aics_event_status_from_str(buf);
				res = 0;
			}
		} else {
			res = 2;
			break;
		}
		cpy = ptr;
		ptr = strchr(cpy, '&');
		if (ptr)
			*ptr++ = '\0';
	};
	return res;
}

const char *aics_event_to_str(struct aics_event *array, const int size)
{
	static char unsafe_buf801[80];
	struct aics_event *head = array;
	int i, shift = 0;
	char * delim = "";

	unsafe_buf801[0] = '\0';
    if (head) {
    	for (i = 1; i <= size; i++) {
    		//ast_log(LOG_NOTICE, "%d head-status %d\n", i, head->status);
    		if (shift)
    			delim = "&";
    		switch (head->status) {
    		case AICS_EVENT_OFF:
    			break;
    		case AICS_EVENT_DROP:
    		    head->status = AICS_EVENT_OFF;
    		case AICS_EVENT_ON:
    			shift += snprintf(&unsafe_buf801[shift], sizeof(unsafe_buf801) - shift, "%sevent%d:%s", delim, i, aics_event_status_to_str(head->status));
    		}
			head++;
    	}
    }
   // ast_log(LOG_NOTICE, "Event tostr %s shift %d\n", unsafe_buf801, shift);
    return unsafe_buf801;
}

void aics_priority_set(struct aics_proxy_params *params, const char *text)
{
	params->priority = aics_priority_from_str(text);
	params->validity.as_bits.priority = (params->priority > -1 ? 1 : 0);
	//ast_log(LOG_NOTICE, "Validity priority %d\n", params->validity.as_bits.priority);
}

const char *aics_priority_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.priority)
		return NULL;
	return aics_priority_to_str(params->priority);
}

void aics_direction_set(struct aics_proxy_params *params, const char *text)
{
	params->direction = aics_direction_from_str(text);
	params->validity.as_bits.direction = 1;
	//ast_log(LOG_NOTICE, "Validity direction %d\n", params->validity.as_bits.direction);
}

const char *aics_direction_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.direction)
		return NULL;
	return aics_direction_to_str(params->direction);
}

void aics_scenario_set(struct aics_proxy_params *params, const char *text)
{
	params->scenario = aics_scenario_from_str(text);
	params->validity.as_bits.scenario = 1;
	//ast_log(LOG_NOTICE, "Validity scenario %d\n", params->validity.as_bits.scenario);
}

const char *aics_scenario_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.scenario)
		return NULL;
	return aics_scenario_to_str(params->scenario);
}

void aics_relay_set(struct aics_proxy_params *params, const char *text)
{
	params->validity.as_bits.relay = (aics_relay_from_str(&params->relay[0], sizeof(params->relay)/sizeof(params->relay[0]), text) == 0 ? 1 : 0);
	//ast_log(LOG_NOTICE, "Validity relay %d\n", params->validity.as_bits.relay);
}

const char *aics_relay_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.relay)
		return NULL;
	return aics_relay_to_str(&params->relay[0], sizeof(params->relay)/sizeof(params->relay[0]));
}

void aics_event_set(struct aics_proxy_params *params, const char *text)
{
	params->validity.as_bits.event = (aics_event_from_str(&params->event[0], sizeof(params->event)/sizeof(params->event[0]), text) == 0  ? 1 : 0);
	//ast_log(LOG_NOTICE, "Validity event %d\n", params->validity.as_bits.event);
}

const char *aics_event_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.event)
		return NULL;
	return aics_event_to_str(&params->event[0], sizeof(params->event)/sizeof(params->event[0]));
}

void aics_playlist_set(struct aics_proxy_params *params, const char *text)
{
	params->validity.as_bits.playlist = (aics_playlist_from_str(&params->playlist, text) == 0 ? 1 : 0);
	//ast_log(LOG_NOTICE, "Validity playlist %d\n", params->validity.as_bits.playlist);
}

const char *aics_playlist_get(struct aics_proxy_params *params)
{
	if(!params)
		return NULL;
	if(!params->validity.as_bits.playlist)
		return NULL;
	return aics_playlist_to_str(&params->playlist);
}

//void aics_debug_channels(void)
//{
//	aics_list_channel_snapshot();
//}
//
//void aics_list_channel_snapshot(void)
//{
////#define FORMAT_STRING  "%-20.20s %-20.20s %-7.7s %-30.30s\n"
//#define CONCISE_FORMAT_STRING  "%s!%s!%s!%d!%s!%s!%s!%s!%s!%s!%d!%s!%s!%s\n"
//
//	RAII_VAR(struct ao2_container *, channels, NULL, ao2_cleanup);
//	struct ao2_iterator it_chans;
//	struct stasis_message *msg;
//
//	if (!(channels = stasis_cache_dump(ast_channel_cache_by_name(), ast_channel_snapshot_type()))) {
//		ast_log(LOG_NOTICE, "++ Failed to retrieve cached channels\n");
//		return ;
//	}
//
//	it_chans = ao2_iterator_init(channels, 0);
//	for (; (msg = ao2_iterator_next(&it_chans)); ao2_ref(msg, -1)) {
//		struct ast_channel_snapshot *cs = stasis_message_data(msg);
//		char durbuf[10] = "-";
//
//		if (!ast_tvzero(cs->creationtime)) {
//			int duration = (int)(ast_tvdiff_ms(ast_tvnow(), cs->creationtime) / 1000);
//			int durh = duration / 3600;
//			int durm = (duration % 3600) / 60;
//			int durs = duration % 60;
//			snprintf(durbuf, sizeof(durbuf), "%02d:%02d:%02d", durh, durm, durs);
//		}
//
//		ast_log(LOG_NOTICE, CONCISE_FORMAT_STRING, cs->name, cs->context, cs->exten, cs->priority, ast_state2str(cs->state),
//			S_OR(cs->appl, "(None)"),
//			cs->data,
//			cs->caller_number,
//			cs->accountcode,
//			cs->peeraccount,
//			cs->amaflags,
//			durbuf,
//			cs->bridgeid,
//			cs->uniqueid);
//
//		aics_list_chanvars(cs->name);
//
////		char locbuf[40] = "(None)";
////		char appdata[40] = "(None)";
////		if (!cs->context && !cs->exten)
////			snprintf(locbuf, sizeof(locbuf), "%s@%s:%d", cs->exten, cs->context, cs->priority);
////		if (cs->appl)
////			snprintf(appdata, sizeof(appdata), "%s(%s)", cs->appl, S_OR(cs->data, ""));
////		ast_log(LOG_NOTICE, FORMAT_STRING, cs->name, locbuf, ast_state2str(cs->state), appdata);
//	}
//	ao2_iterator_destroy(&it_chans);
////#undef FORMAT_STRING
//#undef CONCISE_FORMAT_STRING
//}
//
//void aics_list_chanvars(const char *channame)
//{
//	struct ast_channel *chan;
//	struct ast_var_t *var;
//
//	chan = ast_channel_get_by_name(channame);
//	if (!chan) {
//		ast_log(LOG_NOTICE, "Channel '%s' not found\n", channame);
//		return ;
//	}
//
//	ast_channel_lock(chan);
//aics_addons_notice(ast_channel_addons(chan), channame);
//
//	AST_LIST_TRAVERSE(ast_channel_varshead(chan), var, entries) {
//		ast_log(LOG_NOTICE, "%s=%s\n", ast_var_name(var), ast_var_value(var));
//	}
//
//	ast_channel_unlock(chan);
//
//	ast_channel_unref(chan);
//}

//void aics_pvt_addons_init(struct aics_pvt_addons *init)
//{
//	if (!init)
//		return;
//	init->direction = AICS_DIRECTION_SENDRECV;
//	init->last_direction = AICS_DIRECTION_SENDRECV;
//	init->priority_handle = aics_priority_handle_str2int(DEFAULT_PRIORITYHANDLE);
//	init->callpriority = aics_priority_str2int(DEFAULT_CALLPRIORITY);
//	init->hangup_anounce = ast_strdup(DEFAULT_HANGUPANOUNCE);
//}
//
//void aics_pvt_addons_notice(struct aics_pvt_addons *info, char *prefix)
//{
//	if (!info)
//		return;
//	char prBuf[30] = "";
//	aics_priority_int2str(info->callpriority, prBuf);
////	ast_log(LOG_NOTICE, "%s pvt addon direction %d phandle %d priority %d\n",
////				prefix ? prefix : "_",
////				info->direction,
////				info->priority_handle,
////				info->callpriority);
//	ast_log(LOG_NOTICE, "%s pvt addon direction %s/%s phandle %s hanounce %s priority %s\n",
//			prefix ? prefix : "_",
//			aics_direction_type[info->direction],aics_direction_type[info->last_direction],
//			aics_priority_handle_type[info->priority_handle],
//			info->hangup_anounce ? info->hangup_anounce : "null",
//			prBuf);
//}
//
//void aics_pvt_addons_copy(struct aics_pvt_addons *dst, struct aics_pvt_addons *src)
//{
////	aics_pvt_addons_notice(dst, "pvt addons copy dst");
////	aics_pvt_addons_notice(src, "pvt addons copy src");
//
//	if (!dst || !src || dst == src)
//		return;
//	dst->direction = src->direction;
//	dst->last_direction = src->last_direction;
//	dst->priority_handle = src->priority_handle;
//	dst->callpriority = src->callpriority;
//	if (dst->hangup_anounce)
//		ast_free(dst->hangup_anounce);
//	dst->hangup_anounce = ast_strdup(src->hangup_anounce);
//}
//
//void aics_pvt_addons_free(struct aics_pvt_addons *done)
//{
//	if (!done)
//		return;
//	if (done->hangup_anounce)
//		ast_free(done->hangup_anounce);
//	aics_pvt_addons_init(done);
//}
//
//void aics_pvt_addons_set_direction(struct aics_pvt_addons *info, enum aics_direction direction)
//{
//	if (!info)
//		return;
//	info->last_direction = info->direction;
//	info->direction = direction;
//}

//void aics_addons_init(struct aics_channel_addons *init)
//{
//	if (!init)
//		return;
//	init->platform = NULL;
//	init->ctrlchan = NULL;
//	init->scenario = AICS_SCENARIO_NONE;
//	aics_pvt_addons_init(&init->pvt_addons);
//}
//
//void aics_addons_copy(struct aics_channel_addons *dst, struct aics_channel_addons *src)
//{
//	if (!dst || !src || dst == src)
//		return;
//	if (dst->platform)
//		ast_free(dst->platform);
//	dst->platform = ast_strdup(src->platform);
//	if (dst->ctrlchan)
//		ast_free(dst->ctrlchan);
//	dst->ctrlchan = ast_strdup(src->ctrlchan);
//
//	dst->scenario = src->scenario;
//	aics_pvt_addons_copy(&dst->pvt_addons, &src->pvt_addons);
//}
//
//void aics_addons_free(struct aics_channel_addons *done)
//{
//	if (!done)
//		return;
//	if (done->platform)
//		ast_free(done->platform);
//	if (done->ctrlchan)
//		ast_free(done->ctrlchan);
////	ast_free(done);
////	done = NULL;
//	aics_pvt_addons_free(&done->pvt_addons);
//	aics_addons_init(done);
//}
//
//void aics_addons_notice(struct aics_channel_addons *info, char *prefix)
//{
//	//ast_log(LOG_NOTICE, "addons notice\n");
//	if (!info)
//		return;
//	char prBuf[30] = "";
//	aics_priority_int2str(info->pvt_addons.callpriority, prBuf);
//	ast_log(LOG_NOTICE, "%s addon platform %s ctrlchan %s scenario %s direction %s/%s phandle %s hanounce %s priority %s\n",
//			prefix ? prefix : "_",
//			info->platform ? info->platform : "null",
//			info->ctrlchan ? info->ctrlchan : "null",
//			aics_scenario_type[info->scenario],
//			aics_direction_type[info->pvt_addons.direction],aics_direction_type[info->pvt_addons.last_direction],
//			aics_priority_handle_type[info->pvt_addons.priority_handle],
//			info->pvt_addons.hangup_anounce ? info->pvt_addons.hangup_anounce : "null",
//			prBuf);
//}

