/*
 * MIT DARPA Urban Challenge Team
 *
 * Module Name: procman_deputy
 *
 * Description:
 *
 * The procman_deputy module is a process-management daemon that manages a
 * collection of processes.  It listens for commands over LCM and starts and
 * stops processes according to the commands it receives.  Addditionally, the
 * procman_deputy periodically transmits the state of the processes that it is
 * managing.
 *
 * Maintainer: Albert Huang <albert@csail.mit.edu>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <errno.h>

#include <glib.h>

#include <lcm/lcm.h>

#include <lcmtypes/bot_procman_printf_t.h>
#include <lcmtypes/bot_procman_info_t.h>
#include <lcmtypes/bot_procman_orders_t.h>
#include "procman.h"
#include "procinfo.h"
#include "signal_pipe.h"
#include "lcm_util.h"

#define ESTIMATED_MAX_CLOCK_ERROR_RATE 1.001

#define dbg(args...) fprintf(stderr, args)
//#undef dbg
//#define dbg(args...)

static int64_t timestamp_now()
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

static void dbgt (const char *fmt, ...) 
{
    va_list ap;
    va_start (ap, fmt);

    char timebuf[1024];
    time_t now;
    time (&now);
    struct tm now_tm;
    localtime_r (&now, &now_tm);
    strftime (timebuf, sizeof (timebuf)-1, "%F %T", &now_tm);

    char buf[4096];
    vsnprintf (buf, sizeof(buf), fmt, ap);

    va_end (ap);

    fprintf (stderr, "%s: %s", timebuf, buf);
}

typedef struct _procman_deputy {
    procman_t *pm;

    lcm_t *lcm;

    char hostname[1024];

    GMainLoop * mainloop;

    int norders_slm;       // total bot_procman_orders_t observed Since Last MARK
    int norders_forme_slm; // total bot_procman_orders_t for this deputy slm
    int nstale_orders_slm; // total stale bot_procman_orders_t for this deputy slm

    GList *observed_sheriffs_slm; // names of observed sheriffs slm
    char *last_sheriff_name;      // name of the most recently observed sheriff

    sys_cpu_mem_t cpu_time[2];
    float cpu_load;

    int verbose;
} procman_deputy_t;

typedef struct _pmd_cmd_moreinfo {
    // glib handles for IO watches
    GIOChannel *stdout_ioc;
    guint stdout_sid;
    int32_t actual_runid;
    int32_t sheriff_id;

    proc_cpu_mem_t cpu_time[2];
    float cpu_usage;

    char *group;
    char *nickname;

    int num_kills_sent;
    int64_t last_kill_time;
    int remove_requested;
} pmd_cmd_moreinfo_t;

// make this global so that the signal handler can access it
static procman_deputy_t global_pmd;

static void
transmit_proc_info (procman_deputy_t *s);

static void
transmit_str (procman_deputy_t *pmd, int sid, char * str)
{
    bot_procman_printf_t msg;
    msg.deputy_name = pmd->hostname;
    msg.sheriff_id = sid;
    msg.text = str;
    msg.utime = timestamp_now ();
    bot_procman_printf_t_publish (pmd->lcm, "PMD_PRINTF", &msg);
}

static void
printf_and_transmit (procman_deputy_t *pmd, int sid, char *fmt, ...) {
    int len;
    char buf[256];
    va_list ap;
    va_start (ap, fmt);
    
    len = vsnprintf (buf, sizeof (buf), fmt, ap);
    if (pmd->verbose)
        fputs (buf, stderr);

    if (len) {
        bot_procman_printf_t msg;
        msg.deputy_name = pmd->hostname;
        msg.sheriff_id = sid;
        msg.text = buf;
        msg.utime = timestamp_now ();
        bot_procman_printf_t_publish (pmd->lcm, "PMD_PRINTF", &msg);
    } else {
        dbgt ("uh oh.  printf_and_transmit printed zero bytes\n");
    }
}

// invoked when a child process writes something to its stdout/stderr fd
static int
pipe_data_ready (GIOChannel *source, GIOCondition condition, 
        procman_cmd_t *cmd)
{
    pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)cmd->user;
    int result = TRUE;
    int anycondition = 0;

    if (condition & G_IO_IN) {
        char buf[1024];
        int bytes_read = read (cmd->stdout_fd, buf, sizeof (buf)-1);
        if (bytes_read < 0) {
            snprintf (buf, sizeof (buf), "procman [%s] read: %s (%d)\n", 
                    cmd->cmd->str, strerror (errno), errno);
            dbgt (buf);
            transmit_str (&global_pmd, mi->sheriff_id, buf);
        } else if ( bytes_read == 0) {
            dbgt ("zero byte read\n");
        } else {
            buf[bytes_read] = '\0';
            transmit_str (&global_pmd, mi->sheriff_id, buf);
        }
        anycondition = 1;
    }
    if (condition & G_IO_ERR) {
        transmit_str (&global_pmd, mi->sheriff_id, 
                "procman deputy: detected G_IO_ERR.\n");
        dbgt ("G_IO_ERR from [%s]\n", cmd->cmd->str);
        anycondition = 1;
    }
    if (condition & G_IO_HUP) {
        transmit_str (&global_pmd, mi->sheriff_id, 
                "procman deputy: detected G_IO_HUP.  end of output\n");
        dbgt ("G_IO_HUP from [%s]\n", cmd->cmd->str);
        result = FALSE;
        anycondition = 1;
    }
    if (condition & G_IO_NVAL) {
        transmit_str (&global_pmd, mi->sheriff_id, 
                "procman deputy: detected G_IO_NVAL.  end of output\n");
        dbgt ("G_IO_NVAL from [%s]\n", cmd->cmd->str);
        result = FALSE;
        anycondition = 1;
    }
    if (condition & G_IO_PRI) {
        transmit_str (&global_pmd, mi->sheriff_id,
                "procman deputy: unexpected G_IO_PRI... wtf?\n");
        dbgt ("G_IO_PRI from [%s]\n", cmd->cmd->str);
        anycondition = 1;
    }
    if (condition & G_IO_OUT) {
        transmit_str (&global_pmd, mi->sheriff_id,
                "procman deputy: unexpected G_IO_OUT... wtf?\n");
        dbgt ("G_IO_OUT from [%s]\n", cmd->cmd->str);
        anycondition = 1;
    }
    if (!anycondition) {
        dbgt ("wtf??? [%s] pipe has condition 0x%X\n", cmd->cmd->str, 
                condition);
    }
    return result;
}

static int 
start_cmd (procman_deputy_t *pmd, procman_cmd_t *cmd, int desired_runid) 
{
    int status;
    status = procman_start_cmd (pmd->pm, cmd);
    if (0 != status) {
        printf_and_transmit (pmd, 0, "couldn't start [%s]\n", cmd->cmd->str);
        dbgt ("couldn't start [%s]\n", cmd->cmd->str);

        pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)cmd->user;
        printf_and_transmit (pmd, mi->sheriff_id, 
                "ERROR!  couldn't start [%s]\n", cmd->cmd->str);
        return -1;
    }
    pmd_cmd_moreinfo_t *gh = (pmd_cmd_moreinfo_t*)cmd->user;

    // add stdout for this process to IO watch list
    if (gh->stdout_ioc) {
        dbgt ("ERROR: expected gh->stdout_ioc to be NULL [%s]\n",
                cmd->cmd->str);
    }

    gh->stdout_ioc = g_io_channel_unix_new (cmd->stdout_fd);
    g_io_channel_set_encoding (gh->stdout_ioc, NULL, NULL);
    fcntl (cmd->stdout_fd, F_SETFL, O_NONBLOCK);
    gh->stdout_sid = g_io_add_watch (gh->stdout_ioc,
            G_IO_IN, (GIOFunc)pipe_data_ready, cmd);

    gh->actual_runid = desired_runid;
    gh->num_kills_sent = 0;
    gh->last_kill_time = 0;
    return 0;
}

static int 
stop_cmd (procman_deputy_t *pmd, procman_cmd_t *cmd) 
{
    if (!cmd->pid) return 0;

    pmd_cmd_moreinfo_t *gh = (pmd_cmd_moreinfo_t*)cmd->user;

    /* Try killing no faster than 1 Hz */
    int64_t now = timestamp_now ();
    if (gh->last_kill_time && now < gh->last_kill_time + 900000)
        return 0;

    int status;
    if (gh->num_kills_sent > 5) {
        status = procman_kill_cmd (pmd->pm, cmd, SIGKILL);
    } else {
        status = procman_kill_cmd (pmd->pm, cmd, SIGTERM);
    }
    gh->num_kills_sent ++;
    gh->last_kill_time = now;

    if (0 != status) {
        printf_and_transmit (pmd, gh->sheriff_id, 
                "kill: %s\n", strerror (-status));
    }
    return status;
}

