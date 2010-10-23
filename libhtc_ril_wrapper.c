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
#include <cutils/sockets.h>
#include <termios.h>
#include <utils/Log.h>

#define LOG_TAG "RILW"

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)

static const struct RIL_Env *s_rilenv;

static void *ril_handler=NULL;

static int pppMode = 0;

int file_exists (char * fileName)
{
   struct stat buf;
   int i = stat ( fileName, &buf );
     /* File found */
     if ( i == 0 )
     {
       return 1;
     }
     return 0;
       
}


int hackWrite (int s_fd, const char *s)
{
	size_t cur = 0;
	size_t len = strlen(s);
	ssize_t written;

	LOGD("AT> %s\n", s);

	/* the main string */
	while (cur < len) {
		do {
			written = write (s_fd, s + cur, len - cur);
		} while (written < 0 && errno == EINTR);

		if (written < 0) {
			return AT_ERROR_GENERIC;
		}

		cur += written;
	}

	/* the \r  */
	
	do {
		written = write (s_fd, "\r" , 1);
	} while ((written < 0 && errno == EINTR) || (written == 0));

	if (written < 0) {
		return AT_ERROR_GENERIC;
	}

	return 0;
}


void hackDeactivateData(void *data, size_t datalen, RIL_Token t)
{
	int err;
	char * cmd;
	char * cid;
	int fd,fd_smd,i,fd2;
	ATResponse *p_response = NULL;
	struct termios  ios;

	LOGD("PL: hackDeactivateData: starting\n");
	fd_smd = open ("/dev/smd0", O_RDWR);

	if(fd_smd  == -1)  {
		LOGD("PL:hackDeactivateData: Error opening smd0\n");
	}

	tcgetattr( fd_smd, &ios );
	ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
	tcsetattr( fd_smd, TCSANOW, &ios );

	cid = ((char **)data)[0];
	asprintf(&cmd, "AT+CGACT=0,%s", cid);
	err = hackWrite(fd_smd, cmd);
	free(cmd);
	close(fd_smd);

	// turn off pppd calling it once again, on failure kill it
	system("pppd /dev/smd1");
	sleep(1);
	system("killall pppd");
	sleep(1);

	LOGD("PL:hackDeactivateData: killing pppd\n");

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

	LOGD("PL:hackDeactivateData: pppd killed");

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

	LOGD("PL:hackSetupData\n");

	fd = open ("/dev/smd0", O_RDWR);

	if(fd == -1)  {
		LOGD("PL:hackSetupData: Error opening smd0\n");
	}
	tcgetattr( fd, &ios );
	ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
	tcsetattr( fd, TCSANOW, &ios );

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


	LOGD("PL:hackSetupData - InitModem\n");

	LOGD("PL:hackSetupData - AT+GCDCONT\n");
	asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", apn);
	//FIXME check for error here
	//err = at_send_command(cmd, NULL);
	hackWrite(fd, cmd);
	free(cmd);
	sleep(1);
	// Set required QoS params to default
	LOGD("PL:hackSetupData - AT+GCDREQ\n");
	err = hackWrite(fd, "AT+CGQREQ=1");
	sleep(1);
	// Set minimum QoS params to default
	LOGD("PL:hackSetupData - AT+GCDMIN\n");
	err = hackWrite(fd, "AT+CGQMIN=1");
	sleep(1);
	// packet-domain event reporting
	LOGD("PL:hackSetupData - AT+GCEREP\n");
	err = hackWrite(fd, "AT+CGEREP=1,0");
	sleep(1);
	// Hangup anything that's happening there now
	LOGD("PL:hackSetupData - AT+CGACT\n");
	err = hackWrite(fd, "AT+CGACT=0,1");
	sleep(1);
	// Start data on PDP context 1
	LOGD("PL:hackSetupData - ATD*99\n");
	err = hackWrite(fd, "ATD*99***1#");

	sleep(1);
	close(fd);

	LOGD("PL: hackSetupData: saving secrets\n");
	asprintf(&userpass, "%s * %s\n", user, pass);
	len = strlen(userpass);

	// if file does not exist we will create it
	system("touch /etc/ppp/pap-secrets");	
	fd = open("/etc/ppp/pap-secrets",O_WRONLY);
	if(fd < 0) {
		LOGD("PL: unable to open /etc/ppp/pap-secrets\n");
		goto error;
	}
	write(fd, userpass, len);
	close(fd);

	// if file does not exist we will create it
	system("touch /etc/ppp/chap-secrets");
	fd = open("/etc/ppp/chap-secrets",O_WRONLY);
	if(fd < 0) {
		LOGD("PL: unable to open /etc/ppp/chap-secrets\n");
		goto error;
	}
	write(fd, userpass, len);
	close(fd);
	free(userpass);

	LOGD("PL: hackSetupData: saving options\n");
	// if file does not exist we will create it
	system("touch /etc/ppp/options.smd");
	pppconfig = fopen("/etc/ppp/options.smd","r");
	if(!pppconfig) {
		LOGD("PL: unable to open /etc/ppp/options.smd\n");
		goto error;
	}

	//filesize
	fseek(pppconfig, 0, SEEK_END);
	buffSize = ftell(pppconfig);
	rewind(pppconfig);

	//allocate memory
	buffer = (char *) malloc (sizeof(char)*buffSize);
	if (buffer == NULL) {
		LOGD("PL: hackSetupData: malloc buffer failed\n");
		goto error;
	}

	//read in the original file
	len = fread (buffer,1,buffSize,pppconfig);
	if (len != buffSize) {
		LOGD("PL: hackSetupData: fread failed\n");
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

	// The modem replies immediately even if it's not connected!
	// so wait a short time.

	sleep(5);
	LOGD("PL: hackSetupData: launching pppd\n");
	mypppstatus = system("/system/bin/pppd /dev/smd1 defaultroute");
	if (mypppstatus != 0) {
		LOGD("PL: system(/system/bin/pppd failed\n");
		goto error;
	}
	else {
		LOGD("PL: hackSetupData: system(/system/bin/pppd returned %d\n", mypppstatus);
	}
	sleep(5); // allow time for ip-up to run


	LOGD("PL:hack normal exit\n");
	
	RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
	return;
	error:
		LOGD("PL:hackSetupData: ERROR ?");
		RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static int
checkHaretBoot()
{
  int fd;
  char cmdline[512];

  fd = open ("/proc/cmdline", O_RDONLY);

  if (fd < 0)
    {
      LOGD("Unable to open /proc/cmdline");
      return -1;
    }

  if (read (fd, cmdline, sizeof(cmdline)) < 0)
    {
      LOGD("Unable to read /proc/cmdline");
      return -1;
    }
  LOGD("Kernel command line is: '%s'", cmdline);
  
  close (fd);
  return strstr(cmdline,"nand_boot=0");
}


void writeAdditionalNandInit(){
	LOGD("NAND boot, writing additional init commands to /dev/smd0");
	int err = 0;
	int fd_smd;
	struct termios  ios;

	fd_smd = open ("/dev/smd0", O_RDWR);

	if(fd_smd  == -1)  {
		LOGD("PL:writeInit: Error opening smd0\n");
	}

	tcgetattr( fd_smd, &ios );
	ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
	tcsetattr( fd_smd, TCSANOW, &ios );
	
	err += hackWrite(fd_smd, "AT@BRIC=0");
	sleep(1);
	err += hackWrite(fd_smd, "AT+CFUN=0");
	sleep(1);
	err += hackWrite(fd_smd, "AT+COPS=2");
	sleep(1);
	
	close(fd_smd);
}

void (*htc_onRequest)(int request, void *data, size_t datalen, RIL_Token t);
void onRequest(int request, void *data, size_t datalen, RIL_Token t) {
	if(pppMode){
		if(request==RIL_REQUEST_SETUP_DATA_CALL){ // Let's have fun !
			hackSetupData(data, datalen, t);
			return;
		} else if(request==RIL_REQUEST_DEACTIVATE_DATA_CALL) {
			hackDeactivateData(data, datalen, t);
			return;
		}
	}
	return htc_onRequest(request, data, datalen, t);
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv) {
	s_rilenv = env;

	if(!checkHaretBoot()) writeAdditionalNandInit();
	
	if(file_exists("/system/etc/ppp/active")){
			pppMode = 1;
			LOGD("Using PPP mode");
		} else {
			LOGD("Using RMNET mode");
		}
	
	ril_handler=dlopen("/system/lib/libhtc_ril.so", 0/*Need to RTFM, 0 seems fine*/);
	RIL_RadioFunctions* (*htc_RIL_Init)(const struct RIL_Env *env, int argc, char **argv);

	htc_RIL_Init=dlsym(ril_handler, "RIL_Init");
	RIL_RadioFunctions *s_callbacks;
	s_callbacks=htc_RIL_Init(env, argc, argv);
	htc_onRequest=s_callbacks->onRequest;
	s_callbacks->onRequest=onRequest;
	return s_callbacks;
}