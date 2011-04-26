/*
 * Credits go to LeTama for the data connection code and phh for the wrapper code
 * Copyright (C) 2010 Sebastian Heinecke (gauner1986)
 * 
 * 0.8 by cedesmith
 */

#include <telephony/ril.h>
#include <dlfcn.h>

#include <utils/Log.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <poll.h>
#include <cutils/properties.h>
#include <arpa/inet.h>

#undef LOG_TAG
#define LOG_TAG "RIL_WRAP"

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define msleep(x) usleep(x*1000);

static const struct RIL_Env *s_rilenv;
static struct RIL_Env htcril_env;

static void *ril_handler=NULL;

static int rmnet_mode = 1;
static int nand_init = 0;
static volatile int pppd_pid;
static volatile int delayedNetworkReady=0;
static volatile RIL_Token request_registration_state_token = NULL;
char current_apn[80];
char current_user[80];
char current_addr[16];
static volatile int registrationState=0;
static volatile int gprsRegistrationState=0;
static volatile RIL_LastDataCallActivateFailCause lastDataError=PDP_FAIL_ERROR_UNSPECIFIED;

char* logtime()
{
    static char sTime[10];
    time_t t = time(NULL);
    strftime(sTime, sizeof(sTime), "%H:%M:%S", localtime(&t));
    return sTime;
}

typedef enum {
    DATA_STATE_DISCONNECTED=0,
    DATA_STATE_DISCONNECTING,
    DATA_STATE_PPP_DIED,
    DATA_STATE_CONNECTING,
    DATA_STATE_CONNECTED,
    DATA_STATE_CONNECTION_KILL,
} Wrap_DataCallState;
static volatile Wrap_DataCallState dataConnectionState=DATA_STATE_DISCONNECTED;

static volatile int fd_smd=-1;
int open_modem() 
{
    if(fd_smd!=-1) return fd_smd;
    
    fd_smd = open ("/dev/smd0", O_RDWR);
    if(fd_smd  == -1) {
        LOGE("%s: send_modem: Error opening smd0", logtime());
        return -1; //AT_ERROR_GENERIC;
    }
    
    struct termios  ios;
    tcgetattr( fd_smd, &ios );
    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
    tcsetattr( fd_smd, TCSANOW, &ios );
    
    //fcntl(fd_smd, F_SETFL, O_NONBLOCK | fcntl(fd_smd, F_GETFL, 0));
    
    return fd_smd;
}
void close_modem(){
    if(fd_smd!=-1) {
        close(fd_smd);
        fd_smd=-1;
    }
}
int send_modem(const char * cmd)
{
    int err = 0;
    
    size_t cur = 0;
    ssize_t written;
    size_t len = strlen(cmd);
    
    if(open_modem() == -1) return -1; //AT_ERROR_GENERIC;

    LOGD("%s: AT> %s", logtime(), cmd);

    /* the main string */
    while (cur < len) {
        do {
            written = write (fd_smd, cmd + cur, len - cur);
        } while (written < 0 && errno == EINTR);

        if (written < 0) return -1;// AT_ERROR_GENERIC;

        cur += written;
    }
    /* the \r  */

    do {
        written = write (fd_smd, "\r" , 1);
    } while ((written < 0 && errno == EINTR) || (written == 0));

    if (written < 0) {
        LOGE("%s: send_modem: write failure", logtime());
        return -1; //AT_ERROR_GENERIC;
    }

    return written;
}

int read_modem(char* response, size_t responseLen)
{
    if(open_modem() == -1) return -1;
    
    char *pread=response;
    while( pread<response+responseLen){
        if(read(fd_smd, pread, 1)<=0) break;
        if((*pread=='\n' || *pread=='\r') && pread==response) continue;
        if(*pread=='\r') break;
        pread++;
    }
    *pread=0;
    
    if(pread!=response) LOGD("%s: MODEM> %s", logtime(), response);
    else LOGD("%s MODEM>", logtime());
    
    return pread-response;
}

