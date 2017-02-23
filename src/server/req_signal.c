/*
 * Copyright (C) 1994-2016 Altair Engineering, Inc.
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
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.
 *  
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *  
 * Commercial License Information: 
 * 
 * The PBS Pro software is licensed under the terms of the GNU Affero General 
 * Public License agreement ("AGPL"), except where a separate commercial license 
 * agreement for PBS Pro version 14 or later has been executed in writing with Altair.
 *  
 * Altair’s dual-license business model allows companies, individuals, and 
 * organizations to create proprietary derivative works of PBS Pro and distribute 
 * them - whether embedded or bundled with other software - under a commercial 
 * license agreement.
 * 
 * Use of Altair’s trademarks, including but not limited to "PBS™", 
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's 
 * trademark licensing policies.
 *
 */
/**
 * @file	req_signaljob.c
 *
 * @brief
 * 		req_signaljob.c - functions dealing with sending a signal
 *		     to a running job.
 *
 * Functions included are:
 * 	req_signaljob()
 * 	req_signaljob2()
 * 	issue_signal()
 * 	post_signal_req()
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "work_task.h"
#include "pbs_error.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "sched_cmds.h"


/* Private Function local to this file */

static void post_signal_req(struct work_task *);
static void req_signaljob2(struct batch_request *preq, job *pjob);
void set_admin_suspend(job *pjob, int set_remove_nstate);

/* Global Data Items: */

extern char *msg_momreject;
extern char *msg_signal_job;

/**
 * @brief
 * 		req_signaljob - service the Signal Job Request
 * @par
 *		This request sends (via MOM) a signal to a running job.
 *
 * @param[in]	preq	-	Signal Job Request
 */

