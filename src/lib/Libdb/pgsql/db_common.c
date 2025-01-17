/*
 * Copyright (C) 1994-2021 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/**
 *
 * @brief
 *	This file contains Postgres specific implementation of functions
 *	to access the PBS postgres database.
 *	This is postgres specific data store implementation, and should not be
 *	used directly by the rest of the PBS code.
 *
 */

#include <pbs_config.h> /* the master config generated by configure */
#include "pbs_db.h"
#include "db_postgres.h"
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include "ticket.h"
#include "log.h"
#include "server_limits.h"

#define IPV4_STR_LEN 15

char *errmsg_cache = NULL;
pg_conn_data_t *conn_data = NULL;
pg_conn_trx_t *conn_trx = NULL;
static char pg_ctl[MAXPATHLEN + 1] = "";
static char *pg_user = NULL;

static int is_conn_error(void *conn, int *failcode);
static char *get_dataservice_password(char *user, char *errmsg, int len);
static char *db_escape_str(void *conn, char *str);
static char *get_db_connect_string(char *host, int timeout, int *err_code, char *errmsg, int len);
static int db_prepare_sqls(void *conn);
static int db_cursor_next(void *conn, void *state, pbs_db_obj_info_t *obj);

extern char *pbs_get_dataservice_usr(char *, int);
extern int pbs_decrypt_pwd(char *, int, size_t, char **, const unsigned char *, const unsigned char *);
extern unsigned char pbs_aes_key[][16];
extern unsigned char pbs_aes_iv[][16];

// clang-format off
/**
 * An array of structures(of function pointers) for each of the database object
 */
pg_db_fn_t db_fn_arr[PBS_DB_NUM_TYPES] = {
	{	/* PBS_DB_SVR */
		pbs_db_save_svr,
		NULL,
		pbs_db_load_svr,
		NULL,
		NULL,
		pbs_db_del_attr_svr
	},
	{	/* PBS_DB_SCHED */
		pbs_db_save_sched,
		pbs_db_delete_sched,
		pbs_db_load_sched,
		pbs_db_find_sched,
		pbs_db_next_sched,
		pbs_db_del_attr_sched
	},
	{	/* PBS_DB_QUE */
		pbs_db_save_que,
		pbs_db_delete_que,
		pbs_db_load_que,
		pbs_db_find_que,
		pbs_db_next_que,
		pbs_db_del_attr_que
	},
	{	/* PBS_DB_NODE */
		pbs_db_save_node,
		pbs_db_delete_node,
		pbs_db_load_node,
		pbs_db_find_node,
		pbs_db_next_node,
		pbs_db_del_attr_node
	},
	{	/* PBS_DB_MOMINFO_TIME */
		pbs_db_save_mominfo_tm,
		NULL,
		pbs_db_load_mominfo_tm,
		NULL,
		NULL,
		NULL
	},
	{	/* PBS_DB_JOB */
		pbs_db_save_job,
		pbs_db_delete_job,
		pbs_db_load_job,
		pbs_db_find_job,
		pbs_db_next_job,
		pbs_db_del_attr_job
	},
	{	/* PBS_DB_JOBSCR */
		pbs_db_save_jobscr,
		NULL,
		pbs_db_load_jobscr,
		NULL,
		NULL,
		NULL
	},
	{	/* PBS_DB_RESV */
		pbs_db_save_resv,
		pbs_db_delete_resv,
		pbs_db_load_resv,
		pbs_db_find_resv,
		pbs_db_next_resv,
		pbs_db_del_attr_resv
	}
};

// clang-format on

/**
 * @brief
 *	Initialize a query state variable, before being used in a cursor
 *
 * @param[in]	conn - Database connection handle
 * @param[in]	query_cb - Object handler query back function
 *
 * @return	void *
 * @retval	NULL - Failure to allocate memory
 * @retval	!NULL - Success - returns the new state variable
 *
 */
static void *
db_initialize_state(void *conn, query_cb_t query_cb)
{
	db_query_state_t *state = malloc(sizeof(db_query_state_t));
	if (!state)
		return NULL;
	state->count = -1;
	state->res = NULL;
	state->row = -1;
	state->query_cb = query_cb;
	return state;
}

/**
 * @brief
 *	Destroy a query state variable.
 *	Clears the database resultset and free's the memory allocated to
 *	the state variable
 *
 * @param[in]	st - Pointer to the state variable
 *
 * @return void
 */
static void
db_destroy_state(void *st)
{
	db_query_state_t *state = st;
	if (state) {
		if (state->res)
			PQclear(state->res);
		free(state);
	}
}

/**
 * @brief
 *	Search the database for exisitn objects and load the server structures.
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	pbs_db_obj_info_t - The pointer to the wrapper object which
 *		describes the PBS object (job/resv/node etc) that is wrapped
 *		inside it.
 * @param[in/out]	pbs_db_query_options_t - Pointer to the options object that can
 *		contain the flags or timestamp which will effect the query.
 * @param[in]	callback function which will process the result from the database
 * 		and update the server strctures.
 *
 * @return	int
 * @retval	0	- Success but no rows found
 * @retval	-1	- Failure
 * @retval	>0	- Success and number of rows found
 *
 */
