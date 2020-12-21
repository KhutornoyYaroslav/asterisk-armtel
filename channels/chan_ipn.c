/*
 * chan_ipn.c
 *
 *  Created on: Jun 26, 2013
 *      Author: forint
 */

#include "asterisk.h"
#include "aics.h"
#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/unaligned.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/acl.h"
#include "asterisk/callerid.h"
#include "asterisk/file.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"
#include "asterisk/rtp_engine.h"
#include "asterisk/causes.h"
#include "asterisk/utils.h"
#include "asterisk/linkedlists.h"

#include "ipn.h"

#include <signal.h>
#include <stdbool.h>

#ifndef FALSE
	#define FALSE 0
#endif

#ifndef TRUE
	#define TRUE 1
#endif

#define AST_MODULE "chan_ipn"
#define IPN_VERSION "1.17"

static const char ipn_channel_desc[] = "Armtel IPN Channel Driver";
static const char ipn_config_file[]  = "ipn.conf";

#define IPN_TIMER_HANGUP 100

AST_MUTEX_DEFINE_STATIC(ipn_monitor_lock);
AST_MUTEX_DEFINE_STATIC(ipn_reload_lock);

struct ast_sched_context *sched = NULL; 	// The scheduling context
static struct io_context *io = NULL;		// The IO context

static pthread_t ipn_monitor_thread = AST_PTHREADT_NULL;
static int ipn_reload_flag = FALSE;
static enum channelreloadreason ipn_reload_reason = CHANNEL_MODULE_LOAD;



 struct ipn_rtp{

	unsigned int ssrc;		/*!< Synchronization source, RFC 3550, page 10. */
	unsigned int lastts;
	unsigned short seqno;		/*!< Sequence number, RFC 3550, page 13 */
	struct ast_sockaddr ip;
	unsigned char prio;
	unsigned char line;
	char number[16];
};


struct ipn_pvt {  //структура определяющее соединение
	AST_DECLARE_STRING_FIELDS(
		/*! Name of the device */
		AST_STRING_FIELD(name);
		/*! Default context for outgoing calls */
		AST_STRING_FIELD(context);
		/*! Default extension for outgoing calls */
		AST_STRING_FIELD(exten);
		AST_STRING_FIELD(ipn_num); //номер абонента IPN
		AST_STRING_FIELD(aics_num);//номер абонента AICS
    );
	int sock; //сокет открытый для приема  IPN RTP пакетов
	int dir; //
	struct ast_channel *owner;//ук-ль на IPN канал соединения 
	struct ipn_rtp rtp;//параметры текущего RTP соединения
	struct ast_frame *frame;//ук-ль на фрейм для накопления  IPN RTP пакетов
	AST_LIST_HEAD(, ast_frame) rd_queue; //очередь фреймов для отправки AICS абонентам

};

struct ipn_settings {
	char    bind_addr[SETTING_MAX_LEN];
	char    igmp_addr[SETTING_MAX_LEN];
	char    ifase[SETTING_MAX_LEN];
	char    default_context[SETTING_MAX_LEN];
	char    hwaddr[SETTING_MAX_LEN];
	int		status_port;
	int		users_port;
	int     groups_port;
	int     group_count;
	int 	default_priority;
	struct ast_sockaddr status_addr;
};


struct ipn_subscriber{
	AST_DECLARE_STRING_FIELDS(
	 AST_STRING_FIELD(context);
	 AST_STRING_FIELD(name);
	 AST_STRING_FIELD(number);//номер абонента IPN
	);
	struct ast_sockaddr ip;// IP адрес абонента IPN(определяется при получении статусного пакета по абоненскому номеру)
	int type;//тип абонента IPN(абонент или группа)
	int idx; //индекс группы(если тип группа)
	int timer;
	int state;//состояние абонента
};

struct aics_subscriber{
	AST_DECLARE_STRING_FIELDS(
	 AST_STRING_FIELD(name);
	 AST_STRING_FIELD(tech);
	 AST_STRING_FIELD(number);//номер абонента AICS
	);
	struct ast_sockaddr ip;// IP адрес абонента IPN(определяется при получении статусного пакета по абоненскому номеру)
	int sock;//сокет открытый для приема  IPN RTP пакетов
	int* io_id;
	int idx;//индекс группы(если тип группа)
	int type;//тип абонента IPN(абонент или группа)
	int timer;//таймер для отправки статусных пакетов
	int timer_hangup; //таймер для отбоя по отсутствию IPN RTP пакетов
	int state;//состояние абонента
	int astate;//состояние абонента
	struct ipn_pvt *pvt;
	int duplex;
	int dir;
};

static struct ao2_container *ipn_space; /*! */
static struct ao2_container *aics_space; /*! */
static struct ao2_container *pvts; /*! */
static struct ipn_settings ipn_cfg;

static struct ast_channel *ipn_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause);
static int ipn_call(struct ast_channel *ast, const char *dest, int timeout);
static int ipn_hangup(struct ast_channel *ast);
static struct ast_frame *ipn_read(struct ast_channel *ast);
static int ipn_write(struct ast_channel *ast, struct ast_frame *f);
static struct ast_frame *ipn_exception(struct ast_channel *ast);
static int ipn_answer(struct ast_channel *ast);

//*********************************************************
// Channel driver declaration
//*********************************************************
static struct ast_channel_tech ipn_channel_tech = {
	.type = "IPN",
	.description = ipn_channel_desc,
	.requester = ipn_request,
	.answer = ipn_answer,
	.call = ipn_call,
	.hangup = ipn_hangup,
	.read = ipn_read,
	.write = ipn_write,
	.exception = ipn_exception,
};


static int ipnsock = -1;
static int *ipnsock_read_id = NULL;
AST_MUTEX_DEFINE_STATIC(ipnsock_lock);

static unsigned int global_tos_ipn;
static unsigned int global_cos_ipn;

struct ast_frame* ff=NULL;

/*
static struct {
	int state;
	char* device;
} ipnstate[9] =
{
		{0, "SIP/301"},
		{1, "SIP/302"},
		{0, "IPN/101"},
		{1, "IPN/102"},
		{1, "IPN/103"},
		{0, "IPN/104"},
		{1, "IPN/105"},
		{0, "IPN/106"},
		{0, "IPN/107"},
};
*/

int ipn_parse_status( char* p,int sz,char* num,unsigned char* status,char*ext);
void get_hwaddr(char* hwaddr);
void ipn_transmit_status(int soc,const char* num ,unsigned char status ,const char* ext);
void ipn_open_aics_socket(char* bind_addr,int port,char* igmp,int gport);
static void  ipn_close_aics_socket(void);
void ipn_update_io(void);
int ipn_open_socket(char* bind_addr,int port,char* igmp,struct ast_sockaddr* ip );
int info_state_cb(  char* con,char *id, struct ast_state_cb_info *info, void* data);
void ipn_timer(void);
void ipn_show(struct ast_cli_args *a);
static struct ipn_subscriber* find_ipn_subscriber(const char *number);
static struct aics_subscriber* find_aics_subscriber(const char *number);
void str_to_hex(char* hex ,unsigned char* str,unsigned char len );
int ipn_update_status(const char* num,struct ast_sockaddr* ip,int status );
int parse_ipn_rtp_ext(char * p,char* num,unsigned char* prio,unsigned char* line);
struct aics_subscriber* find_aics_subscriber_sock(int sock);
struct ipn_pvt * init_incoming_call(const char *num,unsigned char prio,unsigned char line ,int sock,struct ast_sockaddr* ip);
void calls_show(struct ast_cli_args *a);
void init_state_aics(void);
int ipnsock_read_real(int);

//--------------------------------------------------------------------
// Функции работы с структурами pvt
//--------------------------------------------------------------------
static int ipn_pvt_hash_cb(const void *obj, const int flags)
{
	const struct ipn_pvt *pvt = obj;

	return ast_str_case_hash(pvt->ipn_num);
}
static int ipn_pvt_cmp_cb(void *obj, void *arg, int flags)
{
	struct ipn_pvt *pvt = obj,*pvt2=arg;

	return !strcasecmp(pvt->ipn_num, pvt2->ipn_num) ? CMP_MATCH | CMP_STOP : 0;
}
static void ipn_pvt_destructor(void *obj)
{
	struct ipn_pvt *pvt = obj;
	struct aics_subscriber *aics = NULL ;
	struct ast_frame *cur;
	int queued_frames=0;

	AST_LIST_TRAVERSE(&pvt->rd_queue, cur, frame_list) {
		AST_LIST_REMOVE(&pvt->rd_queue, cur, frame_list);
		ast_frame_free(cur,0);
		queued_frames++;
	}
	ast_log(LOG_NOTICE, "Hang up call %s %s %s [prio=%d;line =%d]%d\n",pvt->ipn_num,pvt->dir ? "->":"<-",pvt->aics_num ,pvt->rtp.prio ,pvt->rtp.line,queued_frames);

	aics=find_aics_subscriber(pvt->aics_num);
    if(aics) {
    	aics->pvt=NULL;
    	aics->timer_hangup=IPN_TIMER_HANGUP*2;
    }


	ast_string_field_free_memory(pvt);

}
static struct ipn_pvt* find_ipn_pvt(const char *number)
{
	struct ipn_pvt tmp_pvt = {
		.ipn_num = number,
	};

	return ao2_find(pvts, &tmp_pvt, OBJ_POINTER);
}


static struct ipn_pvt  *init_pvt(const char *p,const char *c,struct ast_sockaddr*ip,  int prio ,int line,int dir)
{
  struct ipn_pvt *pv,*pvt=NULL;
  struct aics_subscriber *aics = NULL ;
  struct ao2_iterator i;