void
req_signaljob(struct batch_request *preq)
{
	int		  anygood = 0;
	int		  i;
	int		  j;
	char		 *jid;
	int		  jt;		/* job type */
	int		  offset;
	char		 *pc;
	job		 *pjob;
	job		 *parent;
	char		 *range;
	int		  suspend = 0;
	int		  resume = 0;
	char		 *vrange;
	int		  x, y, z;

	jid = preq->rq_ind.rq_signal.rq_jid;
	parent = chk_job_request(jid, preq, &jt);
	if (parent == (job *)0)
		return;		/* note, req_reject already called */

	if (strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_RESUME) == 0 || strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0)
		resume = 1;
	else if (strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_SUSPEND) == 0 || strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_SUSPEND) == 0)
		suspend = 1;

	
	if (suspend || resume) {

		if ((preq->rq_perm & (ATR_DFLAG_OPRD | ATR_DFLAG_OPWR |
			ATR_DFLAG_MGRD | ATR_DFLAG_MGWR)) == 0) {
			/* for suspend/resume, must be mgr/op */
			req_reject(PBSE_PERM, 0, preq);
			return;
		}
	}

	if (jt == IS_ARRAY_NO) {

		/* just a regular job, pass it on down the line and be done */

		req_signaljob2(preq, parent);
		return;

	} else if (jt == IS_ARRAY_Single) {

		/* single subjob, if running can signal */

		offset = subjob_index_to_offset(parent, get_index_from_jid(jid));
		if (offset == -1) {
			req_reject(PBSE_UNKJOBID, 0, preq);
			return;
		}
		i = get_subjob_state(parent, offset);
		if (i == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		} else if (i == JOB_STATE_RUNNING) {
			pjob = find_job(jid);		/* get ptr to the subjob */
			if (pjob) {
				req_signaljob2(preq, pjob);
			} else {
				req_reject(PBSE_BADSTATE, 0, preq);
				return;
			}
		} else {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
		return;

	} else if (jt == IS_ARRAY_ArrayJob) {

		/* The Array Job itself ... */

		if (parent->ji_qs.ji_state != JOB_STATE_BEGUN) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}

		/* for each subjob that is running, signal it via req_signaljob2 */

		++preq->rq_refct;	/* protect the request/reply struct */

		for (i=0; i<parent->ji_ajtrk->tkm_ct; i++) {
			if (get_subjob_state(parent, i) == JOB_STATE_RUNNING) {
				pjob = find_job(mk_subjob_id(parent, i));
				if (pjob) {
					/* if suspending,  skip those already suspended,  */
					if (suspend && (pjob->ji_qs.ji_svrflags & JOB_SVFLG_Suspend))
						continue;
					/* if resuming, skip those not suspended         */
					if (resume && !(pjob->ji_qs.ji_svrflags & JOB_SVFLG_Suspend))
						continue;

					dup_br_for_subjob(preq, pjob, req_signaljob2);
				}
			}
		}
		/* if not waiting on any running subjobs, can reply; else */
		/* it is taken care of when last running subjob responds  */
		if (--preq->rq_refct == 0)
			reply_send(preq);
		return;

	}
	/* what's left to handle is a range of subjobs, foreach subjob 	*/
	/* if running, signal it					*/

	range = get_index_from_jid(jid);
	if (range == NULL) {
		req_reject(PBSE_IVALREQ, 0, preq);
		return;
	}

	/* first check that any in the subrange are in fact running */

	vrange = range;
	while (1) {
		if ((i = parse_subjob_index(vrange, &pc, &x, &y, &z, &j)) == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			return;
		} else if (i == 1)
			break;
		while (x <= y) {
			i = numindex_to_offset(parent, x);
			if (i >= 0) {
				if (get_subjob_state(parent, i) == JOB_STATE_RUNNING) {
					anygood++;
				}
			}
			x += z;
		}
		vrange = pc;
	}
	if (anygood == 0) { /* no running subjobs in the range */
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}

	/* now do the deed */

	++preq->rq_refct;	/* protect the request/reply struct */

	while (1) {
		if ((i = parse_subjob_index(range, &pc, &x, &y, &z, &j)) == -1) {
			req_reject(PBSE_IVALREQ, 0, preq);
			break;
		} else if (i == 1)
			break;
		while (x <= y) {
			i = numindex_to_offset(parent, x);
			if (i < 0) {
				x += z;
				continue;
			}

			if (get_subjob_state(parent, i) == JOB_STATE_RUNNING) {
				pjob = find_job(mk_subjob_id(parent, i));
				if (pjob) {
					dup_br_for_subjob(preq, pjob, req_signaljob2);
				}
			}
			x += z;
		}
		range = pc;
	}

	/* if not waiting on any running subjobs, can reply; else */
	/* it is taken care of when last running subjob responds  */
	if (--preq->rq_refct == 0)
		reply_send(preq);
	return;
}
/**
 * @brief
 * 		req_signaljob2 - service the Signal Job Request
 * @par
 *		This request sends (via MOM) a signal to a running job.
 *
 * @param[in]	preq	-	Signal Job Request
 */