static int 
remove_all_cmds (procman_deputy_t *pmd)
{
    int status = 0;
    const GList *all_cmds = procman_get_cmds (pmd->pm);

    GList *toremove = g_list_copy ((GList*) all_cmds);
    for (GList *iter=toremove; iter; iter=iter->next) {
        procman_cmd_t *cmd = (procman_cmd_t*)iter->data;
        
        if (cmd->pid) {
            if (0 != stop_cmd (pmd, cmd)) {
                status = -1;
            }
        }
        pmd_cmd_moreinfo_t *mi = cmd->user;
        free(mi->group);
        free(mi->nickname);
        free(mi);
        cmd->user = NULL;
        procman_remove_cmd (pmd->pm, cmd);
    }
    g_list_free(toremove);
    return status;
}

static void
check_for_dead_children (procman_deputy_t *pmd)
{
    procman_cmd_t *cmd = NULL;
    procman_check_for_dead_children (pmd->pm, &cmd);

    while (cmd) {
        int status;
        pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)cmd->user;

        // check the stdout pipes to see if there is anything from stdout /
        // stderr.
        struct pollfd pfd = { cmd->stdout_fd, POLLIN, 0 };
        status = poll (&pfd, 1, 0);
        if (pfd.revents & POLLIN) {
            pipe_data_ready (NULL, G_IO_IN, cmd);
        }

        // did the child terminate with a signal?
        if (WIFSIGNALED (cmd->exit_status)) {
            int signum = WTERMSIG (cmd->exit_status);

            printf_and_transmit (pmd, mi->sheriff_id, 
                    "%s\n", 
                    strsignal (signum), signum);
            if (WCOREDUMP (cmd->exit_status)) {
                printf_and_transmit (pmd, mi->sheriff_id, "Core dumped.\n");
            }
        }

        // cleanup the glib hooks if necessary
        pmd_cmd_moreinfo_t *gh = (pmd_cmd_moreinfo_t*)cmd->user;

        if (gh->stdout_ioc) {
            dbgt ("removing [%s] glib event sources\n", cmd->cmd->str);
            // detach from the glib event loop
            g_io_channel_unref (gh->stdout_ioc);
            g_source_remove (gh->stdout_sid);
            gh->stdout_ioc = NULL;
            gh->stdout_sid = 0;

            procman_close_dead_pipes (pmd->pm, cmd);
        }

        // remove ?
        if (gh->remove_requested) {
            dbgt ("removing [%s]\n", cmd->cmd->str);
            // cleanup the private data structure used
            pmd_cmd_moreinfo_t *mi = cmd->user;
            free(mi->group);
            free(mi->nickname);
            free(mi);
            cmd->user = NULL;
            procman_remove_cmd (pmd->pm, cmd);
        }
        cmd = NULL;
        procman_check_for_dead_children (pmd->pm, &cmd);
        transmit_proc_info (pmd);
    }
}