const char* requestToString(int request) 
{
    switch(request) {
        case RIL_REQUEST_GET_SIM_STATUS: return "GET_SIM_STATUS";
        case RIL_REQUEST_ENTER_SIM_PIN: return "ENTER_SIM_PIN";
        case RIL_REQUEST_ENTER_SIM_PUK: return "ENTER_SIM_PUK";
        case RIL_REQUEST_ENTER_SIM_PIN2: return "ENTER_SIM_PIN2";
        case RIL_REQUEST_ENTER_SIM_PUK2: return "ENTER_SIM_PUK2";
        case RIL_REQUEST_CHANGE_SIM_PIN: return "CHANGE_SIM_PIN";
        case RIL_REQUEST_CHANGE_SIM_PIN2: return "CHANGE_SIM_PIN2";
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION: return "ENTER_NETWORK_DEPERSONALIZATION";
        case RIL_REQUEST_GET_CURRENT_CALLS: return "GET_CURRENT_CALLS";
        case RIL_REQUEST_DIAL: return "DIAL";
        case RIL_REQUEST_GET_IMSI: return "GET_IMSI";
        case RIL_REQUEST_HANGUP: return "HANGUP";
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND: return "HANGUP_WAITING_OR_BACKGROUND";
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND: return "HANGUP_FOREGROUND_RESUME_BACKGROUND";
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE: return "SWITCH_WAITING_OR_HOLDING_AND_ACTIVE";
        case RIL_REQUEST_CONFERENCE: return "CONFERENCE";
        case RIL_REQUEST_UDUB: return "UDUB";
        case RIL_REQUEST_LAST_CALL_FAIL_CAUSE: return "LAST_CALL_FAIL_CAUSE";
        case RIL_REQUEST_SIGNAL_STRENGTH: return "SIGNAL_STRENGTH";
        case RIL_REQUEST_REGISTRATION_STATE: return "REGISTRATION_STATE";
        case RIL_REQUEST_GPRS_REGISTRATION_STATE: return "GPRS_REGISTRATION_STATE";
        case RIL_REQUEST_OPERATOR: return "OPERATOR";
        case RIL_REQUEST_RADIO_POWER: return "RADIO_POWER";
        case RIL_REQUEST_DTMF: return "DTMF";
        case RIL_REQUEST_SEND_SMS: return "SEND_SMS";
        case RIL_REQUEST_SEND_SMS_EXPECT_MORE: return "SEND_SMS_EXPECT_MORE";
        case RIL_REQUEST_SETUP_DATA_CALL: return "SETUP_DATA_CALL";
        case RIL_REQUEST_SIM_IO: return "SIM_IO";
        case RIL_REQUEST_SEND_USSD: return "SEND_USSD";
        case RIL_REQUEST_CANCEL_USSD: return "CANCEL_USSD";
        case RIL_REQUEST_GET_CLIR: return "GET_CLIR";
        case RIL_REQUEST_SET_CLIR: return "SET_CLIR";
        case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS: return "QUERY_CALL_FORWARD_STATUS";
        case RIL_REQUEST_SET_CALL_FORWARD: return "SET_CALL_FORWARD";
        case RIL_REQUEST_QUERY_CALL_WAITING: return "QUERY_CALL_WAITING";
        case RIL_REQUEST_SET_CALL_WAITING: return "SET_CALL_WAITING";
        case RIL_REQUEST_SMS_ACKNOWLEDGE: return "SMS_ACKNOWLEDGE";
        case RIL_REQUEST_GET_IMEI: return "GET_IMEI";
        case RIL_REQUEST_GET_IMEISV: return "GET_IMEISV";
        case RIL_REQUEST_ANSWER: return "ANSWER";
        case RIL_REQUEST_DEACTIVATE_DATA_CALL: return "DEACTIVATE_DATA_CALL";
        case RIL_REQUEST_QUERY_FACILITY_LOCK: return "QUERY_FACILITY_LOCK";
        case RIL_REQUEST_SET_FACILITY_LOCK: return "SET_FACILITY_LOCK";
        case RIL_REQUEST_CHANGE_BARRING_PASSWORD: return "CHANGE_BARRING_PASSWORD";
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE: return "QUERY_NETWORK_SELECTION_MODE";
        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC: return "SET_NETWORK_SELECTION_AUTOMATIC";
        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL: return "SET_NETWORK_SELECTION_MANUAL";
        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS : return "QUERY_AVAILABLE_NETWORKS ";
        case RIL_REQUEST_DTMF_START: return "DTMF_START";
        case RIL_REQUEST_DTMF_STOP: return "DTMF_STOP";
        case RIL_REQUEST_BASEBAND_VERSION: return "BASEBAND_VERSION";
        case RIL_REQUEST_SEPARATE_CONNECTION: return "SEPARATE_CONNECTION";
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE: return "SET_PREFERRED_NETWORK_TYPE";
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE: return "GET_PREFERRED_NETWORK_TYPE";
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS: return "GET_NEIGHBORING_CELL_IDS";
        case RIL_REQUEST_SET_MUTE: return "SET_MUTE";
        case RIL_REQUEST_GET_MUTE: return "GET_MUTE";
        case RIL_REQUEST_QUERY_CLIP: return "QUERY_CLIP";
        case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE: return "LAST_DATA_CALL_FAIL_CAUSE";
        case RIL_REQUEST_DATA_CALL_LIST: return "DATA_CALL_LIST";
        case RIL_REQUEST_RESET_RADIO: return "RESET_RADIO";
        case RIL_REQUEST_OEM_HOOK_RAW: return "OEM_HOOK_RAW";
        case RIL_REQUEST_OEM_HOOK_STRINGS: return "OEM_HOOK_STRINGS";
        case RIL_REQUEST_SET_BAND_MODE: return "SET_BAND_MODE";
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE: return "QUERY_AVAILABLE_BAND_MODE";
        case RIL_REQUEST_STK_GET_PROFILE: return "STK_GET_PROFILE";
        case RIL_REQUEST_STK_SET_PROFILE: return "STK_SET_PROFILE";
        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND: return "STK_SEND_ENVELOPE_COMMAND";
        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE: return "STK_SEND_TERMINAL_RESPONSE";
        case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM: return "STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM";
        case RIL_REQUEST_SCREEN_STATE: return "SCREEN_STATE";
        case RIL_REQUEST_EXPLICIT_CALL_TRANSFER: return "EXPLICIT_CALL_TRANSFER";
        case RIL_REQUEST_SET_LOCATION_UPDATES: return "SET_LOCATION_UPDATES";
        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION:return"CDMA_SET_SUBSCRIPTION";
        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:return"CDMA_SET_ROAMING_PREFERENCE";
        case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:return"CDMA_QUERY_ROAMING_PREFERENCE";
        case RIL_REQUEST_SET_TTY_MODE:return"SET_TTY_MODE";
        case RIL_REQUEST_QUERY_TTY_MODE:return"QUERY_TTY_MODE";
        case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:return"CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE";
        case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:return"CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE";
        case RIL_REQUEST_CDMA_FLASH:return"CDMA_FLASH";
        case RIL_REQUEST_CDMA_BURST_DTMF:return"CDMA_BURST_DTMF";
        case RIL_REQUEST_CDMA_SEND_SMS:return"CDMA_SEND_SMS";
        case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:return"CDMA_SMS_ACKNOWLEDGE";
        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:return"GSM_GET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:return"GSM_SET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG:return "CDMA_GET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG:return "CDMA_SET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION:return "CDMA_SMS_BROADCAST_ACTIVATION";
        case RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY: return"CDMA_VALIDATE_AND_WRITE_AKEY";
        case RIL_REQUEST_CDMA_SUBSCRIPTION: return"CDMA_SUBSCRIPTION";
        case RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM: return "CDMA_WRITE_SMS_TO_RUIM";
        case RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM: return "CDMA_DELETE_SMS_ON_RUIM";
        case RIL_REQUEST_DEVICE_IDENTITY: return "DEVICE_IDENTITY";
        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE: return "EXIT_EMERGENCY_CALLBACK_MODE";
        case RIL_REQUEST_GET_SMSC_ADDRESS: return "GET_SMSC_ADDRESS";
        case RIL_REQUEST_SET_SMSC_ADDRESS: return "SET_SMSC_ADDRESS";
        case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS: return "REPORT_SMS_MEMORY_STATUS";
        case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED: return "UNSOL_RESPONSE_RADIO_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED: return "UNSOL_RESPONSE_CALL_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED: return "UNSOL_RESPONSE_NETWORK_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_NEW_SMS: return "UNSOL_RESPONSE_NEW_SMS";
        case RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT: return "UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT";
        case RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM: return "UNSOL_RESPONSE_NEW_SMS_ON_SIM";
        case RIL_UNSOL_ON_USSD: return "UNSOL_ON_USSD";
        case RIL_UNSOL_ON_USSD_REQUEST: return "UNSOL_ON_USSD_REQUEST(obsolete)";
        case RIL_UNSOL_NITZ_TIME_RECEIVED: return "UNSOL_NITZ_TIME_RECEIVED";
        case RIL_UNSOL_SIGNAL_STRENGTH: return "UNSOL_SIGNAL_STRENGTH";
        case RIL_UNSOL_STK_SESSION_END: return "UNSOL_STK_SESSION_END";
        case RIL_UNSOL_STK_PROACTIVE_COMMAND: return "UNSOL_STK_PROACTIVE_COMMAND";
        case RIL_UNSOL_STK_EVENT_NOTIFY: return "UNSOL_STK_EVENT_NOTIFY";
        case RIL_UNSOL_STK_CALL_SETUP: return "UNSOL_STK_CALL_SETUP";
        case RIL_UNSOL_SIM_SMS_STORAGE_FULL: return "UNSOL_SIM_SMS_STORAGE_FUL";
        case RIL_UNSOL_SIM_REFRESH: return "UNSOL_SIM_REFRESH";
        case RIL_UNSOL_DATA_CALL_LIST_CHANGED: return "UNSOL_DATA_CALL_LIST_CHANGED";
        case RIL_UNSOL_CALL_RING: return "UNSOL_CALL_RING";
        case RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED: return "UNSOL_RESPONSE_SIM_STATUS_CHANGED";
        case RIL_UNSOL_RESPONSE_CDMA_NEW_SMS: return "UNSOL_NEW_CDMA_SMS";
        case RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS: return "UNSOL_NEW_BROADCAST_SMS";
        case RIL_UNSOL_CDMA_RUIM_SMS_STORAGE_FULL: return "UNSOL_CDMA_RUIM_SMS_STORAGE_FULL";
        case RIL_UNSOL_RESTRICTED_STATE_CHANGED: return "UNSOL_RESTRICTED_STATE_CHANGED";
        case RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE: return "UNSOL_ENTER_EMERGENCY_CALLBACK_MODE";
        case RIL_UNSOL_CDMA_CALL_WAITING: return "UNSOL_CDMA_CALL_WAITING";
        case RIL_UNSOL_CDMA_OTA_PROVISION_STATUS: return "UNSOL_CDMA_OTA_PROVISION_STATUS";
        case RIL_UNSOL_CDMA_INFO_REC: return "UNSOL_CDMA_INFO_REC";
        case RIL_UNSOL_OEM_HOOK_RAW: return "UNSOL_OEM_HOOK_RAW";
        case RIL_UNSOL_RINGBACK_TONE: return "UNSOL_RINGBACK_TONE";
        case RIL_UNSOL_RESEND_INCALL_MUTE: return "UNSOL_RESEND_INCALL_MUTE";
        default: return "<unknown request>";
    }
}