static void
req_signaljob2(struct batch_request *preq, job *pjob)
{
	int		  rc;
	char 		 *pnodespec;
	int		suspend = 0;
	int		resume = 0;

	if ((pjob->ji_qs.ji_state != JOB_STATE_RUNNING)	||
		((pjob->ji_qs.ji_state == JOB_STATE_RUNNING) && (pjob->ji_qs.ji_substate == JOB_SUBSTATE_PROVISION))) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}
	if ((strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0 && !(pjob->ji_qs.ji_svrflags & JOB_SVFLG_AdmSuspd)) ||
		(strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_RESUME) == 0 && (pjob->ji_qs.ji_svrflags & JOB_SVFLG_AdmSuspd))) {
		req_reject(PBSE_WRONG_RESUME, 0, preq);
		return;
	}
	
	if (strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_RESUME) == 0 || strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0)
		resume = 1;
	else if (strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_SUSPEND) == 0 || strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_SUSPEND) == 0)
		suspend = 1;

	/* Special pseudo signals for suspend and resume require op/mgr */

	if (suspend || resume) {

		preq->rq_extra = pjob;	/* save job ptr for post_signal_req() */

		sprintf(log_buffer, "%s job by %s@%s",
			preq->rq_ind.rq_signal.rq_signame,
			preq->rq_user, preq->rq_host);
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_JOB, LOG_INFO,
			pjob->ji_qs.ji_jobid, log_buffer);
		if (resume) {
			if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_Suspend) != 0) {

				if (preq->rq_fromsvr == 1 || 
				    strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0) {
					/* from Scheduler, resume job */
					pnodespec = pjob->ji_wattr[(int)JOB_ATR_exec_vnode].at_val.at_str;
					if (pnodespec) {
						rc = assign_hosts(pjob, pnodespec, 0);
						if (rc == 0) {
							set_resc_assigned((void *)pjob, 0, INCR);
							/* if resume fails,need to free resources */
						} else {
							req_reject(rc, 0, preq);
							return;
						}
					}
				} else {
					/* not from scheduler, change substate so the  */
					/* scheduler will resume the job when possible */
					svr_setjobstate(pjob, JOB_STATE_RUNNING, JOB_SUBSTATE_SCHSUSP);
					set_scheduler_flag(SCH_SCHEDULE_NEW);
					reply_send(preq);
					return;
				}
			} else {
				/* Job can be resumed only on suspended state */
				req_reject(PBSE_BADSTATE, 0, preq);
				return;
			}
		}
	}

	/* log and pass the request on to MOM */

	sprintf(log_buffer, msg_signal_job, preq->rq_ind.rq_signal.rq_signame,
		preq->rq_user, preq->rq_host);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
		pjob->ji_qs.ji_jobid, log_buffer);
	rc = relay_to_mom(pjob, preq, post_signal_req);
	if (rc) {
		if (resume)
			rel_resc(pjob);
		req_reject(rc, 0, preq);	/* unable to get to MOM */
	}

	/* After MOM acts and replies to us, we pick up in post_signal_req() */
}

/**
 * @brief
 * 		issue_signal - send an internally generated signal to a running job
 *
 * @param[in,out]	pjob	-	running job
 * @param[in]	signame	-	name of the signal to send
 * @param[in]	func	-	function pointer taking work_task structure as argument.
 * @param[in]	extra	-	extra parameter to be stored in sig request
 *
 * @return	int
 * @retval	0	- success
 * @retval	non-zero	- error code
 */

int
issue_signal(job *pjob, char *signame, void (*func)(struct work_task *), void *extra)
{
	struct batch_request *newreq;

	/* build up a Signal Job batch request */

	if ((newreq = alloc_br(PBS_BATCH_SignalJob))==(struct batch_request *)0)
		return (PBSE_SYSTEM);

	newreq->rq_extra = extra;
	(void)strcpy(newreq->rq_ind.rq_signal.rq_jid, pjob->ji_qs.ji_jobid);
	(void)strncpy(newreq->rq_ind.rq_signal.rq_signame, signame, PBS_SIGNAMESZ);
	return (relay_to_mom(pjob, newreq, func));

	/* when MOM replies, we just free the request structure */
}

/**
 * @brief
 * 		post_signal_req - complete a Signal Job Request (externally generated)
 *
 * @param[in,out]	pwt	-	work_task which contains Signal Job Request
 */