int
pbs_db_search(void *conn, pbs_db_obj_info_t *obj, pbs_db_query_options_t *opts, query_cb_t query_cb)
{
	void *st;
	int ret;
	int totcount;
	int refreshed;
	int rc;

	st = db_initialize_state(conn, query_cb);
	if (!st)
		return -1;

	ret = db_fn_arr[obj->pbs_db_obj_type].pbs_db_find_obj(conn, st, obj, opts);
	if (ret == -1) {
		/* error in executing the sql */
		db_destroy_state(st);
		return -1;
	}
	totcount = 0;
	while ((rc = db_cursor_next(conn, st, obj)) == 0) {
		query_cb(obj, &refreshed);
		if (refreshed)
			totcount++;
	}

	db_destroy_state(st);
	return totcount;
}

/**
 * @brief
 *	Get the next row from the cursor. It also is used to get the first row
 *	from the cursor as well.
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	state - The cursor state handle.
 * @param[out]	pbs_db_obj_info_t - The pointer to the wrapper object which
 *		describes the PBS object (job/resv/node etc) that is wrapped
 *		inside it. The row data is loaded into this parameter.
 *
 * @return	Error code
 * @retval	-1  - Failure
 * @retval	0  - success
 * @retval	1  - Success but no more rows
 *
 */
static int
db_cursor_next(void *conn, void *st, pbs_db_obj_info_t *obj)
{
	db_query_state_t *state = (db_query_state_t *) st;
	int ret;

	if (state->row < state->count) {
		ret = db_fn_arr[obj->pbs_db_obj_type].pbs_db_next_obj(conn, st, obj);
		state->row++;
		return ret;
	}
	return 1; /* no more rows */
}

/**
 * @brief
 *	Delete an existing object from the database
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	pbs_db_obj_info_t - Wrapper object that describes the object
 *		(and data) to delete
 *
 * @return	int
 * @retval	-1  - Failure
 * @retval	0   - success
 * @retval	1   -  Success but no rows deleted
 *
 */
int
pbs_db_delete_obj(void *conn, pbs_db_obj_info_t *obj)
{
	return (db_fn_arr[obj->pbs_db_obj_type].pbs_db_delete_obj(conn, obj));
}

/**
 * @brief
 *	Load a single existing object from the database
 *
 * @param[in]	conn - Connected database handle
 * @param[in/out]pbs_db_obj_info_t - Wrapper object that describes the object
 *		(and data) to load. This parameter used to return the data about
 *		the object loaded
 *
 * @return      Error code
 * @retval       0  - success
 * @retval	-1  - Failure
 * @retval	 1 -  Success but no rows loaded
 *
 */
int
pbs_db_load_obj(void *conn, pbs_db_obj_info_t *obj)
{
	return (db_fn_arr[obj->pbs_db_obj_type].pbs_db_load_obj(conn, obj));
}

/**
 * @brief
 *	Initializes all the sqls before they can be used
 *
 * @param[in]   conn - Connected database handle
 *
 * @return      Error code
 * @retval       0  - success
 * @retval	-1  - Failure
 *
 *
 */
static int
db_prepare_sqls(void *conn)
{
	if (db_prepare_job_sqls(conn) != 0)
		return -1;
	if (db_prepare_svr_sqls(conn) != 0)
		return -1;
	if (db_prepare_que_sqls(conn) != 0)
		return -1;
	if (db_prepare_resv_sqls(conn) != 0)
		return -1;
	if (db_prepare_node_sqls(conn) != 0)
		return -1;
	if (db_prepare_sched_sqls(conn) != 0)
		return -1;
	return 0;
}

/**
 * @brief
 *	Execute a direct sql string on the open database connection
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	sql  - A string describing the sql to execute.
 *
 * @return      Error code
 * @retval	-1  - Error
 * @retval       0  - success
 * @retval	 1  - Execution succeeded but statement did not return any rows
 *
 */
int
db_execute_str(void *conn, char *sql)
{
	PGresult *res;
	char *rows_affected = NULL;
	int status;

	res = PQexec((PGconn *) conn, sql);
	status = PQresultStatus(res);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		char *sql_error = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		db_set_error(conn, &errmsg_cache, "Execution of string statement\n", sql, sql_error);
		PQclear(res);
		return -1;
	}
	rows_affected = PQcmdTuples(res);
	if ((rows_affected == NULL || strtol(rows_affected, NULL, 10) <= 0) && (PQntuples(res) <= 0)) {
		PQclear(res);
		return 1;
	}

	PQclear(res);
	return 0;
}

/**
 * @brief
 *	Function to start/stop the database service/daemons
 *	Basically calls the psql command with the specified command.
 *
 * @return      Error code
 * @retval       !=0 - Failure
 * @retval         0 - Success
 *
 */