struct RequestInfo
{
    RIL_Token token;
    int request;
    time_t startTime;
};

struct RequestInfo pendingRequests[200];
void requestStarted(RIL_Token t, int request)
{
    LOGD("%s: Request Received %p %s", logtime(), t, requestToString(request)); 
    for(size_t i=0; i<sizeof(pendingRequests)/sizeof(struct RequestInfo); i++) {
        if(pendingRequests[i].token==NULL) {
            pendingRequests[i].token=t;
            pendingRequests[i].request=request;
            pendingRequests[i].startTime=clock();
            return;
        }
    }
    LOGD("%s: Request list full", logtime()); 
}
void requestRemoveAt(int idx)
{
    size_t i;
    for(i=idx+1; i<sizeof(pendingRequests)/sizeof(struct RequestInfo) && pendingRequests[i].token!=NULL; i++)
        pendingRequests[i-1]=pendingRequests[i];
    
    pendingRequests[i-1].token=NULL;
    pendingRequests[i-1].request=0;
    pendingRequests[i-1].startTime=0;
}
struct RequestInfo requestCompleted(RIL_Token t)
{
    struct RequestInfo r = { .token=NULL, .request=0, .startTime=0};
    for(size_t i=0; i<sizeof(pendingRequests)/sizeof(struct RequestInfo) && pendingRequests[i].token!=NULL; i++) {
        if(pendingRequests[i].token==t) {
            r=pendingRequests[i];
            LOGD("%s: Request Complete %p %s after %d ms", logtime(), r.token, requestToString(r.request), (unsigned int)(clock()-r.startTime)/(CLOCKS_PER_SEC/1000));
            requestRemoveAt(i);
            return r;
        }
    }
    LOGD("%s: Request Complete %p not found in started list", logtime(),t);
    return r;
}

