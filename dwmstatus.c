/*
 * Copy me if you can.
 * by 20h
 */

#define _BSD_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <err.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

#include <X11/Xlib.h>

#define SONG_TITLE_LEN 128

char *tzcopenhagen = "Europe/Copenhagen";

static Display *dpy;

struct {
	char *buf;
	pthread_mutex_t mutex;
} status;

struct {
	char buf[SONG_TITLE_LEN];
	pthread_mutex_t mutex;
} song_title;

char *
smprintf(char *fmt, ...)
{
	va_list fmtargs;
	char *ret;
	int len;

	va_start(fmtargs, fmt);
	len = vsnprintf(NULL, 0, fmt, fmtargs);
	va_end(fmtargs);

	ret = malloc(++len);
	if (ret == NULL) {
		err(EXIT_FAILURE, "malloc");
	}

	va_start(fmtargs, fmt);
	vsnprintf(ret, len, fmt, fmtargs);
	va_end(fmtargs);

	return ret;
}

void
settz(char *tzname)
{
	setenv("TZ", tzname, 1);
}

char *
mktimes(char *fmt, char *tzname)
{
	char buf[129];
	time_t tim;
	struct tm *timtm;

	settz(tzname);
	tim = time(NULL);
	timtm = localtime(&tim);
	if (timtm == NULL)
		return smprintf("");

	if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
		fprintf(stderr, "strftime == 0\n");
		return smprintf("");
	}

	return smprintf("%s", buf);
}

void
setstatus()
{
	pthread_mutex_lock(&status.mutex);

	char *tmcph;
	tmcph = mktimes("%a %d %b %H:%M %Y", tzcopenhagen);

	pthread_mutex_lock(&song_title.mutex);
	status.buf = smprintf(
		"%s%s%s",
		song_title.buf,
		song_title.buf[0] ? "   " : "",
		tmcph
	);
	pthread_mutex_unlock(&song_title.mutex);

	XStoreName(dpy, DefaultRootWindow(dpy), status.buf);
	XSync(dpy, False);

	free(tmcph);
	free(status.buf);

	pthread_mutex_unlock(&status.mutex);
}

char *
readfile(char *base, char *file)
{
	char *path, line[513];
	FILE *fd;

	memset(line, 0, sizeof(line));

	path = smprintf("%s/%s", base, file);
	fd = fopen(path, "r");
	free(path);
	if (fd == NULL)
		return NULL;

	if (fgets(line, sizeof(line)-1, fd) == NULL)
		return NULL;
	fclose(fd);

	return smprintf("%s", line);
}

// Continually get the currently playing song.
// We do this by forking `mpc` in a loop,
// writing the current song playing whenever the
// MPD status changes.
// If an error occurs (e.g. no MPD server is running),
// we try again every 60 seconds.
void *
currently_playing(void *_arg)
{
	// pipe used to communicate between the forked process
	int pipefd[2];
	// return status of forked `mpc` command
	int wstatus;
	// PID of forked child process (cpid), and return value of `waitpid` (w)
	pid_t cpid, w;

	bool no_song_title = true;

	// We wait one second for the MPD server to start up,
	// otherwise this will immediately fail and then wait 60 seconds...
	sleep(1);

	// continually try to get the status of the current song.
	while(true) {
		// open a pipe
		if (pipe(pipefd) == -1) {
			err(EXIT_FAILURE, "pipe");
		}

		// we fork the process
		cpid = fork();
		if (cpid == -1) {
			// if fork failed, somethings amiss
			err(EXIT_FAILURE, "fork");
		} if (cpid == 0) {
			// Child case: set up pipe and execve `mpc` command.

			// close unused read end of pipe
			if (close(pipefd[0]) == -1) {
				err(EXIT_FAILURE, "close");
			}

			// redirect stdout to the pipe
			if (dup2(pipefd[1], 1) == -1) {
				err(EXIT_FAILURE, "dup2");
			}

			static char *cmd = "mpc";
			// we use the `--wait` argument so the process first exits when the song changes.
			// The alternative is to run this in a short loop,
			// but then we either do many forks (lots of overhead),
			// or we risk being late with the status by several seconds.
			static char *args[] = {
				NULL,
				"--format=%artist% - %title%",
				"current",
				"--wait",
				NULL
			};
			args[0] = cmd;
			// If we have no current song loaded,
			// we remove the '--wait' parameter.
			if (no_song_title) {
				args[3] = NULL;
			}

			execvp(cmd, args);
			err(EXIT_FAILURE, "execvp");
		} else {
			// Parent case

			// close unused write end of pipe
			if (close(pipefd[1]) == -1) {
				err(EXIT_FAILURE, "close");
			}

			w = waitpid(cpid, &wstatus, 0);

			// check if waitpid failed
			if (w == -1) {
				perror("waitpid");
				goto songerr;
			} 

			// check if the child exited normally
			if (!WIFEXITED(wstatus)) {
				fprintf(stderr, "Child stopped unexpectedly...");
				goto songerr;
			}

			// get exit status
			int exit_status = WEXITSTATUS(wstatus);
			if (exit_status != 0) {
				fprintf(stderr, "Child stopped with non-zero exit status [%d]\n", exit_status);
				goto songerr;
			}

			char buf[SONG_TITLE_LEN] = {0};
			int ret;
			if ((ret = read(pipefd[0], &buf, SONG_TITLE_LEN-1)) == -1) {
				perror("read");
				goto songerr;
			}
			if (ret > 0) {
				no_song_title = false;
			}

			// close now unused read end of pipe
			if (close(pipefd[0]) == -1) {
				err(EXIT_FAILURE, "close");
			}

			// Remove any newlines
			for (int i = 0; buf[i]; i++) {
				if (buf[i] == '\n') {
					buf[i] = '\0';
					break;
				}
			}

			pthread_mutex_lock(&song_title.mutex);
			memcpy(song_title.buf, buf, SONG_TITLE_LEN);
			pthread_mutex_unlock(&song_title.mutex);

			setstatus();
		}

		// normally we just skip this block
		if (0) {
songerr:
			fprintf(stderr, "Error getting song status. Retrying in 60 seconds...\n");

			pthread_mutex_lock(&song_title.mutex);
			song_title.buf[0] = '\0';
			pthread_mutex_unlock(&song_title.mutex);

			setstatus();

			no_song_title = true;

			sleep(60);
		}
	}
}

int
main(void)
{
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwmstatus: cannot open display.\n");
		return 1;
	}

	pthread_mutex_init(&status.mutex, 0);
	pthread_mutex_init(&song_title.mutex, 0);
	
	int s;
	pthread_t music_thread_id;
	 
	s = pthread_create(
		&music_thread_id, NULL,
		&currently_playing, NULL
	);

	if (s != 0) {
		perror("pthread_create");
	}

	for (;;sleep(10)) {
		setstatus();
	}

	// cancel music thread
	pthread_cancel(music_thread_id);
	s = pthread_join(music_thread_id, NULL);
	if (s != 0) {
		perror("pthread_join");
	}

	XCloseDisplay(dpy);

	return 0;
}