int
pbs_dataservice_control(char *cmd, char *pbs_ds_host, int pbs_ds_port)
{
	char dbcmd[4 * MAXPATHLEN + 1];
	int rc = 0;
	int ret = 0;
	char errfile[MAXPATHLEN + 1];
	char log_file[MAXPATHLEN + 1];
	char oom_file[MAXPATHLEN + 1];
	char *oom_score_adj = "/proc/self/oom_score_adj";
	char *oom_adj = "/proc/self/oom_adj";
	char *oom_val = NULL;
	struct stat stbuf;
	int fd = 0;
	char *p = NULL;
	char *pg_bin = NULL;
	char *pg_libstr = NULL;
	char *errmsg = NULL;

	if (pg_ctl[0] == '\0') {
		pg_libstr = getenv("PGSQL_LIBSTR");
		if ((pg_bin = getenv("PGSQL_BIN")) == NULL) {
			if (errmsg_cache)
				free(errmsg_cache);
			errmsg_cache = strdup("PGSQL_BIN not found in the environment. Please run PBS_EXEC/libexec/pbs_db_env and try again.");
			return -1;
		}
		sprintf(pg_ctl, "%s %s/pg_ctl -D %s/datastore", pg_libstr ? pg_libstr : "", pg_bin, pbs_conf.pbs_home_path);
	}
	if (pg_user == NULL) {
		if (errmsg_cache) {
			free(errmsg_cache);
			errmsg_cache = NULL;
		}
		errmsg = (char *) malloc(PBS_MAX_DB_CONN_INIT_ERR + 1);
		if (errmsg == NULL) {
			errmsg_cache = strdup("Out of memory\n");
			return -1;
		}
		if ((pg_user = pbs_get_dataservice_usr(errmsg, PBS_MAX_DB_CONN_INIT_ERR)) == NULL) {
			errmsg_cache = strdup(errmsg);
			free(errmsg);
			return -1;
		}
		free(errmsg);
	}

	if (!(strcmp(cmd, PBS_DB_CONTROL_START))) {
		/*
		 * try protect self from Linux OOM killer
		 * but don't fail if can't update OOM score
		 */
#ifndef __FreeBSD__
		if (access(oom_score_adj, F_OK) != -1) {
			strcpy(oom_file, oom_score_adj);
			oom_val = strdup("-1000");
		} else if (access(oom_adj, F_OK) != -1) {
			strcpy(oom_file, oom_adj);
			oom_val = strdup("-17");
		}
		if (oom_val != NULL) {
			if ((fd = open(oom_file, O_TRUNC | O_WRONLY, 0600)) != -1) {
				if (write(fd, oom_val, strlen(oom_val)) == -1)
					ret = PBS_DB_OOM_ERR;
				close(fd);
			} else
				ret = PBS_DB_OOM_ERR;
			free(oom_val);
		}
#endif
		sprintf(errfile, "%s/spool/pbs_ds_monitor_errfile", pbs_conf.pbs_home_path);
		/* launch monitoring program which will fork to background */
		sprintf(dbcmd, "%s/sbin/pbs_ds_monitor monitor > %s 2>&1", pbs_conf.pbs_exec_path, errfile);
		rc = system(dbcmd);
		if (WIFEXITED(rc))
			rc = WEXITSTATUS(rc);
		if (rc != 0) {
			/* read the contents of errfile and and see */
			/* if pbs_ds_monitor is already running */
			if ((fd = open(errfile, 0)) != -1) {
				if (fstat(fd, &stbuf) != -1) {
					errmsg = (char *) malloc(stbuf.st_size + 1);
					if (errmsg == NULL) {
						close(fd);
						unlink(errfile);
						return -1;
					}
					rc = read(fd, errmsg, stbuf.st_size);
					if (rc == -1)
						return -1;
					*(errmsg + stbuf.st_size) = 0;
					p = errmsg + strlen(errmsg) - 1;
					while ((p >= errmsg) && (*p == '\r' || *p == '\n'))
						*p-- = 0; /* suppress the last newline */
					if (strstr((char *) errmsg, "Lock seems to be held by pid")) {
						/* pbs_ds_monitor is already running */
						rc = 0;
					} else {
						if (errmsg_cache)
							free(errmsg_cache);
						errmsg_cache = strdup(errmsg);
					}
					free(errmsg);
				}
				close(fd);
			}
			if (rc)
				return -1;
		}
		unlink(errfile);
	}

	/* create unique filename by appending pid */
	sprintf(errfile, "%s/spool/db_errfile_%s_%d", pbs_conf.pbs_home_path, cmd, getpid());
	sprintf(log_file, "%s/spool/db_start.log", pbs_conf.pbs_home_path);

	if (!(strcmp(cmd, PBS_DB_CONTROL_START))) {
		sprintf(dbcmd,
			"su - %s -c \"/bin/sh -c '%s -o \\\"-p %d \\\" -W start -l %s > %s 2>&1'\"",
			pg_user,
			pg_ctl,
			pbs_ds_port,
			log_file,
			errfile);
	} else if (!(strcmp(cmd, PBS_DB_CONTROL_STATUS))) {
		sprintf(dbcmd,
			"su - %s -c \"/bin/sh -c '%s -o \\\"-p %d \\\" -w status > %s 2>&1'\"",
			pg_user,
			pg_ctl,
			pbs_ds_port,
			errfile);
	} else if (!(strcmp(cmd, PBS_DB_CONTROL_STOP))) {
		sprintf(dbcmd,
			"su - %s -c \"/bin/sh -c '%s -w stop -m fast > %s 2>&1'\"",
			pg_user,
			pg_ctl,
			errfile);
	}

	rc = system(dbcmd);
	if (WIFEXITED(rc))
		rc = WEXITSTATUS(rc);

	if (rc != 0) {
		ret = 1;
		if (!(strcmp(cmd, PBS_DB_CONTROL_STATUS))) {
			sprintf(errfile, "%s/spool/pbs_ds_monitor_errfile", pbs_conf.pbs_home_path);
			/* check further only if pg_ctl thinks no DATABASE is running */
			sprintf(dbcmd, "%s/sbin/pbs_ds_monitor check > %s 2>&1", pbs_conf.pbs_exec_path, errfile);
			rc = system(dbcmd);
			if (WIFEXITED(rc))
				rc = WEXITSTATUS(rc);
			if (rc != 0)
				ret = 2;
		} else if (!(strcmp(cmd, PBS_DB_CONTROL_START))) {
			/* read the contents of logfile */
			if ((fd = open(log_file, 0)) != -1) {
				if (fstat(fd, &stbuf) != -1) {
					errmsg = (char *) malloc(stbuf.st_size + 1);
					if (errmsg == NULL) {
						close(fd);
						unlink(log_file);
						return -1;
					}
					rc = read(fd, errmsg, stbuf.st_size);
					if (rc == -1) {
						close(fd);
						unlink(log_file);
						return -1;
					}
					*(errmsg + stbuf.st_size) = 0;
					p = errmsg + strlen(errmsg) - 1;
					while ((p >= errmsg) && (*p == '\r' || *p == '\n'))
						*p-- = 0; /* suppress the last newline */
					if (strstr((char *) errmsg, "database files are incompatible with server"))
						ret = 3; /* DB version mismatch */
					free(errmsg);
				}
				close(fd);
			}
		}
		if (rc != 0) {
			/* read the contents of errfile and load to errmsg */
			if ((fd = open(errfile, 0)) != -1) {
				if (fstat(fd, &stbuf) != -1) {
					errmsg = (char *) malloc(stbuf.st_size + 1);
					if (errmsg == NULL) {
						close(fd);
						unlink(errfile);
						return -1;
					}
					rc = read(fd, errmsg, stbuf.st_size);
					if (rc == -1)
						return -1;
					*(errmsg + stbuf.st_size) = 0;
					p = errmsg + strlen(errmsg) - 1;
					while ((p >= errmsg) && (*p == '\r' || *p == '\n'))
						*p-- = 0; /* suppress the last newline */
					if (errmsg_cache)
						free(errmsg_cache);
					errmsg_cache = strdup(errmsg);
					free(errmsg);
				}
				close(fd);
			}
		}
	} else if (rc == 0 && !(strcmp(cmd, PBS_DB_CONTROL_START))) {
		/* launch systemd setup script */
#ifndef __FreeBSD__
		sprintf(dbcmd, "%s/sbin/pbs_ds_systemd", pbs_conf.pbs_exec_path);
		rc = system(dbcmd);
#endif
		if (WIFEXITED(rc))
			rc = WEXITSTATUS(rc);
		if (rc != 0) {
			if (errmsg_cache)
				free(errmsg_cache);
			errmsg_cache = strdup("systemd service setup for pbs failed");
			return -1;
		}
	}
	unlink(log_file);
	unlink(errfile);
	return ret;
}

