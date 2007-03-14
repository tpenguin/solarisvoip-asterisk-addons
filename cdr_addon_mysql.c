/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * MySQL CDR logger 
 * 
 * James Sharp <jsharp@psychoses.org>
 *
 * Modified August 2003
 * Tilghman Lesher <asterisk__cdr__cdr_mysql__200308@the-tilghman.com>
 *
 * Modified August 6, 2005
 * Joseph Benden <joe@thrallingpenguin.com>
 * Added mysql connection timeout parameter
 * Added an automatic reconnect as to not lose a cdr record
 * Cleaned up the original code to match the coding guidelines
 *
 * Modified June 11, 2006
 * Joseph Benden <joe@thrallingpenguin.com>
 * Added SQL spooling of INSERT statements for when the database connection is lost.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License.
 *
 */

#include <sys/types.h>
#include <asterisk/config.h>
#include <asterisk/options.h>
#include <asterisk/channel.h>
#include <asterisk/cdr.h>
#include <asterisk/module.h>
#include <asterisk/logger.h>
#include <asterisk/cli.h>

#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <mysql.h>
#include <errmsg.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define DATE_FORMAT "%Y-%m-%d %T"

static char *desc = "MySQL CDR Backend";
static char *name = "mysql";
static char *config = "cdr_mysql.conf";
static char *hostname = NULL, *dbname = NULL, *dbuser = NULL, *password = NULL, *dbsock = NULL, *dbtable = NULL, *dbspool = NULL;
static int hostname_alloc = 0, dbname_alloc = 0, dbuser_alloc = 0, password_alloc = 0, dbsock_alloc = 0, dbtable_alloc = 0, dbspool_alloc = 0;
static int dbport = 0;
static int connected = 0;
static time_t connect_time = 0;
static int records = 0;
static int totalrecords = 0;
static int userfield = 0;
static unsigned int timeout = 0;

AST_MUTEX_DEFINE_STATIC(mysql_lock);

static MYSQL mysql;

static char cdr_mysql_status_help[] =
"Usage: cdr mysql status\n"
"       Shows current connection status for cdr_mysql\n";
static char cdr_mysql_unspool_help[] =
"Usage: cdr mysql unspool\n"
"       Attempts to unspool the available queries to the database\n";
static char cdr_mysql_connect_help[] =
"Usage: cdr mysql connect\n"
"       Attempts to connect to the database server\n";
static char cdr_mysql_disconnect_help[] = 
"Usage: cdr mysql disconnect\n"
"       Disconnects from the database server\n";

static int handle_cdr_mysql_unspool(void)
{
	struct stat st;
	char buf[2048] = "";
	int error = 0, good = 0;
	char *spoolbackup = NULL;
	FILE *back = NULL, *mf = NULL;
	char *myerror;

	if (!dbspool)
		return 0;

	if (0 == stat(dbspool, &st)) {
		ast_log(LOG_WARNING, "cdr_mysql: Loading spool records from %s\n", dbspool);
		mf = fopen(dbspool, "r");
		if (!mf)
			ast_log(LOG_ERROR, "cdr_mysql: Unable to open %s (%s)\n", dbspool, strerror(errno));
		else {
			while (fgets(buf, 2048, mf)) {
				if (!error) {
					// execute mysql statement
					ast_log(LOG_DEBUG, "cdr_mysql: Executing SQL: %s\n", buf);
					if (mysql_real_query(&mysql,buf,strlen(buf))) {
						ast_log(LOG_ERROR,"cdr_mysql: Failed to insert into database: (%d) %s",mysql_errno(&mysql),mysql_error(&mysql));
						error++;
					} else {
						good++;
					}
				}
				if (error && !spoolbackup) {
					spoolbackup = malloc(strlen(dbspool) + 5);
					if (!spoolbackup) {
						ast_log(LOG_ERROR, "cdr_mysql: Fatal error in spool load, out of memory.\n");
						return -1;
					}
					sprintf(spoolbackup, "%s.bak", dbspool);
					back = fopen(spoolbackup, "a");
					if (!back) {
						ast_log(LOG_ERROR, "cdr_mysql: Fatal error in spool load, couldn't open %s\n", spoolbackup);
						return -1;
					}
				}
				if (back) {
					fprintf(back, "%s", buf);
				}
			}
			fclose(mf);
			unlink(dbspool);
			if (back) {
				fclose(back);
				rename(spoolbackup, dbspool);
				free(spoolbackup);
			}
			ast_log(LOG_WARNING, "cdr_mysql: Finished loading from spool: %d commands executed successfully, %d failed\n", good, error);
		}
	} /* end stat check */
	return 0;
}