	  aics=find_aics_subscriber(c);
	 // если aics абонента вызывает несколько ipn абонентов	
	
       if(aics) {
         i=ao2_iterator_init(pvts,0);
        while((pv=ao2_iterator_next(&i))){
          if(strcasecmp(pv->aics_num, c)==0)
	      {
			if(prio >  pv->rtp.prio){
	// приоритетность пока не получается :((			
	//		   ast_softhangup_nolock(pv->owner,AST_SOFTHANGUP_DEV);
			   ast_verb( 120, "RRIO softhangup %s %s %s[prio=%d;new prio=%d ] \n",pv->ipn_num,pv->dir ? "->":"<-",pv->aics_num, pv->rtp.prio,prio); 
			}
			else{
			   ast_verb( 120, "BUSY %s %s %s \n",pv->ipn_num,pv->dir ? "->":"<-",pv->aics_num); 
			}
		    ao2_ref(pv,-1);
			ao2_iterator_destroy(&i);
			return NULL;
	      }
		  ao2_ref(pv,-1);
	    }
        ao2_iterator_destroy(&i); 
	  }
  //---------------------------------------------------

//   pvt= find_ipn_pvt(p);
//   if(pvt == NULL){
	 pvt=ao2_alloc(sizeof(*pvt),ipn_pvt_destructor);
	 if(pvt){
      if (ast_string_field_init(pvt, 32))
		return NULL;
      ast_string_field_set(pvt, ipn_num, p);
      ast_string_field_set(pvt, aics_num, c);
//	  aics=find_aics_subscriber(c);
	  pvt->sock=-1;
	  if(aics) {
    	  pvt->sock =aics->sock;
//    	  if(dir == FROM_AICS) aics->pvt=pvt;
    	  aics->pvt=pvt;
    	  aics->dir=dir;
    	  pvt->dir=dir;
    	  ao2_ref(aics,-1);
      }
      pvt->rtp.prio=prio;
      pvt->rtp.line=line;
      pvt->rtp.ssrc = ast_random();
      pvt->rtp.seqno = ast_random() & 0xffff;
      pvt->rtp.lastts=0;
      ast_sockaddr_copy(&pvt->rtp.ip,ip);
      pvt->frame=NULL;
      ao2_link(pvts, pvt);
   		  ast_log(LOG_NOTICE, "New call %s %s %s [prio=%d;line =%d]\n",pvt->ipn_num,pvt->dir ? "->":"<-",pvt->aics_num , prio,line);
      return pvt;
   }
   else return NULL;
//   {
//	 ast_log(LOG_NOTICE, "pvt=%s ;prio =%d ;line =%d  \n",pvt->ipn_num,prio,line);
//     ao2_ref(pvt, -1);
//   }
//   return NULL;
}

//--------------------------------------------------------------------
// Функции драйвера канала
//--------------------------------------------------------------------

static struct ast_frame *ipn_exception(struct ast_channel *ast)
{
	ast_log(LOG_NOTICE, "ipn_exception\n");

	  return &ast_null_frame;
}

static struct ast_channel *ipn_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *data, int *cause)
{
	struct ast_channel *chan=NULL;
	struct ipn_subscriber *ipn = NULL ;
//	struct aics_subscriber *aics = NULL ;
	struct ipn_pvt* pvt = NULL;
	unsigned char i,relay=0;

	struct ast_party_caller* caller = ast_channel_caller(( struct ast_channel*)requestor);
	//ast_log(LOG_NOTICE, "ipn_request =%s \n",caller->id.number.str);

	if (ast_strlen_zero(data)) {
			ast_log(LOG_ERROR, "Unable to create channel with empty destination.\n");
			*cause = AST_CAUSE_CHANNEL_UNACCEPTABLE;
			return NULL;
	}
	else{
		 struct aics_proxy_params *aipr;
		 aipr = ast_channel_proxy(( struct ast_channel*)requestor);
		 if(aipr->validity.as_bits.relay){
			for (i = 0; i < 8; i++) {
		    	if (aipr->relay[i].status){
		    		relay |= (1<<i);
		    	}
			}
		}
		pvt= find_ipn_pvt(data); //среди текущих соединений ищем по номеру ipn абонента
		if(pvt == NULL){
		  ipn=find_ipn_subscriber(data);
		  if(ipn){
			// ast_log(LOG_NOTICE, "ipn_dest=%s[%s]prio=%d\n",data,ast_sockaddr_stringify(&ipn->ip),aipr->priority);
			 pvt = init_pvt(data,(char*)caller->id.number.str,&ipn->ip,aipr->priority,relay,FROM_AICS);
			 ao2_ref(ipn,-1);
		  }
		  else{
			ast_log(LOG_NOTICE, "not ipn subscriber=%s \n",data);
		  }
		}
		else{
			if(aipr->priority > pvt->rtp.prio){
			//   ast_log(LOG_NOTICE, "Priority call=%s[%d] \n",caller->id.number.str,aipr->priority);
			   ast_softhangup_nolock(pvt->owner,AST_SOFTHANGUP_DEV);
			   //pvt->owner;
			   ipn_pvt_destructor(pvt);
			   ao2_unlink(pvts,pvt);
			 //  ao2_ref(pvt,-1);
			//   ipn_hangup(pvt->owner);
			   ipn=find_ipn_subscriber(data);
			   if(ipn){
					// ast_log(LOG_NOTICE, "ipn_dest=%s[%s]prio=%d\n",data,ast_sockaddr_stringify(&ipn->ip),aipr->priority);
					 pvt = init_pvt(data,(char*)caller->id.number.str,&ipn->ip,aipr->priority,relay,FROM_AICS);
					 ao2_ref(ipn,-1);
			   }
			   else{
					ast_log(LOG_NOTICE, "not ipn subscriber=%s \n",data);
			  }
			}
			else{
			//   ast_log(LOG_NOTICE, "Priority call=NULL \n");
			   return NULL;

			}
		}
	}

    if(pvt){
	 if (!(chan = ast_channel_alloc(1, AST_STATE_DOWN, "", "", "", "", "", assignedids, requestor, 0, "IPN/%s", pvt->ipn_num))) {
		goto failure;
	 }
	 ast_channel_set_fd(chan,0,pvt->sock);
	 ast_channel_tech_set(chan, &ipn_channel_tech);
	 ast_format_set(ast_channel_readformat(chan),AST_FORMAT_ALAWDCN, 0);
	 ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_readformat(chan));
	 ast_format_set(ast_channel_writeformat(chan),AST_FORMAT_ALAWDCN, 0);
	 ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_writeformat(chan));
	 pvt->owner=chan;
	 ast_string_field_set(pvt, context, ast_channel_context(chan));
	 ast_channel_tech_pvt_set(chan, pvt);
	 ao2_ref(pvt,-1);
	 ast_channel_unlock(chan);
    }
	 return chan;
failure:
	*cause = AST_CAUSE_FAILURE;
	return NULL;
}
static int ipn_call(struct ast_channel *ast, const char *dest, int timeout)
{
    //ast_log(LOG_NOTICE, "ipn_call\n");
//	struct ipn_pvt *pvt = ast_channel_tech_pvt(ast);

	ast_queue_control(ast, AST_CONTROL_ANSWER);
	ast_setstate(ast, AST_STATE_UP);

	return 0;

}
static int ipn_answer(struct ast_channel *ast)
{
//	struct ipn_pvt  *pvt = ast_channel_tech_pvt(ast);
//	struct aics_subscriber *aics=NULL;
//	if(pvt){
//	  ao2_lock(aics);
//	  aics=find_aics_subscriber(pvt->aics_num);
//	  if(aics){
	  //	  if(aics->dir == TO_AICS) aics->pvt=pvt;
//	  }
//	}
    ast_log(LOG_NOTICE, "ipn_answer[%s]\n",ast_channel_name(ast));
	ast_setstate(ast, AST_STATE_UP);
	return 0;
}

static int ipn_hangup(struct ast_channel *ast)
{
	struct ipn_pvt  *pvt = ast_channel_tech_pvt(ast);
	struct aics_subscriber *aics=NULL;

//	 pvt = ao2_find(pvts, pvt, OBJ_POINTER);
	if(pvt){
//	  ao2_lock(aics);
	  aics=find_aics_subscriber(pvt->aics_num);
	  if(aics){
	//	  ast_log(LOG_NOTICE, "ipn_hangup=%s - %s[%p==%p] \n",ast_channel_name(ast),aics->number,aics->pvt,pvt);
		  if(aics->pvt){
			  if(aics->type== SUBSCRIBER_USER){
		        ipn_transmit_status(aics->sock,aics->number ,ELLANSTAT_DISC ,aics->pvt->ipn_num);
		        ipn_transmit_status(aics->sock,aics->number ,ELLANSTAT_DISC ,aics->pvt->ipn_num);
			  }
		  }
	  }
//	  ao2_unlock(aics);
  	  ao2_unlink(pvts,pvt);
	  
	  
	  
  	  ast_channel_tech_pvt_set(ast, NULL);
//  	  ao2_ref(pvt,-1);
	}
	return 0;
}
static struct ast_frame *ipn_read(struct ast_channel *ast)
{
	struct ipn_pvt *pvt = ast_channel_tech_pvt(ast);
	struct ast_frame *f;
//	unsigned char *p;
//	char hex[32];
	if(pvt){

		ipnsock_read_real(pvt->sock);
//		struct aics_subscriber* aics=find_aics_subscriber(pvt->aics_num);
//		if (aics){
//	      ao2_lock(aics);
//		  aics->timer_hangup=0;
//		}
//	    ao2_unlock(aics);
        f= AST_LIST_REMOVE_HEAD(&pvt->rd_queue, frame_list);
	    if(f) {
//		  p= (unsigned char*)(f->data.ptr-64);
//		   ast_verbose("%d-",*p);
//	     ast_verbose("*");
		 return f;
	    }
	}
//	ast_verbose("--0--\n");
	return &ast_null_frame;
}