/**
 * @brief
 *	Function to check whether data-service is running
 *
 * @return      Error code
 * @retval      -1  - Error in routine
 * @retval       0  - Data service running on local host
 * @retval       1  - Data service not running
 * @retval       2  - Data service running on another host
 *
 */
int
pbs_status_db(char *pbs_ds_host, int pbs_ds_port)
{
	return (pbs_dataservice_control(PBS_DB_CONTROL_STATUS, pbs_ds_host, pbs_ds_port));
}

/**
 * @brief
 *	Start the database daemons/service in synchronous mode.
 *  This function waits for the database to complete startup.
 *
 * @param[out]	errmsg - returns the startup error message if any
 *
 * @return       int
 * @retval       0     - success
 * @retval       !=0   - Failure
 *
 */
int
pbs_start_db(char *pbs_ds_host, int pbs_ds_port)
{
	return (pbs_dataservice_control(PBS_DB_CONTROL_START, pbs_ds_host, pbs_ds_port));
}

/**
 * @brief
 *	Function to stop the database service/daemons
 *	This passes the parameter STOP to the
 *	pbs_dataservice script.
 *
 * @param[out]	errmsg - returns the db error message if any
 *
 * @return      Error code
 * @retval       !=0 - Failure
 * @retval        0  - Success
 *
 */
int
pbs_stop_db(char *pbs_ds_host, int pbs_ds_port)
{
	return (pbs_dataservice_control(PBS_DB_CONTROL_STOP, pbs_ds_host, pbs_ds_port));
}

/**
 * @brief
 *	Function to create new databse user or
 *  change password of current user.
 *
 * @param[in] conn[in]: The database connection handle which was created by pbs_db_connection.
 * @param[in] user_name[in]: Databse user name.
 * @param[in] password[in]:  New password for the database.
 * @param[in] olduser[in]: old database user name.
 *
 * @return      Error code
 * @retval       -1 - Failure
 * @retval        0  - Success
 *
 */