static int handle_cdr_mysql_spool(char *cmd)
{
	FILE *mf = NULL;

	if (!dbspool)
		return 0;

	mf = fopen(dbspool, "a");
	if (!mf)
		ast_log(LOG_ERROR, "cdr_mysql: Unable to open %s (%s)\n", dbspool, strerror(errno));
	else {
		fprintf(mf, "%s\n", cmd);
		fflush(mf);
		fclose(mf);
	}
	return 0;
}
	
static int handle_cdr_mysql_status(int fd, int argc, char *argv[])
{
	if (connected) {
		char status[256], status2[100] = "";
		int ctime = time(NULL) - connect_time;
		if (dbport)
			snprintf(status, 255, "Connected to %s@%s, port %d", dbname, hostname, dbport);
		else if (dbsock)
			snprintf(status, 255, "Connected to %s on socket file %s", dbname, dbsock);
		else
			snprintf(status, 255, "Connected to %s@%s", dbname, hostname);

		if (dbuser && *dbuser)
			snprintf(status2, 99, " with username %s", dbuser);
		if (dbtable && *dbtable)
			snprintf(status2, 99, " using table %s", dbtable);
		if (ctime > 31536000) {
			ast_cli(fd, "%s%s for %d years, %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 31536000, (ctime % 31536000) / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 86400) {
			ast_cli(fd, "%s%s for %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 3600) {
			ast_cli(fd, "%s%s for %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 60) {
			ast_cli(fd, "%s%s for %d minutes, %d seconds.\n", status, status2, ctime / 60, ctime % 60);
		} else {
			ast_cli(fd, "%s%s for %d seconds.\n", status, status2, ctime);
		}
		if (records == totalrecords)
			ast_cli(fd, "  Wrote %d records since last restart.\n", totalrecords);
		else
			ast_cli(fd, "  Wrote %d records since last restart and %d records since last reconnect.\n", totalrecords, records);
		return RESULT_SUCCESS;
	} else {
		ast_cli(fd, "Not currently connected to a MySQL server.\n");
		return RESULT_FAILURE;
	}
}

static int cli_cdr_mysql_unspool(int fd, int argc, char *argv[])
{
	int res = RESULT_FAILURE;
	ast_mutex_lock(&mysql_lock);
	if (!connected) {
		ast_cli(fd, "Not currently connected. Skipping.\n");
	} else {
		handle_cdr_mysql_unspool();
		res = RESULT_SUCCESS;
	}
	ast_mutex_unlock(&mysql_lock);
	return res;
}

static int cli_cdr_mysql_connect(int fd, int argc, char *argv[])
{
	int res = RESULT_FAILURE;
	
	if (connected) {
		ast_cli(fd, "Already connected.\n");
		return res;
	}
	
	ast_mutex_lock(&mysql_lock);
	
	mysql_init(&mysql);

	if (timeout && mysql_options(&mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *)&timeout)!=0) {
		ast_cli(fd, "cdr_mysql: mysql_options returned (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
	}

	if (!mysql_real_connect(&mysql, hostname, dbuser, password, dbname, dbport, dbsock, 0)) {
		ast_cli(fd, "Failed to connect to mysql database %s on %s.\n", dbname, hostname);
		connected = 0;
		records = 0;
	} else {
		ast_cli(fd, "Successfully connected to MySQL database.\n");
		connected = 1;
		records = 0;
		connect_time = time(NULL);
		res = RESULT_SUCCESS;
	}
	ast_mutex_unlock(&mysql_lock);
	return res;
}

static int cli_cdr_mysql_disconnect(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&mysql_lock);
	connected = 0;
	records = 0;
	mysql_close(&mysql);
	ast_mutex_unlock(&mysql_lock);
	ast_cli(fd, "Disconnected.\n");
	return RESULT_SUCCESS;
}

static struct ast_cli_entry cdr_mysql_status_cli =
	{ { "cdr", "mysql", "status", NULL },
	handle_cdr_mysql_status, "Show connection status of cdr_mysql",
	cdr_mysql_status_help, NULL };
static struct ast_cli_entry cdr_mysql_unspool_cli =
	{ { "cdr", "mysql", "unspool", NULL },
	cli_cdr_mysql_unspool, "Unspool CDR records",
	cdr_mysql_unspool_help, NULL };
static struct ast_cli_entry cdr_mysql_connect_cli =
	{ { "cdr", "mysql", "connect", NULL },
	cli_cdr_mysql_connect, "Connect to database",
	cdr_mysql_connect_help, NULL };
static struct ast_cli_entry cdr_mysql_disconnect_cli =
	{ { "cdr", "mysql", "disconnect", NULL },
	cli_cdr_mysql_disconnect, "Disconnect from the database",
	cdr_mysql_disconnect_help, NULL };

static int mysql_log(struct ast_cdr *cdr)
{
	struct tm tm;
	struct timeval tv;
	struct localuser *u;
	char *userfielddata = NULL;
	char sqlcmd[2048], timestr[128];
	char *clid=NULL, *dcontext=NULL, *channel=NULL, *dstchannel=NULL, *lastapp=NULL, *lastdata=NULL;
	int retries = 5;
#ifdef MYSQL_LOGUNIQUEID
	char *uniqueid = NULL;
#endif

	ast_mutex_lock(&mysql_lock);

	memset(sqlcmd, 0, 2048);

	localtime_r(&cdr->start.tv_sec, &tm);
	strftime(timestr, 128, DATE_FORMAT, &tm);

db_reconnect:
	if ((!connected) && (hostname || dbsock) && dbuser && password && dbname && dbtable ) {
		/* Attempt to connect */
		mysql_init(&mysql);
		/* Add option to quickly timeout the connection */
		if (timeout && mysql_options(&mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *)&timeout)!=0) {
			ast_log(LOG_ERROR, "cdr_mysql: mysql_options returned (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
		}
		if (mysql_real_connect(&mysql, hostname, dbuser, password, dbname, dbport, dbsock, 0)) {
			connected = 1;
			connect_time = time(NULL);
			records = 0;
			handle_cdr_mysql_unspool();
		} else {
			if (dbspool)
				ast_log(LOG_ERROR, "cdr_mysql: cannot connect to database server %s. CDR will be spooled.\n", hostname);
			else
				ast_log(LOG_ERROR, "cdr_mysql: cannot connect to database server %s. CDR will not be spooled.\n", hostname);
			connected = 0;
		}
	} else {
		/* Long connection - ping the server */
		int error;
		if ((error = mysql_ping(&mysql))) {
			connected = 0;
			records = 0;
			/* This is needed because the value returned from
			   mysql_ping do not contain the full error 
			   possibilities */
			error = mysql_errno(&mysql);
			switch (error) {
				case CR_CONN_HOST_ERROR:
				case CR_CONNECTION_ERROR:
				case CR_SOCKET_CREATE_ERROR:
					ast_log(LOG_ERROR, "cdr_mysql: Unable to connect to server.\n");
					goto db_skipconnection;
				case CR_SERVER_GONE_ERROR:
				case CR_SERVER_LOST:
					ast_log(LOG_ERROR, "cdr_mysql: Server has gone away. Attempting to reconnect.\n");
					break;
				default:
					ast_log(LOG_ERROR, "cdr_mysql: Unknown connection error: (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
			}
			retries--;
			if (retries)
				goto db_reconnect;
			else
				ast_log(LOG_ERROR, "cdr_mysql: Retried to connect fives times, giving up.\n");
		}
	}

db_skipconnection:
	/* Maximum space needed would be if all characters needed to be escaped, plus a trailing NULL */
	/* WARNING: This code previously used mysql_real_escape_string, but the use of said function
	   requires an active connection to a database.  If we are not connected, then this function
	    cannot be used.  This is a problem since we need to store off the SQL statement into our
	   spool file for later restoration.
	   So the question is, what's the best way to handle this?  This works for now.
	*/
	if ((clid = alloca(strlen(cdr->clid) * 2 + 1)) != NULL)
		mysql_escape_string(clid, cdr->clid, strlen(cdr->clid));
	if ((dcontext = alloca(strlen(cdr->dcontext) * 2 + 1)) != NULL)
		mysql_escape_string(dcontext, cdr->dcontext, strlen(cdr->dcontext));
	if ((channel = alloca(strlen(cdr->channel) * 2 + 1)) != NULL)
		mysql_escape_string(channel, cdr->channel, strlen(cdr->channel));
	if ((dstchannel = alloca(strlen(cdr->dstchannel) * 2 + 1)) != NULL)
		mysql_escape_string(dstchannel, cdr->dstchannel, strlen(cdr->dstchannel));
	if ((lastapp = alloca(strlen(cdr->lastapp) * 2 + 1)) != NULL)
		mysql_escape_string(lastapp, cdr->lastapp, strlen(cdr->lastapp));
	if ((lastdata = alloca(strlen(cdr->lastdata) * 2 + 1)) != NULL)
		mysql_escape_string(lastdata, cdr->lastdata, strlen(cdr->lastdata));
#ifdef MYSQL_LOGUNIQUEID
	if ((uniqueid = alloca(strlen(cdr->uniqueid) * 2 + 1)) != NULL)
		mysql_escape_string(uniqueid, cdr->uniqueid, strlen(cdr->uniqueid));
#endif
	if (userfield && ((userfielddata = alloca(strlen(cdr->userfield) * 2 + 1)) != NULL))
		mysql_escape_string(userfielddata, cdr->userfield, strlen(cdr->userfield));

	/* Check for all alloca failures above at once */
#ifdef MYSQL_LOGUNIQUEID
	if ((!clid) || (!dcontext) || (!channel) || (!dstchannel) || (!lastapp) || (!lastdata) || (!uniqueid)) {
#else
	if ((!clid) || (!dcontext) || (!channel) || (!dstchannel) || (!lastapp) || (!lastdata)) {
#endif
		ast_log(LOG_ERROR, "cdr_mysql:  Out of memory error (insert fails)\n");
		ast_mutex_unlock(&mysql_lock);
		return -1;
	}

	ast_log(LOG_DEBUG, "cdr_mysql: inserting a CDR record.\n");

	if (userfield && userfielddata) {
#ifdef MYSQL_LOGUNIQUEID
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,uniqueid,userfield) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext, channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, ast_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, uniqueid, userfielddata);
#else
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,userfield) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext,channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, ast_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, userfielddata);
#endif
	} else {
#ifdef MYSQL_LOGUNIQUEID
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode,uniqueid) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s','%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext,channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, ast_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode, uniqueid);
#else
		sprintf(sqlcmd, "INSERT INTO %s (calldate,clid,src,dst,dcontext,channel,dstchannel,lastapp,lastdata,duration,billsec,disposition,amaflags,accountcode) VALUES ('%s','%s','%s','%s','%s', '%s','%s','%s','%s',%i,%i,'%s',%i,'%s')", dbtable, timestr, clid, cdr->src, cdr->dst, dcontext, channel, dstchannel, lastapp, lastdata, cdr->duration, cdr->billsec, ast_cdr_disp2str(cdr->disposition), cdr->amaflags, cdr->accountcode);
