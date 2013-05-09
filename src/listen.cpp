// --------------------------------------------------------------------------
// Copyright � 2012 Peter Moss (peter@petermoss.com)
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.  See GNULicenseV3.txt for more details.
// --------------------------------------------------------------------------
//#include <windows.h>
#include <stdlib.h>
#include <winsock2.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys\stat.h>

#include "log.h"
#include "config.h"
#include "miner_api.h"
#include "miner_monitor.h"
#include "listen.h"
#include "wdmain.h"
#include "smtp.h"
#include "pool.h"
#include "btc.h"
#include "network.h"
#include "adl.h"

#define WEB_STATUS       "status"
#define WEB_REBOOT       "reboot"
#define WEB_RESTART      "restart"
#define WEB_GETLOG       "getlog"
#define WEB_GETLOG1      "getlog1"
#define WEB_GETLOG2      "getlog2"
#define WEB_GETLOG3      "getlog3"
#define WEB_DISABLEGPUS  "disable-gpus"
#define WEB_ENABLEGPUS   "enable-gpus"
#define WEB_SWITCH_POOL  "switch_pool"
#define WEB_HELP         "help"

#define RESPONSE_SIZE 2048
#define SERVICE_UNAVAILABLE "<html><body><font face=\"Courier\">Service unavailable.</font></body></html>"

int restartCount = 0;

void incrementRestartCount() { restartCount++; };

int shouldWeReboot(CGMConfig * cfg) { return restartCount > cfg->wdogNumberOfRestarts; }

BOOL systemReboot()
{
   HANDLE hToken; 
   TOKEN_PRIVILEGES tkp; 
 
   // Get a token for this process. 
 
   if (!OpenProcessToken(GetCurrentProcess(), 
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) 
      return( FALSE ); 
 
   // Get the LUID for the shutdown privilege. 
 
   LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, 
        &tkp.Privileges[0].Luid); 
 
   tkp.PrivilegeCount = 1;  // one privilege to set    
   tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; 
 
   // Get the shutdown privilege for this process. 
 
   AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, 
        (PTOKEN_PRIVILEGES)NULL, 0); 
 
   if (GetLastError() != ERROR_SUCCESS) 
   {
	  CloseHandle(hToken);
      return FALSE; 
   }
 
   // Shut down the system and force all applications to close. 
 
   if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 
               SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
               SHTDN_REASON_MINOR_UPGRADE |
               SHTDN_REASON_FLAG_PLANNED)) 
   {
	  CloseHandle(hToken);
      return FALSE; 
   }

   CloseHandle(hToken);
   //shutdown was successful
   return TRUE;
}

void reboot(int reason)
{
	BOOL rc = FALSE;

	send_smtp_rebooted_msg(reason);

	debug_log(LOG_SVR, "main(): ----------- Rebooting -------------");
	WSACleanup();
	debug_logClose();

	rc = systemReboot();
}

