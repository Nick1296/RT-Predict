/*
 * DDR Profiling user-space utility using perf 
 * 
 * Copyright (c) Boston University
 */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#define MAX_BENCHMARKS 10
#define MAX_PARAMS 10
#define DEFAULT_CYCLES 256
#define BUFLEN 256
#define MAX_PERF_EVENTS 10
#define WRITE_CSV_HEADER 1

#define USAGE_STR                                                              \
	"Usage: %s -o <output file> [-p cycles]"                               \
	" [-n how many perf events] [-m array of events]"                      \
	" [-c core to monitor] \n"                                             \
	" bmark1;arg1;arg2;... bmark2;arg1;arg2;..."

int default_perf_events_list[MAX_PERF_EVENTS] = { PERF_COUNT_HW_CACHE_MISSES,
						  PERF_COUNT_HW_INSTRUCTIONS,
						  PERF_COUNT_HW_BRANCH_MISSES,
						  PERF_COUNT_SW_PAGE_FAULTS };
int perf_event_type_list[MAX_PERF_EVENTS] = { PERF_TYPE_HARDWARE,
					      PERF_TYPE_HARDWARE,
					      PERF_TYPE_HARDWARE,
					      PERF_TYPE_SOFTWARE };

int running_bms = 0;
pid_t pids[MAX_BENCHMARKS];
uint64_t start_ts[MAX_BENCHMARKS];
volatile int done = 0;
struct perf_event_attr perf_event_attrs[MAX_PERF_EVENTS];
int perf_fds[MAX_PERF_EVENTS];
uint64_t perf_ids[MAX_PERF_EVENTS];
unsigned long long perf_data[MAX_PERF_EVENTS];

static inline unsigned long get_timing(void)
{
	unsigned a, d;
	asm volatile("rdtsc" : "=a"(a), "=d"(d));
	return ((unsigned long)a) | (((unsigned long)d) << 32);
}

/* Function to spawn all the listed benchmarks */
void launch_benchmark(char *bm, int bm_id)
{
	/* Launch all the BMs one by one */
	pid_t cpid = fork();
	if (cpid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}
	/* Child process */
	if (cpid == 0) {
		int p = 0;
		/* Assume that there is only at most one paramer */
		char **args = (char **)malloc(MAX_PARAMS * sizeof(char *));

		/* To follow execv's convention*/
		while (bm && p < MAX_PARAMS - 1) {
			args[p++] = strsep(&bm, " ");
		}
		args[p] = NULL;

		sched_yield();

		execv(args[0], args);

		/* This point can only be reached if execl fails. */
		perror("Unable to run benchmark");
		exit(EXIT_FAILURE);
	}
	/* Parent process */
	else {
		/* Keep track of the new bm that has been launched */
		printf("Running: %s (PID = %d)\n", bm, cpid);

		start_ts[bm_id] = get_timing();
		pids[bm_id] = cpid;
		running_bms++;
		done = 0;
		//cpid_arr[i*NUM_SD_VBS_BENCHMARKS_DATASETS+j] = cpid;
	}

	(void)pids;
}

/* Handler for SIGCHLD signal to detect benchmark termination */
/* Adapted from https://docs.oracle.com/cd/E19455-01/806-4750/signals-7/index.html */
void proc_exit_handler(int signo, siginfo_t *info, void *extra)
{
	int wstat;
	pid_t pid;

	(void)signo;
	(void)info;
	(void)extra;

	for (;;) {
		pid = waitpid(-1, &wstat, WNOHANG);
		if (pid == 0)
			/* No change in the state of the child(ren) */
			return;
		else if (pid == -1) {
			/* Something went wrong */
			perror("Waitpid() exited with error");
			exit(EXIT_FAILURE);
			return;
		} else {
			uint64_t end = get_timing();
			int i;
			printf("PID %d Done. Return code: %d\n", pid,
			       WEXITSTATUS(wstat));

			/* Record runtime of this benchmark */
			for (i = 0; i < MAX_BENCHMARKS; ++i) {
				if (pids[i] == pid) {
					start_ts[i] = end - start_ts[i];
					break;
				}
			}

			/* Detect completion of all the benchmarks */
			if (--running_bms == 0) {
				done = 1;
				return;
			}
		}
	}
}

/* Wait for completion using signals */
void wait_completion(void)
{
	sigset_t waitmask;
	struct sigaction chld_sa;

	/* Use RT POSIX extension */
	chld_sa.sa_flags = SA_SIGINFO;
	chld_sa.sa_sigaction = proc_exit_handler;
	sigemptyset(&chld_sa.sa_mask);
	sigaddset(&chld_sa.sa_mask, SIGCHLD);

	/* Install SIGCHLD signal handler */
	sigaction(SIGCHLD, &chld_sa, NULL);

	/* Wait for any signal */
	sigemptyset(&waitmask);
	while (!done) {
		sigsuspend(&waitmask);
	}
}

// Open the perf event counters by issuing a syscall.
static long perf_event_open(struct perf_event_attr *event_type, pid_t pid,
			    int cpu, int group_fd, unsigned long flags)
{
	int ret;
	ret = syscall(__NR_perf_event_open, event_type, pid, cpu, group_fd,
		      flags);
	return ret;
}