unsigned requestsPending()
{
    unsigned count=0;
    for(size_t i=0; i<sizeof(pendingRequests)/sizeof(struct RequestInfo) && pendingRequests[i].token!=NULL; i++) {
        if(pendingRequests[i].token!=NULL) {
            if((clock()-pendingRequests[i].startTime)/CLOCKS_PER_SEC > 10)
            {
                LOGD("%s: Request delete %p %s after %d ms", logtime(), pendingRequests[i].token, requestToString(pendingRequests[i].request), (unsigned int)(clock()-pendingRequests[i].startTime)/(CLOCKS_PER_SEC/1000));
                requestRemoveAt(i);
                i--;
            } else {
                count++;
            }
        }
    }
    return count;
}

void requestsLOGD()
{
    for(size_t i=0; i<sizeof(pendingRequests)/sizeof(struct RequestInfo) && pendingRequests[i].token!=NULL; i++)
        LOGD("    %p %s for %d ms", pendingRequests[i].token, requestToString(pendingRequests[i].request), (unsigned int)(clock()-pendingRequests[i].startTime)/(CLOCKS_PER_SEC/1000));
}

void requestsWaitComplete(char *msg){
    if(requestsPending()>0) {
        LOGD("%s: Request pending... waiting to complete", logtime());
        requestsLOGD();
        while(requestsPending()>0) msleep(1);
        LOGD("%s: Request completed... continue %s", logtime(), msg!=NULL ? msg : NULL);
    }
}