void getWatchdogStatus(char * status, int statusSize, double * avg, double * util, int * hw)
{
	Miner_Info _mi;
	cgmProcessInfo _minerProcessInfo;
	cgmProcessInfo _akbashProcessInfo;
	ADLInfo _adlInfo;

    char _temp[GPU_STATS_STR_LEN] = {0};
	int i = 0;
	int len = 0;
	double avgTotal = 0.0;
	double utilTotal = 0.0;

	char ageStr[16];

	getMinerStats(&_mi);

	getProcessInfo(cfg->minerExeName, &_minerProcessInfo);
	getProcessInfo(cfg->wdogExeName, &_akbashProcessInfo);

	getADLStats(&_adlInfo);

	memset(status, 0, statusSize);
	strcat_s(status, statusSize, "<html><body><font face=\"Courier\">");

	memset(_temp, 0, sizeof(_temp));

	// --------------
	// Miner details.
	// --------------
	strcat_s(status, statusSize, "miner details:<br>--------------<br>");
	sprintf_s( _temp, sizeof(_temp),
		       "total avg: %.2f Mh/s, a: %d, r: %d, hw: %d, e: %.2f%%, u: %.2f/m, bs: %.2fK<br><br>",
			   _mi.summary.mhsAvg, 
			   _mi.summary.accepted, 
			   _mi.summary.rejected,
			   _mi.summary.hw, 
			   _mi.summary.getworks ? _mi.summary.accepted*100.0/_mi.summary.getworks : 0,
			   _mi.summary.util,
			   _mi.summary.bestshare			   
			);

	strcat_s(status, statusSize, _temp);

	sprintf_s( _temp, sizeof(_temp), 
		       "age: %03d:%02d:%02d:%02d, ", 
               _mi.summary.days, _mi.summary.hrs, _mi.summary.min, _mi.summary.secs
			 );
    strcat_s(status, statusSize, _temp);


	sprintf_s( _temp, 
		    sizeof(_temp),
			"s: %s, rc: %d/%d pools: %d<br>",
			gpuStatusStr(_mi.status),
			restartCount, 
			cfg->wdogNumberOfRestarts,
			_mi.config.poolCount
		);

	strcat_s(status, statusSize, _temp);


	sprintf_s( _temp, 
		       sizeof(_temp),
		     "pid: %d, ws: %u/%u, ", 
			 _minerProcessInfo.processID,			 
			 _minerProcessInfo.workingSetSize/1024/1024,
			 cfg->minerWorkingSetThreshold/1024/1024
		   );
	strcat_s(status, statusSize, _temp);

	sprintf_s( _temp, 
		       sizeof(_temp),
		     "hc: %d/%u<br><br>", 
			 _minerProcessInfo.handleCount,
			 cfg->minerHandleCountThreshold
		   );
	strcat_s(status, statusSize, _temp);

	*avg  = _mi.summary.mhsAvg;
	*hw   = _mi.summary.hw;
	*util = _mi.summary.util;

	if (_mi.config.gpuCount > 0)
	{
		for (i=0; i < _mi.config.gpuCount; i++)
		{
			if (waitForShutdown(1)) break;

			sprintf_s( _temp, sizeof(_temp), "gpu %d: %s (%s), %.2f Mh/s, %.0fC@%02d%%, %d/%d, hw: %d, u: %.2f/m<br>", 
					   _mi.gpu[i].id, gpuStatusStr(_mi.gpu[i].status), 
					   _mi.gpu[i].disabled ? "OFF" : "ON",
					   _mi.gpu[i].avg, _mi.gpu[i].temp, _mi.gpu[i].fan, _mi.gpu[i].engine, _mi.gpu[i].mem, _mi.gpu[i].hw, _mi.gpu[i].util
					 );

			strcat_s(status, statusSize, _temp);
			avgTotal += _mi.gpu[i].avg;
			utilTotal += _mi.gpu[i].util;
		}

		if (waitForShutdown(1)) return;

		sprintf_s( _temp, sizeof(_temp),
		    "total GPU avg: %.2f Mh/s, u: %.2f/m<br><br>",
			avgTotal, 
			utilTotal
		);

		strcat_s(status, statusSize, _temp);
	}

	avgTotal = 0.0;
	utilTotal = 0.0;

	for (i=0; i < _mi.config.pgaCount; i++)
	{
		if (waitForShutdown(1)) break;

		sprintf_s( _temp, sizeof(_temp), "pga %d: %s (%s), %.2f Mh/s, %.0fC, hw: %4d, u: %.2f/m<br>", 
			       _mi.pga[i].id, 
				   gpuStatusStr(_mi.pga[i].status), 
				   _mi.pga[i].disabled ? "OFF" : "ON",
				   _mi.pga[i].avg, _mi.pga[i].temp, _mi.pga[i].hw, _mi.pga[i].util 
				 );
		strcat_s(status, statusSize, _temp);

		avgTotal += _mi.pga[i].avg;
		utilTotal += _mi.pga[i].util;
	}

	if (_mi.config.pgaCount > 0)
	{
		sprintf_s( _temp, sizeof(_temp),
		    "total PGA avg: %.2f Mh/s, u: %.2f/m<br><br>",
			avgTotal, 
			utilTotal
		);

		strcat_s(status, statusSize, _temp);
	}

	if (_mi.config.poolCount > 0)
	{
		for (i=0; i < _mi.config.poolCount; i++)
		{
			sprintf_s( _temp, sizeof(_temp),
				"pool: %d, p: %d, url: %s, s: %s<br>",
				_mi.pools[i].id, 
				_mi.pools[i].priority,
				_mi.pools[i].url,
				_mi.pools[i].status
			);

			strcat_s(status, statusSize, _temp);
		}
	}

		// --------------------
	// AMD GPU H/W details.
	// --------------------
	if (cfg->wdogAdlDisabled == 0)
	{
		strcat_s(status, statusSize, "<br>amd gpu h/w details:<br>--------------------<br>");

		sprintf_s( _temp, 
				sizeof(_temp),
				"u: %d%%, t: %dC, fans ok: %s<br>", 
				_adlInfo.iMinActivity,
				_adlInfo.iMaxTemp,
				_adlInfo.iZeroFan == 0 ? "yes" : "<b>no!!!</b>"
			);

		strcat_s(status, statusSize, _temp);
	}


	if (cfg->poolInfoDisabled == 0)
	{
		// -------------
		// Pool details.
		// -------------
		sprintf_s(_temp, sizeof(_temp), "<br>pool (%s) statistics:<br>", cfg->poolHost);
		strcat_s(status, statusSize, _temp);
	
		len = strlen(cfg->poolHost);

		len += 19;

		for (i=0; i < len; i++)
			strcat_s(status, statusSize, "-");
	
		strcat_s(status, statusSize, "<br>");

		getPoolStatsStr(_temp);
		strcat_s(status, statusSize, _temp);
		strcat_s(status, statusSize, "<br>");
	}

	if (cfg->btcQuotesDisabled == 0 || cfg->ltcQuotesDisabled == 0)
		strcat_s(status, statusSize, "<br>currency quotes:<br>----------------<br>");

	if (cfg->btcQuotesDisabled == 0)
	{
		// -----------
		// BTC quotes.
		// -----------
		getBTCStatsStr(_temp);
		strcat_s(status, statusSize, _temp);
	}

	if (cfg->ltcQuotesDisabled == 0)
	{
		// -----------
		// LTC quotes.
		// -----------
		getLTCStatsStr(_temp);
		if (cfg->btcQuotesDisabled == 0)
			strcat_s(status, statusSize, "<br>");

		strcat_s(status, statusSize, _temp);
	}

	// -----------------
	// Watchdog details.
	// -----------------
	age(ageStr, sizeof(ageStr));
	sprintf_s( _temp, 
		       sizeof(_temp),
		     "<br><br>watchdog details:<br>-----------------<br>pid: %d, ws: %d, hc: %d, age: %s<br>",
			 _akbashProcessInfo.processID,
			 _akbashProcessInfo.workingSetSize/1024/1024,
			 _akbashProcessInfo.handleCount,
			 ageStr
		   );
	strcat_s(status, statusSize, _temp);

	strcat_s(status,  statusSize, "<br><br></font></body></html>");

	debug_log(LOG_DBG, "getWatchdogStatus(): status len: %d", strlen(status));
} // end of getWatchdogStatus()