int
pbs_db_password(void *conn, char *userid, char *password, char *olduser)
{
	char sqlbuff[1024];
	char *pquoted = NULL;
	char prog[] = "pbs_db_password";
	int change_user = 0;

	if (userid[0] != 0) {
		if (strcmp(olduser, userid) != 0) {
			change_user = 1;
		}
	}

	/* escape password to use in sql strings later */
	if ((pquoted = db_escape_str(conn, password)) == NULL) {
		fprintf(stderr, "%s: Out of memory\n", prog);
		return -1;
	}

	if (change_user == 1) {
		/* check whether user exists */
		snprintf(sqlbuff, sizeof(sqlbuff), "select usename from pg_user where usename = '%s'", userid);
		if (db_execute_str(conn, sqlbuff) == 1) {
			/* now attempt to create new user & set the database passwd to the un-encrypted password */
			snprintf(sqlbuff, sizeof(sqlbuff), "create user \"%s\" SUPERUSER ENCRYPTED PASSWORD '%s'", userid, pquoted);
		} else {
			/* attempt to alter new user & set the database passwd to the un-encrypted password */
			snprintf(sqlbuff, sizeof(sqlbuff), "alter user \"%s\" SUPERUSER ENCRYPTED PASSWORD '%s'", userid, pquoted);
		}
	} else {
		/* now attempt to set the database passwd to the un-encrypted password */
		/* alter user ${user} SUPERUSER ENCRYPTED PASSWORD '${passwd}' */
		sprintf(sqlbuff, "alter user \"%s\" SUPERUSER ENCRYPTED PASSWORD '%s'", olduser, pquoted);
	}
	free(pquoted);

	if (db_execute_str(conn, sqlbuff) == -1)
		return -1;
	if (change_user) {
		/* delete the old user from the database */
		sprintf(sqlbuff, "drop user \"%s\"", olduser);
		if (db_execute_str(conn, sqlbuff) == -1)
			return -1;
	}
	return 0;
}

/**
 * @brief
 *	Static helper function to retrieve postgres error string,
 *	analyze it and find out what kind of error it is. Based on
 *	that, a PBS DB layer specific error code is generated.
 *
 * @param[in]	conn - Connected database handle
 *
 * @return      Connection status
 * @retval      -1 - Connection down
 * @retval       0 - Connection fine
 *
 */
static int
is_conn_error(void *conn, int *failcode)
{
	/* Check to see that the backend connection was successfully made */
	if (conn == NULL || PQstatus(conn) == CONNECTION_BAD) {
		if (conn) {
			db_set_error(conn, &errmsg_cache, "Connection:", "", "");
			if (strstr((char *) errmsg_cache, "Connection refused") || strstr((char *) errmsg_cache, "No such file or directory"))
				*failcode = PBS_DB_CONNREFUSED;
			else if (strstr((char *) errmsg_cache, "authentication"))
				*failcode = PBS_DB_AUTH_FAILED;
			else if (strstr((char *) errmsg_cache, "database system is starting up"))
				*failcode = PBS_DB_STILL_STARTING;
			else
				*failcode = PBS_DB_CONNFAILED; /* default failure code */
		} else
			*failcode = PBS_DB_CONNFAILED; /* default failure code */
		return 1;			       /* true - connection error */
	}
	return 0; /* no connection error */
}

/**
 * @brief
 *	Create a new connection structure and initialize the fields
 *
 * @param[out]  conn - Pointer to database connection handler.
 * @param[in]   host - The hostname to connect to
 * @param[in]	port - The port to connect to
 * @param[in]   timeout - The connection attempt timeout
 *
 * @return      int - failcode
 * @retval      non-zero  - Failure
 * @retval      0 - Success
 *
 */
int
pbs_db_connect(void **db_conn, char *host, int port, int timeout)
{
	int failcode = PBS_DB_SUCCESS;
	int len = PBS_MAX_DB_CONN_INIT_ERR;
	char db_sys_msg[PBS_MAX_DB_CONN_INIT_ERR + 1] = {0};
	char *conn_info = NULL;

	conn_data = malloc(sizeof(pg_conn_data_t));
	if (!conn_data) {
		failcode = PBS_DB_NOMEM;
		return failcode;
	}

	/*
	 * calloc ensures that everything is initialized to zeros
	 * so no need to explicitly set fields to 0.
	 */
	conn_trx = calloc(1, sizeof(pg_conn_trx_t));
	if (!conn_trx) {
		free(conn_data);
		failcode = PBS_DB_NOMEM;
		return failcode;
	}

	conn_info = get_db_connect_string(host, timeout, &failcode, db_sys_msg, len);
	if (!conn_info) {
		errmsg_cache = strdup(db_sys_msg);
		goto db_cnerr;
	}

	/* Make a connection to the database */
	*db_conn = (PGconn *) PQconnectdb(conn_info);

	/*
	 * For security remove the connection info from the memory.
	 */
	memset(conn_info, 0, strlen(conn_info));
	free(conn_info);

	/* Check to see that the backend connection was successfully made */
	if (!(is_conn_error(*db_conn, &failcode))) {
		if (db_prepare_sqls(*db_conn) != 0) {
			/* this means there is programmatic/unrecoverable error, so we quit */
			failcode = PBS_DB_ERR;
			pbs_stop_db(host, port);
		}
	}

db_cnerr:
	if (failcode != PBS_DB_SUCCESS) {
		free(conn_data);
		free(conn_trx);
		*db_conn = NULL;
	}
	return failcode;
}

/**
 * @brief
 *	Disconnect from the database and frees all allocated memory.
 *
 * @param[in]   conn - Connected database handle
 *
 * @return      Error code
 * @retval       0  - success
 * @retval      -1  - Failure
 *
 */
int
pbs_db_disconnect(void *conn)
{
	if (!conn)
		return -1;

	if (conn)
		PQfinish(conn);

	free(conn_data);
	free(conn_trx);

	return 0;
}

