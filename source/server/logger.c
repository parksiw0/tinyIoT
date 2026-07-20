#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#include "util.h"
#include "logger.h"
#include "config.h"

char *log_buffer;
FILE *log_file;
static pthread_mutex_t logger_lock = PTHREAD_MUTEX_INITIALIZER;

int logger_is_enabled(LOGLEVEL level)
{
    return level >= LOG_LEVEL;
}

/**
 * @brief Initialize logger
 * @return void
 */
void logger_init()
{
    char logFileName[100];
    log_buffer = malloc(sizeof(char) * LOG_BUFFER_SIZE);
#ifdef SAVE_LOG
    time_t now;
    time(&now);
    char *t = get_local_time(0);
    sprintf(logFileName, "%s.log", t);
    log_file = fopen(logFileName, "w");
    free(t);
#endif
}

/**
 * @brief Free logger
 * @return void
 */
void logger_free()
{
	pthread_mutex_lock(&logger_lock);
    free(log_buffer);
    log_buffer = NULL;
#ifdef SAVE_LOG
    if (log_file)
        fclose(log_file);
    log_file = NULL;
#endif
    pthread_mutex_unlock(&logger_lock);
}

/**
 * @brief Print log message to stderr
 * @param tag Tag of the log message
 * @param level Log level (DEBUG, INFO, WARN, ERROR, FATAL)
 * @param msg Message to print includes format strings
 * @param ... Arguments for format strings
 * @return Number of characters printed
 */
int logger(const char *tag, LOGLEVEL level, const char *msg, ...)
{

    va_list ap;
    char t[26] = {0};
    char *llChar;
    int charsCnt = 0, fcolor = 0;
    switch (level)
    {
    case LOG_LEVEL_DEBUG:
        llChar = "DEBUG";
        fcolor = BLUE;
        break;

    case LOG_LEVEL_INFO:
        llChar = "INFO";
        fcolor = GREEN;
        break;

    case LOG_LEVEL_WARN:
        llChar = "WARN";
        fcolor = YELLOW;
        break;

    case LOG_LEVEL_ERROR:
        llChar = "ERROR";
        fcolor = BR_RED;
        break;

    case LOG_LEVEL_FATAL:
        llChar = "FATAL";
        fcolor = RED;
        break;

    default:
        return 0;
    }

    if (logger_is_enabled(level))
    {
        pthread_mutex_lock(&logger_lock);
        time_t now;
        time(&now);
        ctime_r(&now, t);
        t[24] = '\0';
        fprintf(stderr, "\r%s \033[0;%dm%-5s\033[0m [%s]: ", t, fcolor, llChar, tag);

        va_start(ap, msg);
        charsCnt = log_buffer ? vsnprintf(log_buffer, LOG_BUFFER_SIZE, msg, ap) : 0;
        va_end(ap);
        if (log_buffer)
            fprintf(stderr, "%s\n", log_buffer);

#ifdef SAVE_LOG
        if (log_file && log_buffer)
            fprintf(log_file, "\r%s %-5s [%s]: %s\n", t, llChar, tag, log_buffer);
#endif
        pthread_mutex_unlock(&logger_lock);
    }
    return charsCnt;
}
