/*  

 Copyright (C) 2016-2021 Michael Boich

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
*/

#include <time.h>
#include <stdio.h>
#include <unistd.h>

#define LOG_FILE_NAME "vc-log"
void vc_log_private(char *msg)
{
    time_t now;
    struct tm *info;
    char time_buffer[80];
    FILE *logfile_fd;

    time(&now);
    info = localtime(&now);
    strftime(time_buffer, 80, "%c", info);
    printf("Formatted date & time : |%s|\n", time_buffer);
    
    logfile_fd = fopen(LOG_FILE_NAME,"a+");
    fprintf(logfile_fd,"%s %s\n", time_buffer,msg);
    fclose(logfile_fd);
}