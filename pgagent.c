/*-------------------------------------------------------------------------
 *
 * src/pgagent.c
 *
 * Implementation of the pgagent task scheduler.
 *
 * author: yangjie
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif
#include <stdarg.h>

#include "postgres.h"
#include "fmgr.h"
#include "postmaster/bgworker.h"
#include "storage/proc.h"
#include "utils/fmgrprotos.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "pgstat.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "inttypes.h"
#include "access/xact.h"
#include "postgres_fe.h"
#include "libpq-fe.h"
#include "utils/guc.h"
#include "pqexpbuffer.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "lib/stringinfo.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"

#include "parser/parser.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define MAXATTEMPTS 10

/* global settings */
static char     *agentdbname = "postgres";

/* extension */
void _PG_init(void);
void pgagent_main(Datum args);
static void run_pgagent();

static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

static void pgagent_sighup(SIGNAL_ARGS);
static void pgagent_sigterm(SIGNAL_ARGS);

static MemoryContext pgagent_ctx = NULL;

void		*job(void *arg);

static void
pgagent_sighup(SIGNAL_ARGS)
{

        int save_errno = errno;
        got_sighup = true;
        if (MyProc != NULL)
        {
                SetLatch(&MyProc->procLatch);
        }
        errno = save_errno;
}

static void
pgagent_sigterm(SIGNAL_ARGS)
{
        int save_errno = errno;
        got_sigterm = true;

        if (MyProc != NULL)
        {
                SetLatch(&MyProc->procLatch);
        }
        errno = save_errno;
}

static void
pgagent_exit(int code)
{
        if(pgagent_ctx) {
                MemoryContextDelete(pgagent_ctx);
                pgagent_ctx  = NULL;
        }
        proc_exit(code);
}

void * 
job(void *arg)
{
	PGconn			*conn;
	char            	conninfo[1024];
	PGresult		*res = NULL;
	PQExpBufferData		query;
	int			pid;
	int			jobid;
	int			logid;
	char			status;
	int			nstep;
	int			r;

	int *p = (int *)arg;
	jobid = *p;
	logid = *p;

	sprintf(conninfo, "dbname = %s", agentdbname);
	conn = PQconnectdb(conninfo);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
		PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
 
	res = PQexec(conn, "select * from pgagent.pga_jobagent");
	pid = atoi(PQgetvalue(res, 0, 0));
	PQclear(res);
	
	/**/
        initPQExpBuffer(&query);
        appendPQExpBuffer(&query, "UPDATE pgagent.pga_job SET jobagentid=%d, joblastrun=now() WHERE jobagentid IS NULL AND jobid=%d", pid, jobid);
        PQexec(conn, query.data);
        termPQExpBuffer(&query);

	initPQExpBuffer(&query);
	{
		res = PQexec(conn, "SELECT nextval('pgagent.pga_joblog_jlgid_seq') AS id");
		logid = atoi(PQgetvalue(res, 0, 0));
		appendPQExpBuffer(&query, "INSERT INTO pgagent.pga_joblog(jlgid, jlgjobid, jlgstatus) VALUES (%d, %d, 'r')", logid, jobid);
		PQclear(res);
		res = PQexec(conn, query.data);
		if(res)
		{
			status = 'r';
		}
	}
	termPQExpBuffer(&query);
	PQclear(res);

	/**/
	initPQExpBuffer(&query);
        appendPQExpBuffer(&query, "SELECT * FROM pgagent.pga_jobstep WHERE jstenabled AND jstjobid=%d ORDER BY jstname, jstid", jobid);
	res = PQexec(conn, query.data);
	for(nstep = 0; nstep < PQntuples(res); nstep++)
	{
		switch (toascii(PQgetvalue(res, nstep, 5)[0]))
		{
			case 's':
			{
				PQexec(conn, PQgetvalue(res, nstep, 6));		

				break;
			}
			case 'b':
			{
				r = system(PQgetvalue(res, nstep, 6));
				if (r != 0)
				{
					exit(1);
				}

				break;
			}
			default:
			{

			}
		}
			
	}
        termPQExpBuffer(&query);
	PQclear(res);

	if(status != ' ')
	{
		initPQExpBuffer(&query);
		appendPQExpBuffer(&query, "UPDATE pgagent.pga_joblog SET jlgstatus='%c', jlgduration=now() - jlgstart WHERE jlgid=%d;", status, logid);
		PQexec(conn, query.data);
		termPQExpBuffer(&query);

		/**/
		initPQExpBuffer(&query);
		appendPQExpBuffer(&query, "UPDATE pgagent.pga_job SET jobagentid=NULL, jobnextrun=NULL WHERE jobid=%d", jobid);
		PQexec(conn, query.data);
		termPQExpBuffer(&query);
	}

	PQfinish(conn);
	return NULL;
}