/**
 * @brief
 *	Saves a new object into the database
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	pbs_db_obj_info_t - Wrapper object that describes the object (and data) to insert
 * @param[in]	savetype - quick or full save
 *
 * @return      Error code
 * @retval	-1  - Failure
 * @retval	 0  - Success
 * @retval	 1  - Success but no rows inserted
 *
 */
int
pbs_db_save_obj(void *conn, pbs_db_obj_info_t *obj, int savetype)
{
	return (db_fn_arr[obj->pbs_db_obj_type].pbs_db_save_obj(conn, obj, savetype));
}

/**
 * @brief
 *	Delete attributes of an object from the database
 *
 * @param[in]	conn - Connected database handle
 * @param[in]	pbs_db_obj_info_t - Wrapper object that describes the object
 * @param[in]	id - Object id
 * @param[in]	@param[in]	db_attr_list - pointer to the structure of type pbs_db_attr_list_t for deleting from DB
 * @return      Error code
 *
 * @retval      0  - success
 * @retval     -1  - Failure
 *
 */
int
pbs_db_delete_attr_obj(void *conn, pbs_db_obj_info_t *obj, void *obj_id, pbs_db_attr_list_t *db_attr_list)
{
	return (db_fn_arr[obj->pbs_db_obj_type].pbs_db_del_attr_obj(conn, obj_id, db_attr_list));
}

/**
 * @brief
 *	Function to set the database error into the db_err field of the
 *	connection object
 *
 * @param[in]	conn - Pointer to db connection handle.
 * @param[out]	conn_db_err - Pointer to cached db error.
 * @param[in]	fnc - Custom string added to the error message
 *			This can be used to provide the name of the functionality.
 * @param[in]	msg - Custom string added to the error message. This can be
 *			used to provide a failure message.
 * @param[in]	diag_msg - Additional diagnostic message from the resultset, if any
 */
void
db_set_error(void *conn, char **conn_db_err, char *fnc, char *msg, char *diag_msg)
{
	char *str;
	char *p;
	char fmt[] = "%s %s failed: %s %s";

	if (*conn_db_err) {
		free(*conn_db_err);
		*conn_db_err = NULL;
	}

	str = PQerrorMessage((PGconn *) conn);
	if (!str)
		return;

	p = str + strlen(str) - 1;
	while ((p >= str) && (*p == '\r' || *p == '\n'))
		*p-- = 0; /* supress the last newline */

	if (!diag_msg)
		diag_msg = "";

	pbs_asprintf(conn_db_err, fmt, fnc, msg, str, diag_msg);

#ifdef DEBUG
	printf("%s\n", *conn_db_err);
	fflush(stdout);
#endif
}

/**
 * @brief
 *	Function to prepare a database statement
 *
 * @param[in]	conn - The connnection handle
 * @param[in]	stmt - Name of the statement
 * @param[in]	sql  - The string sql that has to be prepared
 * @param[in]	num_vars - The number of parameters in the sql ($1, $2 etc)
 *
 * @return      Error code
 * @retval	-1 Failure
 * @retval	 0 Success
 *
 */
int
db_prepare_stmt(void *conn, char *stmt, char *sql, int num_vars)
{
	PGresult *res;
	res = PQprepare((PGconn *) conn, stmt, sql, num_vars, NULL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		char *sql_error = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		db_set_error(conn, &errmsg_cache, "Prepare of statement", stmt, sql_error);
		PQclear(res);
		return -1;
	}
	PQclear(res);
	return 0;
}

/**
 * @brief
 *	Execute a prepared DML (insert or update) statement
 *
 * @param[in]	conn - The connnection handle
 * @param[in]	stmt - Name of the statement (prepared previously)
 * @param[in]	num_vars - The number of parameters in the sql ($1, $2 etc)
 *
 * @return      Error code
 * @retval	-1 - Execution of prepared statement failed
 * @retval	 0 - Success and > 0 rows were affected
 * @retval	 1 - Execution succeeded but statement did not affect any rows
 *
 *
 */
int
db_cmd(void *conn, char *stmt, int num_vars)
{
	PGresult *res;
	char *rows_affected = NULL;

	res = PQexecPrepared((PGconn *) conn, stmt, num_vars,
			     conn_data->paramValues,
			     conn_data->paramLengths,
			     conn_data->paramFormats, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		char *sql_error = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		db_set_error(conn, &errmsg_cache, "Execution of Prepared statement", stmt, sql_error);
		PQclear(res);
		return -1;
	}
	rows_affected = PQcmdTuples(res);

	/*
	*  we can't call PQclear(res) yet, since rows_affected
	* (used below) is a pointer to a field inside res (PGresult)
	*/
	if (rows_affected == NULL || strtol(rows_affected, NULL, 10) <= 0) {
		PQclear(res);
		return 1;
	}
	PQclear(res);

	return 0;
}

/**
 * @brief
 *	Execute a prepared query (select) statement
 *
 * @param[in]	conn - The connnection handle
 * @param[in]	stmt - Name of the statement (prepared previously)
 * @param[in]	num_vars - The number of parameters in the sql ($1, $2 etc)
 * @param[out]  res - The result set of the query
 *
 * @return      Error code
 * @retval	-1 - Execution of prepared statement failed
 * @retval	 0 - Success and > 0 rows were returned
 * @retval	 1 - Execution succeeded but statement did not return any rows
 *
 */