static int ipn_write(struct ast_channel *ast, struct ast_frame *f)
{
	char hex[64];
//	 ast_verbose("$");

//   ast_log(LOG_NOTICE, "ipn_write[%s] %d [%d]\n", ast_getformatname(&f->subclass.format),f->datalen,f->samples);
//   ast_log(LOG_NOTICE, "ipn %d; %d; %d; %d\n", f->seqno,f->offset,f->ts,f->delivery);
	unsigned char i,buf[255],*p1,*p=&buf[16],len;
	unsigned int *ptr = (unsigned int *)buf;
	struct ipn_pvt *pvt = ast_channel_tech_pvt(ast);
    int soc=ipnsock;
	struct aics_subscriber* aics=find_aics_subscriber(pvt->aics_num);

	if (aics){
      soc=aics->sock;
        if((aics->type==SUBSCRIBER_GROUP) || (aics->duplex==0 )){
          ao2_ref(aics,-1);
          return 0;
        }
    }
  	put_unaligned_uint32(ptr++, htonl((2 << 30) | (1 << 28) | (9 << 16) | (pvt->rtp.seqno)));
	put_unaligned_uint32(ptr++, htonl(pvt->rtp.lastts));
	put_unaligned_uint32(ptr++, htonl(pvt->rtp.ssrc));
	put_unaligned_uint32(ptr++, htonl((0x55<<24) | (0x48<<16) | (3) ));
    len=strlen((char*)pvt->aics_num);
    *p++ =((1<<4) | (len - 1));
//    memset(p,0,7);
    strncpy((char*)p,pvt->aics_num,7);
	p=p+7;
	*p++=0x20;
	*p++=pvt->rtp.prio;
	*p++=0x30;
	*p=pvt->rtp.line;
     p=&buf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN];
     p1=(unsigned char*)f->data.ptr;
	 for(i=0;i<f->samples/2;i++){
				*p++= *p1++;
	}
	str_to_hex(hex,&buf[12],16);
	ast_verb( 99, "send[%s]\n",hex );

//		 ast_verbose("%d =send[%s]\n",soc,ast_sockaddr_stringify(&pvt->rtp.ip)/*hex*/);
    ast_sendto(soc, buf, IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN+f->samples/2 , 0, &pvt->rtp.ip);

    pvt->rtp.seqno++;
	pvt->rtp.lastts+=f->samples/2;

	ptr = (unsigned int *)buf;
	put_unaligned_uint32(ptr++, htonl((2 << 30) | (1 << 28) | (9 << 16) | (pvt->rtp.seqno)));
	put_unaligned_uint32(ptr++, htonl(pvt->rtp.lastts));
    p=&buf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN];
    p1=(unsigned char*)f->data.ptr;
    p1+=f->samples/2;
	for(i=0;i<f->samples/2;i++){
				*p++= *p1++;
	}
    ast_sendto(soc, buf, IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN+f->samples/2 , 0, &pvt->rtp.ip);

    pvt->rtp.seqno++;
	pvt->rtp.lastts+=f->samples/2;

//	ao2_ref(pvt,-1);
//    return f->datalen;
    return 0;
}

//--------------------------------------------------------------------
//Разбор статусного пакет IPN абонента
//вход: *p- ук-ль на пакет
//      sz- размер пакета
//вход:выход: num- номер абонента
//            status- состояние абонента
//            ext- номер абонента в соединении с  абонентом приславшим статус
//------------------------------------------------------------------
int ipn_parse_status( char* p,int sz,char* num,unsigned char* status,char*ext)
{
	unsigned char tmp,i;

    if (sz< 30) return FALSE;
    if (*p++ !='I') return FALSE;
    if (*p++ !='P') return FALSE;
    if (*p++ !='N') return FALSE;
    if (*p++ !='0') return FALSE;
    if (*p++ !='1') return FALSE;
    tmp=*p++;
    for( i=0;i<tmp;i++) *num++ = *p++;
    *num=0;
    p = p+18;
//        ast_log(LOG_NOTICE, "status7[%d]\n",*p);
    if(*p++ != 1)return FALSE;
    *status = *p++;
    tmp=*p++;
    if(tmp!=0) ;
    for( i=0;i<tmp;i++) *ext++ = *p++;
    *ext=0;
    return TRUE;
}

//--------------------------------------------------------------------
//Получение mac адреса

void get_hwaddr(char* hwaddr)
{
    int s,i=0;
    struct ifreq buffer;
    s = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&buffer, 0x00, sizeof(buffer));
    strcpy(buffer.ifr_name,ipn_cfg.ifase /*"enp2s0"*/);
    ioctl(s, SIOCGIFHWADDR, &buffer);
    close(s);
    for( s = 0; s < 6; s++ )
    {
        i+=sprintf(hwaddr+i,"%.2X", (unsigned char)buffer.ifr_hwaddr.sa_data[s]);
    }
}
//--------------------------------------------------------------------
//Отправка пакетов статуса
//  вход :  soc
//         *num -  номер абонента
//         status  - состояние абонента
//         *ext -номер абонента в соединении с  абонентом отправителем
//------------------------------------------------------------------
void ipn_transmit_status(int soc,const char* num ,unsigned char status ,const char* ext)
{
 unsigned char buf[40],*p=buf,tmp,i ;
 char hb[255];

	if(soc<0) return;
	*p++='I';
	*p++='P';
	*p++='N';
	*p++='0';
	*p++='1';
    tmp = strlen(num);
	*p++=tmp;
     for( i=0;i<tmp;i++) *p++ = *num++;
	*p++=12 ;
     for( i=0;i<12;i++) *p++ = ipn_cfg.hwaddr[i];
//	get_hwaddr((char*)p);
//	p+=12;
	*p++=4;
	*p++=0x39;
	*p++=0x39;
	*p++=0x39;
	*p++=0x39;
	*p++=1;
	*p++=status;
	if(ext !=0){
	  tmp = strlen(ext);
	  *p++= tmp;
	  for( i=0;i<tmp;i++) *p++ = *ext++;
	}
	else *p++=0;
	tmp=0;

	for( i = 0; i < p-buf; i++ )
    {

        tmp+=sprintf(hb+tmp,"%.2X ", buf[i]);
    }
    hb[tmp]=0;
//    ast_log(LOG_NOTICE, "status:[%s]\n",hb );
    i = ast_sendto(soc,buf,p-buf , 0, &ipn_cfg.status_addr);
    if (i != p-buf) {
    		ast_log(LOG_WARNING, "Err send status to %s returned %d: %s\n", ast_sockaddr_stringify(&ipn_cfg.status_addr), i, strerror(errno));
    }

}

//--------------------------------------------------------------------
//Открытие сокетов aics абонентов для работы по IPN протоколу
//  вход :
//         *bind_addr - IP адрес
//          port  - стартовый порт для абонентов aics(слушаем входящий RTP трафик)
//         *igmp  -групповой IP адрес
//          gport -стартовый порт групп (слушаем входящий RTP трафик)
//------------------------------------------------------------------
 void ipn_open_aics_socket(char* bind_addr,int port,char* igmp,int gport)
{
	struct aics_subscriber *aics=NULL;
	struct ao2_iterator i;


     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
          if(aics->type ==SUBSCRIBER_GROUP){
    	     aics->sock = ipn_open_socket( bind_addr,gport+(aics->idx*2),igmp,&aics->ip);
          }
          else{
     	     aics->sock = ipn_open_socket( bind_addr,port,NULL,&aics->ip);
     	     port+=2;
          }
    	  if(aics->sock < 0){
  		     ast_log(LOG_WARNING, "Unable to create AICS socket: %s[num=%s,type=%d]\n", strerror(errno),aics->number,aics->type);
    	  }
    	  ao2_ref(aics,-1);
      }

    ao2_iterator_destroy(&i);
}
//--------------------------------------------------------------------
//Закрытие сокетов aics абонентов для работы по IPN протоколу
//--------------------------------------------------------------------
static void  ipn_close_aics_socket(void)
{
	struct aics_subscriber *aics=NULL;
	struct ao2_iterator i;

     if(aics_space == NULL) return;
     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
    	  if(aics->sock > -1){
    			close( aics->sock );
    			aics->sock = -1;
    	  }
    	  ao2_ref(aics,-1);
      }

    ao2_iterator_destroy(&i);
}
//--------------------------------------------------------------------
//Обновляет статус IPN абонента и привязывает IP адрес к номеру
//вход: *num - ук-ль номер абонента
//вход: *ip - ук-ль IP адресa
//вход: status -статус IPN абонента
//--------------------------------------------------------------------
int ipn_update_status(const char* num,struct ast_sockaddr* ip,int status )
{
	struct ipn_subscriber *ipn = NULL ;

	ipn=find_ipn_subscriber(num);
	if(ipn){
		ast_sockaddr_copy(&ipn->ip,ip);
	    if(ipn->state !=status){
	    	ipn->state=status;
			if( ipn->state ) {
				ast_devstate_changed_literal( AST_DEVICE_BUSY, AST_DEVSTATE_CACHABLE, ipn->name );
			}
			else {
				ast_devstate_changed_literal( AST_DEVICE_NOT_INUSE, AST_DEVSTATE_CACHABLE, ipn->name );
			}
	    	ao2_ref(ipn,-1);
	    	return 1;
	    }
	    ao2_ref(ipn,-1);
	}
	return 0;
}
//--------------------------------------------------------------------
//Преобразование  бинарного массива в HEX формат
//вход:выход: *hex -ук-ль на HEX строку
//вход:  *str -ук-ль на массив
//вход:   len -размер массива
//--------------------------------------------------------------------
void str_to_hex(char* hex ,unsigned char* str,unsigned char len )
{
	int i,k=0,j;
	if (len) j=len;
	else j=strlen((const char*)str);
	if(j>32) j=32;

	for(i=0;i<j;i++) k+=sprintf(hex+k,"%3x",(unsigned char)*str++);
    *(hex+k)=0;
}

