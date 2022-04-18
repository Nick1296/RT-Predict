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
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#define MAX_BENCHMARKS 10
#define MAX_PARAMS 10
#define DEFAULT_CYCLES 256
#define BUFLEN 256
#define WRITE_CSV_HEADER 1
#define NANOSECONDS (1UL)
#define MICROSECONDS (1000 * NANOSECONDS)
#define MILLISECONDS (1000 * MICROSECONDS)
#define SECONDS (1000 * MILLISECONDS)
#define MINUTES (60 * SECONDS)
#define PERF_EVENTS_NUM 5
#define SHM_PATH "/stap_func_id"
#define SHM_SIZE 512
#define TARGET_STARTED 1

#define USAGE_STR                                                              \
	"Usage: %s -o <output file> [-p cycles]"                               \
	" [-n how many perf events] [-m array of events]"                      \
	" [-c core to monitor] \n"                                             \
	" bmark1;arg1;arg2;... bmark2;arg1;arg2;..."

int running_bms = 0;
pid_t pids[MAX_BENCHMARKS];
uint64_t start_ts[MAX_BENCHMARKS];
volatile int done = 0;
int file_needs_header = WRITE_CSV_HEADER;
int outfd = -1;
char *bm_path, *bm_name, *bm_input;
int target_started;

struct perf_counter_set {
	int events[PERF_EVENTS_NUM];
	int types[PERF_EVENTS_NUM];
	char *names[PERF_EVENTS_NUM];
};

static struct perf_event_attr perf_event_attrs[PERF_EVENTS_NUM];
static int perf_fds[PERF_EVENTS_NUM];
static uint64_t perf_ids[PERF_EVENTS_NUM];
static unsigned long long perf_data[PERF_EVENTS_NUM];

static void *shm_addr = NULL;
int shm_fd = -1;

static int default_perf_events_list[PERF_EVENTS_NUM] = {
	PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_CACHE_REFERENCES,
	PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES,
	PERF_COUNT_SW_PAGE_FAULTS
};

static int perf_event_type_list[PERF_EVENTS_NUM] = {
	PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE,
	PERF_TYPE_HARDWARE, PERF_TYPE_SOFTWARE
};

char *perf_events[PERF_EVENTS_NUM] = { "cache misses", "cache references",
				       "retired instructions", "branch misses",
				       "page faults" };

static timer_t timer;

static unsigned long counters_first_values[PERF_EVENTS_NUM] = { 0 };

static inline unsigned long get_timing(void)
{
	unsigned a, d;
	asm volatile("rdtsc" : "=a"(a), "=d"(d));
	return ((unsigned long)a) | (((unsigned long)d) << 32);
}

/* Function to spawn all the listed benchmarks */
void launch_benchmark(char *bm, int bm_id)
{
	int res;
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
		printf("Launched: %s (PID = %d)\n", bm, cpid);
		res = kill(cpid, SIGSTOP);
		if (res < 0) {
			perror("cannot pause monitoring benchmark");
		}
		printf("Paused: %s (PID = %d)\n", bm, cpid);

		start_ts[bm_id] = get_timing();
		pids[bm_id] = cpid;
		running_bms++;
		done = 0;
		//cpid_arr[i*NUM_SD_VBS_BENCHMARKS_DATASETS+j] = cpid;
		//get benchmark name and get also input size if we have SD-VBS from rt-bench
		bm_path = malloc(sizeof(char) * strlen(bm) + 1);
		memset(bm_path, 0, strlen(bm) + 1);
		strcpy(bm_path, bm);
		int SD_VBS = 0, rtbench = 0;
		char *token;
		token = strsep(&bm_path, " ");
		bm_path = token;
		while (token != NULL) {
			if (strcmp(token, "rt-bench") == 0) {
				rtbench = 1;
			}
			if (rtbench) {
				if (strcmp(token, "vision") == 0) {
					SD_VBS = 1;
				}
			}

			token = strsep(&bm_path, "/");
			if (token != NULL) {
				if (SD_VBS) {
					bm_input = bm_name;
				}
				bm_name = token;
			}
		}
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
void perf_event_setup(pid_t target_pid, int cpuid)
{
	int i;
	struct perf_event_attr *pe;
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
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
			fprintf(stderr,
				"Error opening leader %llx for counter num %d\n",
				pe->config, i);
			exit(EXIT_FAILURE);
		}
	}
	fprintf(stderr, "perf event open\n");
}