void* pppd_thread(void *param)
{
    LOGD("%s: pppd_thread enter", logtime());
    
    requestsWaitComplete("pppd_thread");
    
    pppd_pid = fork();
    if(pppd_pid == 0) {
        
        char buff[256];
        kill(getppid(), SIGSTOP); //stop stealing my mojo 
        
        int act=0;
        
        send_modem("AT+CGACT?");
        do {
            read_modem(buff, sizeof(buff));
            char* actpos=strstr(buff, "+CGACT: 1,");
            if(actpos!=NULL) act=atoi(actpos+10);
        }while(buff[0]!='0');
        
        if(act!=0) {
            kill(getppid(), SIGCONT);
            exit(202);
        }

        sprintf(buff, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", current_apn);
        send_modem(buff);
        read_modem(buff, sizeof(buff));
        
        send_modem("ATD*99***1#");
        //send_modem("AT+CGDATA=\"PPP\",1");
        while(read_modem(buff, sizeof(buff))>0 && buff[0]=='+');
        //read_modem(buff, sizeof(buff));
        
        kill(getppid(), SIGCONT);

        int atd=atoi(buff);
        if(atd!=1 && atd!=3) exit(201); 

        sleep(1);

        close_modem(); //close modem handle before we transform to pppd
        int err = execl("/system/bin/pppd", "pppd", "/dev/smd1", "local","nodetach", "defaultroute", "noipdefault", "usepeerdns", "user", current_user, "debug", NULL);
        LOGE("%s: PPPD EXEC FAILED (%d)", logtime(),err);
        exit(200);
    } else {
        LOGD("%s: pppd pid is %d", logtime(),pppd_pid);
        
        int status=0;
        time_t start = time(NULL);
        waitpid(pppd_pid, &status, 0);
        pppd_pid=0;
        int runTime = time(NULL)-start;
        LOGD("%s: PPPD DIED after %d seconds with status %d", logtime(), runTime, status);
        
        if(lastDataError==PDP_FAIL_ERROR_UNSPECIFIED && WIFEXITED(status)){
            switch(WEXITSTATUS(status))
            {
                case 1: lastDataError=PDP_FAIL_INSUFFICIENT_RESOURCES; break;
                case 2: lastDataError=PDP_FAIL_SERVICE_OPTION_NOT_SUPPORTED; break;
                case 3: 
                case 4:
                    lastDataError=PDP_FAIL_PROTOCOL_ERRORS; break;
                case 19: lastDataError=PDP_FAIL_USER_AUTHENTICATION; break;
            }
        }
        
        requestsWaitComplete("pppd_thread");
        send_modem("AT+CGACT=0,1"); 
        
        if(dataConnectionState==DATA_STATE_CONNECTED || dataConnectionState==DATA_STATE_CONNECTION_KILL) {
            dataConnectionState=DATA_STATE_DISCONNECTED;
            RIL_Data_Call_Response dataCall={ .cid=1, .active=0, .type="IP", .apn=current_apn, .address=current_addr };
            s_rilenv->OnUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, &dataCall, sizeof(RIL_Data_Call_Response));
        }
    }

    LOGD("%s: pppd_thread exit", logtime());
    return NULL;
}