//--------------------------------------------------------------------
//Разбор расширенного заголовка  PTP IPN пакета
//вход:   *p - ук-ль на пакет
//вход:выход: *num - ук-ль номер абонента
//вход:выход: *prio - ук-ль на приоритет
//вход:выход: *line - ук-ль на байт линий
//выход: результат  TRUE -пакет опознан как  IPN
//--------------------------------------------------------------------
int parse_ipn_rtp_ext(char *p,char*num,unsigned char* prio,unsigned char* line)
{
	char hex[64];
	unsigned char len;

	str_to_hex(hex,(unsigned char*)p,16);
	ast_verb( 99, "Recv from[%s]\n",hex );

	    if((*p & 0xF0)!=0x10) return FALSE;
	    len= (*p++ & 0x0F)+1 ;
		memset(num, 0, 7);
	    strncpy(num,(const char*)p,len);
		p=p+7;
		if((*p++ & 0xF0)!=0x20) return FALSE;
	    *prio=*p;
		p++;
		if((*p++ & 0xF0)!=0x30) return FALSE;
	    *line=*p;
	    return TRUE;
}
//--------------------------------------------------------------------
// Создание фрейма из принятого пакета IPN RTP (фрейм содержит два пакета IPN RTP т.к период 10ms)
// вход: *start - ук-ль на голосовые данные в RTP пакете
// выход: ук-ль на созданный фрейм
//--------------------------------------------------------------------
static struct ast_frame *create_ipn_frame(const char *start)
{
	unsigned char *data;
	struct ast_frame *f;
//	static int s=0,t=0;

	data = ast_calloc(1, IPN_FRAME_DATA_LEN + 64);
	f = ast_calloc(1, sizeof(*f));
	if (f == NULL || data == NULL) {
		ast_log(LOG_WARNING, "--- frame error f %p data %p len %d format %d\n",
				f, data, IPN_FRAME_DATA_LEN, AST_FORMAT_ALAWDCN);
		if (f)
			ast_free(f);
		if (data)
			ast_free(data);
		return NULL;
	}
//	data[0]=s++;
	memcpy(&data[64], start, IPN_FRAME_DATA_LEN/2);
	f->data.ptr = &data[64];


	f->mallocd = AST_MALLOCD_DATA | AST_MALLOCD_HDR;
	f->datalen = IPN_FRAME_DATA_LEN;
	f->frametype = AST_FRAME_VOICE;
	f->subclass.format.id = AST_FORMAT_ALAWDCN;
	f->samples = IPN_FRAME_DATA_LEN;
	f->offset = 64;
	f->src = "ipn";
	f->seqno = 0;
	f->ts=0;
	f->delivery.tv_sec = 0;
	f->delivery.tv_usec = 0;

	AST_LIST_NEXT(f, frame_list) = NULL;
    return f;
}
//--------------------------------------------------------------------
// Добавление голосовых данных в ранее созданный фрейм
// вход: *f - ук-ль на фрейм
// вход: *start - ук-ль на голосовые данные в RTP пакете
// выход: ук-ль на  фрейм
//--------------------------------------------------------------------
static struct ast_frame *add_data_frame(struct ast_frame * f,const char *start)
{
	memcpy(f->data.ptr+IPN_FRAME_DATA_LEN/2, start, IPN_FRAME_DATA_LEN/2);
    return f;
}
//--------------------------------------------------------------------
//Анализ расширенного заголовка входящего IPN RTP пакета
//вход: *num -номер абонента
//вход:  prio-приоритет
//вход:  line-байт линий
//вход:  sock-сокет принявштй пакет
//выход:  ук-ль на структуру pvt определяющую текущее или новое соединение
//--------------------------------------------------------------------
struct ipn_pvt * init_incoming_call(const char *num,unsigned char prio,unsigned char line ,int sock,struct ast_sockaddr* ip)
{
	struct ast_channel *chan;
    struct ipn_pvt *pvt=NULL;
	struct aics_subscriber* aics=NULL;
    struct ipn_subscriber* ipn=NULL;
//	struct aics_proxy_params *aipp=NULL;

	   pvt= find_ipn_pvt(num); //среди текущих соединений ищем по номеру ipn абонента  
	   if(pvt == NULL){
			  // если нет: создаем  новое соединение
		   aics=find_aics_subscriber_sock(sock);
		   if (aics)  {
			    pvt=init_pvt(num ,aics->number,ip, prio ,line,TO_AICS);
		        ao2_ref(aics, -1);
		   }


		   if(pvt){
            ipn=find_ipn_subscriber(pvt->ipn_num);
//если создать канал с AST_STATE_RINGING при вызове группы из IPN попадаем в исключение (**Ошибка сегментирования) в  _AST_ANSWER  !!!!!
	        chan = ast_channel_alloc(0, AST_STATE_UP /*AST_STATE_RINGING*/, pvt->ipn_num, pvt->name, NULL, pvt->aics_num, ipn ? ipn->context :ipn_cfg.default_context, NULL, NULL, 0, "ipn/%s",num);
	     	if (!chan) {
	     		        ast_log(LOG_ERROR, "ast_channel_alloc error\n");
	     	    ao2_ref(pvt,-1);
	     		return 0;
	     	}
	     	pvt->owner = chan;
//		    ao2_link(pvts, pvt);
//		    ao2_unlock(pvts);

		    ast_channel_set_fd(chan,0,sock);
		    ast_channel_tech_set(chan, &ipn_channel_tech);
	     	ast_format_set(ast_channel_readformat(chan),AST_FORMAT_ALAWDCN, 0);
	     	ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_readformat(chan));
		   	ast_format_set(ast_channel_writeformat(chan),AST_FORMAT_ALAWDCN, 0);
	     	ast_format_cap_add(ast_channel_nativeformats(chan), ast_channel_writeformat(chan));
			ast_string_field_set(pvt, context, ast_channel_context(chan));
	     	ast_channel_tech_pvt_set(chan, pvt);
	    	struct aics_proxy_params *aipp;
	    	int i;
	    	aipp = ast_channel_proxy(chan);
	    	//  в sip телефонах приоритетность не поддержена все поступающие вызовы принимаются
	    	if(aipp !=NULL){
	    	    aipp->priority=	prio;
	    	    aipp->validity.as_bits.priority =1;

	    		for (i = 0; i < 8; i++) {
	    	    	if(line & (1<<i) ){
	    	    		aipp->relay[i].status=1;
	    	    	}
	    	    }
	    	    aipp->validity.as_bits.relay =1;
	    	}
	    	else{
     	        ast_log(LOG_ERROR, "Not aics proxy!\n");

	    	}
	     	ast_channel_unlock(chan);
	     	if (ast_pbx_start(chan)) {
	     			ast_channel_hangupcause_set(chan, AST_CAUSE_SWITCH_CONGESTION);
	     			ast_hangup(chan);
	     			chan = NULL;
	     	        ast_log(LOG_ERROR, "ast_pbx_start error\n");
	     	 }
		   //  ast_log(LOG_NOTICE, "pvt=%s<->%s ;prio =%d ;line =%d ;fd=%d \n",pvt->ipn_num,pvt->aics_num,prio,line,sock);
	        return pvt;
	     }
	   }
	   else{
		   //если есть: по номеру сокета принявшего пакет определяем номер aics абонента  получателя пакета
		  aics=NULL;
	      aics=find_aics_subscriber_sock(sock);

		  if( (!strcasecmp(pvt->ipn_num,num)) && //
			  (aics)	 &&
			  (!strcasecmp(pvt->aics_num,aics->number)) &&
			  (pvt->rtp.prio ==prio) 
//			  && (pvt->rtp.line == line)
			)
		  {  //если номер получателя и поля приоритет и линии равны
			 // возвращаем структуру соединения pvt  для последующего формирования фрейма , записи в очередь и отпрвки получателю (ipn_read)
			  if(aics) {
				  aics->timer_hangup=0;
				  ao2_ref(aics, -1);
			  }
			  return pvt;
		    // ast_log(LOG_NOTICE, "*");
		  }
		  else{
			  if( (!strcasecmp(pvt->ipn_num,num)) &&
				  (aics)	 &&
				  (!strcasecmp(pvt->aics_num,aics->number)) 
//				  && (pvt->rtp.line == line)
				)
			  {
				  //если номер получателя и  линии равны, приоритет не равен
				  // возвращаем структуру соединения pvt  для последующего формирования фрейма , записи в очередь и отпрвки получателю (ipn_read)
				  if(aics){
				   aics->timer_hangup=0;
				   ao2_ref(aics, -1);
				  }
				  return pvt;
			  }
			  else{

			  }
//			 ast_verbose("#");
		   //  ast_log(LOG_NOTICE,"DROP %s=%s ;%s=%s ;%d =%d ;%d=%d \n",pvt->ipn_num,num,pvt->aics_num,aics->number,pvt->rtp.prio,prio,pvt->rtp.line,line);
		  }
		  if(aics) ao2_ref(aics, -1);
	      ao2_ref(pvt, -1);
	   }
	   return NULL;
}