//Print the value of the counters in a csv file.
void report_perf_events(char *func)
{
	int i;
	// print header of csv file
	char *header_init = "benchmark,input,pid,function,";
	//fprintf(stderr, "printing a row\n");
	if (file_needs_header == WRITE_CSV_HEADER) {
		dprintf(outfd, "%s", header_init);
		for (i = 0; i < PERF_EVENTS_NUM; i++) {
			dprintf(outfd, "%s", perf_events[i]);
			if (i + 1 < PERF_EVENTS_NUM) {
				dprintf(outfd, ",");
			}
		}
		dprintf(outfd, "\n");
	}

	//print the counter values, measurement per line
	dprintf(outfd, "%s,%s,%s,", bm_name, bm_input, func);

	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		dprintf(outfd, "%llu", perf_data[i]);
		if (i + 1 < PERF_EVENTS_NUM) {
			dprintf(outfd, ",");
		}
	}
	dprintf(outfd, "\n");
	//fprintf(stderr, "printed a row\n");
}

//read data from perf counters
void perf_event_read_counters(void)
{
	int i, ret;
	unsigned long long tmp[PERF_EVENTS_NUM];
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		ret = read(perf_fds[i], tmp + i, sizeof(unsigned long long));
		if (ret == -1) {
			perror("Error, cannot read counter");
		}
		if (tmp[i] - counters_first_values[i] > 0) {
			perf_data[i] = tmp[i] - counters_first_values[i];
		} else {
			fprintf(stderr,
				"WARNING: %s counter has an invalid value\n",
				perf_events[i]);
		}
	}
}

//Start counting events in the opened counters.
void perf_event_start(void)
{
	int i;
	fprintf(stderr, "starting perf events monitoring\n");
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		ioctl(perf_fds[i], PERF_EVENT_IOC_RESET, 0);
		ioctl(perf_fds[i], PERF_EVENT_IOC_ENABLE, 0);
		counters_first_values[i] = 0;
	}
	fprintf(stderr, "started perf events monitoring\n");
	perf_event_read_counters();
	//setup the counters first values
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		counters_first_values[i] = perf_data[i];
	}
}

//Stop counting events
void perf_event_stop(void)
{
	int i, ret;
	fprintf(stderr, "stopping perf events monitoring\n");
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		ioctl(perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
	}
	fprintf(stderr, "stopped per events monitoring\n");
}

void perf_event_close(void)
{
	int i;
	for (i = 0; i < PERF_EVENTS_NUM; i++) {
		close(perf_fds[i]);
	}
}

static void sampling(int signo, siginfo_t *siginfo, void *dummy)
{
	int i;
	char func[SHM_SIZE];
	//read the function name
	memcpy(func, shm_addr, sizeof(char) * SHM_SIZE);
	//check if the target application has started
	if (target_started != TARGET_STARTED &&
	    strcmp(func, "-1,systemtap_setup") != 0) {
		target_started = TARGET_STARTED;
	}
	if (target_started == TARGET_STARTED) {
		//read the counters
		perf_event_read_counters();
		report_perf_events(func);
		//after reporting the first time we don't need to reprint the csv header
		file_needs_header = ~WRITE_CSV_HEADER;
	}
}