static void
glib_handle_signal (int signal, procman_deputy_t *pmd) {
    if (signal == SIGCHLD) {
        // a child process died.  check to see which one, and cleanup its
        // remains.
        check_for_dead_children (pmd);
    }
    else {
        // quit was requested.  kill all processes and quit
        dbgt ("received signal %d (%s).  stopping all processes\n", signal,
                strsignal (signal));
        remove_all_cmds (pmd);
        dbgt ("stopping deputy main loop\n");
        g_main_loop_quit (pmd->mainloop);
    }
}

static void
transmit_proc_info (procman_deputy_t *s)
{
    int i;
    bot_procman_info_t msg;

    // build a deputy info message
    memset (&msg, 0, sizeof (msg));

    const GList *allcmds = procman_get_cmds (s->pm);

    msg.utime = timestamp_now ();
    msg.host = s->hostname;
    msg.cpu_load = s->cpu_load;
    msg.phys_mem_total_bytes = s->cpu_time[1].memtotal;
    msg.phys_mem_free_bytes = s->cpu_time[1].memfree;
    msg.swap_total_bytes = s->cpu_time[1].swaptotal;
    msg.swap_free_bytes = s->cpu_time[1].swapfree;

    msg.ncmds = g_list_length ((GList*) allcmds);
    msg.cmds = 
        (bot_procman_deputy_cmd_t *) calloc (msg.ncmds, sizeof (bot_procman_deputy_cmd_t));

    const GList *iter = allcmds;
    for (i=0; i<msg.ncmds; i++) {
        procman_cmd_t *cmd = (procman_cmd_t*)iter->data;
        pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)cmd->user;

        msg.cmds[i].name = cmd->cmd->str;
	msg.cmds[i].nickname = mi->nickname;
        msg.cmds[i].actual_runid = mi->actual_runid;
        msg.cmds[i].pid = cmd->pid;
        msg.cmds[i].exit_code = cmd->exit_status;
        msg.cmds[i].sheriff_id = mi->sheriff_id;
        msg.cmds[i].group = mi->group;
        msg.cmds[i].cpu_usage = mi->cpu_usage;
        msg.cmds[i].mem_vsize_bytes = mi->cpu_time[1].vsize;
        msg.cmds[i].mem_rss_bytes = mi->cpu_time[1].rss;

        iter = iter->next;
    }

    if (s->verbose) printf ("transmitting deputy info!\n");
    bot_procman_info_t_publish (s->lcm, "PMD_INFO", &msg);

    // release memory
    free (msg.cmds);
}