//--------------------------------------------------------------------
//Callback на чтение открытых сокетов
//--------------------------------------------------------------------
int ipnsock_read_real(int fd)
{
	int res;
	static char readBuf[65535];
	struct ast_sockaddr fromAddr;
	char num[NUMBER_MAX_LEN],ext[NUMBER_MAX_LEN];
	unsigned char status,prio,line;
	struct ipn_pvt *pvt=NULL;
	int queued_frames;
	struct ast_frame *cur;

	res = ast_recvfrom( fd, readBuf, sizeof(readBuf)-1, 0, &fromAddr );
	if( res < 0 ) {
        ast_verb( 100, "Recv error from[%s]\n",ast_sockaddr_stringify(&fromAddr) );
		return 1;
	}

	readBuf[res] = 0x00;
//	ast_verb( 1, "Recv %u bytes from %s\n", res, ast_sockaddr_stringify(&fromAddr) );
	if(ipn_parse_status(readBuf,res,num,&status,ext)){
	 //  ast_verb( 5, "Recv from[%s] num=%s,status=%d\n",ast_sockaddr_stringify(&fromAddr),num,status );
	   ipn_update_status(num,&fromAddr,status);
	}
	else{
		if(res<=188){
		  if( parse_ipn_rtp_ext(&readBuf[16],num,&prio,&line)){

		      ast_verb( 99, "Recv from[%s] num=%s,prio=%d,line=%d\n",ast_sockaddr_stringify(&fromAddr),num,prio,line );
		      pvt = init_incoming_call(num,prio,line,fd,&fromAddr);
		      if(pvt){
			  //    ao2_lock(pvt);
		    	  // накапливаем фрейм из двух IPN RTP пакетов
		    	  if(pvt->frame){
		    		  add_data_frame(pvt->frame,&readBuf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN]);
		    		  AST_LIST_INSERT_TAIL(&pvt->rd_queue, pvt->frame, frame_list);
		    		  pvt->frame= NULL;
                        queued_frames=0;
					  	AST_LIST_TRAVERSE(&pvt->rd_queue, cur, frame_list) {
		                  queued_frames++;
	                    }
		             //    ast_verb( 1, "queued_frames=%d\n",queued_frames);

//		    		  str_to_hex(hex,&readBuf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN],10);
//		    		  ast_log(LOG_NOTICE, "2[%s]\n",hex);
		    	  }
		    	  else{
		    		  pvt->frame=create_ipn_frame(&readBuf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN]);
//		    		  str_to_hex(hex,&readBuf[IPN_RTP_HEADER_LEN + IPN_RTP_EXT_LEN],10);
//		    		  ast_log(LOG_NOTICE, "1[%s]\n",hex);
		    	  }
			//      ao2_unlock(pvt);
		    	     ao2_ref(pvt, -1);
				     return 1; 
		      }
		  }
		}
	}
	return 1;


}
//--------------------------------------------------------------------
//Callback на чтение открытых сокетов
//--------------------------------------------------------------------
static int ipnsock_read(int *id, int fd, short events, void *ignore)
{
    struct aics_subscriber* aics=NULL;
     
     aics=find_aics_subscriber_sock(fd);
     if(aics && aics->pvt){
	//	 ast_log(LOG_NOTICE, "DROP\n");
		 return 1;
	 }
    return(ipnsock_read_real(fd));

}

//--------------------------------------------------------------------
//Подключение aics сокетов  к io контексту и установка callback
//--------------------------------------------------------------------
void ipn_update_io(void)
{
	struct aics_subscriber *aics=NULL;
	struct ao2_iterator i;

     if(aics_space == NULL) return;
     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
    	  if(aics->sock > -1){
				if( aics->io_id ) aics->io_id = ast_io_change(io, aics->io_id, aics->sock, NULL, 0,  NULL );
				else aics->io_id = ast_io_add(io, aics->sock, ipnsock_read, AST_IO_IN, NULL );
		  }
		  else if( ipnsock_read_id ) {
				ast_io_remove(io, aics->io_id );
				aics->io_id = NULL;
		  }
          ao2_ref(aics,-1);
      }

    ao2_iterator_destroy(&i);
}
//--------------------------------------------------------------------
//Открытие сокета
//         *bind_addr - IP адрес
//          port  - порт
//         *igmp  -групповой IP адрес
//--------------------------------------------------------------------
int ipn_open_socket(char* bind_addr,int port,char* igmp ,struct ast_sockaddr* ip)
{
	int sock = -1;
	struct ast_sockaddr bindaddr;


	ast_sockaddr_parse(&bindaddr,bind_addr,0);
	if( !ast_sockaddr_port(&bindaddr) ) {
		ast_sockaddr_set_port(&bindaddr,port);
	}
	if(ip !=NULL)	ast_sockaddr_copy(ip,&bindaddr);

	sock = socket( ast_sockaddr_is_ipv6(&bindaddr) ? AF_INET6 : AF_INET, SOCK_DGRAM, 0 );
	if( sock < 0 ) {
		ast_log(LOG_WARNING, "Unable to create IPN socket: %s\n", strerror(errno));
		return -1;
	}
	else {
		//? Allow IPN clients on the same host to access us
		const int reuseFlag = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*) &reuseFlag, sizeof(reuseFlag));
		char ttl = 16;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl, sizeof(ttl));
	//	char loop = 0;
	//	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&loop, sizeof(loop));
			// Enable packet fragmentation
		ast_enable_packet_fragmentation(sock);
			// Bind IPN socket
		if( ast_bind(sock, &bindaddr) < 0 ) {
				ast_log(LOG_WARNING, "Failed to bind to %s: %s\n", ast_sockaddr_stringify(&bindaddr), strerror(errno));
				close(sock);
				return -1;
		}
		else {
				ast_verb(50, "IPN listening on %s\n", ast_sockaddr_stringify(&bindaddr));
		}
	}
	if( sock && (igmp != NULL) ) {
		// Set IPN packet QoS
		ast_set_qos(sock, global_tos_ipn, global_cos_ipn, "IPN");
		// Join to multicast group
		struct {
			struct in_addr imr_multiaddr;
			struct in_addr imr_interface;
		} mreg;
		mreg.imr_multiaddr.s_addr = inet_addr(igmp);
		mreg.imr_interface.s_addr = htonl(INADDR_ANY);
		if( setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*) &mreg, sizeof(mreg)) < 0 ) {
			ast_log(LOG_WARNING, "Failed to join on multicast group: %s[%s] \n", strerror(errno),ipn_cfg.igmp_addr);
			close(sock);
			return -1;
		}
	}
	return sock;
}

//*********************************************************
//функции работы с коллекциями абонентов ipn_space aics_space
//*********************************************************

static int aics_space_hash_cb(const void *obj, const int flags)
{
	const struct aics_subscriber *sub = obj;

	return ast_str_case_hash(sub->number);
}

static int aics_space_cmp_cb(void *obj, void *arg, int flags)
{
	struct aics_subscriber *sub = obj,*sub2=arg;

	return !strcasecmp(sub->number, sub2->number) ? CMP_MATCH | CMP_STOP : 0;
}

static int aics_space_cmp_cb_sock(void *obj, void *arg, int flags)
{
	struct aics_subscriber *sub = obj;

	return (sub->sock == (int)arg ? CMP_MATCH | CMP_STOP : 0);
}
static int ipn_space_hash_cb(const void *obj, const int flags)
{
	const struct ipn_subscriber *sub = obj;

	return ast_str_case_hash(sub->number);
}
static int ipn_space_cmp_cb(void *obj, void *arg, int flags)
{
	struct ipn_subscriber *sub = obj,*sub2=arg;

	return !strcasecmp(sub->number, sub2->number) ? CMP_MATCH | CMP_STOP : 0;
}
static void ipn_sub_destructor(void *obj)
{
	struct ipn_subscriber *tmp_sub = obj;
	//ast_log(LOG_NOTICE, "ipn_sub_destructor\n");

	ast_string_field_free_memory(tmp_sub);
}
static void aics_sub_destructor(void *obj)
{
	struct aics_subscriber *tmp_sub = obj;
	//ast_log(LOG_NOTICE, "aics_sub_destructor\n");

	ast_string_field_free_memory(tmp_sub);
}

static struct ipn_subscriber* find_ipn_subscriber(const char *number)
{
	struct ipn_subscriber tmp_sub = {
		.number = number,
	};

	return ao2_find(ipn_space, &tmp_sub, OBJ_POINTER);
}
static struct aics_subscriber* find_aics_subscriber(const char *number)
{
	struct aics_subscriber tmp_sub = {
		.number = number,
	};

	return ao2_find(aics_space, &tmp_sub, OBJ_POINTER);
}
struct aics_subscriber* find_aics_subscriber_sock(int sock)
{
  struct aics_subscriber *a=NULL;
	  a=ao2_callback(aics_space,0,aics_space_cmp_cb_sock,(void*)sock);
	  return(a);
}

