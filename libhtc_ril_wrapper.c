/*
 * Credits go to LeTama for the data connection code and phh for the wrapper code
 * Copyright (C) 2010 Sebastian Heinecke (gauner1986)
 */

#include <telephony/ril.h>
#include <dlfcn.h>

#include <utils/Log.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <utils/Log.h>
#include <time.h>

#undef LOG_TAG
#define LOG_TAG "RIL_WRAP"

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)

static const struct RIL_Env *s_rilenv;
static struct RIL_Env htcril_env;

static void *ril_handler=NULL;

static int rmnet_mode = 0;
static int nand_init = 0;
static int is_data_active=0;
static int is_network_ready=0;
static int pppd_pid, monitor_pid;
static RIL_Token request_call_list_token = 0;
static RIL_Token request_registration_state_token = 0;
static char current_apn[80];

inline int msleep(int msec) 
{
    return usleep(msec*1000);
}

int send_modem(const char * cmd)
{
	int err = 0;
	int fd_smd;
	struct termios  ios;
	size_t cur = 0;
	ssize_t written;
	size_t len = strlen(cmd);


	fd_smd = open ("/dev/smd0", O_RDWR);

	if(fd_smd  == -1)  {
		LOGE("send_modem: Error opening smd0\n");
        return AT_ERROR_GENERIC;
	}

	tcgetattr( fd_smd, &ios );
	ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
	tcsetattr( fd_smd, TCSANOW, &ios );

	LOGD("AT> %s\n", cmd);

	/* the main string */
	while (cur < len) {
		do {
			written = write (fd_smd, cmd + cur, len - cur);
		} while (written < 0 && errno == EINTR);

		if (written < 0) {
            close(fd_smd);
			return AT_ERROR_GENERIC;
		}

		cur += written;
	}
	/* the \r  */
	
	do {
		written = write (fd_smd, "\r" , 1);
	} while ((written < 0 && errno == EINTR) || (written == 0));

	if (written < 0) {
        LOGE("send_modem: write failure");
        close(fd_smd);
		return AT_ERROR_GENERIC;
	}

	close(fd_smd);
    return written;
}

void set_network_ready()
{
    LOGW("Mon got notif netup");
    is_network_ready = 1;
}

void reset_network_ready()
{
    LOGW("Mon git notif netdown");
    is_network_ready = 0;
}

void sigint_signal()
{
    LOGE("Mon recv SIGINT, killed by android?");
    exit(0);
}

void sigquit_signal()
{
    LOGE("Mon recv SIGQUIT, killed by android?");
    exit(0);
}

void sigterm_signal()
{
    LOGW("Mon recv SIGTERM, quit");
    if(pppd_pid != 0) {
        // kill pppd
        kill(pppd_pid, SIGTERM);
    }
    exit(0);
}

void reset_data()
{
}

void launch_pppd()
{
    int status;
    LOGW("start mon");
    monitor_pid = fork();
    if(monitor_pid == 0) {
        time_t last_run, die_time;
        int failure_cnt = 0;
        // install signal handler to be notified of network avail
        signal(SIGUSR1, set_network_ready);
        signal(SIGUSR2, reset_network_ready);

        // exit signals to debug zombie problem
        signal(SIGINT, sigint_signal);
        signal(SIGTERM, sigterm_signal);
        signal(SIGQUIT, sigquit_signal);
        while(1) {
            LOGW("Mon: start pppd (net=%d)", is_network_ready);
            if(!is_network_ready) {
                LOGW("net not ready, waiting");
                sleep(1);
            }
            else {
                int err;
                time(&last_run);
                pppd_pid = fork();
                if(pppd_pid == 0) {
                    LOGD("atd+ppd");
                    send_modem("AT+CGACT=0,1");
                    msleep(300);
                    send_modem("ATD*99***1#");

                    // The modem replies immediately even if it's not connected!
                    // so wait a short time.
                    sleep(3);
                    err = execl("/system/bin/pppd", "/system/bin/pppd", "nodetach", "/dev/smd1", "defaultroute", "debug", (char *)0);
                    LOGE("PPPD EXEC FAILED (%d)", err);
                }
                else {
                    LOGD("pppd pid is %d", pppd_pid);
                    while(wait(&status) != pppd_pid)
                        LOGD("PL:wait:%d", status);
                    LOGW("PPPD DIED! (0x%x)", status);
                    time(&die_time);
                    if(die_time - last_run < 5) {
                        // pppd died in less than 5 seconds
                        failure_cnt++;
                        if(failure_cnt > 3) {
                            LOGD("fail cnt=%d", failure_cnt);
                            // atd won't cut it, slow down respawn
                            sleep(5 * failure_cnt);
                        } else if(failure_cnt == 5) {
                            char *cmd;
                            // we waited long enough, let be more radical
                            LOGE("too much retries, full restart");
                            asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", current_apn);
                            send_modem(cmd);
                            free(cmd);
                            msleep(300);
                        } else if(failure_cnt > 5) {
                            // nothing does it, don't hammer modem
                            LOGE("full restart doesn't cut it, giving up");
                            sleep(30);
                        }
                    }
                    else {
                        failure_cnt = 1;
                    }
                    sleep(3);
                }
            }
        }
    }
}

void interceptOnRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen)
{
    int i;
//    LOGD("PL:InterceptRequestComplete(%d, %d)", (int)t, responselen);
    if(request_call_list_token == t) {
        // response from data call list
        request_call_list_token = 0;
        LOGW("DataCallList");
        RIL_Data_Call_Response *data_calls = (RIL_Data_Call_Response *)response;
        for(i = 0 ; i < (int)(responselen / sizeof(RIL_Data_Call_Response)); i++) {
            LOGD("call[%d]:cid=%d, active=%d, type=%s, apn=%s, add=%s\n",
                 i,
                 data_calls[i].cid,
                 data_calls[i].active,
                 data_calls[i].type,
                 data_calls[i].apn,
                 data_calls[i].address
                );
        }
    } else if(request_registration_state_token == t) {
        char **strings;
        request_registration_state_token = 0;
        strings = (char **)response;
        char dbg[128];
        int status;
        int strings_cnt = (int)(responselen / sizeof(char *));

        strcpy(dbg, "RegState:");
        dbg[127] = 0;
        for(i = 0 ; i < strings_cnt; i++) {
            strncat(dbg, strings[i] ? strings[i] : "null", 127);
            strncat(dbg, " ", 127);
        }
        LOGW(dbg);
        // Workaround for htc_ril bug, it sometimes set null to string[0] that make rild crash
        if(strings[0] == NULL)
            strings[0] = "0";

        if((strings_cnt > 3) && (strings[0] != NULL) && (strings[3] != NULL)) {
            if((atoi(strings[0]) != 0) && (atoi(strings[3]) != 0)) {
                LOGW("State: Net ready");
                is_network_ready = 1;
            }
            else {
                LOGW("State: no Net");
                is_network_ready = 0;
            }
            // Notify monitoring thread
            if(monitor_pid) {
                LOGW("Notif mon(%d) up", monitor_pid);
                if(kill(monitor_pid, is_network_ready ? SIGUSR1 : SIGUSR2) == -1) {
                    LOGE("MONITOR DIED?, respawn it");
                    // wait to remove zombie
                    if(waitpid(monitor_pid, &status, WNOHANG|WUNTRACED) > 0) {
                        LOGW("monitor was zombie");
                        // waitpid found a process
                        if(WIFSIGNALED(status)) {
                            LOGW("monitor has received a signal (%d) ??", WTERMSIG(status));
                        } else if(WIFSTOPPED(status)) {
                            LOGW("monitor has been stopped by signal (%d) ??", WSTOPSIG(status));
                        }
                    }
                    if(is_network_ready && is_data_active) {
                        LOGW("data active, try to restart monitor after death");
                        launch_pppd();
                    }
                }
            }
        }
    }
    s_rilenv->OnRequestComplete(t, e, response, responselen);
}

void interceptOnUnsolicitedResponse(int unsolResponse, const void *data, size_t datalen)
{
    LOGD("Unsol(%d, %u)", unsolResponse, datalen);
    if(is_data_active) {
    }
    s_rilenv->OnUnsolicitedResponse(unsolResponse, data, datalen);
}

void interceptRequestTimedCallback(RIL_TimedCallback callback, void *param,
                                   const struct timeval *relativeTime)
{
    LOGD("PL:interceptRequestTimedCallback");
    s_rilenv->RequestTimedCallback(callback, param, relativeTime);
}