int setup_perf_sampler(void)
{
	struct sigaction sa;
	int res = 0, i;
	struct sigevent event;
	// setup signal handler
	// we set the signals to ignore while handling the specified signal
	res = sigemptyset(&sa.sa_mask);
	if (res == -1) {
		perror("Error during sigemptyset for signal handler");
		return res;
	}
	// we mask SIGRTMIN
	res = sigaddset(&sa.sa_mask, SIGRTMIN);
	if (res == -1) {
		perror("Error during sigaddset for signal handler");
		return res;
	}
	// install the signal handler.
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = sampling;
	res = sigaction(SIGRTMIN, &sa, NULL);
	if (res == -1) {
		perror("Error during signal handler installation");
		return res;
	}
	memset(&event, 0, sizeof(event));
	// the timer will generate a signal
	event.sigev_notify = SIGEV_SIGNAL;
	// the signal generated by the timer
	event.sigev_signo = SIGRTMIN;
	// creation of the timer
	res = timer_create(CLOCK_REALTIME, &event, &timer);
	if (res != 0) {
		perror("Error during HR timer creation");
		return res;
	}
	//open the shared memory file

	shm_fd = shm_open(SHM_PATH, O_RDONLY, S_IRUSR | S_IRGRP | S_IROTH);
	if (shm_fd == -1) {
		perror("error during shm open");
		return shm_fd;
	}

	// map shared memory
	shm_addr = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
	if (shm_addr == MAP_FAILED) {
		perror("shm mmap failed");
		return -1;
	}
	return res;
}

/// Assumes stop has been performed before
void teardown_perf_sampler(void)
{
	int res = timer_delete(timer);
	if (res < 0) {
		perror("Error during period timer deletion");
	}
	munmap(shm_addr, SHM_SIZE);
	close(shm_fd);
}

void start_sampling(unsigned long it_input_time_s,
		    unsigned long it_input_time_ns,
		    unsigned long val_input_time_s,
		    unsigned long val_input_time_ns)
{
	int res;
	struct itimerspec timer_spec;
	perf_event_start();
	timer_spec.it_interval.tv_sec = it_input_time_s;
	timer_spec.it_interval.tv_nsec = it_input_time_ns;
	//start sampling ASAP
	timer_spec.it_value.tv_sec = val_input_time_s;
	timer_spec.it_value.tv_nsec = val_input_time_ns;
	res = timer_settime(timer, 0, &timer_spec, NULL);
	if (res < 0) {
		perror("Error during timer setup");
		exit(EXIT_FAILURE);
	}
	printf("sampling started\n");
}

void stop_sampling(void)
{
	int res;
	struct itimerspec timer_spec;
	memset(&timer_spec, 0, sizeof(struct itimerspec));
	res = timer_settime(timer, 0, &timer_spec, NULL);
	if (res < 0) {
		perror("Error during timer setup");
		exit(EXIT_FAILURE);
	}
	perf_event_stop();
	printf("sampling stopped\n");
}

int main(int argc, char **argv)
{
	char *strbuffer = NULL;
	int memfd, opt;
	unsigned long cycles = DEFAULT_CYCLES;
	char *bms[MAX_BENCHMARKS];
	void *mem;
	int bm_count = 0;
	int i = 0;
	int res;
	unsigned core_id = -1;
	int perf_event_count = 4;
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
			} else {
				file_needs_header = ~WRITE_CSV_HEADER;
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

	res = setup_perf_sampler();
	if (res < 0) {
		return res;
	}

	for (i = 0; i < bm_count; i++) {
		/* Now that profiling has been started, kick off the benchmarks */
		target_started = ~TARGET_STARTED;
		launch_benchmark(bms[i], i);
		// open the perf counters
		perf_event_setup(pids[i], core_id);

		/* Start Monitoring */
		start_sampling(0, 100 * MICROSECONDS, 0, 1);
		res = kill(pids[i], SIGCONT);
		if (res < 0) {
			perror("cannot resume monitored benchmark");
		}
		printf("Restarted: %s (PID = %d)\n", bms[i], pids[i]);
		/* Wait for bms to finish */
		wait_completion();

		/* Stop Monitoring*/
		stop_sampling();
		perf_event_close();
	}
	teardown_perf_sampler();
	printf("synching output file\n");
	fsync(outfd);
	if (outfd >= 0)
		close(outfd);

	return EXIT_SUCCESS;
}
