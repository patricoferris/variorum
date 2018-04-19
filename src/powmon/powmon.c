/* powmon.c
 *
 * Copyright (c) 2011-2016, Lawrence Livermore National Security, LLC.
 * LLNL-CODE-645430
 *
 * Produced at Lawrence Livermore National Laboratory
 * Written by  Barry Rountree,   rountree@llnl.gov
 *             Daniel Ellsworth, ellsworth8@llnl.gov
 *             Scott Walker,     walker91@llnl.gov
 *             Kathleen Shoga,   shoga1@llnl.gov
 *
 * All rights reserved.
 *
 * This file is part of libmsr.
 *
 * libmsr is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * libmsr is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libmsr. If not, see <http://www.gnu.org/licenses/>.
 *
 * This material is based upon work supported by the U.S. Department of
 * Energy's Lawrence Livermore National Laboratory. Office of Science, under
 * Award number DE-AC52-07NA27344.
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "highlander.h"

#if 0
/********/
/* RAPL */
/********/
static double total_joules = 0.0;
static double limit_joules = 0.0;
static double max_watts = 0.0;
static double min_watts = 1024.0;
#endif

/*************************/
/* HW Counter Structures */
/*************************/
static unsigned long start;
static unsigned long end;
static FILE *summaryfile = NULL;
static FILE *logfile = NULL;

static pthread_mutex_t mlock;
static int *shmseg;
static int shmid;

static int running = 1;

#include "common.c"