static void destroy_subscribers(void)
{
	struct ipn_subscriber *ipn=NULL ;
	struct aics_subscriber *aics=NULL;
	struct ipn_pvt *pvt=NULL;
	struct ao2_iterator i;
	if(ipn_space){
       ao2_lock(ipn_space);
      i=ao2_iterator_init(ipn_space,0);
      while((ipn=ao2_iterator_next(&i))){
    	  ao2_unlink(ipn_space,ipn);
    	  ao2_ref(ipn,-1);
      }
      ao2_unlock(ipn_space);
      ao2_ref(ipn_space,-1);
      ao2_iterator_destroy(&i);
	}
	if(aics_space){
     ao2_lock(aics_space);
     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
    	  ao2_unlink(aics_space,aics);
    	  ao2_ref(aics,-1);
      }
     ao2_unlock(aics_space);
     ao2_ref(aics_space,-1);
     ao2_iterator_destroy(&i);
	}

	if(pvts){
     ao2_lock(pvts);
     i=ao2_iterator_init(pvts,0);
      while((pvt=ao2_iterator_next(&i))){
    	  ao2_unlink(pvts,pvt);
    	  ao2_ref(pvt,-1);
      }
     ao2_unlock(pvts);
     ao2_ref(pvts,-1);
     ao2_iterator_destroy(&i);
	}


}

//*********************************************************
//добавление абонента в соответствующую коллекцию по типу
//вход:*num -номер абонента
//вход:*v -параметры абонента
//выход: результат  "0" - не добавлен
//*********************************************************

static int add_subscriber(const char *num, struct ast_variable *v)

{
	struct ipn_subscriber *ipn=NULL ;
	struct aics_subscriber *aics=NULL;

	for (; v ; v = v->next) {
//		ast_log(LOG_NOTICE, "[%s]: %s = %s\n",num, v->name, v->value );
		if (!strcasecmp(v->name, "type")) {
			if((!strcasecmp(v->value, "ipn_user"))|| (!strcasecmp(v->value, "ipn_group"))  ){
				ipn=find_ipn_subscriber(num);
				if(!ipn)
				{
				 ipn=ao2_alloc(sizeof(*ipn),ipn_sub_destructor);
				 if(!strcasecmp(v->value, "ipn_user"))ipn->type=SUBSCRIBER_USER;else ipn->type=SUBSCRIBER_GROUP;
				 break;
				}
				else{
			        ao2_ref(ipn,-1);
					return 0;
				}
			}
			else if((!strcasecmp(v->value, "aics_user"))|| (!strcasecmp(v->value, "aics_group"))  ){
				aics=find_aics_subscriber(num);
				if(!aics)
				{
				 aics=ao2_alloc(sizeof(*aics),aics_sub_destructor);
				 if(!strcasecmp(v->value, "aics_user"))aics->type=SUBSCRIBER_USER;else aics->type=SUBSCRIBER_GROUP;
				 break;
				}
				else{
			        ao2_ref(aics,-1);
					return 0;
				}
			}
		}
	}
    if(ipn){
        if (ast_string_field_init(ipn, 32))return 0;
        ao2_lock(ipn_space);
        ast_string_field_set(ipn, number, num);
        ast_string_field_set(ipn, context,ipn_cfg.default_context);
    	for (; v ; v = v->next) {
			if(!strcasecmp(v->name, "context")){
		        ast_string_field_set(ipn, context,v-> value);
    	    }
			else if(!strcasecmp(v->name, "group_index")){
		        ipn->idx= atoi(v->value);

    	    }
    	}
    	if(ipn->type==SUBSCRIBER_GROUP){
    		ast_sockaddr_parse(&ipn->ip,ipn_cfg.igmp_addr,0);
    		if( !ast_sockaddr_port(&ipn->ip) ) {
    			ast_sockaddr_set_port(&ipn->ip,ipn_cfg.groups_port+(ipn->idx*2));
    		}
    	}
    	ast_string_field_build(ipn,name,"IPN/%s",ipn->number);
//	    sprintf(ipn->name,"IPN/%s",ipn->number);
        ao2_link(ipn_space, ipn);
        ao2_unlock(ipn_space);
        ao2_ref(ipn,-1);
		return 1;
    }
    else if(aics){
    	aics->idx=0;
    	if (ast_string_field_init(aics, 32))return 0;
        ao2_lock(aics_space);
        ast_string_field_set(aics, number, num);
    	for (; v ; v = v->next) {
			if(!strcasecmp(v->name, "tech")){
		        ast_string_field_set(aics, tech, v->value);
    	    }
			else
			if(!strcasecmp(v->name, "group_index")){
				aics->idx= atoi(v->value);
				if(aics->idx>=ipn_cfg.group_count){
		           aics->idx=0;
				}			
    	    }
  	        else if(!strcasecmp(v->name, "duplex")){
				if(!strcasecmp(v->value, "yes"))
					aics->duplex=1;
				else
					aics->duplex=0;
    	    }
    	}
		if(aics->type==SUBSCRIBER_USER){
		  ast_string_field_build(aics,name,"%s%s",aics->tech,aics->number);
		  aics->astate=ast_device_state(aics->name);
		}
		else{
			aics->duplex=0;
		}
    	aics->sock=-1;
    	aics->timer_hangup=IPN_TIMER_HANGUP*2;
        ao2_link(aics_space, aics);
        ao2_unlock(aics_space);
        ao2_ref(aics,-1);
		return 1;
    }
    return 0;
}

//*********************************************************
//	Channel IPN load configuration file
//*********************************************************

// IPN reload configuration
static int ipn_reload_config( enum channelreloadreason reason )
{
	struct ast_config *cfg;
	struct ast_variable *v;
	struct ast_flags cfgFlags = { reason == CHANNEL_MODULE_LOAD ? 0 : CONFIG_FLAG_FILEUNCHANGED };
	char* cat=NULL;

	cfg = ast_config_load(ipn_config_file, cfgFlags );
	if( !cfg ) {
		ast_log(LOG_NOTICE, "Unable to load config %s\n", ipn_config_file );
		ast_verb(50, "IPN: Unable to load config %s\n", ipn_config_file );
		return -1;
	}
	else if( cfg == CONFIG_STATUS_FILEUNCHANGED ) {
		ast_verb(50, "IPN: config file is unchanged.\n");
		return 1;
	}
	else if( cfg == CONFIG_STATUS_FILEINVALID ) {
		ast_log(LOG_ERROR, "Contents of %s are invalid and cannot be parsed\n", ipn_config_file );
		ast_verb(50, "IPN: Contents of %s are invalid and cannot be parsed\n", ipn_config_file );
		return 1;
	}

	// Set define parameters
	global_tos_ipn = DEFAULT_TOS_IPN;
	global_cos_ipn = DEFAULT_COS_IPN;

	// Read the [general] config section
	ast_copy_string(ipn_cfg.bind_addr, DEFAULT_BIND_ADDR, sizeof(DEFAULT_BIND_ADDR));
	ast_copy_string(ipn_cfg.igmp_addr, DEFAULT_IGMP_ADDR, sizeof(DEFAULT_IGMP_ADDR));
	ipn_cfg.status_port= DEFAULT_STATUS_PORT;
	ipn_cfg.users_port= DEFAULT_USERS_PORT;
	ipn_cfg.groups_port= DEFAULT_GROUPS_PORT;
    ipn_cfg.group_count=64;
	for( v=ast_variable_browse(cfg,"general"); v; v=v->next ) {
		ast_verb(50,"IPN: %s = %s\n", v->name, v->value );
		if (!strcasecmp(v->name, "ipn_bind_address")) {
			ast_copy_string(ipn_cfg.bind_addr, v->value, sizeof(ipn_cfg.bind_addr));
		}else if (!strcasecmp(v->name, "ipn_igmp_address")) {
			ast_copy_string(ipn_cfg.igmp_addr, v->value, sizeof(ipn_cfg.igmp_addr));
            if( ! IN_MULTICAST(ntohl(inet_addr(ipn_cfg.igmp_addr))))
			{
			  ast_log(LOG_WARNING, "IP address is not multicast %s.\n",ipn_cfg.igmp_addr);
			  ast_copy_string(ipn_cfg.igmp_addr, DEFAULT_IGMP_ADDR, sizeof(DEFAULT_IGMP_ADDR));
			}
		}else if (!strcasecmp(v->name, "context")) {
			ast_copy_string(ipn_cfg.default_context, v->value, sizeof(ipn_cfg.default_context));
		}else if (!strcasecmp(v->name, "ipn_status_port")) {
			ipn_cfg.status_port= atoi(v->value);
		}else if (!strcasecmp(v->name, "ipn_users_port")) {
			ipn_cfg.users_port= atoi(v->value);
		}else if (!strcasecmp(v->name, "ipn_groups_port")) {
			ipn_cfg.groups_port= atoi(v->value);
		}
		else if (!strcasecmp(v->name, "ipn_groups_count")) {
			ipn_cfg.group_count= atoi(v->value);
			if(ipn_cfg.group_count >IPN_GROUP_MAX) ipn_cfg.group_count=IPN_GROUP_MAX;
		}
	    else if (!strcasecmp(v->name, "ipn_interfase")) {
			ast_copy_string(ipn_cfg.ifase, v->value, sizeof(ipn_cfg.ifase));
		}	
	}
	ast_sockaddr_parse(&ipn_cfg.status_addr ,ipn_cfg.igmp_addr,0);
	if( !ast_sockaddr_port(&ipn_cfg.status_addr) ) {
		ast_sockaddr_set_port(&ipn_cfg.status_addr,ipn_cfg.status_port);
	}
    get_hwaddr(ipn_cfg.hwaddr); 

	ipn_close_aics_socket();
	destroy_subscribers();
	ipn_space=ao2_container_alloc(IPN_MAX_SUBSCRIBER,ipn_space_hash_cb,ipn_space_cmp_cb);
	aics_space=ao2_container_alloc(AICS_MAX_SUBSCRIBER,aics_space_hash_cb,aics_space_cmp_cb);
	pvts=ao2_container_alloc(AICS_MAX_SUBSCRIBER,ipn_pvt_hash_cb,ipn_pvt_cmp_cb);
	while ( (cat = ast_category_browse(cfg, cat)) ) {
		add_subscriber(cat, ast_variable_browse(cfg, cat));
	}

	ipn_open_aics_socket(ipn_cfg.bind_addr,ipn_cfg.users_port, ipn_cfg.igmp_addr,ipn_cfg.groups_port);
	// Open IPN multicast socket
	ast_mutex_lock(&ipnsock_lock);
	if( ipnsock > -1 ) {
		close( ipnsock );
		ipnsock = -1;
	}
	if( ipnsock < 0 ) {
		ipnsock = ipn_open_socket( ipn_cfg.bind_addr,ipn_cfg.status_port, ipn_cfg.igmp_addr,NULL );
		if( ipnsock < 0 ) {
			ast_log(LOG_WARNING, "Unable to create IPN socket: %s\n", strerror(errno));
			ast_mutex_unlock(&ipnsock_lock);
			ast_config_destroy(cfg);
			return -1;
		}
	}
	ast_mutex_unlock(&ipnsock_lock);
	ast_config_destroy(cfg);
	return 0;
}
//*********************************************************
//таймер ~ 10 ms
//*********************************************************