DWORD WINAPI listenForCommands( LPVOID lpParam )
{
	struct hostent *ip = NULL;
	struct sockaddr_in serv;
	char response[SOCK_MAX_HTTP_LINE+1] = {0};
	char * id = NULL;
	char ok[WDOG_STATUS_STR_LEN] = {0};
	char * p = NULL;
	char *cmd = NULL;

	int i = 0;
	int rc = 0;

	SOCKET sock;
	int ret = 0;
	int outLen = 0;
	int len = 0;

	int index = 0;
	int err = 0;
	int rebootCmdReceived = 0;
	int getLogReceived = 0;
	int restartCmdReceived = 0;
	int helpCmdReceived = 0;

	struct sockaddr_in saClient;
	int nSALen = sizeof(saClient);
	int bytesSent = 0;
	int bytesLeft = 0;
	int lastError = 0;
	char * chunkPtr = NULL;
	double avg = 0;
	double util = 0;
	int hw = 0;

	WSAEVENT acceptEvent = NULL;
	
	CGMConfig * cfg = getCGMConfig();

	ip = gethostbyname(cfg->wdogListenIP);

	if (ip == NULL) 
	{
		debug_log(LOG_ERR, "listenForCommands(): unable to resolve: %s, error: %d", cfg->wdogListenIP, WSAGetLastError());
		return 0;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET) 
	{
		debug_log(LOG_ERR, "listenForCommands(): socket() failed: %d", WSAGetLastError());
		return 0;
	}

	memset(&serv, 0, sizeof(serv));
	serv.sin_family = AF_INET;
	serv.sin_addr = *((struct in_addr *)ip->h_addr);
	serv.sin_port = htons(cfg->wdogListenPort);

	ret = bind(sock, (SOCKADDR *) & serv, sizeof (serv));
    if (ret == SOCKET_ERROR) 
	{
        debug_log(LOG_ERR, "listenForCommands(): bind function failed with error %d\n", WSAGetLastError());

        ret = closesocket(sock);
        if (ret == SOCKET_ERROR)
            debug_log(LOG_ERR, "listenForCommands(): closesocket function failed with error %d\n", WSAGetLastError());

        return 0;
    }

    // ---------------------------------------
    // Listen for incoming connection requests 
    // on the created socket
	// ---------------------------------------
    if (listen(sock, SOMAXCONN) == SOCKET_ERROR)
	{
        debug_log(LOG_ERR, "listenForCommands(): listen function failed with error: %d\n", WSAGetLastError());
		
		ret = closesocket(sock);

        if (ret == SOCKET_ERROR)
            debug_log(LOG_ERR, "listenForCommands(): closesocket function failed with error %d\n", WSAGetLastError());

		return 0;
	} 

	debug_log(LOG_INF, "listenForCommands(): listening on %s@%d", inet_ntoa(serv.sin_addr), cfg->wdogListenPort);

	acceptEvent = WSACreateEvent();

	if (WSAEventSelect(sock, acceptEvent, FD_ACCEPT) == SOCKET_ERROR)
	{
		debug_log(LOG_ERR, "listenForCommands(): WSAEventSelect function failed with error: %d\n", WSAGetLastError());
		closesocket(sock);
		return 0;
	}

	while (waitForShutdown(1) == 0)
	{
		// ------------------------------------------------
		// Create a SOCKET for accepting incoming requests.
		// ------------------------------------------------
		SOCKET acceptSock;

		// ----------------------
		// Accept the connection.
		// ----------------------
		nSALen = sizeof( saClient );
		memset(&saClient, 0, nSALen);
		
		acceptSock = accept( sock, (struct sockaddr*)&saClient, &nSALen );

		if (acceptSock == INVALID_SOCKET) 
		{
			int rc = WSAGetLastError();
			if (rc != WSAEWOULDBLOCK)
			{
				debug_log(LOG_ERR, "listenForCommands(): accept failed with error: %ld\n", WSAGetLastError());
				closesocket(sock);
				return 0;
			} else
			{
				wait(1500);

				if (waitForShutdown(1) == 1)
					break;

				continue; // go back to block on accept
			}
		} else
		{
			debug_log(LOG_INF, "listenForCommands(): new connection from: %s", inet_ntoa(saClient.sin_addr));
		}

		memset(response, 0, sizeof(response));
		outLen = 0;
		rc = net_recvHttpResponse(acceptSock, response, sizeof(response), SOCK_TIMEOUT_INTERVAL);
		//debug_log("listenForCommands(): net_recvHttpResponse() response: %s, rc: %d", response, rc);
		if ( rc != SOCK_NO_ERROR) 
		{
			debug_log(LOG_ERR, "listenForCommands(): net_recvHttpResponse() failed: %d", rc);
			goto tend;
		}

		getLogReceived = 0;
		restartCmdReceived = 0;
		helpCmdReceived = 0;
		id = NULL;
	    p = NULL;
	    cmd = NULL;
		memset(ok, 0, sizeof(ok));

		avg = 0.0;
		hw = 0;
		util = 0.0;

		if (waitForShutdown(1)) break;

		if (strlen(response) > 20)
			id = strstr(response, "wd_cmd=");  // "GET /wd_cmd=status;"

		if (id != NULL)
		{
			p = strstr(id+7, ";");
			if (p != NULL)
			{
				*p = 0;
				cmd = (char *) calloc(1, p-id);

				strcpy(cmd, id+7);

				if (strncmp(cmd, WEB_STATUS, strlen(WEB_STATUS)) == 0)
				{
					if (waitForShutdown(1)) break;

					if (cfg->wdogDisableRemoteStatus == 0)  // Enabled
					{
						debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_STATUS);
						
						getWatchdogStatus(ok, sizeof(ok), &avg, &util, &hw);
	
						//debug_log("listenForCommands(): ok \'%s\' command", ok);

						if (waitForShutdown(1)) break;
					} else
					{
						strcpy( ok, SERVICE_UNAVAILABLE);
					}
				} else
				{
					if (strncmp(cmd, WEB_RESTART, strlen(WEB_RESTART)) == 0)
					{
						debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_RESTART);
						
						if (cfg->wdogDisableRemoteRestart == 1)  
						{
							restartCmdReceived = 0;
							strcpy( ok, SERVICE_UNAVAILABLE);
						} else
						{
							restartCmdReceived = 1;
							strcpy(ok, "<html><body>200 OK</body></html>");
						}
					} else
					{
						if (strncmp(cmd, WEB_REBOOT, strlen(WEB_REBOOT)) == 0)
						{
							debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_REBOOT);

							if (cfg->wdogDisableRemoteReboot == 1)
							{
								rebootCmdReceived = 0;
								strcpy( ok, SERVICE_UNAVAILABLE);
							} else
							{
								rebootCmdReceived = 1;
								strcpy(ok, "<html><body>200 OK</body></html>");
							}

						} else
						{
							if (strncmp(cmd, WEB_GETLOG, strlen(WEB_GETLOG)) == 0)
							{
								debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_GETLOG);
								getLogReceived = 1;
							} else
							{
								if (strncmp(cmd, WEB_HELP, strlen(WEB_HELP)) == 0)
								{
									debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_HELP);
									helpCmdReceived = 1;
								}
								else
								{
									if (strncmp(cmd, WEB_DISABLEGPUS, strlen(WEB_DISABLEGPUS)) == 0)
									{
										debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_DISABLEGPUS);
										disableGPUs();
										cfg->smartMeterOnPeak = 1;
										strcpy(ok, "<html><body>200 OK</body></html>");
									} else
									{
										if (strncmp(cmd, WEB_ENABLEGPUS, strlen(WEB_ENABLEGPUS)) == 0)
										{
											debug_log(LOG_DBG, "listenForCommands(): received \'%s\' command", WEB_ENABLEGPUS);
											enableGPUs();
											cfg->smartMeterOnPeak = 0;
											strcpy(ok, "<html><body>200 OK</body></html>");
										} else											
										{
											if (strncmp(cmd, WEB_SWITCH_POOL, strlen(WEB_SWITCH_POOL)) == 0)
											{
												debug_log(LOG_SVR, "listenForCommands(): received %s command", WEB_SWITCH_POOL);
												p = cmd+strlen(WEB_SWITCH_POOL);
												int poolNum = atoi(p);

												debug_log(LOG_SVR, "listenForCommands(): pool to switch: %d", poolNum);
												switchPool(poolNum);
												strcpy(ok, "<html><body>200 OK</body></html>");
											} else
											{											
												debug_log(LOG_SVR, "listenForCommands(): received unknown command");
											}
										}
									}															
								}

							}
						}
					} 
				}
				free(cmd);
			} else
			{
				debug_log(LOG_SVR, "listenForCommands(): received incorrect command");
				helpCmdReceived = 1;
			}

		} else
		{
			debug_log(LOG_SVR, "listenForCommands(): received invalid request");
			helpCmdReceived = 1;						
		}
		
		if (waitForShutdown(1)) break;

		if (getLogReceived == 1)
		{
			if (cfg->wdogDisableRemoteGetLog ==  0) // Enabled
			{
				int fd = 0;
				long flen = 0;
				long count = 0;
				long bytesSent = 0;
				struct _stat stBuf;

				// Open a file.
				if( _sopen_s( &fd, cfg->wdogLogFileName, _O_RDONLY, _SH_DENYNO, _S_IREAD) )
				{
					debug_log(LOG_ERR, "listenForCommands(): Unable to open \'%s\' file.  _sopen_s() failed: \'%s\'", cfg->wdogLogFileName, strerror(errno));
					goto tend;
				}

				memset(&stBuf, 0, sizeof(stBuf));
				if (_fstat(fd, &stBuf))
				{
					debug_log(LOG_ERR, "listenForCommands(): Unable to get stats for open \'%s\' file.  _fstat() failed: \'%s\'", cfg->wdogLogFileName, strerror(errno));
					_close(fd);
					goto tend;
				}

				flen = stBuf.st_size;

				build_http_response(response, flen+45);

				outLen = 0;
				rc = net_sendBytes(acceptSock, response, strlen(response), &outLen, SOCK_TIMEOUT_INTERVAL);				
				if ( rc != SOCK_NO_ERROR)				
				{
					debug_log(LOG_ERR, "listenForCommands(): send() response header error: %d, len: %d, outLen: %d", rc, len, outLen);
					_close(fd);
					goto tend;
				}

				if (rc == SOCK_NO_ERROR)
				{
					sprintf(ok, "<html><body><pre><br>");
					len = strlen(ok);
					outLen = 0;
					rc = net_sendBytes(acceptSock, ok, len, &outLen, SOCK_TIMEOUT_GETLOG);
					if ( rc != SOCK_NO_ERROR)				
					{
						debug_log(LOG_ERR, "listenForCommands(): send() error: %d, len: %d, outLen: %d", rc, len, outLen);
						_close(fd);
						goto tend;
					} 

					if (rc == SOCK_NO_ERROR)
					{
						FILE * o = _fdopen(fd, "rb" );
						char * chunk = NULL;

						if (o == NULL)
						{
							debug_log(LOG_ERR, "listenForCommands(): failed to fdopen() the log file. errno: %s", strerror(errno));
							goto tend;
						}

						chunk = (char *) calloc(1, 14900);	
						if (chunk == NULL)
						{
							debug_log(LOG_ERR, "listenForCommands(): failed to allocate memory for chunk of log file. errno: %s", strerror(errno));
							fclose(o);
							goto tend;
						}

						while (!feof(o))
						{
							count = 0;
							memset(chunk, 0, 14900);

							// Attempt to read in 100 bytes:
							count = fread( chunk, sizeof( char ), 14900, o );
							if(ferror( o ) != 0)  
							{
								debug_log(LOG_ERR, "ferror() non zero, error: %s\n",  strerror(errno));
								break;
							}

							if (count > (flen-bytesSent))
								count = flen-bytesSent;

							outLen = 0;
							rc = net_sendBytes(acceptSock, chunk, count, &outLen, SOCK_TIMEOUT_GETLOG);
							if ( rc != SOCK_NO_ERROR)	
							{
								debug_log(LOG_ERR, "listenForCommands(): send() error: %d, count: %d, outLen: %d", rc, count, outLen);
								break;
							}

							bytesSent += outLen;
						} // while (bytesSent < flen)

						fclose(o);
						free(chunk);
					}
				}

				if (rc == SOCK_NO_ERROR)
				{
					sprintf(ok, "<br></pre></body></html>");
					len = strlen(ok);
					outLen = 0;
					rc = net_sendBytes(acceptSock, ok, len, &outLen, SOCK_TIMEOUT_GETLOG);
					if ( rc != SOCK_NO_ERROR)				
					{
						debug_log(LOG_ERR, "listenForCommands(): send(end of html) error: %d, len: %d, outLen: %d", rc, len, outLen);
					}
				} else
				{
					goto tend;
				}
			
			} else
			{
				strcpy( ok, SERVICE_UNAVAILABLE);
				len = strlen(ok);
				outLen = 0;
				rc = net_sendBytes(acceptSock, ok, len, &outLen, SOCK_TIMEOUT_INTERVAL);
				if ( rc != SOCK_NO_ERROR)				
				{
					debug_log(LOG_ERR, "listenForCommands(): send() error: %d, len: %d, outLen: %d", rc, len, outLen);
				}
			}
		} else
		{
			if (helpCmdReceived == 1)
			{
				if (cfg->wdogDisableRemoteHelp ==  1)
				{
					strcpy( ok, SERVICE_UNAVAILABLE);
				} else
				{
					sprintf( ok, 
  						     "<html><body><font face=\"Courier\"><pre><br>The following commands are supported by Akbash ver. %s:<br><br>- <a href=\"/wd_cmd=status;\">wd_cmd=status;</a> - provides the latest miner status and statistics<br><br>- <a href=\"/wd_cmd=restart;\">wd_cmd=restart;</a> - restarts the miner process<br><br>- <a href=\"/wd_cmd=reboot;\">wd_cmd=reboot;</a> - reboots the OS!!!<br><br>- <a href=\"/wd_cmd=getlog;\">wd_cmd=getlog;</a> - downloads contents of a log file<br><br>- <a href=\"/wd_cmd=switch_poolN;\">wd_cmd=switch_poolN;</a> - switches current pool to pool N (0,1,2..)<br><br>- <a href=\"wd_cmd=help;\">wd_cmd=help;</a> - this page<br><br><br>Any other GET requests will be ignored.  All POST requests are ignored.<br><br>"SEND_DONATIONS_TO"<br></font></pre></body></html>",
						     WDOG_VERSION
						   );
				}
			} 

			len = strlen(ok);
			memset(response, 0, sizeof(response));
			build_http_response(response, len);
			outLen = 0;
			//debug_log("listenForCommands(): sending response header: %s", response);
			rc = net_sendBytes(acceptSock, response, strlen(response), &outLen, SOCK_TIMEOUT_INTERVAL);
			if ( rc != SOCK_NO_ERROR)				
			{
				debug_log(LOG_ERR, "listenForCommands(): send() response header error: %d, len: %d, outLen: %d", rc, len, outLen);
			} else
			{
				//debug_log("listenForCommands(): sending msg body: %s", ok);
				outLen = 0;
				rc = net_sendBytes(acceptSock, ok, len, &outLen, SOCK_TIMEOUT_INTERVAL);
				if ( rc != SOCK_NO_ERROR)				
				{
					debug_log(LOG_ERR, "listenForCommands(): send() content page error: %d, len: %d, outLen: %d", rc, len, outLen);
				}
				//debug_log("listenForCommands(): sending msg body, bytes sent: %d", outLen);
				//Sleep(1000);
			}
		}
tend:
		debug_log(LOG_INF, "listenForCommands(): closing accepted connection.");
		closesocket(acceptSock);

		if (rebootCmdReceived)
		{
			closesocket(sock);
			reboot(WDOG_REBOOT_VIA_HTTP);
		}

		if (restartCmdReceived)
			restartMiner(DEFAULT_RESTART_DELAY, WDOG_RESTART_VIA_HTTP);  // 5 seconds delay

	} // while (shutdown == 0)

	debug_log(LOG_SVR, "listenForCommands(): exiting thread: 0x%04x", GetCurrentThreadId());

	closesocket(sock);
	ExitThread(0);

	return 0; // ok
} // end of listenForCommands()