static void
update_cpu_times (procman_deputy_t *s)
{
    const GList *allcmds = procman_get_cmds (s->pm);
    const GList *iter;
    int status;

    status = procinfo_read_sys_cpu_mem (&s->cpu_time[1]);
    if(0 != status) {
        perror("update_cpu_times - procinfo_read_sys_cpu_mem");
    }

    sys_cpu_mem_t *a = &s->cpu_time[1];
    sys_cpu_mem_t *b = &s->cpu_time[0];

    uint64_t ellapsed_jiffies = a->user - b->user + 
                                a->user_low - b->user_low + 
                                a->system - b->system + 
                                a->idle - b->idle;
    uint64_t loaded_jiffies = a->user - b->user +
                              a->user_low - b->user_low + 
                              a->system - b->system;
    if (! ellapsed_jiffies) {
        s->cpu_load = 0;
    } else {
        s->cpu_load = (double)loaded_jiffies / ellapsed_jiffies;
    }

    for (iter = allcmds; iter; iter=iter->next) {
        procman_cmd_t *cmd = (procman_cmd_t*)iter->data;
        pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)cmd->user;

        if (cmd->pid) {
            status = procinfo_read_proc_cpu_mem (cmd->pid, &mi->cpu_time[1]);
            if (0 != status) {
                mi->cpu_usage = 0;
                mi->cpu_time[1].vsize = 0;
                mi->cpu_time[1].rss = 0;
                perror("update_cpu_times - procinfo_read_proc_cpu_mem");
                // TODO handle this error
            } else {
                proc_cpu_mem_t *pa = &mi->cpu_time[1];
                proc_cpu_mem_t *pb = &mi->cpu_time[0];

                uint64_t used_jiffies = pa->user - pb->user + 
                                        pa->system - pb->system;

                if (! ellapsed_jiffies || pb->user == 0 || pb->system == 0) {
                    mi->cpu_usage = 0;
                } else {
                    mi->cpu_usage = (double)used_jiffies / ellapsed_jiffies;
                }
            }
        } else {
            mi->cpu_usage = 0;
            mi->cpu_time[1].vsize = 0;
            mi->cpu_time[1].rss = 0;
        }

        memcpy (&mi->cpu_time[0], &mi->cpu_time[1], sizeof (proc_cpu_mem_t));
    }

    memcpy (&s->cpu_time[0], &s->cpu_time[1], sizeof (sys_cpu_mem_t));
}

