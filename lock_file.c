#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

#define PARENT_TO  SIGUSR1
#define CHILD_OK   SIGUSR1
#define CHILD_FAIL SIGUSR2
#define UNLOCK     SIGUSR2

#define MAX_PID_LEN 10

int child = 0;

/*
 * Child process functions
 */
 
int child_loop(const char *filename, int ppid, int script_pid, int no_block) {
	int  fd,
	     pid = getpid();
	char pid_str[MAX_PID_LEN+1] = {0};
	
	/*
	 * Set the child flag to let the signal handler know
	 * which process is running
	 */
	child = 1;
		
	/*
	 * Open file
	 */
	errno = 0;
	if ((fd = open(filename, O_CREAT | O_RDWR, 0700)) < 0) {
		printf("Failed to open file %s: %s\n", filename, strerror(errno));
		kill(ppid, SIGUSR2);
		return 1;
	}
	
	/*
	 * Lock file
	 */
	errno = 0;
	if (lockf(fd, (no_block) ? F_TLOCK : F_LOCK, 0) == -1) {
		printf("Failed to lock file %s (%i): %s\n", filename, fd, strerror(errno));
		kill(ppid, SIGUSR2);
		return 1;
	}
	
	/*
	 * File is locked - write our PID to it
	 */
	snprintf(pid_str, MAX_PID_LEN, "%i", pid);
	lseek(fd, 0, SEEK_SET);
	ftruncate(fd, 0);
	write(fd, pid_str, strlen(pid_str));
	
	/*
	 * Now send a signal to tell the parent process we have locked the file
	 */
	kill(ppid, SIGUSR1);
	
	/*
	 * We've locked the file and told the parent to exit.
	 * Now we need to hang around waiting for the signal
	 * to unlock.
	 *
	 * We should also unlock and exit if we detect the
	 * calling script has exited without calling unlock!
	 *
	 * Check for the script pid using the null signal.
	 */
	while(kill(script_pid, 0) == 0) {
		sleep(1);
	}
	
	/*
	 * Calling script must have exited
	 */

	return 1;
}

void child_sig_handler(int sig) {
	/*
	 * Child catches signal if parent times out
	 */
	switch(sig) {
		case PARENT_TO:
			printf("Parent process signalled timeout - exiting\n");
			exit(1);
			break;
		case UNLOCK:
			printf("Unlocking\n");
			exit(0);
			break;
		default:
			printf("Child caught unknown signal: %i\n", sig);
			break;
	}
}

/*
 * Parent process functions
 */

int parent_loop(int cpid, int timeout) {
	int time = 0;

	/*
	 * All the parent process needs to do now is wait
	 * for either the USR1 signal or the timeout
	 */
	while (timeout == 0 || time++ < timeout) {
		sleep(1);
	}
	
	/*
	 * Reached this point without exiting due
	 * to signals so must have timed out.
	 */
	kill(cpid, SIGUSR1);
	
	return 0;
}

void parent_sig_handler(int sig) {
	/*
	 * Parent catches signal if child locks file
	 * or if child fails to lock file.
	 */
	switch(sig) {
		case CHILD_OK:
			printf("Child has successfully locked file - exiting\n");
			exit(0);
			break;
		case CHILD_FAIL:
			printf("Child process failed to lock file\n");
			exit(1);
			break;
		default:
			printf("Parent caught unknown signal: %i\n", sig);
			break;
	}
}

/*
 * Signal handler for both processes
 */
void sig_handler(int sig) {
	if (child)
		child_sig_handler(sig);
	else
		parent_sig_handler(sig);
}

/*
 * Function to unlock the file
 */