//Setup the datastructures to open the perf events counters and open them.
void perf_event_setup(char *perf_attr_type[], int perf_event_count,
		      pid_t target_pid, int cpuid)
{
	int i;
	struct perf_event_attr *pe;
	for (i = 0; i < perf_event_count; i++) {
		pe = perf_event_attrs + i;
		memset(pe, 0, sizeof(struct perf_event_attr));
		pe->type = perf_event_type_list[i];
		pe->size = sizeof(struct perf_event_attr);
		pe->config = default_perf_events_list[i];
		pe->disabled = 1;
		pe->exclude_kernel = 1;
		pe->exclude_hv = 1;
		perf_fds[i] = perf_event_open(pe, target_pid, cpuid, -1, 0);
		if (perf_fds[i] == -1) {
			fprintf(stderr, "Error opening leader %llx\n",
				pe->config);
			exit(EXIT_FAILURE);
		}
	}
	fprintf(stderr, "perf event open\n");
}

//Print the value of the counters in a csv file.
void report_perf_events(int outfd, char **perf_events, int perf_events_count,
			char *bm, int needs_header)
{
	int i;
	// print header of csv file
	char *header_init = "benchmark,";
	fprintf(stderr, "printing a row\n");
	if (needs_header == WRITE_CSV_HEADER) {
		dprintf(outfd, "%s", header_init);
		for (i = 0; i < perf_events_count; i++) {
			dprintf(outfd, "%s", perf_events[i]);
			if (i + 1 < perf_events_count) {
				dprintf(outfd, ",");
			}
		}
		dprintf(outfd, "\n");
	}
	//print the counter values, one benchmark per line
	dprintf(outfd, "%s,", bm);

	for (i = 0; i < perf_events_count; i++) {
		dprintf(outfd, "%llu", perf_data[i]);
		if (i + 1 < perf_events_count) {
			dprintf(outfd, ",");
		}
	}
	dprintf(outfd, "\n");
	fprintf(stderr, "printed a row\n");
}

//Start counting events in the opened counters.
void perf_event_start(int perf_event_count)
{
	int i;
	fprintf(stderr, "starting perf events monitoring\n");
	for (i = 0; i < perf_event_count; i++) {
		ioctl(perf_fds[i], PERF_EVENT_IOC_RESET, 0);
		ioctl(perf_fds[i], PERF_EVENT_IOC_ENABLE, 0);
	}
	fprintf(stderr, "started perf events monitoring\n");
}

//Stop counting events, and read the counters value.
void perf_event_stop(int perf_event_count)
{
	int i, ret;
	fprintf(stderr, "stopping per events monitoring\n");
	for (i = 0; i < perf_event_count; i++) {
		ioctl(perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
		ret = read(perf_fds[i], perf_data + i,
			   sizeof(unsigned long long));
		if (ret == -1) {
			perror("Error, cannot read perf counter");
		}
	}
	fprintf(stderr, "stopped per events monitoring\n");
}

int main(int argc, char **argv)
{
	char *strbuffer = NULL;
	int outfd = -1, memfd, opt;
	unsigned long cycles = DEFAULT_CYCLES;
	char *bms[MAX_BENCHMARKS];
	void *mem;
	int bm_count = 0;
	int i = 0;
	unsigned core_id = -1;
	int perf_event_count = 4;
	char *perf_events[MAX_PERF_EVENTS * 2] = {
		"PERF_COUNT_HW_CACHE_MISSES", "PERF_COUNT_HW_INSTRUCTIONS",
		"PERF_COUNT_HW_BRANCH_MISSES", "PERF_COUNT_SW_PAGE_FAULTS"
	};
	int file_needs_header = ~WRITE_CSV_HEADER;
	int file_common_flags = 0 | O_RDWR | O_CLOEXEC;
	/* Get input from user */
	while ((opt = getopt(argc, argv, "-o:p:c:m:n:")) != -1) {
		switch (opt) {
		case 1:
			bms[bm_count++] = argv[optind - 1];
			break;
		case 'o':
			strbuffer = (char *)malloc(BUFLEN);
			sprintf(strbuffer, "%s", optarg);

			if ((outfd = open(strbuffer,
					  O_APPEND | file_common_flags, 0660)) <
			    0) {
				fprintf(stderr,
					"File does not exist, it will be created.\n");
				file_needs_header = WRITE_CSV_HEADER;
				outfd = open(strbuffer,
					     O_CREAT | O_TRUNC |
						     file_common_flags,
					     0660);
				if (outfd < 0) {
					perror("Cannot create output file");
					exit(EXIT_FAILURE);
				}
			}

			free(strbuffer);
			break;
		case 'p':
			cycles = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			core_id = atoi(optarg);
			break;
		case 'n':
			perf_event_count = atoi(optarg);
			break;
		case 'm':
			strbuffer = (char *)malloc(BUFLEN);
			sprintf(strbuffer, "%s", optarg);
			/* create perf events */
			const char s[2] = " ";
			char *token;

			/* get the first token */
			token = strtok(strbuffer, s);

			/* walk through other tokens */
			while (token != NULL) {
				//printf( " %s\n", token );
				perf_events[i] = token;
				i++;
				token = strtok(NULL, s);
			}
			//printf("%s\n", perf_events[1]);
			free(strbuffer);
			break;
		default:
			fprintf(stderr, USAGE_STR, argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	// open the perf counters
	perf_event_setup(perf_events, perf_event_count, pids[0], core_id);

	for (i = 0; i < bm_count; i++) {
		/* Now that profiling has been started, kick off the benchmarks */
		launch_benchmark(bms[i], i);

		/* Start Monitoring */
		perf_event_start(perf_event_count);

		/* Wait for bms to finish */
		wait_completion();

		/* Stop Monitoring*/
		perf_event_stop(perf_event_count);
		report_perf_events(outfd, perf_events, perf_event_count, bms[i],
				   file_needs_header);
		//after reporting the first time we don't need to reprint the csv header
		file_needs_header = ~WRITE_CSV_HEADER;
	}
	if (outfd >= 0)
		close(outfd);

	return EXIT_SUCCESS;
}