#endif
	}
	
	ast_log(LOG_DEBUG, "cdr_mysql: SQL command as follows: %s\n", sqlcmd);
	
	if (connected) {
		if (mysql_real_query(&mysql, sqlcmd, strlen(sqlcmd))) {
			handle_cdr_mysql_spool(sqlcmd);
			ast_log(LOG_ERROR, "mysql_cdr: Failed to insert into database: (%d) %s", mysql_errno(&mysql), mysql_error(&mysql));
			mysql_close(&mysql);
			connected = 0;
		} else {
			records++;
			totalrecords++;
		}
	} else {
		handle_cdr_mysql_spool(sqlcmd);
	}
	ast_mutex_unlock(&mysql_lock);
	return 0;
}

char *description(void)
{
	return desc;
}

static int my_unload_module(void)
{ 
	ast_cli_unregister(&cdr_mysql_status_cli);
	ast_cli_unregister(&cdr_mysql_unspool_cli);
	ast_cli_unregister(&cdr_mysql_connect_cli);
	ast_cli_unregister(&cdr_mysql_disconnect_cli);
	if (connected) {
		mysql_close(&mysql);
		connected = 0;
		records = 0;
	}
	if (hostname && hostname_alloc) {
		free(hostname);
		hostname = NULL;
		hostname_alloc = 0;
	}
	if (dbname && dbname_alloc) {
		free(dbname);
		dbname = NULL;
		dbname_alloc = 0;
	}
	if (dbuser && dbuser_alloc) {
		free(dbuser);
		dbuser = NULL;
		dbuser_alloc = 0;
	}
	if (dbsock && dbsock_alloc) {
		free(dbsock);
		dbsock = NULL;
		dbsock_alloc = 0;
	}
	if (dbtable && dbtable_alloc) {
		free(dbtable);
		dbtable = NULL;
		dbtable_alloc = 0;
	}
	if (dbtable && dbtable_alloc) {
		free(dbtable);
		dbtable = NULL;
		dbtable_alloc = 0;
	}
	if (password && password_alloc) {
		free(password);
		password = NULL;
		password_alloc = 0;
	}
	if (dbspool && dbspool_alloc) {
		free(dbspool);
		dbspool = NULL;
		dbspool_alloc = 0;
	}
	dbport = 0;
	ast_cdr_unregister(name);
	return 0;
}