int unlock_file(const char *filename, int timeout, int no_block) {
	int   fd,
	      locked,
	      pid  = 0,
	      time = 0;
	char  pid_str[MAX_PID_LEN+1] = {0},
	     *end;

	/*
	 * Open the file and check that it is locked
	 */
	errno = 0;
	if ((fd = open(filename, O_RDONLY)) < 0) {
		printf("Failed to open file %s: %s\n", filename, strerror(errno));
		return 1;
	}
	
	errno = 0;
	if ((locked = lockf(fd, F_TEST, 0)) == 0) {
		printf("File %s was not locked\n", filename);
	}
	
	/*
	 * Now read the PID from that file
	 */
	if (read(fd, pid_str, MAX_PID_LEN) > 0) {
		pid = (int)strtol(pid_str, &end, 10);
		if (*end != '\0' && *end != '\n')
			pid = 0;
	}
	if (pid == 0) {
		printf("Failed to read pid from file %s\n", filename);
		return 1;
	}
	
	timeout = timeout * 10;
	while (time++ < timeout || timeout == 0) {
		errno = 0;
		if (kill(pid, SIGUSR2) < 0) {
			if (time == 1)
				printf("Failed to send signal to child process %i: %s\n", pid, strerror(errno));
			break;
		}
		
		/*
		 * If file was unlocked, send a signal to child process
		 * to try to clean up, but don't wait for it - it could
		 * be some other process reusing the same PID
		 */
		if (!locked)
			break;
		
		usleep(1000+100);
	}

	if (time == timeout) {
		printf("Timed out\n");
		return 1;
	}
	else {
		return 0;
	}
}

int main(int argc, char **argv) {
	char *filename,
	      opt,
	     *end;
	int   longopt_idx,
	      timeout  = -1,
	      no_block = 0,
	      unlock   = 0;
	pid_t pid,
	      ppid,
	      cpid;
	
	/*
	 * Get command line args
	 */
	static struct option long_options[] = {
		{"timeout",  required_argument, 0, 't'},
		{"no-block", no_argument,       0, 'n'},
		{"unlock",   no_argument,       0, 'u'},
		{0, 0, 0, 0}
	};
	
	while ((opt = getopt_long(argc, argv, "t:nu", long_options, &longopt_idx)) != -1) {
		switch (opt) {
			case 't':
				timeout = (int)strtol(optarg, &end, 10);
				if (*end != '\0' || timeout < 0) {
					printf("Timeout argument should be a positive integer\n");
					return 1;
				}
				break;
			
			case 'n':
				no_block = 1;
				break;
			
			case 'u':
				unlock = 1;
				break;
			
			default:
				printf("Unrecognised option %c\n", opt);
				return 1;
		}
	}
	
	/*
	 * no-block means return straight away - timeout doesn't make sense
	 */
	if (no_block && timeout >= 0) {
		printf("Cannot set no-block and timeout together\n");
		return 1;
	}
	
	/*
	 * If timeout has not been changed, default to 0 (wait forever)
	 */
	if (timeout == -1)
		timeout = 0;
	
	/*
	 * Now get filename argument
	 */
	if (optind < argc) {
		filename = argv[optind];
	}
	else {
		printf("No filename given\n");
		return 1;
	}
	
	/*
	 * End: command line args
	 */
	
	/*
	 * Handle the unlock if required
	 */
	if (unlock)
		return unlock_file(filename, timeout, no_block);
	
	/*
	 * When the child locks the file, it sends us a USR1 signal to let us know.
	 * If it fails for any reason it can send USR2 signal instead.
	 * The parent can send USR1 to the child to kill it after a timeout.
	 *
	 * Set the signal handler now to avoid any race conditions after the fork.
	 */
	signal(SIGUSR1, sig_handler);
	signal(SIGUSR2, sig_handler);
	
	/*
	 * 3 PIDs to be interested in:
	 *  cpid : child process PID
	 *   pid : parent process PID
	 *  ppid : parent's parent PID
	 */
	pid  = getpid();
	ppid = getppid();
	cpid = fork();
	
	if (cpid == 0) {
		/*
		 * Child process
		 */
		return child_loop(filename, pid, ppid, no_block);
	}
	else {
		/*
		 * Parent process
		 */
		return parent_loop(cpid, timeout);
	}	
}