void* DeactivateData(void* t)
{
    dataConnectionState = DATA_STATE_DISCONNECTING;
    LOGD("%s: DeactivateData", logtime());
    
    int pid=pppd_pid; //work with pppd_pid copy as thread will set it to 0 after kill
    if(pid!=0) {
        int status=0;
        LOGD("    waiting for pppd to end %d", pid);
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        LOGD("%s: DeactivateData: pppd ended", logtime());
    }
    
    //clear dns entries
    property_set("net.ppp0.dns1", "");
    property_set("net.ppp0.dns2", "");
    
    dataConnectionState = DATA_STATE_DISCONNECTED;
    RIL_onRequestComplete((RIL_Token) t, RIL_E_SUCCESS, NULL, 0);
    return NULL;
}

void hackDeactivateData(void *data, size_t datalen, RIL_Token t)
{
    LOGD("%s: DeactivateData Request", logtime());
    char* cid = ((char **)data)[0];
    if(atoi(cid)!=1) {
        LOGE("    wrong CID %s expected 1", cid);
        return;
    }
    pthread_t tid;
    pthread_create(&tid, NULL, DeactivateData, t);   
}

int ifc_get_info(const char *name, unsigned *addr, unsigned *mask, unsigned *flags);
int ifc_init(void);
void ifc_close(void);
void* SetupData(void* t)
{    
    //this should never happen but let's check
    if(pppd_pid!=0 ) {
        LOGD("    waiting pppd to die");
        int status=0;
        kill(pppd_pid, SIGTERM);
        waitpid(pppd_pid, &status, 0);
    }
    
    //reset ppp.dns 
    property_set("net.ppp0.dns1", "0.0.0.0");
    property_set("net.ppp0.dns2", "0.0.0.0");
    strcpy(current_addr, "255.255.255.255");
    
    pthread_t thread;
    pthread_create(&thread, NULL, pppd_thread, NULL);
   
    //wait for pppd connect
    if(ifc_init()) {
        LOGE("%s: IFC failed to init", logtime());
        sleep(7);
    } else {
        clock_t start=clock();
        //loop till timeout or connected
        while(1) {
            //check if ppp0 interface is up, if true break loop, else record dnschange value
            unsigned addr, mask, flags;
            ifc_get_info("ppp0", &addr, &mask, &flags);
            if(flags & 1) 
            {
                struct in_addr in_addr = {.s_addr=addr};
                strcpy(current_addr, inet_ntoa(in_addr));
                LOGD("%s: IP: %s", logtime(),current_addr); 
                break;        
            }
            //if timeout goto error
            if ( (clock()-start)/CLOCKS_PER_SEC > 60 ){
                LOGE("%s: ppp0 connect timed out, giving up", logtime());
                ifc_close();
                goto error;
            }
            int status, pid=pppd_pid;
            if(pid==0 || waitpid(pid, &status, WNOHANG)>0){
                LOGE("%s: ppp0 connect timed out, giving up", logtime());
                ifc_close();
                goto error;
            }
            msleep(100);
        }
    }
    ifc_close();
     
    //if ip-up exists wait for dns change
    char dns1[PROPERTY_VALUE_MAX];
    char dns2[PROPERTY_VALUE_MAX];
    struct stat sts;
    if(stat("/etc/ppp/ip-up", &sts)==0 && (S_ISREG(sts.st_mode) || S_ISLNK(sts.st_mode))) {
        clock_t start=clock();
        while(1) {
            //check if dnschange changed
            property_get("net.ppp0.dns1", dns1, "0.0.0.0");
            property_get("net.ppp0.dns2", dns2, "0.0.0.0");
            if(strcmp(dns1, "0.0.0.0")!=0 && strcmp(dns2, "0.0.0.0")!=0) break;
            
            if((clock()-start)/CLOCKS_PER_SEC > 2) {
                LOGE("%s: timeout waiting for dns change", logtime());
                break;
            }
            msleep(100);
        } 
    }
    
    //check ppp.dns values and set defaults if suspect wrong
    property_get("net.ppp0.dns1", dns1, "");
    if(strlen(dns1)<7 || strcmp(dns1,"0.0.0.0")==0 || strcmp(dns1, "10.11.12.13")==0) {
        LOGD("%s: DNS1: %s wrong setting to 8.8.8.8", logtime(),dns1);
        property_set("net.ppp0.dns1", "8.8.8.8");
    } else {
        LOGD("%s: DNS1: %s", logtime(),dns1);
    }
    property_get("net.ppp0.dns2", dns2, "");
    if(strlen(dns2)<7 || strcmp(dns2, "0.0.0.0")==0 || strcmp(dns2, "10.11.12.14")==0) {
        LOGD("%s: DNS2: %s wrong setting to 8.8.4.4", logtime(),dns2);
        property_set("net.ppp0.dns2", "8.8.4.4");
    } else {
        LOGD("%s: DNS2: %s", logtime(),dns2);
    }
    
    char *response[3] = { "1", "ppp0", current_addr };
    dataConnectionState=DATA_STATE_CONNECTED;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    return NULL;
    
    error:
        dataConnectionState=DATA_STATE_DISCONNECTED;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return NULL;
}
void hackSetupData(char **data, size_t datalen, RIL_Token t)
{
    LOGD("%s: SetupData(%s) Request", logtime(),((const char **)data)[2]);
    
    lastDataError=PDP_FAIL_ERROR_UNSPECIFIED;
    
    if(*data[0]=='0') {
        LOGE("    Android want us to connect as CDMA while we are a GSM phone !");
        goto error;
    }
    
    if(!registrationState) {
        LOGE("    network registration state wrong");
        lastDataError=PDP_FAIL_REGISTRATION_FAIL;
        goto error;
    }
    if(!gprsRegistrationState) {
        LOGE("    gprs registration state wrong");
        lastDataError=PDP_FAIL_GPRS_REGISTRATION_FAIL;
        goto error;
    }
    
    
    dataConnectionState = DATA_STATE_CONNECTING;
    char *apn = ((char **)data)[2];
    char *user = ((char **)data)[3];
    char *pass = ((char **)data)[4];
    if(apn==NULL) apn="";
    if(user==NULL || strlen(user)<2) user = "dummy";
    if(pass==NULL || strlen(pass)<2) pass = "dummy";
    strcpy(current_apn, apn);
    strcpy(current_user, user);
    
    //save auth
    truncate("/etc/ppp/pap-secrets", 0);
    truncate("/etc/ppp/chap-secrets", 0);
    char buff[128];
    sprintf(buff, "%s * %s\n", user, pass);
    int fd;
    if((fd=creat("/etc/ppp/pap-secrets", S_IRUSR|S_IWUSR))==-1) {
        LOGE("Failed to create /etc/ppp/pap-secrets");
        lastDataError=PDP_FAIL_USER_AUTHENTICATION;
        goto error;
    }
    write(fd, buff, strlen(buff));
    close(fd);
    if((fd=creat("/etc/ppp/chap-secrets", S_IRUSR|S_IWUSR))==-1) {
        LOGE("Failed to create /etc/ppp/chap-secrets");
        lastDataError=PDP_FAIL_USER_AUTHENTICATION;
        goto error;
    }
    write(fd, buff, strlen(buff));
    close(fd);  

    pthread_t tid;
    pthread_create(&tid, NULL, SetupData, t);  
    return;
    
    error:
        dataConnectionState=DATA_STATE_DISCONNECTED;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

void interceptOnRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen)
{      
    if(!rmnet_mode) {
        struct RequestInfo requestInfo= requestCompleted(t);
        if( requestInfo.request==RIL_REQUEST_REGISTRATION_STATE) {
            if(responselen>=14*sizeof(char *)) {
                char **strings = (char **)response; 
                int registration = atoi(strings[0]);    //1 - Registered, home network; 5 - Registered, roaming
                int radio = atoi(strings[3]);           //0 == unknown
                registrationState=((registration==1 || registration==5) && radio!=0); 
                LOGD("%s: Registration state %d %d = %d", logtime(), registration, radio, registrationState);
                if(!registrationState && pppd_pid!=0 && dataConnectionState==DATA_STATE_CONNECTED){
                    LOGE("%s: data disconnect due to network registration state", logtime());
                    lastDataError=PDP_FAIL_REGISTRATION_FAIL;
                    dataConnectionState=DATA_STATE_CONNECTION_KILL;
                    kill(pppd_pid, SIGTERM);
                }
            }
        }
        if( requestInfo.request==RIL_REQUEST_GPRS_REGISTRATION_STATE) {
            if(responselen>=4*sizeof(char *)) {
                char **strings = (char **)response; 
                int registration = atoi(strings[0]);    //1 - Registered, home network; 5 - Registered, roaming
                int radio = atoi(strings[3]);           //0 == unknown; 4 ("unknown") is treated as "out of service" in the Android telephony system
                gprsRegistrationState=((registration==1 || registration==5) && (radio!=0 && radio!=4)); 
                LOGD("%s: Registration state %d %d = %d", logtime(), registration, radio, gprsRegistrationState);
                if(!gprsRegistrationState && pppd_pid!=0 && dataConnectionState==DATA_STATE_CONNECTED){
                    LOGE("%s: data disconnect due to gprs registration state", logtime());
                    lastDataError=PDP_FAIL_GPRS_REGISTRATION_FAIL;
                    dataConnectionState=DATA_STATE_CONNECTION_KILL;
                    kill(pppd_pid, SIGTERM);
                }
            }
        }
    }
    s_rilenv->OnRequestComplete(t, e, response, responselen);
}