int main(int argc, char **argv)
{
    const char *usage = "\n"
                        "NAME\n"
                        "  powmon - Package and DRAM power monitor\n"
                        "SYNOPSIS\n"
                        "  %s [--help | -h] [-c] -a \"<executable> <args> ...\"\n"
                        "OVERVIEW\n"
                        "  Powmon is a utility for sampling and printing the\n"
                        "  power consumption (for package and DRAM) and power\n"
                        "  limit per socket in a node.\n"
                        "OPTIONS\n"
                        "  --help | -h\n"
                        "      Display this help information, then exit.\n"
                        "  -a\n"
                        "      Application and arguments in quotes.\n"
                        "  -c\n"
                        "      Remove stale shared memory.\n"
                        "\n";
    if (argc == 1 || (argc > 1 && (
                          strncmp(argv[1], "--help", strlen("--help")) == 0 ||
                          strncmp(argv[1], "-h", strlen("-h")) == 0 )))
    {
        printf(usage, argv[0]);
        return 0;
    }

    int opt;
    int app_flag = 0;
    char *app;
    char **arg = NULL;

    while ((opt = getopt(argc, argv, "ca:")) != -1)
    {
        switch(opt)
        {
            case 'c':
                highlander_clean();
                printf("Exiting powmon...\n");
                return 0;
            case 'a':
                app_flag = 1;
                app = optarg;
                break;
            case '?':
                fprintf(stderr, "\nError: unknown parameter \"-%c\"\n", optopt);
                fprintf(stderr, usage, argv[0]);
                return 1;
            default:
                return 1;
        }
    }
    if (app_flag == 0)
    {
        fprintf(stderr, "\nError: must specify \"-a\"\n");
        fprintf(stderr, usage, argv[0]);
        return 1;
    }

    char *app_split = strtok(app, " ");
    int n_spaces = 0;
    while (app_split)
    {
        arg = realloc(arg, sizeof(char *) * ++n_spaces);

        if (arg == NULL)
        {
            return 1; /* memory allocation failed */
        }
        arg[n_spaces-1] = app_split;
        app_split = strtok(NULL, " ");
    }
    arg = realloc(arg, sizeof(char *) * (n_spaces + 1));
    arg[n_spaces] = 0;

#ifdef VARIORUM_DEBUG
    int i;
    for (i = 0; i < (n_spaces+1); ++i)
    {
        printf ("arg[%d] = %s\n", i, arg[i]);
    }
#endif

    char *fname_dat;
    char *fname_summary;
    if (highlander())
    {
        /* Start the log file. */
        int logfd;
        char hostname[64];
        gethostname(hostname,64);

        asprintf(&fname_dat, "%s.powmon.dat", hostname);

        logfd = open(fname_dat, O_WRONLY|O_CREAT|O_EXCL|O_NOATIME|O_NDELAY, S_IRUSR|S_IWUSR);
        if (logfd < 0)
        {
            fprintf(stderr, "Fatal Error: %s on %s cannot open the appropriate fd for %s -- %s.\n", argv[0], hostname, fname_dat, strerror(errno));
            return 1;
        }
        logfile = fdopen(logfd, "w");
        if (logfile == NULL)
        {
            fprintf(stderr, "Fatal Error: %s on %s fdopen failed for %s -- %s.\n", argv[0], hostname, fname_dat, strerror(errno));
            return 1;
        }

        /* Start power measurement thread. */
        pthread_attr_t mattr;
        pthread_t mthread;
        pthread_attr_init(&mattr);
        pthread_attr_setdetachstate(&mattr, PTHREAD_CREATE_DETACHED);
        pthread_mutex_init(&mlock, NULL);
        pthread_create(&mthread, &mattr, power_measurement, NULL);

        /* Fork. */
        pid_t app_pid = fork();
        if (app_pid == 0)
        {
            /* I'm the child. */
            printf("Profiling:");
            int i = 0;
            for (i = 0; i < n_spaces; i++)
            {
                printf(" %s", arg[i]);
            }
            printf("\n");
            execvp(arg[0], &arg[0]);
            printf("Fork failure\n");
            return 1;
        }
        /* Wait. */
        waitpid(app_pid, NULL, 0);
        sleep(1);

        highlander_wait();

        /* Stop power measurement thread. */
        running = 0;
        take_measurement();
        end = now_ms();

        /* Output summary data. */
        asprintf(&fname_summary, "%s.powmon.summary", hostname);

        logfd = open(fname_summary, O_WRONLY|O_CREAT|O_EXCL|O_NOATIME|O_NDELAY, S_IRUSR|S_IWUSR);
        if (logfd < 0)
        {
            fprintf(stderr, "Fatal Error: %s on %s cannot open the appropriate fd for %s -- %s.\n", argv[0], hostname, fname_summary, strerror(errno));
            return 1;
        }
        summaryfile = fdopen(logfd, "w");
        if (summaryfile == NULL)
        {
            fprintf(stderr, "Fatal Error: %s on %s fdopen failed for %s -- %s.\n", argv[0], hostname, fname_summary, strerror(errno));
            return 1;
        }

        char *msg;
        //asprintf(&msg, "host: %s\npid: %d\ntotal_joules: %lf\nallocated: %lf\nmax_watts: %lf\nmin_watts: %lf\nruntime ms: %lu\nstart: %lu\nend: %lu\n", hostname, app_pid, total_joules, limit_joules, max_watts, min_watts, end-start, start, end);
        asprintf(&msg, "host: %s\npid: %d\nruntime ms: %lu\nstart: %lu\nend: %lu\n", hostname, app_pid, end-start, start, end);

        fprintf(summaryfile, "%s", msg);
        fclose(summaryfile);
        close(logfd);

        shmctl(shmid, IPC_RMID, NULL);
        shmdt(shmseg);

        pthread_attr_destroy(&mattr);
    }
    else
    {
        /* Fork. */
        pid_t app_pid = fork();
        if (app_pid == 0)
        {
            /* I'm the child. */
            execvp(arg[0], &arg[0]);
            printf("Fork failure: %s\n", argv[1]);
            return 1;
        }
        /* Wait. */
        waitpid(app_pid, NULL, 0);

        highlander_wait();
    }

    printf("Output Files\n"
           "  %s\n"
           "  %s\n\n", fname_dat, fname_summary);
    highlander_clean();
    return 0;
}