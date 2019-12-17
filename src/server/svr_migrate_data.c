/*
 * Copyright (C) 1994-2019 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */

/**
 * @file    svr_migrate_data.c
 *
 * @brief
 * 		svr_migrate_data.c - Functions to migrate pbs server data from one
 * 		version of PBS to another, in cases where the server data structure
 * 		has undergone changes.
 *
 * Included public functions are:
 * 	svr_migrate_data()
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <libutil.h>

#include <dirent.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "libpbs.h"
#include "pbs_ifl.h"
#include "net_connect.h"
#include "log.h"
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "server.h"
#include "credential.h"
#include "ticket.h"
#include "batch_request.h"
#include "work_task.h"
#include "resv_node.h"
#include "job.h"
#include "queue.h"
#include "reservation.h"
#include "pbs_nodes.h"
#include "tracking.h"
#include "provision.h"
#include "svrfunc.h"
#include "acct.h"
#include "pbs_version.h"
#include "rpp.h"
#include "pbs_license.h"
#include "resource.h"
#include "hook.h"
#include "pbs_db.h"

#ifndef SIGKILL
/* there is some weid stuff in gcc include files signal.h & sys/params.h */
#include <signal.h>
#endif


/* global Data Items */
extern char *msg_startup3;
extern char *msg_daemonname;
extern char *msg_init_abt;
extern char *msg_init_queued;
extern char *msg_init_substate;
extern char *msg_err_noqueue;
extern char *msg_err_noqueue1;
extern char *msg_init_noqueues;
extern char *msg_init_noresvs;
extern char *msg_init_resvNOq;
extern char *msg_init_recovque;
extern char *msg_init_recovresv;
extern char *msg_init_expctq;
extern char *msg_init_nojobs;
extern char *msg_init_exptjobs;
extern char *msg_init_norerun;
extern char *msg_init_unkstate;
extern char *msg_init_baddb;
extern char *msg_init_chdir;
extern char *msg_init_badjob;
extern char *msg_script_open;
extern char *msg_unkresc;

extern char *path_svrdb;
extern char *path_priv;
extern char *path_jobs;
extern char *path_users;
extern char *path_rescdef;
extern char *path_spool;

extern char server_host[];
extern char server_name[];
extern struct server server;

char *path_queues;
char *path_nodes;
char *path_nodestate;
char *path_scheddb;
char *path_resvs;
char *path_svrdb_new;
char *path_scheddb_new;

/* Private functions in this file */
extern int chk_save_file(char *filename);
extern void rm_files(char *dirname);

/* extern definitions to older FS based object recovery functions */
extern job *job_recov_fs(char *filename);
extern void *job_or_resv_recov_fs(char *filename, int objtype);
extern char *build_path(char *parent, char *name, char *sufix);

#ifdef NAS /* localmod 005 */

extern int resv_save_db(resc_resv *presv, int updatetype);

extern int pbsd_init(int type);

#endif /* localmod 005 */
extern void init_server_attrs();

/**
 * @brief
 *		Top level function to migrate pbs server data from one
 * 		version of PBS to another, in cases where the server data structure
 * 		has undergone changes.
 * @par
 * 		When a database structure changes, there could be two types of change.
 *			a) Simple structure change, that can be effected by pbs_habitat
 *	   		before new server is started.
 *			b) More complex change, where pbs_habitat could make some changes,
 *	  		but pbs_server needs to load the data in old format and save in
 *	  		the new format.
 * @par
 * 		In general, as part of DB upgrade, our process should be as follows:
 * 			- Add schema changes to pbs_habitat file
 * 			- Start pbs_server with the updatedb switch
 * 			- pbs_server should check the existing schema version number
 * 			- Based on the existing version number, pbs_server will use older data-structures
 *   			to load data that has older semantics.
 * 			- pbs_server should then save the data using the "new" routines, after performing
 *   			any necessary semantical conversions. (sometimes a save with the new routines
 *   			would be enough).
 * 			- If the schema version is unknown, we cannot do an upgrade, return an error
 *
 * @return	Error code
 * @retval	0	: success
 * @retval	-1	: Failure
 *
 */
int
svr_migrate_data()
{
	int db_maj_ver, db_min_ver;
	int i;

	/* if no fs serverdb exists then check db version */
	if (pbs_db_get_schema_version(svr_db_conn, &db_maj_ver, &db_min_ver) != 0) {
		log_err(-1, msg_daemonname, "Failed to get PBS datastore version");
		fprintf(stderr, "Failed to get the PBS datastore version\n");
		if (svr_db_conn->conn_db_err) {
			fprintf(stderr, "[%s]\n", (char *)svr_db_conn->conn_db_err);
			log_err(-1, msg_daemonname, svr_db_conn->conn_db_err);
		}
		return -1;
	}

	if (db_maj_ver == 1 && db_min_ver == 0) {
		/* upgrade to current version */
		/* read all data, including node data, and save all nodes again */
		if (pbsd_init(RECOV_WARM) != 0) {
			return -1;
		}

		/* loop through all the nodes and mark for update */
		for (i = 0; i < svr_totnodes; i++) {
			pbsndlist[i]->nd_modified = NODE_UPDATE_OTHERS;
		}

		if (save_nodes_db(0, NULL) != 0) {
			log_err(errno, "svr_migrate_data", "save_nodes_db failed!");
			return -1;
		}
		return 0;
	}
	if (db_maj_ver == 3 && db_min_ver == 0) {
		/* Do nothing, schema has already taken care by query */
		return 0;
	}

	/*
	 * if the code fall through here, we did not recognize the schema
	 * version, or we are not prepared to handle this version,
	 * upgrade from this version is not possible. Return an error with log
	 */
	snprintf(log_buffer, sizeof(log_buffer), "Cannot upgrade from PBS datastore version %d.%d",
		db_maj_ver, db_min_ver);
	log_err(-1, msg_daemonname, log_buffer);
	fprintf(stderr, "%s\n", log_buffer);

	return -1;
}