static gboolean
one_second_timeout (procman_deputy_t *s)
{
    update_cpu_times (s);

    transmit_proc_info (s);

    return TRUE;
}

static gboolean
introspection_timeout (procman_deputy_t *s)
{
    int mypid = getpid();
    proc_cpu_mem_t pinfo;
    int status = procinfo_read_proc_cpu_mem (mypid, &pinfo);
    if(0 != status)  {
        perror("introspection_timeout - procinfo_read_proc_cpu_mem");
    }

    const GList *allcmds = procman_get_cmds (s->pm);
    int nrunning=0;
    for (const GList *citer=allcmds; citer; citer=citer->next) {
        procman_cmd_t *cmd = (procman_cmd_t*)citer->data;
        if (cmd->pid) nrunning++;
    }

    dbgt ("MARK - rss: %"PRId64" kB vsz: %"PRId64
            " kB procs: %d (%d alive)\n", 
            pinfo.rss / 1024, pinfo.vsize / 1024,
            g_list_length ((GList*)procman_get_cmds (s->pm)),
            nrunning
           );
    dbgt ("       orders: %d forme: %d (%d stale) sheriffs: %d\n",
            s->norders_slm, s->norders_forme_slm, s->nstale_orders_slm,
            g_list_length (s->observed_sheriffs_slm));

    s->norders_slm = 0;
    s->norders_forme_slm = 0;
    s->nstale_orders_slm = 0;

    for (GList *ositer=s->observed_sheriffs_slm; ositer; ositer=ositer->next) {
        free (ositer->data);
    }
    g_list_free (s->observed_sheriffs_slm);
    s->observed_sheriffs_slm = NULL;

    return TRUE;
}

static bot_procman_sheriff_cmd_t *
procmd_orders_find_cmd (const bot_procman_orders_t *a, int32_t sheriff_id)
{
    int i;
    for (i=0; i<a->ncmds; i++) {
        if (sheriff_id == a->cmds[i].sheriff_id) return &a->cmds[i];
    }
    return NULL;
}

static procman_cmd_t *
find_local_cmd (procman_deputy_t *s, int32_t sheriff_id)
{
    const GList *iter;
    for (iter=procman_get_cmds (s->pm); iter; iter=iter->next) {
        procman_cmd_t *cand = (procman_cmd_t*)iter->data;
        pmd_cmd_moreinfo_t *cmi = (pmd_cmd_moreinfo_t*)cand->user;

        if (cmi->sheriff_id == sheriff_id) {
            return cand;
        }
    }
    return NULL;
}

static void
_set_command_group (procman_cmd_t *p, const char *group)
{
    pmd_cmd_moreinfo_t *mi = p->user;
    free (mi->group);
    mi->group = strdup (group);
}