void hackDataCallList(char **data, size_t datalen, RIL_Token t)
{
    RIL_Data_Call_Response dataCall={ .cid=1, .active=0, .type="IP", .apn=current_apn, .address=current_addr };
    LOGD("%s: DataCallList", logtime());
    LOGD("    cid=%d, active=%d, type=%s, apn=%s, add=%s", dataCall.cid, dataCall.active, dataCall.type, dataCall.apn, dataCall.address);
    s_rilenv->OnRequestComplete(t, RIL_E_SUCCESS, &dataCall, sizeof(RIL_Data_Call_Response));
}

void interceptOnUnsolicitedResponse(int unsolResponse, const void *data, size_t datalen)
{
    LOGD("%s: UNSOL %s", logtime(), requestToString(unsolResponse));
    s_rilenv->OnUnsolicitedResponse(unsolResponse, data, datalen);
}

void (*htc_onRequest)(int request, void *data, size_t datalen, RIL_Token t);
void onRequest(int request, void *data, size_t datalen, RIL_Token t) {
    if(!rmnet_mode) {
        switch(request) {
            case RIL_REQUEST_SETUP_DATA_CALL:
                return hackSetupData(data, datalen, t);
            case RIL_REQUEST_DEACTIVATE_DATA_CALL:
                return hackDeactivateData(data, datalen, t);
            case RIL_REQUEST_DATA_CALL_LIST:
                return hackDataCallList(data, datalen, t);
            case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
                s_rilenv->OnRequestComplete(t, RIL_E_SUCCESS,(RIL_LastDataCallActivateFailCause*) &lastDataError, sizeof(RIL_LastDataCallActivateFailCause));
                return;
        }
        requestStarted(t, request);
    }
    return htc_onRequest(request, data, datalen, t);
}