void
pgagent_main(Datum args)
{
	char            conninfo[1024];
	PGconn          *conn;
	PGresult        *res;
        PQExpBufferData         query;
	char            hostname[255];

	pqsignal(SIGHUP, pgagent_sighup);
	pqsignal(SIGTERM, pgagent_sigterm);

	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection(agentdbname, NULL);

	pgagent_ctx = AllocSetContextCreate(CurrentMemoryContext,
                                            "pgagent_ctx",
                                            ALLOCSET_DEFAULT_MINSIZE,
                                            ALLOCSET_DEFAULT_INITSIZE,
                                            ALLOCSET_DEFAULT_MAXSIZE);
	

	MemoryContextSwitchTo(pgagent_ctx);

	sprintf(conninfo, "dbname = %s", agentdbname);

	conn = PQconnectdb(conninfo);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
		PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	
	res = PQexec(conn, "SELECT count(*) As count, pg_backend_pid() AS pid FROM pg_class cl JOIN pg_namespace ns ON ns.oid=relnamespace WHERE relname='pga_job' AND nspname='pgagent'");

	if(0 == atoi(PQgetvalue(res, 0, 0)))
		PQexec(conn, "create extension pgagent;");

	PQexec(conn, "CREATE TEMP TABLE pga_tmp_zombies(jagpid int4)");

        PQexec(conn, "INSERT INTO pga_tmp_zombies (jagpid) "
                                "SELECT jagpid "
                                "FROM pgagent.pga_jobagent AG "
                                "LEFT JOIN pg_stat_activity PA ON jagpid=pid "
                                "WHERE pid IS NULL");

	PQexec(conn, "UPDATE pgagent.pga_joblog SET jlgstatus='d' WHERE jlgid IN ("
				"SELECT jlgid "
				"FROM pga_tmp_zombies z, pgagent.pga_job j, pgagent.pga_joblog l "
				"WHERE z.jagpid=j.jobagentid AND j.jobid = l.jlgjobid AND l.jlgstatus='r');"

				"UPDATE pgagent.pga_jobsteplog SET jslstatus='d' WHERE jslid IN ( "
				"SELECT jslid "
				"FROM pga_tmp_zombies z, pgagent.pga_job j, pgagent.pga_joblog l, pgagent.pga_jobsteplog s "
				"WHERE z.jagpid=j.jobagentid AND j.jobid = l.jlgjobid AND l.jlgid = s.jsljlgid AND s.jslstatus='r');"

				"UPDATE pgagent.pga_job SET jobagentid=NULL, jobnextrun=NULL "
				"  WHERE jobagentid IN (SELECT jagpid FROM pga_tmp_zombies);"

				"DELETE FROM pgagent.pga_jobagent "
				"  WHERE jagpid IN (SELECT jagpid FROM pga_tmp_zombies);");

        PQexec(conn, "DROP TABLE pga_tmp_zombies");

	gethostname(hostname, sizeof(hostname));

	initPQExpBuffer(&query);
        appendPQExpBuffer(&query, "INSERT INTO pgagent.pga_jobagent (jagpid, jagstation) SELECT pg_backend_pid(), '%s'", hostname);
        PQexec(conn, query.data);
        termPQExpBuffer(&query);

	PQclear(res);
	PQfinish(conn);

	while(!got_sigterm)
	{
		int rc;

		int     i;

                pthread_t t0;
                void * result;

		rc = WaitLatch(MyLatch,
                                           WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                                           0L,
                                           PG_WAIT_EXTENSION);

                ResetLatch(MyLatch);

                if (got_sighup)
		{
			got_sighup = false;
		}

		if (rc & WL_POSTMASTER_DEATH)
			pgagent_exit(1);		

		CHECK_FOR_INTERRUPTS();	


		conn = PQconnectdb(conninfo);

		if (PQstatus(conn) != CONNECTION_OK)
		{
			fprintf(stderr, "Connection to database failed: %s",
			PQerrorMessage(conn));
			PQfinish(conn);
			exit(1);
		}

                initPQExpBuffer(&query);
                appendPQExpBuffer(&query, "SELECT J.jobid "
                                        "  FROM pgagent.pga_job J "
                                        " WHERE jobenabled "
                                        "   AND jobagentid IS NULL "
                                        "   AND jobnextrun <= now() "
                                        "   AND (jobhostagent = '' OR jobhostagent = '%s') "
                                        " ORDER BY jobnextrun", hostname);
                res = PQexec(conn, query.data);
                termPQExpBuffer(&query);

                if(res)
                {
                        for(i = 0; i < PQntuples(res); i++)
                        {
                                int jobid;

                                jobid = atoi(PQgetvalue(res, i, 0));

                                if(pthread_create(&t0, NULL, job, (void *)&jobid) == -1){
                                        initPQExpBuffer(&query);
                                        appendPQExpBuffer(&query, "INSERT INTO pgagent.pga_joblog(jlgid, jlgjobid, jlgstatus) "
                                                        " VALUES (nextval('pgagent.pga_joblog_jlgid_seq'), %d, 'i')", jobid);
                                        PQexec(conn, query.data);
                                        termPQExpBuffer(&query);

					PQfinish(conn);
                                        exit(1);
                                }

                                if(pthread_join(t0, &result) == -1)
                                {
                                        exit(1);
                                }

                        }
                }
		PQclear(res);
		PQfinish(conn);

		sleep(3);
		MemoryContextReset(pgagent_ctx);
	}
	pgagent_exit(0);
}

static void
run_pgagent()
{
        BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
	{
		return;
	}

	DefineCustomStringVariable(
		"agent.dbname",
		gettext_noop("Database in which pgagent metadata is kept."),
		NULL,
		&agentdbname,
		"postgres",
		PGC_SIGHUP,
		GUC_SUPERUSER_ONLY,
		NULL, NULL, NULL);

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_notify_pid = 0;
	worker.bgw_restart_time = 1;
	worker.bgw_main_arg = (Datum)NULL;

	sprintf(worker.bgw_library_name, "pgagent");
	sprintf(worker.bgw_function_name, "pgagent_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "pgagent_scheduler");

	RegisterBackgroundWorker(&worker);
}

void _PG_init(void)
{
        run_pgagent();
}