static void
_set_command_nickname (procman_cmd_t *p, const char *nickname)
{
    pmd_cmd_moreinfo_t *mi = p->user;
    free (mi->nickname);
    mi->nickname = strdup (nickname);
}

static void
procman_deputy_order_received (const lcm_recv_buf_t *rbuf, const char *channel, 
        const bot_procman_orders_t *orders, void *user_data)
{
    procman_deputy_t *s = user_data;
    const GList *iter = NULL;
    s->norders_slm ++;

    // ignore orders for other deputies
    if (strcmp (orders->host, s->hostname)) {
        if (s->verbose) 
            printf ("ignoring orders for other host %s\n", orders->host);
        return;
    }
    s->norders_forme_slm++;

    // ignore stale orders (where utime is too long ago)
    int64_t now = timestamp_now ();
    if (now - orders->utime > PROCMAN_MAX_MESSAGE_AGE_USEC) {
        for (int i=0; i<orders->ncmds; i++) {
               bot_procman_sheriff_cmd_t *cmd = &orders->cmds[i];
               printf_and_transmit (s, cmd->sheriff_id, "ignoring stale orders (utime %d seconds ago). You may want to check the system clocks!\n",
                   (int) ((now - orders->utime) / 1000000));
        }
         s->nstale_orders_slm++;
        return;
    }

    // check if we've seen this sheriff since the last MARK.
    GList *ositer = NULL;
    for (ositer=s->observed_sheriffs_slm; ositer; ositer=ositer->next) {
        if (!strcmp ((char*) ositer->data, orders->sheriff_name)) break;
    }
    if (!ositer) {
        s->observed_sheriffs_slm = g_list_prepend (s->observed_sheriffs_slm, 
                    strdup (orders->sheriff_name));
    }
    if (s->last_sheriff_name && 
            strcmp (orders->sheriff_name, s->last_sheriff_name)) {
        free (s->last_sheriff_name);
        s->last_sheriff_name = NULL;
    }

    if (!s->last_sheriff_name) {
        s->last_sheriff_name = strdup (orders->sheriff_name);
    }

    // attempt to carry out the orders
    int action_taken = 0;
    int i;
    if (s->verbose)
        printf ("orders for me received with %d commands\n", orders->ncmds);
    for (i=0; i<orders->ncmds; i++) {

        bot_procman_sheriff_cmd_t *cmd = &orders->cmds[i];

        if (s->verbose)
            printf ("order %d: %s (%d, %d)\n", 
                    i, cmd->name, cmd->desired_runid, cmd->force_quit);

        // do we already have this command somewhere?
        procman_cmd_t *p = find_local_cmd (s, cmd->sheriff_id);
        pmd_cmd_moreinfo_t *mi = NULL;

        if (p) {
            mi = (pmd_cmd_moreinfo_t*) p->user;
        } else {
            // if not, then create it.
            if (s->verbose) printf ("adding new process (%s)\n", cmd->name);
            p = procman_add_cmd (s->pm, cmd->name);

            // allocate a private data structure for glib info
            mi = (pmd_cmd_moreinfo_t*) calloc (1, sizeof (pmd_cmd_moreinfo_t));
            mi->sheriff_id = cmd->sheriff_id;
            mi->group = strdup (cmd->group);
	    mi->nickname = strdup (cmd->nickname);
            p->user = mi;
            action_taken = 1;
        }

        // check if the command needs to be started or stopped
        procman_cmd_status_t cmd_status = procman_get_cmd_status (s->pm, p);

        // rename a command?  does not kill a running command, so effect does
        // not apply until command is restarted.
        if (strcmp (p->cmd->str, cmd->name)) {
            dbgt ("renaming [%s] to [%s]\n", p->cmd->str, cmd->name);
            procman_cmd_change_str (p, cmd->name);

            action_taken = 1;
        }

        // change a command's nickname?
        if (strcmp (mi->nickname, cmd->nickname)) {
            dbgt ("setting nickname of [%s] to [%s]\n", p->cmd->str, 
                    cmd->nickname);
            _set_command_nickname (p, cmd->nickname);
            action_taken = 1;
        }


        // change the group of a command?
        if (strcmp (mi->group, cmd->group)) {
            dbgt ("setting group of [%s] to [%s]\n", p->cmd->str, 
                    cmd->group);
            _set_command_group (p, cmd->group);
            action_taken = 1;
        }

        if (PROCMAN_CMD_STOPPED == cmd_status &&
            (mi->actual_runid != cmd->desired_runid) && 
            ! cmd->force_quit) {
            start_cmd (s, p, cmd->desired_runid);
            action_taken = 1;
            mi->actual_runid = cmd->desired_runid;
        } else if (PROCMAN_CMD_RUNNING == cmd_status && 
                (cmd->force_quit || (cmd->desired_runid != mi->actual_runid))) {
            stop_cmd (s, p);
            action_taken = 1;
        } else {
            mi->actual_runid = cmd->desired_runid;
        }
    }

    // if there are any commands being managed that did not appear in the
    // orders, then stop and remove those commands
    GList *toremove = NULL;
    for (iter=procman_get_cmds (s->pm); iter; iter=iter->next) {
        procman_cmd_t *p = (procman_cmd_t*)iter->data;
        pmd_cmd_moreinfo_t *mi = (pmd_cmd_moreinfo_t*)p->user;
        bot_procman_sheriff_cmd_t *cmd = 
            procmd_orders_find_cmd (orders, mi->sheriff_id);

        if (! cmd) {
            // push the orphaned command into a list first.  remove later, to
            // avoid corrupting the linked list (since this is a borrowed data
            // structure)
            toremove = g_list_append (toremove, p);
        }
    }

    // cull orphaned commands
    for (iter=toremove; iter; iter=iter->next) {
        procman_cmd_t *p = iter->data;
        pmd_cmd_moreinfo_t *mi = p->user;

        if (p->pid) {
            dbgt ("scheduling [%s] for removal\n", p->cmd->str);
            mi->remove_requested = 1;
            stop_cmd (s, p);
        } else {
            dbgt ("removing [%s]\n", p->cmd->str);
            // cleanup the private data structure used
            free (mi->group);
	    free (mi->nickname);
            free (mi);
            p->user = NULL;
            procman_remove_cmd (s->pm, p);
        }

        action_taken = 1;
    }
    g_list_free(toremove);

    if (action_taken)
        transmit_proc_info (s);
    return;
}