void ipn_timer(void)
{
	struct aics_subscriber *aics=NULL;
	struct ao2_iterator i;
    int state;

     i=ao2_iterator_init(aics_space,0);
     while((aics=ao2_iterator_next(&i))){
    	  ao2_lock(aics);
    	  if(aics->type== SUBSCRIBER_USER) { //отправка статусов AICS абонентов с периодом таймера
   		    aics->astate=ast_device_state(aics->name);
    		if (aics->timer==0){
	          ast_verb( 100, "Send status[%s]\n",aics->name );
    		  if(aics->pvt){
        	    if(aics->dir ==FROM_AICS) state= ELLANSTAT_ATO;
        	    else state=ELLANSTAT_AFROM;
   	            ipn_transmit_status(aics->sock,aics->number ,state ,aics->pvt->ipn_num);
    		  }
    		  else{
				  if((aics->astate ==AST_DEVICE_INVALID) || (aics->astate ==AST_DEVICE_UNAVAILABLE)) continue;
    			  if(aics->astate != AST_DEVICE_NOT_INUSE) state=ELLANSTAT_BUSY;
				  else state=ELLANSTAT_IDLE;
    	         ipn_transmit_status(aics->sock,aics->number ,state ,"");
    		  }
    	      aics->timer++;
    	      ao2_ref(aics,-1);
    	      break;
    	    }
    	    else{
    	      if( aics->timer > 100) aics->timer=0;
    	      else aics->timer++;
    	    }
    	    if(aics->pvt)
    	    {
    	       if(aics->timer_hangup == IPN_TIMER_HANGUP){
    	    	  if((aics->duplex == 0) &&(aics->dir==TO_AICS)){
    	    		 // ast_log(LOG_NOTICE, " 1 timer_hangup !!!!!!!!\n");
	ast_log(LOG_NOTICE, "TIMER softhangup %s %s \n",aics->pvt->ipn_num,aics->pvt->aics_num);			 
  			          ast_softhangup_nolock(aics->pvt->owner,AST_SOFTHANGUP_DEV);
    	          }
//    	    	  else if((aics->duplex) &&(aics->astate== AST_EXTENSION_RINGING)){
    	    	  else if((aics->duplex) &&((aics->astate== AST_DEVICE_RINGING ) ||(aics->astate== AST_DEVICE_RINGINUSE ))){
    	    		//  ast_log(LOG_NOTICE, "2 timer_hangup !!!!!!!!\n");
  			          ast_softhangup_nolock(aics->pvt->owner,AST_SOFTHANGUP_DEV);
				  }
//				  else if(aics->astate== AST_EXTENSION_INUSE)
				  else if(aics->astate== AST_DEVICE_INUSE)
				  {
	    		    //ast_log(LOG_NOTICE, "ssrc = ast_random !!!!!!!!\n");
		            aics->pvt->rtp.ssrc = ast_random();
    	    	  }
    	    	  aics->timer_hangup++;
    	       }
    	       if(aics->timer_hangup<=IPN_TIMER_HANGUP) aics->timer_hangup++;
    	    }
    	 }
    	 else {
     	    if((aics->pvt) && (aics->dir==TO_AICS))
     	    {
     	       if(aics->timer_hangup == IPN_TIMER_HANGUP){
            	//  ast_log(LOG_NOTICE, "timer_hangup !!!!!!!!\n");
   			      ast_softhangup_nolock(aics->pvt->owner,AST_SOFTHANGUP_DEV);
   			    //  ao2_unlink(pvts,aics->pvt);
   			    //  ao2_ref(aics->pvt,-1);
           	      aics->timer_hangup++;
       	       }
     	       if(aics->timer_hangup<=100) aics->timer_hangup++;
     	    }
         }
   	     ao2_unlock(aics);
 		 ao2_ref(aics,-1);
    }
    ao2_iterator_destroy(&i);
}

//static int sched_cb(const void *data)
//{
//
//		 ast_log(LOG_NOTICE, "sched_cb\n");
//	return 0;
//}
//*********************************************************
//callback	на изменения состояний абонентов AICS
//*********************************************************

//int info_state_cb(  char* con,char *id, struct ast_state_cb_info *info, void* data)
//{
//    ast_log(LOG_WARNING, "info_state-%s[%s]=%d\n",con,id,info->exten_state);
//	struct aics_subscriber* aics=find_aics_subscriber(id);
//    if (aics){
//      aics->state=info->exten_state;
//      ao2_ref(aics,-1);
//    }
//    return 0;
//}

//*********************************************************
//	Channel IPN monitor thread declarartion
//*********************************************************

// IPN monitor thread function
static void *ipn_monitor_do( void *data )
{
	int res;
	int reloading;
	enum channelreloadreason reason;
	struct timeval timer = { 0, 0 };
	gettimeofday(&timer, NULL);
	int t;

	ast_verb(50,"IPN monitor thread started.\n" );


//	res =ast_extension_state_add(NULL,NULL,info_state_cb,NULL);

//    if ((ast_sched_add(sched, 10000, sched_cb, NULL)) == -1) {
//		 ast_log(LOG_NOTICE, "Failed to add scheduler entry\n");
//	}
//	ast_sched_runq(sched);
	for(;;) {

		// Check for reload request
		ast_mutex_lock(&ipn_reload_lock);
		reloading = ipn_reload_flag;
		reason = ipn_reload_reason;
		ipn_reload_flag = FALSE;
		ast_mutex_unlock(&ipn_reload_lock);
		if( reloading ) {
			ast_verb(50,"Reloading IPN...\n");
			// Reload IPN configuration
			res=ipn_reload_config(reason);
			// Change I/O fd to UDP multicast socket
			if(res==0){
			  if( ipnsock > -1 ) {
				if( ipnsock_read_id ) ipnsock_read_id = ast_io_change(io, ipnsock_read_id, ipnsock, NULL, 0,  NULL );
				else ipnsock_read_id = ast_io_add(io, ipnsock, ipnsock_read, AST_IO_IN, NULL );
			  }
			  else if( ipnsock_read_id ) {
				ast_io_remove(io, ipnsock_read_id );
				ipnsock_read_id = NULL;
			  }
			  ipn_update_io();
			}
			init_state_aics();
		}

		pthread_testcancel(); // Thread is stopped

		// Wait timeout from sheduler

		// Wait timeout from I/O
		ast_io_wait(io, 10);
	//	init_state_aics();
	//	res = ast_sched_wait(sched);
	//     ast_sched_runq(sched);
	//	 ast_log(LOG_NOTICE, "timer=%d\n",res);
         t=ast_tvdiff_ms(ast_tvnow(), timer);
    	if(t >=10){
		  if( ipnsock > -1 ) ipn_timer();
          gettimeofday(&timer, NULL);
	//      ast_log(LOG_WARNING, "TIMER IPN=%d\n",t);
		} 


	}
	return 0;
}


// Start/WakeUp IPN monitor thread
static int ipn_monitor_restart( enum channelreloadreason reason )
{

	// If we're supposed to be stopped -- stay stopped
	if( ipn_monitor_thread == AST_PTHREADT_STOP ){
		ast_verb(50,"IPN monitor already AST_PTHREADT_STOP\n");
		return 0;
	}

	// Previous IPN reload not yet done
	ast_mutex_lock(&ipn_reload_lock);
	if( ipn_reload_flag ) {
		ast_verb(50,"Previous IPN reload not yet done.\n");
	}
	else {
		ipn_reload_flag = TRUE;
		ipn_reload_reason = reason;
	}
	ast_mutex_unlock(&ipn_reload_lock);


	ast_mutex_lock(&ipn_monitor_lock);

	// Can't kill myself
	if( ipn_monitor_thread == pthread_self() ) {
		ast_mutex_unlock(&ipn_monitor_lock);
		ast_log(LOG_ERROR, "Can't kill myself.\n");
		return -1;
	}

	// Wake up thread
	if( ipn_monitor_thread != AST_PTHREADT_NULL ) {
		pthread_kill(ipn_monitor_thread, SIGURG);
	}
	// Start new monitor thread
	else {
		if( ast_pthread_create_background( &ipn_monitor_thread, NULL, ipn_monitor_do 	, NULL ) < 0 ) {
			ast_mutex_unlock(&ipn_monitor_lock);
			ast_log(LOG_ERROR, "Unable to start monitor thread.\n");
			return -1;
		}
	}

	ast_mutex_unlock(&ipn_monitor_lock);
	return 0;
}