void writeAdditionalNandInit(){
    LOGD("NAND boot, writing additional init commands to /dev/smd0");
    send_modem("AT@BRIC=0");
    send_modem("AT+CFUN=0");
    send_modem("AT+COPS=2");
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv) 
{
    int i;
    int ril_argc = 0;
    char **ril_argv;
    RIL_RadioFunctions *(*htc_RIL_Init)(const struct RIL_Env *env, int argc, char **argv);
    RIL_RadioFunctions *s_callbacks;

    s_rilenv = env;
    LOGD("----------- HTC Ril Wrapper v0.8b5 starting ------------");
    // we never free this, but we can't tell if htc ril uses argv after init
    ril_argv = (char **)malloc(argc * sizeof(char*));

    struct stat sts;
    if(stat("/system/ppp", &sts)==0 && (S_ISREG(sts.st_mode) || S_ISLNK(sts.st_mode))) rmnet_mode = 0;
    LOGD("rmnet_mode=%d", rmnet_mode);

    // Parse command line and prepare ril command line
    for(i = 0; i < argc ; i++) {
        LOGW("RIL_Init arg[%d]=%s", i, argv[i]);
        if(strcmp(argv[i], "rmnet_mode") == 0) continue;
        if(strcmp(argv[i], "nand_init") == 0)
            nand_init = 1;
        else {
            ril_argv[ril_argc++] = argv[i];
        }
    }

    if(nand_init) writeAdditionalNandInit();

    ril_handler=dlopen("/system/lib/libhtc_ril.so", 0/*Need to RTFM, 0 seems fine*/);
    htc_RIL_Init = dlsym(ril_handler, "RIL_Init");
    
    // re-route to our man in the middle functions
    htcril_env.OnRequestComplete = interceptOnRequestComplete;
    htcril_env.OnUnsolicitedResponse = interceptOnUnsolicitedResponse;
    htcril_env.RequestTimedCallback = s_rilenv->RequestTimedCallback;

    s_callbacks = htc_RIL_Init(&htcril_env, ril_argc, ril_argv);

    htc_onRequest = s_callbacks->onRequest;
    s_callbacks->onRequest=onRequest;
    return s_callbacks;
}