static void usage()
{
    fprintf (stderr, "usage: bot-procman-deputy [options]\n"
            "\n"
            "  -h, --help        shows this help text and exits\n"
            "  -v, --verbose     verbose output\n"
            "  -n, --name NAME   use deputy name NAME instead of hostname\n"
            "  -l, --log PATH    dump messages to PATH instead of stdout\n"
            "  -u, --lcmurl URL  use specified LCM URL for procman messages\n"
          );
}

int main (int argc, char **argv)
{
    char *optstring = "hvfl:n:u:";
    int c;
    struct option long_opts[] = { 
        { "help", no_argument, 0, 'h' },
        { "verbose", no_argument, 0, 'v' },
        { "log", required_argument, 0, 'l' },
        { "lcmurl", required_argument, 0, 'u' },
        { "name", required_argument, 0, 'n' },
        { 0, 0, 0, 0 }
    };

    char *logfilename = NULL;
    int verbose = 0;
    char *hostname_override = NULL;
    char *lcmurl = NULL;

    g_thread_init(NULL);
    
    while ((c = getopt_long (argc, argv, optstring, long_opts, 0)) >= 0)
    {
        switch (c) {
            case 'v':
                verbose = 1;
                break;
            case 'l':
                free(logfilename);
                logfilename = strdup (optarg);
                break;
            case 'u':
                free(lcmurl);
                lcmurl = strdup(optarg);
                break;
            case 'n':
                free(hostname_override);
                hostname_override = strdup (optarg);
                break;
            case 'h':
            default:
                usage();
                return 1;
        }
    }

     // create the lcm_t structure for doing IPC
     lcm_t *lcm = lcm_create(lcmurl);
     free(lcmurl);
     if (NULL == lcm) {
         fprintf (stderr, "error initializing LCM.  ");
         return 1;
     }

     // redirect stdout and stderr to a log file if the -l command line flag
     // was specified.
     if (logfilename) {
         int fd = open (logfilename, O_WRONLY | O_APPEND | O_CREAT, 0644);
         if (fd < 0) {
             perror ("open");
             fprintf (stderr, "couldn't open logfile %s\n", logfilename);
             return 1;
         }
         close(1); close(2);
         if (dup2(fd, 1) < 0) { return 1; }
         if (dup2(fd, 2) < 0) { return 1; }
         close (fd);
         setlinebuf (stdout);
         setlinebuf (stderr);
     }

     procman_deputy_t *pmd = &global_pmd;

     memset (pmd, 0, sizeof (procman_deputy_t));
     pmd->lcm = lcm;
     pmd->verbose = verbose;
     pmd->norders_slm = 0;
     pmd->nstale_orders_slm = 0;
     pmd->norders_forme_slm = 0;
     pmd->observed_sheriffs_slm = NULL;
     pmd->last_sheriff_name = NULL;

     pmd->mainloop = g_main_loop_new (NULL, FALSE);
     if (!pmd->mainloop) {
         fprintf (stderr, "Error: Failed to create glib mainloop\n");
         return 1;
     }

     // set deputy hostname to the system hostname
     if (hostname_override) {
         strcpy (pmd->hostname, hostname_override);
         free (hostname_override);
     } else {
         gethostname (pmd->hostname, sizeof (pmd->hostname));
     }
//     sprintf (pmd->hostname + strlen (pmd->hostname), "%d", getpid());

     // load config file
     procman_params_t params;
     procman_params_init_defaults (&params, argc, argv);

     pmd->pm = procman_create (&params);
     if (NULL == pmd->pm) {
         fprintf (stderr, "couldn't create procman_t\n");
         return 1;
     }

     // convert Unix signals into glib events
     signal_pipe_init();
     signal_pipe_add_signal (SIGINT);
     signal_pipe_add_signal (SIGHUP);
     signal_pipe_add_signal (SIGQUIT);
     signal_pipe_add_signal (SIGTERM);
     signal_pipe_add_signal (SIGCHLD);
     signal_pipe_attach_glib ((signal_pipe_glib_handler_t) glib_handle_signal, 
             pmd);

     // setup LCM handler
     lcmu_glib_mainloop_attach_lcm (pmd->lcm);

     bot_procman_orders_t_subscription_t *subs = 
         bot_procman_orders_t_subscribe (pmd->lcm, "PMD_ORDERS",
             procman_deputy_order_received, pmd);

     // setup a timer to periodically transmit status information
     g_timeout_add (1000, (GSourceFunc) one_second_timeout, pmd);

     // periodically check memory usage
     g_timeout_add (120000, (GSourceFunc) introspection_timeout, pmd);

     // go!
     g_main_loop_run (pmd->mainloop);

     lcmu_glib_mainloop_detach_lcm (pmd->lcm);

     // cleanup
     signal_pipe_cleanup();

     procman_destroy (pmd->pm);

     g_main_loop_unref (pmd->mainloop);

     bot_procman_orders_t_unsubscribe(pmd->lcm, subs);

     for (GList *siter=pmd->observed_sheriffs_slm; siter; siter=siter->next) {
         free(siter->data);
     }
     g_list_free(pmd->observed_sheriffs_slm);

     lcm_destroy (pmd->lcm);

     if (pmd->last_sheriff_name) free (pmd->last_sheriff_name);

     return 0;
}
