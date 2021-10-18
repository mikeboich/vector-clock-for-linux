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
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include </usr/local/include/cjson/cJSON.h>
#include "weather.h"

#include "font.h"
#include "draw.h"
#include "ViewingLocation.h"

#include <curl/curl.h>
#include <semaphore.h>
#include "util.h"
#include "vc_log.h"

#undef USE_LOCKS
char btc_price_str[64];
char btc_title_str[64];
double btc_price_float;

char btc_in_buf[1024];
char *btc_write_ptr;
extern sem_t curl_mutex;

//Update weather info every this often:
#define BTC_INTERVAL 15

size_t btc_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    debugMsg("btc write_callback for %d bytes\n", size * nmemb);
    for (int i = 0; i < size * nmemb; i++)
    {
        *btc_write_ptr++ = *ptr++;
    }
    return size * nmemb;
}

void parse_btc_payload()
{
    cJSON *curl_JSON, *data_JSON, *price_JSON;

    curl_JSON = cJSON_Parse(btc_in_buf);
    if (curl_JSON == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            vc_log("Error before: %s\n", error_ptr);
            return;
        }
    }

    data_JSON = cJSON_GetObjectItemCaseSensitive(curl_JSON, "data");
    price_JSON = cJSON_GetObjectItemCaseSensitive(data_JSON, "amount");
    btc_price_str[0] = '$';
    btc_price_str[1] = (char)0;
    strcat(btc_price_str, price_JSON->valuestring);
    btc_price_float = strtof(price_JSON->valuestring, NULL);
    sprintf(btc_price_str, "$%.2f", btc_price_float);
    debugMsg("Bitcoin at %s\n", btc_price_str);
    cJSON_Delete(curl_JSON);
}

void *btc_thread()
{
    CURL *btc_handle;
    char *query = "https://api.coinbase.com/v2/prices/BTC-USD/spot";

#ifdef USE_LOCKS
    sem_wait(&curl_mutex);
#endif
    btc_handle = curl_easy_init();
    curl_easy_setopt(btc_handle, CURLOPT_URL, query);
    curl_easy_setopt(btc_handle, CURLOPT_WRITEFUNCTION, btc_write_callback);
    curl_easy_setopt(btc_handle, CURLOPT_NOSIGNAL, 1);

#ifdef USE_LOCKS
    sem_post(&curl_mutex);
#endif

    debugMsg("started btc thread\n");
    while (1)
    {
        btc_write_ptr = btc_in_buf;

#ifdef USE_LOCKS
        sem_wait(&curl_mutex);
        debugMsg("%d lock acquired. (btc) Calling curl_easy_perform..\n", time(NULL));
#endif

        CURLcode c = curl_easy_perform(btc_handle);
        if (c)
        {
            vc_log("curl error\n");
        }
        else
        {
            debugMsg("finished btc curl_easy_perform\n");
            *btc_write_ptr++ = 0; // terminate string for json parsing
            printf("btc_write_ptr - btc_in_buf = %u\n", ((unsigned int)btc_write_ptr - (unsigned int)btc_in_buf));
            debugMsg("btc payload: %s\n", btc_in_buf);
            if (strlen(btc_in_buf) == 0)
            {

                vc_log("empty btc payload\n");
                return (0);
            }
            debugMsg("parsing btc json\n");
            parse_btc_payload();
        }
#ifdef USE_LOCKS
        sem_post(&curl_mutex);
        debugMsg("%d: lock released(btc)\n\n", time(NULL));
#endif

        sleep(BTC_INTERVAL);
    }
}
void render_BTC_price()
{
    compileString(btc_price_str, 255, 60, MAIN_BUFFER, 2, OVERWRITE);

    btc_title_str[0] = (char)(106 + 32); // Bitcoin B
    btc_title_str[1] = 'T';
    btc_title_str[2] = 'C';
    btc_title_str[3] = (char)0;
    compileString(btc_title_str, 255, 166, MAIN_BUFFER, 3, APPEND);
}