int
db_query(void *conn, char *stmt, int num_vars, PGresult **res)
{
	int conn_result_format = 1;
	*res = PQexecPrepared((PGconn *) conn, stmt, num_vars,
			      conn_data->paramValues, conn_data->paramLengths,
			      conn_data->paramFormats, conn_result_format);

	if (PQresultStatus(*res) != PGRES_TUPLES_OK) {
		char *sql_error = PQresultErrorField(*res, PG_DIAG_SQLSTATE);
		db_set_error(conn, &errmsg_cache, "Execution of Prepared statement", stmt, sql_error);
		PQclear(*res);
		return -1;
	}

	if (PQntuples(*res) <= 0) {
		PQclear(*res);
		return 1;
	}

	return 0;
}

/**
 * @brief
 *	Retrieves the database password for an user. Currently, the database
 *	password is retrieved from the file under server_priv, called db_passwd
 *	Currently, this function returns the same username as the password, if
 *	a password file is not found under server_priv. However, if a password
 *	file is found but is not readable etc, then an error (indicated by
 *	returning NULL) is returned.
 *
 * @param[in]	user - Name of the user
 * @param[out]  errmsg - Details of the error
 * @param[in]   len    - length of error messge variable
 *
 * @return      Password String
 * @retval	 NULL - Failed to retrieve password
 * @retval	!NULL - Pointer to allocated memory with password string.
 *			Caller should free this memory after usage.
 *
 */
static char *
get_dataservice_password(char *user, char *errmsg, int len)
{
	char pwd_file[MAXPATHLEN + 1];
	int fd;
	struct stat st;
	char buf[MAXPATHLEN + 1];
	char *str;

	sprintf(pwd_file, "%s/server_priv/db_password", pbs_conf.pbs_home_path);
	if ((fd = open(pwd_file, O_RDONLY)) == -1) {
		return strdup(user);
	} else {
		if (fstat(fd, &st) == -1) {
			close(fd);
			snprintf(errmsg, len, "%s: stat failed, errno=%d", pwd_file, errno);
			return NULL;
		}
		if (st.st_size >= sizeof(buf)) {
			close(fd);
			snprintf(errmsg, len, "%s: file too large", pwd_file);
			return NULL;
		}

		if (read(fd, buf, st.st_size) != st.st_size) {
			close(fd);
			snprintf(errmsg, len, "%s: read failed, errno=%d", pwd_file, errno);
			return NULL;
		}
		buf[st.st_size] = 0;
		close(fd);

		if (pbs_decrypt_pwd(buf, PBS_CREDTYPE_AES, st.st_size, &str, (const unsigned char *) pbs_aes_key, (const unsigned char *) pbs_aes_iv) != 0)
			return NULL;

		return (str);
	}
}

/**
 * @brief
 *	Escape any special characters contained in a database password.
 *	The list of such characters is found in the description of PQconnectdb
 *	at http://www.postgresql.org/docs/<current_pgsql_version>/static/libpq-connect.html.
 *
 * @param[out]	dest - destination string, which will hold the escaped password
 * @param[in]	src - the original password string, which may contain characters
 *		      that must be escaped
 * @param[in]   len - amount of space in the destination string;  to ensure
 *		      successful conversion, this value should be at least one
 *		      more than twice the length of the original password string
 *
 * @return      void
 *
 */
void
escape_passwd(char *dest, char *src, int len)
{
	char *p = dest;

	while (*src && ((p - dest) < len)) {
		if (*src == '\'' || *src == '\\') {
			*p = '\\';
			p++;
		}
		*p = *src;
		p++;
		src++;
	}
	*p = '\0';
}

/**
 * @brief
 *	Creates the database connect string by retreiving the
 *      database password and appending the other connection
 *      parameters.
 *	If parameter host is passed as NULL, then the "host =" portion
 *	of the connection info is not set, allowing the database to
 *	connect to the default host (which is local).
 *
 * @param[in]   host - The hostname to connect to, if NULL the not used
 * @param[in]   timeout - The timeout parameter of the connection
 * @param[in]   err_code - The error code in case of failure
 * @param[out]  errmsg - Details of the error
 * @param[in]   len    - length of error messge variable
 *
 * @return      The newly allocated and populated connection string
 * @retval       NULL  - Failure
 * @retval       !NULL - Success
 *
 */