static int my_load_module(void)
{
	int res;
	struct ast_config *cfg;
	struct ast_variable *var;
	char *tmp;

	cfg = ast_config_load(config);
	if (!cfg) {
		ast_log(LOG_WARNING, "Unable to load config for mysql CDR's: %s\n", config);
		return 0;
	}
	
	var = ast_variable_browse(cfg, "global");
	if (!var) {
		/* nothing configured */
		return 0;
	}

	tmp = ast_variable_retrieve(cfg, "global", "hostname");
	if (tmp) {
		hostname = malloc(strlen(tmp) + 1);
		if (hostname != NULL) {
			hostname_alloc = 1;
			strcpy(hostname, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "MySQL server hostname not specified.  Assuming localhost\n");
		hostname = "localhost";
	}

	tmp = ast_variable_retrieve(cfg, "global", "dbname");
	if (tmp) {
		dbname = malloc(strlen(tmp) + 1);
		if (dbname != NULL) {
			dbname_alloc = 1;
			strcpy(dbname, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "MySQL database not specified.  Assuming asteriskcdrdb\n");
		dbname = "asteriskcdrdb";
	}

	tmp = ast_variable_retrieve(cfg, "global", "user");
	if (tmp) {
		dbuser = malloc(strlen(tmp) + 1);
		if (dbuser != NULL) {
			dbuser_alloc = 1;
			strcpy(dbuser, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "MySQL database user not specified.  Assuming root\n");
		dbuser = "root";
	}

	tmp = ast_variable_retrieve(cfg, "global", "sock");
	if (tmp) {
		dbsock = malloc(strlen(tmp) + 1);
		if (dbsock != NULL) {
			dbsock_alloc = 1;
			strcpy(dbsock, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "MySQL database sock file not specified.  Using default\n");
		dbsock = NULL;
	}

	tmp = ast_variable_retrieve(cfg, "global", "table");
	if (tmp) {
		dbtable = malloc(strlen(tmp) + 1);
		if (dbtable != NULL) {
			dbtable_alloc = 1;
			strcpy(dbtable, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_NOTICE, "MySQL database table not specified.  Assuming \"cdr\"\n");
		dbtable = "cdr";
	}

	tmp = ast_variable_retrieve(cfg, "global", "password");
	if (tmp) {
		password = malloc(strlen(tmp) + 1);
		if (password != NULL) {
			password_alloc = 1;
			strcpy(password, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "MySQL database password not specified.  Assuming blank\n");
		password = "";
	}

	tmp = ast_variable_retrieve(cfg, "global", "port");
	if (tmp) {
		if (sscanf(tmp, "%d", &dbport) < 1) {
			ast_log(LOG_WARNING, "Invalid MySQL port number.  Using default\n");
			dbport = 0;
		}
	}

	tmp = ast_variable_retrieve(cfg, "global", "timeout");
	if (tmp) {
		if (sscanf(tmp,"%d", &timeout) < 1) {
			ast_log(LOG_WARNING, "Invalid MySQL timeout number.  Using default\n");
			timeout = 0;
		}
	}
	
	tmp = ast_variable_retrieve(cfg, "global", "spool");
	if (tmp) {
		dbspool = malloc(strlen(tmp) + 1);
		if (dbspool != NULL) {
			dbspool_alloc = 1;
			strcpy(dbspool, tmp);
		} else {
			ast_log(LOG_ERROR, "Out of memory error.\n");
			return -1;
		}
	} else {
		ast_log(LOG_NOTICE, "MySQL spool file not specified. Will not catch lost database queries.\n");
		dbspool = NULL;
	}

	tmp = ast_variable_retrieve(cfg, "global", "userfield");
	if (tmp) {
		if (sscanf(tmp, "%d", &userfield) < 1) {
			ast_log(LOG_WARNING, "Invalid MySQL configurtation file\n");
			userfield = 0;
		}
	}
	
	ast_config_destroy(cfg);

	ast_log(LOG_DEBUG, "cdr_mysql: got hostname of %s\n", hostname);
	ast_log(LOG_DEBUG, "cdr_mysql: got port of %d\n", dbport);
	ast_log(LOG_DEBUG, "cdr_mysql: got a timeout of %d\n", timeout);
	if (dbsock)
		ast_log(LOG_DEBUG, "cdr_mysql: got sock file of %s\n", dbsock);
	ast_log(LOG_DEBUG, "cdr_mysql: got user of %s\n", dbuser);
	ast_log(LOG_DEBUG, "cdr_mysql: got dbname of %s\n", dbname);
	ast_log(LOG_DEBUG, "cdr_mysql: got password of %s\n", password);

	mysql_init(&mysql);

	if (timeout && mysql_options(&mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *)&timeout)!=0) {
		ast_log(LOG_ERROR, "cdr_mysql: mysql_options returned (%d) %s\n", mysql_errno(&mysql), mysql_error(&mysql));
	}

	if (!mysql_real_connect(&mysql, hostname, dbuser, password, dbname, dbport, dbsock, 0)) {
		ast_log(LOG_ERROR, "Failed to connect to mysql database %s on %s.\n", dbname, hostname);
		connected = 0;
		records = 0;
	} else {
		ast_log(LOG_DEBUG, "Successfully connected to MySQL database.\n");
		connected = 1;
		records = 0;
		connect_time = time(NULL);
	}

	res = ast_cdr_register(name, desc, mysql_log);
	if (res) {
		ast_log(LOG_ERROR, "Unable to register MySQL CDR handling\n");
	} else {
		res = ast_cli_register(&cdr_mysql_status_cli);
		res = ast_cli_register(&cdr_mysql_unspool_cli);
		res = ast_cli_register(&cdr_mysql_connect_cli);
		res = ast_cli_register(&cdr_mysql_disconnect_cli);
	}

	return res;
}

int load_module(void)
{
	return my_load_module();
}

int unload_module(void)
{
	return my_unload_module();
}

int reload(void)
{
	int ret;

	ast_mutex_lock(&mysql_lock);    
	my_unload_module();
	ret = my_load_module();
	ast_mutex_unlock(&mysql_lock);

	return ret;
}

int usecount(void)
{
	/* Simplistic use count */
	if (ast_mutex_trylock(&mysql_lock)) {
		return 1;
	} else {
		ast_mutex_unlock(&mysql_lock);
		return 0;
	}
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