void hackOnRequestRegistrationState(char **data, size_t datalen, RIL_Token t)
{
    LOGD("PL:hackOnRequestRegistrationState token=(%x)", (unsigned int)t);
    request_registration_state_token = t;
}

void hackDeactivateData(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char * cmd;
	char * cid;
	int fd,i,fd2;
	ATResponse *p_response = NULL;

	LOGW("DeactivateData");

	cid = ((char **)data)[0];
	asprintf(&cmd, "AT+CGACT=0,%s", cid);
    send_modem(cmd);
    free(cmd);

    // kill monitor process
    LOGD("Deactdata:killing monitor");
    kill(monitor_pid, SIGTERM);
    waitpid(monitor_pid, NULL, 0);
    monitor_pid = 0;

    // kill remaining pppd
    LOGD("Deactdata:killing pppd");
	system("killall pppd");
	sleep(1);

	i=0;
	while((fd = open("/etc/ppp/ppp0.pid",O_RDONLY)) > 0) {
		if(i%5 == 0) {
			system("killall pppd");
		}
		close(fd);
		if(i>25)
			goto error;
		i++;
		sleep(1);
	}

	LOGD("Deactdata done, waiting 10 sec to let pppd die");
    is_data_active = 0;

    sleep(10);

	RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
	return;

	error:
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

void hackSetupData(char **data, size_t datalen, RIL_Token t)
{
	const char *apn;
	char *user = NULL;
	char *pass = NULL;
	char *cmd;
	char *userpass;
	int err;
	ATResponse *p_response = NULL;
	int fd, pppstatus,i,fd2;
	FILE *pppconfig;
	size_t cur = 0;
	ssize_t written, rlen;
	char status[32] = {0};
	char *buffer;
	long buffSize, len;
	int retry = 10;
	char *response[3] = { "1", "ppp0", "255.255.255.255" };
	int mypppstatus;
	int ret;
	struct termios  ios;

	LOGW("hackSetupData(%s)\n",((const char **)data)[2]);

	apn = ((const char **)data)[2];
	user = ((char **)data)[3];
	if(user != NULL)
	{
		if (strlen(user)<2)
			user = "dummy";
	} else
		user = "dummy";

	pass = ((char **)data)[4];
	if(pass != NULL)
	{
		if (strlen(pass)<2)
			pass = "dummy";
	} else
		pass = "dummy";


	if(*data[0]=='0')
		LOGD("Android want us to connect as CDMA while we are a GSM phone !");

    strcpy(current_apn, apn);
	asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", apn);
	//FIXME check for error here
	//err = at_send_command(cmd, NULL);
	send_modem(cmd);
	free(cmd);
	msleep(300);

	// Set required QoS params to default
	send_modem("AT+CGQREQ=1");
	msleep(300);
	// Set minimum QoS params to default
	send_modem("AT+CGQMIN=1");
	msleep(300);
	// packet-domain event reporting
	send_modem("AT+CGEREP=1,0");
	msleep(300);

	asprintf(&userpass, "%s * %s\n", user, pass);
	len = strlen(userpass);

	// if file does not exist we will create it
	system("touch /etc/ppp/pap-secrets");	
	fd = open("/etc/ppp/pap-secrets",O_WRONLY);
	if(fd < 0) {
		LOGE("unable to open /etc/ppp/pap-secrets\n");
		goto error;
	}
	write(fd, userpass, len);
	close(fd);

	// if file does not exist we will create it
	system("touch /etc/ppp/chap-secrets");
	fd = open("/etc/ppp/chap-secrets",O_WRONLY);
	if(fd < 0) {
		LOGE("unable to open /etc/ppp/chap-secrets\n");
		goto error;
	}
	write(fd, userpass, len);
	close(fd);
	free(userpass);

	// if file does not exist we will create it
	system("touch /etc/ppp/options.smd");
	pppconfig = fopen("/etc/ppp/options.smd","r");
	if(!pppconfig) {
		LOGE("unable to open /etc/ppp/options.smd\n");
		goto error;
	}

	//filesize
	fseek(pppconfig, 0, SEEK_END);
	buffSize = ftell(pppconfig);
	rewind(pppconfig);

	//allocate memory
	buffer = (char *) malloc (sizeof(char)*buffSize);
	if (buffer == NULL) {
		LOGE("hackSetupData: malloc buffer failed\n");
		goto error;
	}

	//read in the original file
	len = fread (buffer,1,buffSize,pppconfig);
	if (len != buffSize) {
		LOGE("hackSetupData: fread failed\n");
		goto error;
	}
	fclose(pppconfig);

	// if file does not exist we will create it
	system("touch /etc/ppp/options.smd1");
	pppconfig = fopen("/etc/ppp/options.smd1","w");
	fwrite(buffer,1,buffSize,pppconfig);
	fprintf(pppconfig,"user %s\n",user);
	fclose(pppconfig);
	free(buffer);

	LOGW("launching pppd\n");
    is_data_active = 1;
    launch_pppd();

    // Give some time to launch pppd
	sleep(7);

	LOGD("setupData exit\n");
	
	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	return;
	error:
		LOGE("PL:hackSetupData: ERROR ?");
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

void writeAdditionalNandInit(){
	LOGD("NAND boot, writing additional init commands to /dev/smd0");
	send_modem("AT@BRIC=0");
	msleep(300);
	send_modem("AT+CFUN=0");
	msleep(300);
	send_modem("AT+COPS=2");
	msleep(300);
}

void (*htc_onRequest)(int request, void *data, size_t datalen, RIL_Token t);
void onRequest(int request, void *data, size_t datalen, RIL_Token t) {
    if(!rmnet_mode){
        if(request==RIL_REQUEST_SETUP_DATA_CALL){ // Let's have fun !
            hackSetupData(data, datalen, t);
            return;
        } else if(request==RIL_REQUEST_DEACTIVATE_DATA_CALL) {
            hackDeactivateData(data, datalen, t);
            return;
        } else if(request == RIL_REQUEST_REGISTRATION_STATE) {
            hackOnRequestRegistrationState(data, datalen, t);
            htc_onRequest(request, data, datalen, t);
            return;
        } else if(request == RIL_REQUEST_DATA_CALL_LIST) {
            // cheat here
            LOGW("*** Request call list, fake reply ***");
            RIL_Data_Call_Response data_call;
            data_call.cid = 1;
            data_call.active = 1;
            data_call.type = "IP";
            data_call.apn = current_apn;
            data_call.address = "";
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &data_call, sizeof(data_call));
            return;
        }
    }
	return htc_onRequest(request, data, datalen, t);
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv) 
{
    int i;
    int ril_argc = 0;
    char **ril_argv;
	RIL_RadioFunctions *(*htc_RIL_Init)(const struct RIL_Env *env, int argc, char **argv);
	RIL_RadioFunctions *s_callbacks;

	s_rilenv = env;
    LOGW("----------- HTC Ril Wrapper v0.7 starting ------------");
    // we never free this, but we can't tell if htc ril uses argv after init
    ril_argv = (char **)malloc(argc * sizeof(char*));

    // Parse command line and prepare ril command line
    for(i = 0; i < argc ; i++) {
        LOGW("RIL_Init arg[%d]=%s", i, argv[i]);
        if(strcmp(argv[i], "rmnet_mode") == 0)
            rmnet_mode = 1;
        else if(strcmp(argv[i], "nand_init") == 0)
            nand_init = 1;
        else {
            ril_argv[ril_argc++] = argv[i];
        }
    }

    if(nand_init)
        writeAdditionalNandInit();
	
	ril_handler=dlopen("/system/lib/libhtc_ril.so", 0/*Need to RTFM, 0 seems fine*/);
	htc_RIL_Init = dlsym(ril_handler, "RIL_Init");

    // re-route to our man in the middle functions
    htcril_env.OnRequestComplete = interceptOnRequestComplete;
    htcril_env.OnUnsolicitedResponse = interceptOnUnsolicitedResponse;
    htcril_env.RequestTimedCallback = interceptRequestTimedCallback;

	s_callbacks = htc_RIL_Init(&htcril_env, ril_argc, ril_argv);

	htc_onRequest = s_callbacks->onRequest;
	s_callbacks->onRequest=onRequest;
	return s_callbacks;
}