static char *
get_db_connect_string(char *host, int timeout, int *err_code, char *errmsg, int len)
{
	char *svr_conn_info;
	int pquoted_len = 0;
	char *p = NULL, *pquoted = NULL;
	char *usr = NULL;
	pbs_net_t hostaddr;
	struct in_addr in;
	char hostaddr_str[IPV4_STR_LEN + 1];
	char *q;
	char template1[] = "hostaddr = '%s' port = %d dbname = '%s' user = '%s' password = '%s' connect_timeout = %d";
	char template2[] = "port = %d dbname = '%s' user = '%s' password = '%s' connect_timeout = %d";

	usr = pbs_get_dataservice_usr(errmsg, len);
	if (usr == NULL) {
		*err_code = PBS_DB_AUTH_FAILED;
		return NULL;
	}

	p = get_dataservice_password(usr, errmsg, len);
	if (p == NULL) {
		free(usr);
		*err_code = PBS_DB_AUTH_FAILED;
		return NULL;
	}

	pquoted_len = strlen(p) * 2 + 1;
	pquoted = malloc(pquoted_len);
	if (!pquoted) {
		free(p);
		free(usr);
		*err_code = PBS_DB_NOMEM;
		return NULL;
	}

	escape_passwd(pquoted, p, pquoted_len);

	svr_conn_info = malloc(MAX(sizeof(template1), sizeof(template2)) +
			       ((host) ? IPV4_STR_LEN : 0) + /* length of IPv4 only if host is not NULL */
			       5 +			     /* possible length of port */
			       strlen(PBS_DATA_SERVICE_STORE_NAME) +
			       strlen(usr) + /* NULL checked earlier */
			       strlen(p) +   /* NULL checked earlier */
			       10);	     /* max 9 char timeout + null char */
	if (svr_conn_info == NULL) {
		free(pquoted);
		free(p);
		free(usr);
		*err_code = PBS_DB_NOMEM;
		return NULL;
	}

	if (host == NULL) {
		sprintf(svr_conn_info,
			template2,
			pbs_conf.pbs_data_service_port,
			PBS_DATA_SERVICE_STORE_NAME,
			usr,
			pquoted,
			timeout);
	} else {
		if ((hostaddr = get_hostaddr(host)) == (pbs_net_t) 0) {
			free(pquoted);
			free(svr_conn_info);
			free(p);
			free(usr);
			snprintf(errmsg, len, "Could not resolve dataservice host %s", host);
			*err_code = PBS_DB_CONNFAILED;
			return NULL;
		}
		in.s_addr = htonl(hostaddr);
		q = inet_ntoa(in);
		if (!q) {
			free(pquoted);
			free(svr_conn_info);
			free(p);
			free(usr);
			snprintf(errmsg, len, "inet_ntoa failed, errno=%d", errno);
			*err_code = PBS_DB_CONNFAILED;
			return NULL;
		}
		strncpy(hostaddr_str, q, IPV4_STR_LEN);
		hostaddr_str[IPV4_STR_LEN] = '\0';

		sprintf(svr_conn_info,
			template1,
			hostaddr_str,
			pbs_conf.pbs_data_service_port,
			PBS_DATA_SERVICE_STORE_NAME,
			usr,
			pquoted,
			timeout);
	}
	memset(p, 0, strlen(p));	     /* clear password from memory */
	memset(pquoted, 0, strlen(pquoted)); /* clear password from memory */
	free(pquoted);
	free(p);
	free(usr);

	return svr_conn_info;
}

/**
 * @brief
 *	Function to escape special characters in a string
 *	before using as a column value in the database
 *
 * @param[in]	conn - Handle to the database connection
 * @param[in]	str - the string to escape
 *
 * @return      Escaped string
 * @retval        NULL - Failure to escape string
 * @retval       !NULL - Newly allocated area holding escaped string,
 *                       caller needs to free
 *
 */
static char *
db_escape_str(void *conn, char *str)
{
	char *val_escaped;
	int error;
	int val_len;

	if (str == NULL)
		return NULL;

	val_len = strlen(str);
	/* Use calloc() to ensure the character array is initialized. */
	val_escaped = calloc(((2 * val_len) + 1), sizeof(char)); /* 2*orig + 1 as per Postgres API documentation */
	if (val_escaped == NULL)
		return NULL;

	PQescapeStringConn((PGconn *) conn, val_escaped, str, val_len, &error);
	if (error != 0) {
		free(val_escaped);
		return NULL;
	}

	return val_escaped;
}

/**
 * @brief
 *	Translates the error code to an error message
 *
 * @param[in]   err_code - Error code to translate
 * @param[out]   err_msg - The translated error message (newly allocated memory)
 *
 */
void
pbs_db_get_errmsg(int err_code, char **err_msg)
{
	if (*err_msg) {
		free(*err_msg);
		*err_msg = NULL;
	}

	switch (err_code) {
		case PBS_DB_STILL_STARTING:
			*err_msg = strdup("PBS dataservice is still starting up");
			break;

		case PBS_DB_AUTH_FAILED:
			*err_msg = strdup("PBS dataservice authentication failed");
			break;

		case PBS_DB_NOMEM:
			*err_msg = strdup("PBS out of memory in connect");
			break;

		case PBS_DB_CONNREFUSED:
			*err_msg = strdup("PBS dataservice not running");
			break;

		case PBS_DB_CONNFAILED:
			*err_msg = strdup("Failed to connect to PBS dataservice");
			break;

		case PBS_DB_OOM_ERR:
			*err_msg = strdup("Failed to protect PBS from Linux OOM killer. No access to OOM score file.");
			break;

		case PBS_DB_ERR:
			*err_msg = NULL;
			if (errmsg_cache)
				*err_msg = strdup(errmsg_cache);
			break;

		default:
			*err_msg = strdup("PBS dataservice error");
			break;
	}
}

/**
 * @brief convert network to host byte order to unsigned long long
 *
 * @param[in]   x - Value to convert
 *
 * @return Value converted from network to host byte order. Return the original
 * value if network and host byte order are identical.
 */
unsigned long long
db_ntohll(unsigned long long x)
{
	if (ntohl(1) == 1)
		return x;

	/*
	 * htonl and ntohl always work on 32 bits, even on a 64 bit platform,
	 * so there is no clash.
	 */
	return (unsigned long long) (((unsigned long long) ntohl((x) &0xffffffff)) << 32) | ntohl(((unsigned long long) (x)) >> 32);
}