static void
post_signal_req(struct work_task *pwt)
{
	job 		     *pjob;
	struct batch_request *preq;
	int		      rc;
	int		      ss;
	int		suspend = 0;
	int		resume = 0;

	if (pwt->wt_aux2 != 1) /* not rpp */
		svr_disconnect(pwt->wt_event);	/* disconnect from MOM */

	preq = pwt->wt_parm1;
	preq->rq_conn = preq->rq_orgconn;  /* restore client socket */
	pjob = preq->rq_extra;
	
	if(strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_SUSPEND)==0 ||
			strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_SUSPEND) == 0)
		suspend = 1;
	else if(strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_RESUME) == 0 ||
			strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0)
		resume = 1;

	if ((rc = preq->rq_reply.brp_code)) {

		/* there was an error on the Mom side of things */

		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_REQUEST, LOG_DEBUG,
			preq->rq_ind.rq_signal.rq_jid, msg_momreject);
		errno = 0;
		if (rc == PBSE_UNKJOBID)
			rc = PBSE_INTERNAL;
		if (resume) {
			/* resume failed, re-release resc and nodes */
			rel_resc(pjob);
		}
		req_reject(rc, 0, preq);

	} else {

		/* everything went ok for signal request at Mom */

		if (suspend && pjob && (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)) {
			if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_Suspend) == 0) {
				if (preq->rq_fromsvr == 1)
					ss = JOB_SUBSTATE_SCHSUSP;
				else
					ss = JOB_SUBSTATE_SUSPEND;
				pjob->ji_qs.ji_svrflags |= JOB_SVFLG_Suspend;
				rel_resc(pjob);  /* release resc and nodes */
				if(strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_SUSPEND) == 0)
					set_admin_suspend(pjob, 1);
				svr_setjobstate(pjob, JOB_STATE_RUNNING, ss);
			}

		} else if (resume && pjob && (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)) {
			/* note - the resources have already been reallocated */
			pjob->ji_qs.ji_svrflags &= ~JOB_SVFLG_Suspend;
			if(strcmp(preq->rq_ind.rq_signal.rq_signame, SIG_ADMIN_RESUME) == 0)
				set_admin_suspend(pjob, 0);
			svr_setjobstate(pjob, JOB_STATE_RUNNING, JOB_SUBSTATE_RUNNING);
		}

		reply_ack(preq);
	}
}
/**
 *	@brief Handle admin-suspend/admin-resume on the job and nodes
 *		set or remove the JOB_SVFLG_AdmSuspd flag on the job
 *		set or remove nodes in state maintenance
 * 
 *	@param[in] pjob - job to act upon
 *	@param[in] set_remove_nstate if 1, set flag/state if 0 remove flag/state
 * 
 *	@return void
 */
void set_admin_suspend(job *pjob, int set_remove_nstate) {
	char *chunk;
	char *execvncopy;
	char *last;
	char *vname;
	struct key_value_pair *pkvp;
	int hasprn;
	int nelem;
	struct pbsnode *pnode;
	attribute new;
	
	if(pjob == NULL)
		return;
	
	execvncopy = strdup(pjob->ji_wattr[(int)JOB_ATR_exec_vnode].at_val.at_str);
	
	if(execvncopy == NULL)
		return;
	
	if(set_remove_nstate)
		pjob->ji_qs.ji_svrflags |= JOB_SVFLG_AdmSuspd;
	 else
		pjob->ji_qs.ji_svrflags &= ~JOB_SVFLG_AdmSuspd;
	
	clear_attr(&new, &node_attr_def[(int)ND_ATR_MaintJobs]);
	decode_arst(&new, ATTR_NODE_MaintJobs, NULL, pjob->ji_qs.ji_jobid);
		
	chunk = parse_plus_spec_r(execvncopy, &last, &hasprn);
		
	while (chunk) {

		if (parse_node_resc(chunk, &vname, &nelem, &pkvp) == 0) {
			pnode = find_nodebyname(vname);
			if(pnode) {
				if(set_remove_nstate) {
					set_arst(&pnode->nd_attr[(int)ND_ATR_MaintJobs], &new, INCR);
					set_vnode_state(pnode, INUSE_MAINTENANCE, Nd_State_Or);
				} else {
					set_arst(&pnode->nd_attr[(int)ND_ATR_MaintJobs], &new, DECR);
					if (pnode->nd_attr[(int)ND_ATR_MaintJobs].at_val.at_arst->as_usedptr == 0)
						set_vnode_state(pnode, ~INUSE_MAINTENANCE, Nd_State_And);
				}
				pnode->nd_modified |= NODE_UPDATE_OTHERS; /* force save of attributes */
			}
		}
		chunk = parse_plus_spec_r(last, &last, &hasprn);
	}
	save_nodes_db(0, NULL);
	free_arst(&new);
	free(execvncopy);
}
