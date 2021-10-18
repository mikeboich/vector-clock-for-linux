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

// libCurl housekeeping:
extern sem_t curl_mutex;

char *write_ptr; // for callback to write to
char weather_in_buf[4096];

//Weather housekeeping

double current_temp_f = 0.0;
double current_humidity = 0;
double current_baro = 0.0;
char *current_last_updated = "no info";
char *current_condition = "no info";

//Update weather info every this-many-seconds:
#define WEATHER_INTERVAL 300

void parse_weather_payload()
{
  int status, result;
  static cJSON *curl_JSON = NULL;
  cJSON *current_JSON, *temp_JSON, *humidity_JSON, *condition_JSON, *baro_JSON, *updated_JSON, *condition_text;

#if 1
  if (NULL != curl_JSON)
  {
    cJSON_Delete(curl_JSON); // free the previous JSON object.  Can't free at the end of this routine, due to shared objects.
  }
#endif

  curl_JSON = cJSON_Parse(weather_in_buf);
  if (curl_JSON == NULL)
  {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL)
    {
      vc_log("Error before: %s\n", error_ptr);
      fprintf(stderr, "Error before: %s\n", error_ptr);
    }
    return;
  }
  
  current_JSON = cJSON_GetObjectItemCaseSensitive(curl_JSON, "current");

  temp_JSON = cJSON_GetObjectItemCaseSensitive(current_JSON, "temp_f");
  current_temp_f = temp_JSON->valuedouble;
  debugMsg("Current temp = %f\n", current_temp_f);

  humidity_JSON = cJSON_GetObjectItemCaseSensitive(current_JSON, "humidity");
  current_humidity = humidity_JSON->valuedouble;
  debugMsg("Current humidity = %f\n", current_humidity);

  baro_JSON = cJSON_GetObjectItemCaseSensitive(current_JSON, "pressure_in");
  current_baro = baro_JSON->valuedouble;
  debugMsg("Current baro = %f\n", current_baro);

  updated_JSON = cJSON_GetObjectItemCaseSensitive(current_JSON, "last_updated");
  current_last_updated = updated_JSON->valuestring;

  condition_JSON = cJSON_GetObjectItemCaseSensitive(current_JSON, "condition");
  condition_text = cJSON_GetObjectItemCaseSensitive(condition_JSON, "text");
  current_condition = condition_text->valuestring;
  debugMsg("Current condition = %s\n", current_condition);
}

void render_current_weather(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char temperature_str[64], humidity_str[64], baro_str[64];

  sem_wait(&curl_mutex);
  sprintf(temperature_str, "Temp %.0f\x8b", current_temp_f); // \x8b is the degree symbol which I arbitrarily added to the character set at decimal 139
  compileString(temperature_str, 255, 120, MAIN_BUFFER, 2, OVERWRITE);

  sprintf(humidity_str, "Humidity %.0f%%", current_humidity); // i learned that "%%" escapes to "%" in formatted print statements :-)
  compileString(humidity_str, 255, 192, MAIN_BUFFER, 1, APPEND);

  sprintf(baro_str, "Barometer %.2f", current_baro);
  compileString(baro_str, 255, 64, MAIN_BUFFER, 1, APPEND);

  sprintf(baro_str, "Updated: %s", current_last_updated);
  compileString(current_last_updated, 255, 32, MAIN_BUFFER, 1, APPEND);

  compileString(current_condition, 255, 230, MAIN_BUFFER, 1, APPEND);

  sem_post(&curl_mutex);
}

size_t weather_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  //fprintf(stderr, "wx write_callback\n");
  debugMsg("wx write_callback\n");
  for (int i = 0; i < size * nmemb; i++)
  {
    *write_ptr++ = *ptr++;
  }
  return size * nmemb;
}

void *weather_thread(void *arg)
{
  CURL *wx_handle;
  char query[128];
  strcpy(query, "http://api.weatherapi.com/v1/current.json?key=bf763eab39764d5a975171130210709&q=");
  struct location *the_location;

  the_location = arg;
  double lat = the_location->latitude;
  double lon = -the_location->longitude;
  vc_log("starting wx thread. lat = %f, lon = %f\n", lat, lon);

  char lat_lon_str[128];
  sprintf(lat_lon_str, "%.2f,%.2f", lat, lon);
  strcat(query, lat_lon_str);
  strcat(query, "&aqi=no");

  debugMsg("wx query = %s\n", query);
#ifdef USE_LOCKS
  sem_wait(&curl_mutex);
#endif
  wx_handle = curl_easy_init();
  curl_easy_setopt(wx_handle, CURLOPT_URL, query);
  curl_easy_setopt(wx_handle, CURLOPT_WRITEFUNCTION, weather_write_callback);
  curl_easy_setopt(wx_handle, CURLOPT_NOSIGNAL, 1);
#ifdef USE_LOCKS
  sem_post(&curl_mutex);
#endif
  while (1)
  {
    write_ptr = weather_in_buf;
#ifdef USE_LOCKS
    sem_wait(&curl_mutex);
    debugMsg("%d: lock acquired (wx)  about to call curl_easy_perform\n", time(NULL));
#endif

    curl_easy_perform(wx_handle);
    debugMsg("finished curl_easy_perform\n");

    *write_ptr++ = 0; // terminate string for json parsing
    debugMsg("payload: %s\n", weather_in_buf);
    printf("wx_write_ptr - weather_in_buf = %u\n", ((unsigned int)write_ptr - (unsigned int)weather_in_buf));
    debugMsg("parsing weather json\n");
    parse_weather_payload();
#ifdef USE_LOCKS
    sem_post(&curl_mutex);
    debugMsg("%d: lock released (wx)\n\n", time(NULL));
#endif

    sleep(WEATHER_INTERVAL);
  }
}