void init_state_aics()
{
	struct aics_subscriber *aics=NULL;
	struct ao2_iterator i;
     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
		  aics->astate=ast_device_state(aics->name);
	  }
     ao2_iterator_destroy(&i);

}


void ipn_show(struct ast_cli_args *a)
{
	struct ipn_subscriber *ipn=NULL ;
	struct aics_subscriber *aics=NULL;
//	struct ipn_pvt *pvt=NULL;
	struct ao2_iterator i;
    int j;
//	ipn_transmit_status(0,"234",5,"100");
  for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
  ast_cli(a->fd,"%40s\n","IPN");
  for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
  ast_cli(a->fd,"%-10s|%-10s|%-30s|%-10s|%-10s|%-10s\n","number","name","ip.addr","context","type","state");
  for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");

     i=ao2_iterator_init(ipn_space,0);
      while((ipn=ao2_iterator_next(&i))){
    	  ast_cli(a->fd,"%-10s|%-10s|%-30s|%-10s|%-10s|%-10d\n",ipn->number,ipn->name,ast_sockaddr_stringify(&ipn->ip),ipn->context,ipn->type ? "group":"user",ipn->state);
    	  ao2_ref(ipn,-1);
      }
      for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
       ast_cli(a->fd,"%40s\n","AICS");
      for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
      ast_cli(a->fd,"%-20s|%-10s|%-30s|%-10s|%-10s|%-10s|%-10s|%-10s\n","name","number","ip.addr","type","duplex","connect","aics_state","timer");
      for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");

     i=ao2_iterator_init(aics_space,0);
      while((aics=ao2_iterator_next(&i))){
          ast_cli(a->fd,"%-20s|%-10s|%-30s|%-10s|%-10s|%-10s|%-10d|%-10d\n",aics->name,aics->number,ast_sockaddr_stringify(&aics->ip),aics->type ? "group":"user",aics->duplex ? "yes":"no",aics->pvt ? aics->pvt->ipn_num:"null",aics->astate,aics->timer_hangup);
    	  ao2_ref(aics,-1);
      }

    ao2_iterator_destroy(&i);

}
void calls_show(struct ast_cli_args *a)
{
	struct ipn_pvt *pvt=NULL;
	struct ao2_iterator i;
    int j;

    for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
     ast_cli(a->fd,"%40s\n","Connections");
    for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");
    ast_cli(a->fd,"%-10s|%-10s|%-10s|%-20s|%-10s|%-10s\n","ipn.num","direction","aics.num","context","priority","line");
    for (j=0;j<20;j++) ast_cli(a->fd,"-----");ast_cli(a->fd,"\n");

    i=ao2_iterator_init(pvts,0);
     while((pvt=ao2_iterator_next(&i))){
      ast_cli(a->fd,"%-10s|%-10s|%-10s|%-20s|%-10d|%-10d\n",pvt->ipn_num,pvt->dir ?"->":"<-" ,pvt->aics_num,pvt->context,pvt->rtp.prio,pvt->rtp.line);
//   	  ast_cli(a->fd,"PVTS: dialog %s<->%s[%s] ; context=%s ;prio=%d ;line=%d \n",pvt->ipn_num,pvt->aics_num,ast_sockaddr_stringify(&pvt->rtp.ip),pvt->context,pvt->rtp.prio,pvt->rtp.line);
   	  ao2_ref(pvt,-1);
     }
    ao2_iterator_destroy(&i);


}


//*********************************************************
//	CLI interface
//*********************************************************

// CLI: Reload IPN
static char* cli_ipn_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch(cmd) {
	case CLI_INIT:
		e->command = "ipn reload";
		e->usage =
			"Usage: ipn reload\n"
			"		Reloads IPN configuration from ipn.conf\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	// Restart monitor thread
	ipn_monitor_restart( (a && a->fd) ? CHANNEL_CLI_RELOAD : CHANNEL_MODULE_RELOAD );

	return CLI_SUCCESS;
}

// CLI: Lists all known IPN users
static char* cli_ipn_show_users(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch(cmd) {
	case CLI_INIT:
		e->command = "ipn show users";
		e->usage =
			"Usage: ipn show users\n"
			"		Lists all known IPN users\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	//...
	//...
	//...
	ipn_show(a);
	return CLI_SUCCESS;
}


// CLI: Lists all known IPN calls
static char* cli_ipn_show_calls(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch(cmd) {
	case CLI_INIT:
		e->command = "ipn show calls";
		e->usage =
			"Usage: ipn show calls\n"
			"		Lists all current connections\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	//...
	//...
	//...
	calls_show(a);
	return CLI_SUCCESS;
}

// CLI: Show basic module settings

static char* cli_ipn_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{

	switch(cmd) {
	case CLI_INIT:
		e->command = "ipn show";
		e->usage =
			"Usage: ipn show\n"
			"		Show basic module settings\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}


	ast_cli(a->fd, "  version      : %s\n",IPN_VERSION );
	ast_cli(a->fd, "  igmp.addr    : %s\n",ipn_cfg.igmp_addr);
	ast_cli(a->fd, "  status.port  : %s[soc=%d:io=%p]\n" ,ast_sockaddr_stringify(&ipn_cfg.status_addr),ipnsock,ipnsock_read_id);
	ast_cli(a->fd, "  ipn.interfase/ipn.mac.addr  : %s/%s\n" ,ipn_cfg.ifase,ipn_cfg.hwaddr);
	ast_cli(a->fd, "  group.count  : %d\n" ,ipn_cfg.group_count);


	return CLI_SUCCESS;
}
/*
// CLI: Show details on specific IPN group
static char* cli_ipn_show_group(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch(cmd) {
	case CLI_INIT:
		e->command = "ipn show group";
		e->usage =
			"Usage: ipn show group <name>\n"
			"		Show details on specific IPN group\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if( a->argc < 4 )
		return CLI_SHOWUSAGE;

	//...
	//...
	//...

	ast_cli(a->fd, "  * Name      : %s\n", a->argv[3] );
	ast_cli(a->fd, "  Context     : \n" );
	ast_cli(a->fd, "  GroupIndex  : \n" );

	return CLI_SUCCESS;
}
*/

static struct ast_cli_entry cli_ipn[] = {
		AST_CLI_DEFINE(cli_ipn_reload, 			"Reload IPN configuration"),
		AST_CLI_DEFINE(cli_ipn_show, 		    "Show basic module settings"),
		AST_CLI_DEFINE(cli_ipn_show_users, 		"List defined IPN users"),
//		AST_CLI_DEFINE(cli_ipn_show_group, 		"Show details on specific IPN group"),
		AST_CLI_DEFINE(cli_ipn_show_calls,		"List current connections"),
};

//*********************************************************
//
//*********************************************************





//*********************************************************
//	Load / Unload / Reload the module interface
//*********************************************************

static int __unload_module(void)
{
	// Unregister CLI interface
	ast_cli_unregister_multiple(cli_ipn, ARRAY_LEN(cli_ipn));

	// Unregister IPN channel
	ast_channel_unregister(&ipn_channel_tech);

	// Destroy supported codecs
	ipn_channel_tech.capabilities = ast_format_cap_destroy(ipn_channel_tech.capabilities);

	// Destroy scheduler context
	if( sched ) {
		ast_sched_context_destroy(sched);
		sched = NULL;
	}

	// Destroy I/O context
	if( io ) {
		io_context_destroy(io);
		io = NULL;
	}

	return 0;
}

static int unload_module(void)
{
	// Kill IPN monitor thread
	ast_mutex_lock(&ipn_monitor_lock);
	if( ipn_monitor_thread && (ipn_monitor_thread != AST_PTHREADT_STOP) && (ipn_monitor_thread != AST_PTHREADT_NULL) ) {
		pthread_cancel(ipn_monitor_thread);
		pthread_kill(ipn_monitor_thread, SIGURG);
		pthread_join(ipn_monitor_thread, NULL);
	}
	ipn_monitor_thread = AST_PTHREADT_STOP;
	ast_mutex_unlock(&ipn_monitor_lock);

	__unload_module();

	return 0;
}



static int load_module(void)
{

	// Register CLI interface
	ast_cli_register_multiple(cli_ipn, ARRAY_LEN(cli_ipn));

	// Register suported codecs
	if (!(ipn_channel_tech.capabilities = ast_format_cap_alloc(0))) {
		return AST_MODULE_LOAD_DECLINE;
	}
	struct ast_format tmpfmt;
	ast_format_cap_add(ipn_channel_tech.capabilities, ast_format_set(&tmpfmt, AST_FORMAT_ULAW, 0));
	ast_format_cap_add(ipn_channel_tech.capabilities, ast_format_set(&tmpfmt, AST_FORMAT_ALAW, 0));

	// Register IPN channel
	if (ast_channel_register(&ipn_channel_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class 'IPN'\n");
		__unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	// Create scheduler context
	if( !( sched = ast_sched_context_create()) ) {
		ast_log(LOG_ERROR, "Unable to create scheduler context.\n");
		__unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	// Create I/O context
	if( !( io = io_context_create()) ) {
		ast_log(LOG_ERROR, "Unable to create I/O context.\n");
		__unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}

	// Start IPN monitor thread
	ipn_monitor_restart(CHANNEL_MODULE_LOAD);

	return AST_MODULE_LOAD_SUCCESS;
}


static int reload_module(void)
{
	return 	ipn_monitor_restart(CHANNEL_MODULE_RELOAD);
}


//*********************************************************
// Module info
//*********************************************************

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Armtel IPN Channel",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
