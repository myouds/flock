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

enum l_type {
	FLOCK = 0,
	FCNTL,
	LOCKF
};

struct lock_request {
	const char *filename;
	int         fd;
	enum l_type type;
	int         no_block;
	int         timeout;
};

int child = 0;

int lock_descriptor(struct lock_request *req) {
	int retval = 1;
	
	switch (req->type) {
		case LOCKF:
			errno = 0;
			if (lockf(req->fd, (req->no_block) ? F_TLOCK : F_LOCK, 0) == -1) {
				printf("Failed to lock file (fd = %i): %s\n", req->fd, strerror(errno));
				retval = 0;
			}
			break;
		case FLOCK:
			if (flock(req->fd, (req->no_block) ? LOCK_EX | LOCK_NB : LOCK_EX) == -1) {
				printf("Failed to lock file (fd = %i): %s\n", req->fd, strerror(errno));
				retval = 0;
			}
			break;
		case FCNTL:
			break;
	}
	
	return retval;
}

/*
 * Function to unlock an open file descriptor
 * The file descriptor will have been passed to us by the user.
 #
 * This function will only be used if the file lock operation
 * was given a file descriptor by the user AND the lock type was
 * flock.
 */
int unlock_descriptor(int fd) {
	int retval = 1;
	/*
	 * Only need to handle flock lock type
	 */
	errno = 0;
	if (flock(fd, LOCK_UN) == -1) {
		printf("Failed to unlock file (fd = %i): %s\n", fd, strerror(errno));
		retval = 0;
	}
	else {
		printf("Unlocked file descriptor %i\n", fd);
	}
	return retval;
}

/*
 * Child process functions
 */
 
int child_loop(struct lock_request *req, int ppid, int script_pid) {
	int  pid = getpid();
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
	if ((req->fd = open(req->filename, O_CREAT | O_RDWR, 0700)) < 0) {
		printf("Failed to open file %s: %s\n", req->filename, strerror(errno));
		kill(ppid, SIGUSR2);
		return 1;
	}
	
	/*
	 * Lock file
	 */
	printf("Locking file %s\n", req->filename);
	if (!lock_descriptor(req)) {
		kill(ppid, SIGUSR2);
		return 1;
	}
	
	/*
	 * File is locked - write our PID to it
	 */
	snprintf(pid_str, MAX_PID_LEN, "%i", pid);
	lseek(req->fd, 0, SEEK_SET);
	ftruncate(req->fd, 0);
	write(req->fd, pid_str, strlen(pid_str));
	
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
int unlock_file(struct lock_request *req) {
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
	if ((fd = open(req->filename, O_RDONLY)) < 0) {
		printf("Failed to open file %s: %s\n", req->filename, strerror(errno));
		return 1;
	}
	
	errno = 0;
	if ((locked = lockf(fd, F_TEST, 0)) == 0) {
		printf("File %s was not locked\n", req->filename);
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
		printf("Failed to read pid from file %s\n", req->filename);
		return 1;
	}
	
	if (req->no_block)
		req->timeout = 0;
	
	req->timeout = req->timeout * 10;
	while (time++ < req->timeout || req->timeout == 0) {
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

	if (time == req->timeout) {
		printf("Timed out\n");
		return 1;
	}
	else {
		return 0;
	}
}

int lock_file(struct lock_request *req) {
	return 1;
}

int main(int argc, char **argv) {
	char                opt,
	                   *end;
	int                 longopt_idx,
	                    unlock  = 0,
	                    do_fork = 1;
	pid_t               pid,
	                    ppid,
	                    cpid;
	struct lock_request req     = {0};
	
	/*
	 * Get command line args
	 */
	static struct option long_options[] = {
		{"timeout",  required_argument, 0, 't'},
		{"no-block", no_argument,       0, 'n'},
		{"unlock",   no_argument,       0, 'u'},
		{"type",     required_argument, 0, 'T'},
		{0, 0, 0, 0}
	};
	
	while ((opt = getopt_long(argc, argv, "t:T:nu", long_options, &longopt_idx)) != -1) {
		switch (opt) {
			case 't':
				req.timeout = (int)strtol(optarg, &end, 10);
				if (*end != '\0' || req.timeout < 0) {
					printf("Timeout argument should be a positive integer\n");
					return 1;
				}
				break;
			
			case 'n':
				req.no_block = 1;
				break;
			
			case 'u':
				unlock = 1;
				break;
			
			case 'T':
				if (strcasecmp(optarg, "lockf") == 0)
					req.type = LOCKF;
				else if (strcasecmp(optarg, "flock") == 0)
					req.type = FLOCK;
				else if (strcasecmp(optarg, "fcntl") == 0)
					req.type = FCNTL;
				else {
					printf("Invalid type: %s\n", optarg);
					return 1;
				}
				break;
			
			default:
				printf("Unrecognised option: %c\n", opt);
				return 1;
		}
	}
	
	/*
	 * no-block means return straight away - timeout doesn't make sense
	 */
	if (req.no_block && req.timeout >= 0) {
		printf("Cannot set no-block and timeout together\n");
		return 1;
	}
	
	/*
	 * If timeout has not been changed, default to 0 (wait forever)
	 */
	if (req.timeout == -1)
		req.timeout = 0;
	
	/*
	 * Now get filename argument
	 */
	if (optind < argc) {
		/*
		 * Work out if we have a filename or a file descriptor
		 */
		end = NULL;
		req.fd = (int)strtol(argv[optind], &end, 10);
		if (*end != '\0') {
			req.fd = 0;
			req.filename = argv[optind];
		}
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
	if (unlock) {
		if (req.fd)
			return unlock_descriptor(req.fd);
		else
			return unlock_file(&req);
	}
	
	/*
	 * Now decide if we need to fork a child process
	 * We only do not need to fork if we have been given a file descriptor
	 * and we have been told to use flock
	 */
	if (req.fd && req.type == FLOCK)
		do_fork = 0;
	
	if (do_fork) {
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
			return child_loop(&req, pid, ppid);
		}
		else {
			/*
			 * Parent process just needs to hang around until
			 * the child has done its locking
			 */
			return parent_loop(cpid, req.timeout);
		}
	}
	else {
		/*
		 * Lock file descriptor
		 */
		printf("Locking file descriptor %i\n", req.fd);
		if (!lock_descriptor(&req)) {
			return 1;
		}
		return 0;
	}
}