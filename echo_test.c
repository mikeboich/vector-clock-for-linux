/*
 * OpenAMP version of Vector Clock
 *
 * Third iteration of vector clock on Zynq.  First was bare metal, which worked well, but lacked the 
 * comforts of a real OS. 
 * Next came a linux version that used a userspace driver to access the programmable logic devices.
 * The linux interrupt latency resulted in poor performance.
 * 
 * This version runs linux on processor 0, and a bare metal environment on processor 1, which truly
 * gives us the best of both worlds.
 * 
 *     MDB Dec 2018
 */
#define VERBOSE
#undef HW_TEST

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/socket.h> /* socket, connect */
#include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
#include <netdb.h>      /* struct hostent, gethostbyname */
#include <curl/curl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <pthread.h>

#include </usr/local/include/cjson/cJSON.h>

#include <sys/types.h>
//#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

//#include "ViewingLocation.h"
#include "JulianDay.h"
#include "sunrise.h"
#include "four_letter.h"
#include "weather.h"
#include "btc.h"

#include <math.h>
#include "font.h"
#include "draw.h"

#include "stdbool.h"
#include <semaphore.h>

#include "vc_log.h"

typedef enum
{
  textMode,
  flwMode,
  bubble_mode,
  pongMode,
  pendulumMode,
  analogMode1,
  secondsOnly,
  sunriseMode,
  moonriseMode,
  sunElevMode,
  moonElevMode,
  trump_elapsed_mode,
  trumpMode,
  wordClockMode,
  xmasMode,
  analogMode0,
  analogMode2,
  gpsDebugMode,
  julianDate,
  currentWeatherMode,
  menuMode
} clock_type;

int nmodes = 16;
int n_auto_modes = 5;
int switch_modes = 0;
clock_type display_mode = sunriseMode;

char *month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                       "October", "November", "December"};

char *day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// mutex for the various libcurl threads:
// (I don't fully understand the threading issues of libcurl, but with some conservative locking/unlocking, it appears to work reliably)
sem_t curl_mutex;

//rpmsg buffer structure:
struct _payload
{
  int cmd;
  int size;
  int which_buf;
  unsigned char data[];
};

static int fd; // file descriptor for writing to bare-metal processor

struct _payload *i_payload; // for messages to the bare metal remoteproc
struct _payload *r_payload; // for responses from the remoteproc

// We use a named pipe (FIFO) to get web commands
const char fifo_name[] = "/tmp/clock_fifo";
int fifo_fd = 0;

// screensaver offsets.  (All drawing is offset by these amounts, which are changed periodically):
//int ss_x_offset = 0;
//int ss_y_offset = 0;

// timers
unsigned long int microseconds()
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long)(1000000 * ts.tv_sec + ts.tv_nsec / 1000);
}

unsigned long int millis()
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long)(1000 * ts.tv_sec + ts.tv_nsec / 1000000);
}

typedef struct timer
{
  unsigned long start_time, end_time, duration;
  bool fired;
} timer;

timer animation_step_timer;

void init_timer(timer *t, unsigned long dur)
{
  t->start_time = millis();
  t->duration = dur;
  t->end_time = t->start_time + t->duration;
  t->fired = false;
}

bool check_timer(timer *t)
{
  return (millis() > t->end_time);
}

void reset_timer(timer *t)
{
  t->start_time = millis();
  t->end_time = t->start_time + t->duration;
  t->fired = false;
}
int next_fps_check = 0;

seg_or_flag fun_pattern[] = {
    {128, 128, 128, 128, lissajou0, 0x88},
    {.flag = 0xff}};
seg_or_flag menagerie_pattern[] = {
    {40, 208, 64, 64, lissajou0, 0x0ff},
    {120, 208, 64, 64, lissajou1, 0x0ff},
    {200, 208, 64, 64, lissajou2, 0x0ff},
    {40, 75, 64, 64, lissajou3, 0x0ff},
    {120, 75, 64, 64, lissajou4, 0x0ff},
    {200, 75, 64, 64, lissajou5, 0x0ff},
    {255, 255, 0, 0, cir, 0x00},
};
vector_font test_pat = {
    {128, 128, 254, 254, cir, 0xff},
    {128, 254, 8, 8, cir, 0xff},
    {254, 128, 8, 8, cir, 0xff},
    {128, 0, 8, 8, cir, 0xff},
    {0, 128, 8, 8, cir, 0xff},
    {.flag = 0xff}};

static struct location my_location = {.initialized = 0, .latitude = 0.0, .longitude = 0.0, .viewing_date = 0, .gmt_offset = 0};
//static struct location my_location = {.initialized=1, .latitude=34.0, .longitude=117.0, .viewing_date=0, .gmt_offset=0};

#define RPMSG_HEADER_LENGTH 12
#define RPMSG_MAX_DATA_LENGTH (400 - RPMSG_HEADER_LENGTH) // ** TO DO: 400 is not the real number, but 512 is too big..

#define CMD_START 0
#define CMD_ADD 1
#define CMD_DONE 2
#define CMD_READBACK 3
#define CMD_CHECK_FPS 4
#define CMD_SS_OFFSETS 5
#define CMD_CHECK_CYCLES_IN_FRAME 6
#define CMD_GET_KNOB_POSITION 7
#define CMD_GET_BUTTON 8

void check_ack(int expected, int received)
{
#ifdef VERBOSE
  if (expected != received)
    printf("\r\nError: expected %d and got %d\r\n", expected, received);
#endif
}

void get_ack(int expect_ack, int len)
{
  printf("get_ack %d\r\n", expect_ack);
  // get reply:
  int bytes_read = 0;
  do
  {

    bytes_read = read(fd, r_payload, len); // all

  } while (bytes_read <= 0);
  check_ack(expect_ack, r_payload->cmd);
}

int check_fps()
{
  int result;
  i_payload->cmd = CMD_CHECK_FPS;
  i_payload->size = 0;
  i_payload->which_buf = MAIN_BUFFER;
  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  if (bytes_written <= 0)
    printf("\r\n****** Failed to write to remote device ******\r\b");
#if 1
  // get reply:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 12); // all

  } while (bytes_read <= 0);
  check_ack(CMD_CHECK_FPS, r_payload->cmd);
#else
  get_ack(CMD_CHECK_FPS, 12);
#endif

  //printf("check_fps received %d bytes\r\n",bytes_read);
  result = r_payload->size;
  // printf("returning %d\r\n", result);
  return (result);
}

int check_cycles_in_frame()
{
  int result;
  i_payload->cmd = CMD_CHECK_CYCLES_IN_FRAME;
  i_payload->size = 0;
  i_payload->which_buf = MAIN_BUFFER;
  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  if (bytes_written <= 0)
    printf("\r\n****** Failed to write to remote device ******\r\b");
#if 1
  // get reply:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 12); // all

  } while (bytes_read <= 0);
  check_ack(CMD_CHECK_CYCLES_IN_FRAME, r_payload->cmd);
#else
  get_ack(CMD_CHECK_CYCLES_IN_FRAME, 12);
#endif

  //printf("check_cycles_in_frame received %d bytes\r\n",bytes_read);
  result = r_payload->size;
  // printf("returning %d\r\n", result);
  return (result);
}
int get_knob_position()
{
  int result;
  i_payload->cmd = CMD_GET_KNOB_POSITION;
  i_payload->size = 0;
  i_payload->which_buf = MAIN_BUFFER;
  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  if (bytes_written <= 0)
    printf("\r\n****** Failed to write to remote device ******\r\b");
#if 1
  // get reply:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 12); // all

  } while (bytes_read <= 0);
  check_ack(CMD_GET_KNOB_POSITION, r_payload->cmd);
#else
  get_ack(CMD_CHECK_CYCLES_IN_FRAME, 12);
#endif

  result = r_payload->size;
  return (result);
}
int get_button()
{
  int result;
  i_payload->cmd = CMD_GET_BUTTON;
  i_payload->size = 0;
  i_payload->which_buf = MAIN_BUFFER;
  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  if (bytes_written <= 0)
    printf("\r\n****** Failed to write to remote device ******\r\b");
#if 1
  // get reply:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 12); // all

  } while (bytes_read <= 0);
  check_ack(CMD_GET_BUTTON, r_payload->cmd);
#else
  get_ack(CMD_CHECK_CYCLES_IN_FRAME, 12);
#endif

  result = r_payload->size;
  return (result);
}

int knob_motion()
{
  int result;
  int position = get_knob_position();
  static int prev_knob_position = -1;
  if (prev_knob_position == -1)
    prev_knob_position = position; // initial case

  // treat wrap-around cases:
  if (prev_knob_position == 255 && position == 0)
  {
    result = 1;
  }
  else if (position == 255 && prev_knob_position == 0)
  {
    result = -1;
  }
  else
  {
    result = position - prev_knob_position;
  }

  prev_knob_position = position;
  //printf("knob_motion returning %d\n",result);
  return result;
}
int update_screen_saver(int x, int y)
{
  int result;
  i_payload->cmd = CMD_SS_OFFSETS;
  i_payload->size = 8;                // two ints
  i_payload->which_buf = MAIN_BUFFER; // not relevant in this case
  i_payload->data[0] = (unsigned char)x;
  i_payload->data[1] = (unsigned char)y;

  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  if (bytes_written <= 0)
    printf("\r\n****** Failed to write to remote device ******\r\b");
#if 1
  // get reply:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 4); // header-only

  } while (bytes_read <= 0);
  check_ack(CMD_SS_OFFSETS, r_payload->cmd);
#else
  get_ack(CMD_SS_OFFSETS, 4);
#endif
}

// some features need sub-second time info.
// This routine gives the fractional portion of the current second:
float fractional_second()
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (ts.tv_nsec / 1000000000.0);
}

void copy_seg_buffer(int which_buf)
{
  int bytes_read = 0;
  int data_bytes_to_send = buf_size(which_buf);
  unsigned int t1 = 0, t0 = 0, total_bytes = 0, n_buffers = 0; // for performance tracking

  // prepare first buffer
  i_payload->cmd = CMD_START;
  i_payload->size = data_bytes_to_send > RPMSG_MAX_DATA_LENGTH ? RPMSG_MAX_DATA_LENGTH : data_bytes_to_send;
  i_payload->which_buf = which_buf;
  unsigned char *src = (unsigned char *)seg_buffer[which_buf];
  unsigned char *dst = i_payload->data;

  t0 = microseconds();

  memcpy(dst, src, i_payload->size);
  src += i_payload->size;
  dst += i_payload->size;

  // send first buffer:
  //printf("sending data\r\n");
  int bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  total_bytes += bytes_written;
  n_buffers += 1;
  //printf("waiting for data ack\r\n");

#if 1
  // wait for ack:
  do
  {
    bytes_read = read(fd, r_payload, 4); // all

  } while (bytes_read <= 0);
  // confirm that the acknowlegement == the command we sent:
  check_ack(CMD_START, r_payload->cmd);
#else
  get_ack(CMD_START, 4);
#endif

  //data_bytes_to_send -= i_payload->size;
  data_bytes_to_send -= (bytes_written - RPMSG_HEADER_LENGTH);

  // send additional buffers as required:
  while (data_bytes_to_send > 0)
  {
    dst = i_payload->data;
    i_payload->size = data_bytes_to_send > RPMSG_MAX_DATA_LENGTH ? RPMSG_MAX_DATA_LENGTH : data_bytes_to_send;
    i_payload->cmd = CMD_ADD;
    i_payload->which_buf = which_buf;

    //for(int i=0;i<i_payload->size;i++){
    // *dst++ = *src++;
    //}
    memcpy(dst, src, i_payload->size);
    src += i_payload->size;
    dst += i_payload->size;

    bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
    total_bytes += bytes_written;
    n_buffers += 1;
    // wait for ack:
    do
    {
      bytes_read = read(fd, r_payload, 4); // all
    } while (bytes_read <= 0);
    check_ack(CMD_ADD, r_payload->cmd);

    data_bytes_to_send -= i_payload->size;
  }

  // send a "done" cmd
  //printf("sending done\r\n");
  i_payload->cmd = CMD_DONE;
  i_payload->size = 0;
  i_payload->which_buf = which_buf;
  bytes_written = write(fd, i_payload, i_payload->size + RPMSG_HEADER_LENGTH);
  total_bytes += bytes_written;
  n_buffers += 1;

  // wait for ack:
  //printf("awaiting DONE ack\r\n");
  do
  {
    int bytes_read = read(fd, r_payload, 4); // all
  } while (bytes_read <= 0);
  check_ack(CMD_DONE, r_payload->cmd);
  t1 = microseconds();
  //if (microseconds() > next_fps_check)
  if (0)
    printf("copy_seg_buffer (%u bytes/%u buffers) took %u microseconds\r\n", total_bytes, n_buffers, t1 - t0);
}

void dump512(unsigned char *char_ptr)
{
  for (int row = 0; row < 16; row++)
  {
    for (int col = 0; col < 32; col++)
      printf(" %x,", char_ptr[7 * row + col]);
    printf("\r\n");
  }
}
// debugging - read the buffer back and compare with local buffer:
void read_back()
{
  printf("\r\nEntering read_back\r\n");
  i_payload->cmd = CMD_READBACK;
  i_payload->size = 0;
  i_payload->which_buf = 0;

  // send command:
  int bytes_written = write(fd, i_payload, RPMSG_HEADER_LENGTH);
  // printf("wrote %d bytes (readback)\r\n",bytes_written);

  // wait for ack:
  int bytes_read;
  do
  {
    bytes_read = read(fd, r_payload, 512); // all

  } while (bytes_read <= 0);
  unsigned char *char_ptr = (unsigned char *)r_payload;
  printf("%d bytes of remote buffer received:\r\n", bytes_read);
  dump512(char_ptr);

  printf("local buffer:\r\n");
  char_ptr = (unsigned char *)seg_buffer[MAIN_BUFFER];
  dump512(char_ptr);
  printf("\r\nExiting read_back\r\n");
}

int sync_window()
{
  struct timespec ts;
  const int tick = 1000000000 / 60;

  clock_gettime(CLOCK_REALTIME, &ts);
  int ns = ts.tv_nsec;
  int delta = ns % tick;
  //return (delta < 1000000 || delta > (tick - 1000000)) ? 1 : 0;
  return (1);
}

void render_ip_address()
{
  int fd;
  struct ifreq ifr;
  char result_str[128];
  fd = socket(AF_INET, SOCK_DGRAM, 0);

  /* I want to get an IPv4 IP address */

  ifr.ifr_addr.sa_family = AF_INET;

  /* I want IP address attached to "wlan0" */
  strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);

  ioctl(fd, SIOCGIFADDR, &ifr);
  close(fd);

  /* display result */
  //printf( inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
  sprintf(result_str, "%s", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
  compileString(result_str, 255, 200, MAIN_BUFFER, 1, OVERWRITE);

  /* display lat/long: */

  sprintf(result_str, "lat: %3.2f", my_location.latitude);
  compileString(result_str, 255, 160, MAIN_BUFFER, 1, APPEND);

  sprintf(result_str, "lon: %3.2f", my_location.longitude);
  compileString(result_str, 255, 120, MAIN_BUFFER, 1, APPEND);

  sprintf(result_str, "GMT Offset: %d", my_location.gmt_offset / 3600);
  compileString(result_str, 255, 80, MAIN_BUFFER, 1, APPEND);
}

void error(const char *msg)
{
  perror(msg);
  exit(0);
}

void render_liss_level(int level)
{
  int row, col;
  seg_or_flag *l_seg;
  static seg_or_flag segs[128];
  int seg_index = 0;

  int spacing = (256 / (level + 1));
  int radius = spacing / 2;

  for (row = 1; row <= level; row++)
  {
    for (col = 1; col <= level; col++)
    {
      segs[seg_index].seg_data.x_offset = col * spacing;
      segs[seg_index].seg_data.y_offset = row * spacing;
      segs[seg_index].seg_data.x_size = 2 * radius;
      segs[seg_index].seg_data.y_size = 2 * radius;
      segs[seg_index].seg_data.arc_type = (row + col) % 2 ? lissajou0 : lissajou4;
      //segs[seg_index].seg_data.arc_type =  lissajou1;
      segs[seg_index].seg_data.mask = 0xff;
      seg_index++;
    }
  }
  segs[seg_index].flag = 255;
  compileSegments(segs, MAIN_BUFFER, OVERWRITE);
}

void render_lissajou_buffer(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  /*seg_or_flag fun_pattern[] = {
    {40,200,60, 60,lissajou0,0xff},
    {120,200,60,60,lissajou1,0xaa},
    {200,200,60,60,lissajou2,0xff},
    
    {40,120,60,60,lissajou1,0xff},
    {120,120,60,60,lissajou0,0xff},
    {200,120,60,60,lissajou1,0xff},

    {40,40,60,60,lissajou2,0xff},
    {120,40,60,60,lissajou1,0xff},
    {200,24,60,60,lissajou0,0xff},

    {255,02,03,04,cir,0x0CA}
  };
  compileSegments(fun_pattern,MAIN_BUFFER,OVERWRITE);  */
  render_liss_level(((now / 6) % 4) + 1);
}
void render_menagerie(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{

  compileSegments(menagerie_pattern, MAIN_BUFFER, OVERWRITE);
}

void countdown_to_event(time_t now, time_t event_time, char *caption0, char *caption1)
{
  double seconds_remaining;
  double days_remaining;
  char seconds_string[64] = "";
  seconds_remaining = difftime(event_time, now);
  days_remaining = fabs(seconds_remaining / 86400.0);

  sprintf(seconds_string, "%.f", fabs(seconds_remaining));

  compileString(seconds_string, 255, 140, MAIN_BUFFER, 2, OVERWRITE);
  compileString(caption0, 255, 90, MAIN_BUFFER, 1, APPEND);
  compileString(caption1, 255, 40, MAIN_BUFFER, 1, APPEND);
}

void render_trump_elapsed_buffer(time_t now)
{
  time_t end_time, start_time;
  struct tm end_of_trump, start_of_trump;
  end_of_trump.tm_year = 2021 - 1900;
  start_of_trump.tm_year = 2017 - 1900;
  start_of_trump.tm_mon = end_of_trump.tm_mon = 1 - 1; // months are 0.11 rather than 1.12!
  start_of_trump.tm_mday = end_of_trump.tm_mday = 20;
  start_of_trump.tm_hour = end_of_trump.tm_hour = 17 + my_location.gmt_offset / 3600; // local time corresponding to 1700UTC
  start_of_trump.tm_min = end_of_trump.tm_min = 0;
  start_of_trump.tm_sec = end_of_trump.tm_sec = 0;

  end_time = mktime(&end_of_trump);
  start_time = mktime(&start_of_trump);

  countdown_to_event(now, start_time, "Seconds of Trump", "elapsed");
}

void render_trump_buffer(time_t now)
{
  time_t end_time, start_time;
  struct tm end_of_trump, start_of_trump;
  start_of_trump.tm_year = end_of_trump.tm_year = 2021 - 1900;
  start_of_trump.tm_mon = end_of_trump.tm_mon = 1 - 1; // months are 0.11 rather than 1.12!
  start_of_trump.tm_mday = end_of_trump.tm_mday = 20;
  start_of_trump.tm_hour = end_of_trump.tm_hour = 17 + my_location.gmt_offset / 3600; // local time corresponding to 1700UTC
  start_of_trump.tm_min = end_of_trump.tm_min = 0;
  start_of_trump.tm_sec = end_of_trump.tm_sec = 0;

  end_time = mktime(&end_of_trump);
  start_time = mktime(&start_of_trump);

  countdown_to_event(now, end_time, "Seconds of Trump", "remaining");
}

void render_BTC_buffer(time_t now)
{
}

/*  Pendulum Clock *** */
void render_pendulum_buffer(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char sec_str[32], hr_min_string[32];
  double x, y, i;
  double f;

  const int pendulum_length = 180;
  const int origin_x = 128;
  const int origin_y = 230;

  f = fractional_second();

  // render the time in seconds
  sprintf(sec_str, "%02i", local_bdt->tm_sec);
  compileString(sec_str, 255, 32, MAIN_BUFFER, 2, OVERWRITE);

  // render the hour and minute:
  sprintf(hr_min_string, "%02i:%02i", local_bdt->tm_hour, local_bdt->tm_min);
  compileString(hr_min_string, 255, 115, MAIN_BUFFER, 3, APPEND);

  // render the pendulum shaft:
  x = origin_x + pendulum_length * sin(sin(2 * M_PI * (f)) / 2.5);
  y = origin_y - pendulum_length * cos(sin(2 * M_PI * (f)) / 2.5);
  line(origin_x, origin_y, x, y, MAIN_BUFFER);

  //render the pendulum bob:
  for (i = 32; i > 0; i -= 8)
    circle(x, y, i, MAIN_BUFFER);

  //render the point from which the pendulum swings:
  circle(origin_x, origin_y, 8, MAIN_BUFFER);
}

void render_characters_buffer(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char kanji[] = {97 + 32, 98 + 32, 105 + 32, 100 + 32, 101 + 32, 0};
  //compileString("abc",255,128,MAIN_BUFFER,1,OVERWRITE);
  compileString("abcdefjhijklmnopqrstuvwxyz", 255, 128, MAIN_BUFFER, 1, OVERWRITE);
  compileString("!@#$%^&*(){}[]|\\", 255, 160, MAIN_BUFFER, 1, APPEND);
  // compileString("1234567890",255,96,MAIN_BUFFER,1,APPEND);
  compileString(kanji, 255, 32, MAIN_BUFFER, 2, APPEND);
}

#define HR_HAND_WIDTH 8
#define HR_HAND_LENGTH 54
#define MIN_HAND_WIDTH 4
#define MIN_HAND_LENGTH 90
#define SEC_HAND_LENGTH 108

void drawClockHands(int h, int m, int s)
{
  if (h > 11)
    h -= 12;                                                                 // hours > 12 folded into 0-11
  float hour_angle = (h / 12.0) * M_PI * 2.0 + (m / 60.0) * (M_PI / 6.0);    // hour hand angle (we'll ignore the seconds)
  float minute_angle = (m / 60.0) * M_PI * 2.0 + (s / 60.0) * (M_PI / 30.0); // minute hand angle
  float second_angle = (((s /* + fractional_second()*/) / 60.0)) * M_PI * 2.0;

  /*
    if(1){
    float smooth_angle = 2*M_PI* (((cycle_count - minute_error)/(60*31250.0)));
    second_angle = smooth_angle;
    }
  */

  // not doing the 2-d hands yet, just lines
  line(128, 128, 128 + sin(hour_angle) * HR_HAND_LENGTH, 128 + cos(hour_angle) * HR_HAND_LENGTH, MAIN_BUFFER); // draw the hour hand
  line(128, 128, 128 + sin(minute_angle) * MIN_HAND_LENGTH, 128 + cos(minute_angle) * MIN_HAND_LENGTH, MAIN_BUFFER);
  if (1)
  {
    line(128, 128, 128 + sin(second_angle) * SEC_HAND_LENGTH, 128 + cos(second_angle) * SEC_HAND_LENGTH, MAIN_BUFFER);
  }
}

void renderAnalogClockBuffer(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  int i;
  float angle = 0.0;
  static char *nums[12] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
  seg_or_flag face[] = {{128, 128, 254, 254, cir, 0xff},
                        {128, 128, 8, 8, cir, 0xff},
                        // {0,0,255,0,pos,0xff},
                        {.flag = 0xff}};
  compileSegments(face, MAIN_BUFFER, OVERWRITE);
  compileString("12", 112, 216, MAIN_BUFFER, 1, APPEND);
  compileString("6", 120, 20, MAIN_BUFFER, 1, APPEND);
  compileString("3", 220, 120, MAIN_BUFFER, 1, APPEND);
  compileString("9", 20, 120, MAIN_BUFFER, 1, APPEND);

  drawClockHands(local_bdt->tm_hour, local_bdt->tm_min, local_bdt->tm_sec);

  if (0)
  {
    // experimental one revoultion/second widget:
    float x = 128.0 + (124) * sin(2 * M_PI * fractional_second());
    float y = 128.0 + (124) * cos(2 * M_PI * fractional_second());
    circle(x, y, 16, MAIN_BUFFER);
  }
}

void render_word_clock(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char *strs[] = {"o'clock", "b"};
  char *hour_strings[] = {"twelve", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven"};
  char *minute_strings[] = {"not-used", "five", "ten", "a quarter", "twenty", "twenty-five"};
  char past_or_until[8];
  int exact = 0;

  char time_string[3][64];

  if (local_bdt->tm_min > 57 || local_bdt->tm_min < 3)
  {
    if (local_bdt->tm_min == 0)
      sprintf(time_string[0], "It's exactly");
    else
      sprintf(time_string[0], "It's about");
    compileString(time_string[0], 255, 160, MAIN_BUFFER, 2, OVERWRITE);
    int the_hour = local_bdt->tm_min > 56 ? local_bdt->tm_hour + 1 : local_bdt->tm_hour;
    sprintf(time_string[0], "%s ", hour_strings[the_hour % 12]);
    compileString(time_string[0], 255, 108, MAIN_BUFFER, 2, APPEND);
    sprintf(time_string[0], "O'clock");
    compileString(time_string[0], 255, 50, MAIN_BUFFER, 2, APPEND);
    return;
  }

  if (local_bdt->tm_min >= 3 && local_bdt->tm_min <= 57)
  {
    if (local_bdt->tm_min > 27 && local_bdt->tm_min < 33)
    {
      if (local_bdt->tm_min == 30)
        compileString("It's exactly", 255, 150, MAIN_BUFFER, 2, OVERWRITE);
      else
        compileString("It's about", 255, 150, MAIN_BUFFER, 2, OVERWRITE);

      compileString("half past", 255, 100, MAIN_BUFFER, 2, APPEND);
      sprintf(time_string[0], "%s", hour_strings[local_bdt->tm_hour % 12]);
      compileString(time_string[0], 255, 50, MAIN_BUFFER, 2, APPEND);
      return;
    }
    else
    {
      // round to nearest 5 minutes:
      int approx_minute = 5 * (local_bdt->tm_min / 5);
      int past_until_index;
      if ((local_bdt->tm_min - approx_minute) > 2)
      {
        approx_minute += 5;
      }
      exact = (approx_minute == local_bdt->tm_min);

      if (exact)
        compileString("It's exactly", 255, 200, MAIN_BUFFER, 2, OVERWRITE);
      else
        compileString("It's about", 255, 200, MAIN_BUFFER, 2, OVERWRITE);
      if (local_bdt->tm_min <= 27)
      {
        past_until_index = approx_minute / 5;
        sprintf(time_string[0], "%s", minute_strings[past_until_index]);
        compileString(time_string[0], 255, 150, MAIN_BUFFER, 2, APPEND);
        sprintf(time_string[0], "past");
        compileString(time_string[0], 255, 100, MAIN_BUFFER, 2, APPEND);
        sprintf(time_string[0], "%s", hour_strings[local_bdt->tm_hour % 12]);
        compileString(time_string[0], 255, 50, MAIN_BUFFER, 2, APPEND);
      }
      if (local_bdt->tm_min >= 33)
      {
        approx_minute = 60 - approx_minute;
        past_until_index = approx_minute / 5;
        sprintf(time_string[0], "%s", minute_strings[(approx_minute / 5)]);
        compileString(time_string[0], 255, 150, MAIN_BUFFER, 2, APPEND);
        sprintf(time_string[0], "'till");
        compileString(time_string[0], 255, 100, MAIN_BUFFER, 2, APPEND);
        sprintf(time_string[0], "%s", hour_strings[((local_bdt->tm_hour + 1)) % 12]);
        compileString(time_string[0], 255, 50, MAIN_BUFFER, 2, APPEND);
      }
      /*           if(now->Min >= 28 && now->Min<=32){
                compileString("It's about",255,200,MAIN_BUFFER,2,OVERWRITE);
                past_until_index = approx_minute / 5;
                sprintf(time_string[0],"half");
                compileString(time_string[0],255,150,MAIN_BUFFER,2,APPEND);
                sprintf(time_string[0],"past");
                compileString(time_string[0],255,100,MAIN_BUFFER,2,APPEND);
                sprintf(time_string[0],"%s",hour_strings[now->Hour % 12]);
                compileString(time_string[0],255,50,MAIN_BUFFER,2,APPEND);
            }
*/
    }
  }
}

/* ************* Pong Game ************* */
#define PADDLE_HEIGHT 24
#define PADDLE_WIDTH 8
#define PONG_TOP 250
#define PONG_BOTTOM 4
#define PONG_LEFT PADDLE_WIDTH
#define PONG_RIGHT 255 - PADDLE_WIDTH
#define PADDLE_MIN PONG_BOTTOM + (PADDLE_HEIGHT / 2)
#define PADDLE_MAX PONG_TOP - (PADDLE_HEIGHT / 2)
#define PADDLE_STEP 4
#define MAX_Y_VELOCITY 9

// global to allow manual play vs ai:
int manual_pong = 0;
int paddle_input = 0;

typedef struct
{
  unsigned int celebrating; //  zero for normal mode, end-of-celebration time if nonzero
  int paddle_position[2];
  int puck_velocity[2];
  int puck_position[2];
  int score[2];
} pong_state;

int pong_hour, pong_minute, pong_second;

pong_state game_state = {
    .celebrating = 0,
    .paddle_position = {96, 140},
    .puck_velocity = {4, 0},
    .puck_position = {128, 128},
    .score = {0, 0}};

#define CELEB_DURATION 40000 // flash some decoration on screen for just over 1 second after a score

void start_celebration()
{
  game_state.celebrating = (unsigned int)time(NULL) + 1;
}

void end_celebration()
{
  game_state.celebrating = 0;
}

//returns which edge puck has struck, or zero otherwise:
// left = 1, right = 2, top = 3, bottom = 4
int puck_at_edge()
{
  if (game_state.puck_position[0] <= PONG_LEFT)
  {
    return (1);
    // doesn't belong here: start_celebration();
  }
  if (game_state.puck_position[0] >= PONG_RIGHT)
  {
    return (2);
    // doesn't belong here: start_celebration();
  }
  if (game_state.puck_position[1] <= PONG_BOTTOM)
    return (3);
  if (game_state.puck_position[1] >= PONG_TOP)
    return (4);

  return (0);
}

int puck_dest()
{
  float delta_x = game_state.puck_velocity[0] < 0 ? game_state.puck_position[0] - PONG_LEFT : PONG_RIGHT - game_state.puck_position[0];
  float delta_t = fabs(delta_x / game_state.puck_velocity[0]); //this many ticks to reach  edge
  float y_intercept = game_state.puck_position[1] + delta_t * game_state.puck_velocity[1];
  while (y_intercept < PONG_BOTTOM || y_intercept > PONG_TOP)
  {
    if (y_intercept < PONG_BOTTOM)
      y_intercept = 2 * PONG_BOTTOM - y_intercept;
    if (y_intercept > PONG_TOP)
      y_intercept = 2 * PONG_TOP - y_intercept;
  }
  return (int)y_intercept;
}

int miss_zone()
{ // find a paddle position that allows us to intentionally miss
  int dst = puck_dest();
  int result;
  if (dst <= PADDLE_HEIGHT)
  { // when they go low, we go high
    result = 2 * PADDLE_HEIGHT + 4;
  }
  else if (dst > PONG_TOP - PADDLE_HEIGHT - 2)
  { // when they go high, we go low
    result = PONG_TOP - 2 * PADDLE_HEIGHT - 4;
  }
  else
  {
    result = dst + PADDLE_HEIGHT + 4; // just go high when there's room
  }
  return result;
}

void constrain(int *x, int xmin, int xmax)
{
  if (*x > xmax)
    *x = xmax;
  if (*x < xmin)
    *x = xmin;
}

#define min(x, y) x < y ? x : y

void update_paddles(int target_offset)
{
  int player;
  int should_miss[2]; //set to 1 if we want that player to miss
  int y_target;

  should_miss[1] = ((pong_minute == 59 && pong_second > 57)) ? 1 : 0;
  should_miss[0] = ((pong_second > 57) && (pong_minute != 59)) ? 1 : 0;

  player = game_state.puck_velocity[0] < 0 ? 0 : 1;
  if (should_miss[player])
  {
    y_target = miss_zone();
  }
  else
  {
    y_target = puck_dest() - target_offset;
  }
  int y_error = abs(y_target - game_state.paddle_position[player]);

  if (manual_pong == 0 || player != 0)
  {
    if (game_state.paddle_position[player] < y_target)
      game_state.paddle_position[player] += min(y_error, PADDLE_STEP);
    else
      game_state.paddle_position[player] -= min(y_error, PADDLE_STEP);
  }
  if (manual_pong)
  { //manual case for player 0:
    game_state.paddle_position[0] += paddle_input;
    paddle_input = 0;
  }

  constrain(&game_state.paddle_position[player], PADDLE_MIN, PADDLE_MAX);
}

// returns the new y velocity for the puck if it hit a paddle, and 0 otherwise
int puck_hit_paddle(int *new_velocity)
{
  int which_paddle;
  int did_hit = 0;
  int offset;

  if (game_state.puck_velocity[0] < 0 && (game_state.puck_position[0] - PONG_LEFT) <= (-game_state.puck_velocity[0]))
    which_paddle = 0;
  else if (game_state.puck_velocity[0] > 0 && ((PONG_RIGHT)-game_state.puck_position[0]) <= (game_state.puck_velocity[0]))
    which_paddle = 1;
  else
    return 0;

  offset = puck_dest() - game_state.paddle_position[which_paddle];

  if (abs(offset) > PADDLE_HEIGHT / 2)
    return 0; // we missed

  *new_velocity = game_state.puck_velocity[1] + offset; // hitting off center of paddle imparts english
  constrain(new_velocity, -MAX_Y_VELOCITY, MAX_Y_VELOCITY);
  return 1;
}

void pong_update()
{
  int dim;
  static int target_offset = 0;
  if (!game_state.celebrating)
  {
    for (dim = 0; dim < 2; dim++)
    {
      game_state.puck_position[dim] += game_state.puck_velocity[dim]; // move the puck
    }
    constrain(&game_state.puck_position[0], PONG_LEFT, PONG_RIGHT);
    constrain(&game_state.puck_position[1], PONG_BOTTOM, PONG_TOP);

    update_paddles(target_offset);
    int new_y_velocity;
    int hit_paddle = puck_hit_paddle(&new_y_velocity);

    if (hit_paddle)
    {
      game_state.puck_velocity[1] = new_y_velocity;
      game_state.puck_velocity[0] = -game_state.puck_velocity[0];
      target_offset = (rand() % 7) - 3;
    }
    int which_edge = puck_at_edge();
    if (which_edge && !hit_paddle)
    {
      if ((which_edge == 1 || which_edge == 2) && !hit_paddle)
      {
        // puck is  exiting the playing area
        start_celebration();
        game_state.puck_velocity[0] = -game_state.puck_velocity[0];
        game_state.puck_velocity[1] = 0;
      }
      if (which_edge == 3 || which_edge == 4)
      { // hit top or bottom edge. reverse y velocity:
        game_state.puck_velocity[1] = -game_state.puck_velocity[1];
      }
    }
  }
  else
  {
    if ((unsigned int)time(NULL) > game_state.celebrating)
      end_celebration();
  }
}

void draw_paddles(pong_state the_state)
{
  int y;
  // draw the left paddle
  for (y = the_state.paddle_position[0] - (PADDLE_HEIGHT / 2); y < the_state.paddle_position[0] + (PADDLE_HEIGHT / 2) + 1; y++)
    line(0, y, PADDLE_WIDTH, y, MAIN_BUFFER);
  // draw the right paddle:
  for (y = the_state.paddle_position[1] - (PADDLE_HEIGHT / 2); y < the_state.paddle_position[1] + (PADDLE_HEIGHT / 2) + 1; y++)
    line(255 - PADDLE_WIDTH, y, 255, y, MAIN_BUFFER);
}

void draw_puck(pong_state the_state)
{
  int x, y;
  x = the_state.puck_position[0];
  for (y = the_state.puck_position[1] - 2; y < the_state.puck_position[1] + 3; y++)
    line(x - 2, y, x + 2, y, MAIN_BUFFER);
}
void draw_celeb(pong_state the_state)
{
  int x, y, r;
  x = the_state.puck_position[0];
  y = the_state.puck_position[1];
  for (r = 2; r < 32; r += 8)
  {
    circle(x, y, r, MAIN_BUFFER);
  }
}

void draw_center_line(pong_state the_state)
{
  int x, y;
  x = 128;
  for (y = PONG_TOP; y > 0; y -= 32)
  {
    line(128, y, 128, y - 16, MAIN_BUFFER);
  }
}

void draw_scores(pong_state the_state, struct tm *local_bdt)
{ // draw the hours and minutes as two scores:
  char time_str[32];
  int the_hour = local_bdt->tm_hour;
  int the_minute = local_bdt->tm_min;
  sprintf(time_str, "%02i", the_hour);
  compileString(time_str, 36, 200, MAIN_BUFFER, 2, APPEND);

  sprintf(time_str, "%02i", the_minute);
  compileString(time_str, 160, 200, MAIN_BUFFER, 2, APPEND);
}
void draw_tick(int seconds)
{ // draw a seconds tick
  float second_angle = ((seconds / 60.0)) * M_PI * 2.0;
  float x0 = 128 + 60 * sin(second_angle);
  float y0 = 128 + 60 * cos(second_angle);
  x0 = 128;
  y0 = 128;
  float x1 = 128 + 64 * sin(second_angle);
  float y1 = 132 + 64 * cos(second_angle);
  circle(128, 132, 8, MAIN_BUFFER);
  circle(128, 132, 80, MAIN_BUFFER);

  line(x0, y0, x1, y1, MAIN_BUFFER);
}
void render_pong_buffer(pong_state the_state, time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  int x, y;

  pong_hour = local_bdt->tm_hour;
  pong_minute = local_bdt->tm_min;
  pong_second = local_bdt->tm_sec;
  clear_buffer(MAIN_BUFFER);
  //draw_tick(local_bdt->tm_sec);
  draw_paddles(the_state);
  if (!the_state.celebrating)
    draw_puck(the_state);
  draw_center_line(the_state);
  draw_scores(the_state, local_bdt);

  if (the_state.celebrating && microseconds() % 200000 > 100000)
    draw_celeb(the_state);

}

void render_text_clock(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char time_string[32];
  char day_of_week_string[12];
  char date_string[15];

  int seconds = local_bdt->tm_sec;
  int minutes = local_bdt->tm_min;
  int hours = local_bdt->tm_hour;

  int day_of_week = local_bdt->tm_wday;
  int month = local_bdt->tm_mon;
  int day_of_month = local_bdt->tm_mday;
  int year = local_bdt->tm_year + 1900;

  sprintf(time_string, "%i:%02i:%02i", hours, minutes, seconds);
  compileString(time_string, 255, 46, MAIN_BUFFER, 3, OVERWRITE);

  sprintf(date_string, "%s %i, %i", month_names[month], day_of_month, year);

  compileString(date_string, 255, 142, MAIN_BUFFER, 1, APPEND);

  char dw[12];
  sprintf(dw, "%s", day_names[day_of_week]);
  compileString(dw, 255, 202, MAIN_BUFFER, 2, APPEND);
}

double julian_date(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  double julian_day = (now / 86400.0) + 2440587.5;
  return julian_day; //  - 2400000.5;
}
// renders the Julian date
void render_julian_date(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  double jd = julian_date(now, local_bdt, utc_bdt);
  char jd_str[32];

  sprintf(jd_str, "Julian Date:");
  compileString(jd_str, 255, 128 + 32, MAIN_BUFFER, 1, OVERWRITE);
  sprintf(jd_str, "%.5lf", jd);
  compileString(jd_str, 255, 128 - 32, MAIN_BUFFER, 1, APPEND);
}

int inBounds(float x, float lower, float upper)
{
  if (x <= upper && x >= lower)
    return 1;
  else
    return 0;
}

// Show a four letter word:
void render_flw(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  char *rw;
  static int lastUpdate = 0;

  if (local_bdt->tm_sec - lastUpdate != 0)
  { // one second update interval.
    rw = random_word();
    //rw = next_word();  // uncomment this line to have sequential, rather than random words
    compileString(rw, 255, 88, MAIN_BUFFER, 5, OVERWRITE);
    lastUpdate = local_bdt->tm_sec;
  }
}

#define SUN_SIZE 64

// Sun elevation diagram, as inspired by SGITeach:
#define LEFT_MARGIN 8
#define RIGHT_MARGIN 248

void renderSunOrMoonElev(time_t now, struct tm *local_bdt, struct tm *utc_bdt, int zeroForSunOneForMoon)
{
  int x, y;
  static seg_or_flag axes[] = {
      {128, 120, 0, 252, legacy_neg, 0x0ff}, // y-axis creates a line from 128,8 to 128,248
      {128, 8, 240, 0, legacy_neg, 0x0ff},   // x-axis creates a line from 8,8 to 248,8
      {255, 255, 0, 0, cir, 0x00},
  };
  // time_t today = midnightInTimeZone(now,global_prefs.prefs_data.utc_offset);
  time_t today = midnightInTimeZone(now, my_location.gmt_offset);
  static time_t last_calcs = 0;
  time_t t;

  static int time_to_y[24][2];
  static int y_at_rise[2], y_at_set[2], x_at_rise[2], x_at_set[2];
  static double rise_elev, set_elev;
  static time_t rise_time[2], set_time[2];

  // render axes:
  clear_buffer(MAIN_BUFFER);
  line(128, 8, 128, 248, MAIN_BUFFER);
  line(8, 8, 248, 8, MAIN_BUFFER);
  // compileSegments(axes,MAIN_BUFFER,OVERWRITE);
  //line(0,8,255,8,MAIN_BUFFER);   // horiz axis

  for (x = 8; x <= 240; x += 10)
  { // horiz axis tick marks
    line(x, 0, x, 16, MAIN_BUFFER);
  }

  //line(128,0,128,240,MAIN_BUFFER);
  for (y = 18; y <= 248; y += 26)
  { // vertical axis tick marks
    line(120, y + 8, 136, y + 8, MAIN_BUFFER);
  }

  // draw a dotted line denoting the current time:
  float day_fraction = local_bdt->tm_hour / 24.0 + local_bdt->tm_min / 1440.0;
  int x_offset = LEFT_MARGIN + (day_fraction) * (RIGHT_MARGIN - LEFT_MARGIN);
  vertical_dashed_line(x_offset, 0, x_offset, 180, MAIN_BUFFER);

  // calc elevations if necessary:
  double elev;

  if (!my_location.initialized)
    init_location(&my_location);

  int i;
  if (today != last_calcs)
  {
    for (i = 0; i < 2; i++)
    {
      rise_time[i] = calcSunOrMoonRiseForDate(today, 1, i + 1, my_location);
      if (i == 0)
        calcSolarAzimuth(NULL, &rise_elev, NULL, NULL, rise_time[i], my_location);
      else
        calcLunarAzimuth(NULL, &rise_elev, NULL, NULL, NULL, rise_time[i], my_location);
      y_at_rise[i] = 2.6 * rise_elev;
      x_at_rise[i] = 8 + (rise_time[i] - today) / 360;
      //rise_time[i] += global_prefs.prefs_data.utc_offset*3600;
      rise_time[i] += my_location.gmt_offset;
      set_time[i] = calcSunOrMoonRiseForDate(today, 2, i + 1, my_location);
      if (i == 0)
        calcSolarAzimuth(NULL, &set_elev, NULL, NULL, set_time[i], my_location);
      else
        calcLunarAzimuth(NULL, &set_elev, NULL, NULL, NULL, set_time[i], my_location);

      y_at_set[i] = 2.6 * set_elev;
      x_at_set[i] = 8 + (set_time[i] - today) / 360;
      //set_time[i] += global_prefs.prefs_data.utc_offset*3600;
      set_time[i] += my_location.gmt_offset;

      int index = 0;
      for (t = today; t < today + 86400; t += 3600)
      {
        if (i == 0)
          calcSolarAzimuth(NULL, &elev, NULL, NULL, t, my_location);
        else
          calcLunarAzimuth(NULL, &elev, NULL, NULL, NULL, t, my_location);
        time_to_y[index][i] = 2.6 * elev;
        index += 1;
      }
      last_calcs = today;
    }
  }
  else
  { // calcs have already been done for today.  Just use the cached values:
    int hour;
    struct tm bdt;
    char event_str[64];

    if (zeroForSunOneForMoon == 0)
    {
      compileString("Sunrise", 16, 220, MAIN_BUFFER, 1, APPEND);
      compileString("Sunset", 154, 220, MAIN_BUFFER, 1, APPEND);
    }
    else
    {
      compileString("Moonrise", 16, 220, MAIN_BUFFER, 1, APPEND);
      compileString("Moonset", 154, 220, MAIN_BUFFER, 1, APPEND);
    }

    bdt = *gmtime(&rise_time[zeroForSunOneForMoon]);
    strftime(event_str, sizeof(event_str), "%l:%M %p", &bdt);
    compileString(event_str, 0, 190, MAIN_BUFFER, 1, APPEND);
    bdt = *gmtime(&set_time[zeroForSunOneForMoon]);
    strftime(event_str, sizeof(event_str), "%l:%M %p", &bdt);
    compileString(event_str, 138, 190, MAIN_BUFFER, 1, APPEND);

    for (hour = 0; hour < 24; hour++)
    {
      y = time_to_y[hour][zeroForSunOneForMoon];
      if (y > 0)
      {
        circle(10 * hour + 8, y + 8, 8, MAIN_BUFFER);
      }
    }
    circle(x_at_rise[zeroForSunOneForMoon], y_at_rise[zeroForSunOneForMoon] + 8, 8, MAIN_BUFFER);
    circle(x_at_set[zeroForSunOneForMoon], y_at_set[zeroForSunOneForMoon] + 8, 8, MAIN_BUFFER);
  }
}

// Sun elevation diagram, as inspired by SGITeach:
void renderSunElev(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  renderSunOrMoonElev(now, local_bdt, utc_bdt, 0);
}

// Moon elevation diagram, as inspired by SGITeach:
void renderMoonElev(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  renderSunOrMoonElev(now, local_bdt, utc_bdt, 1);
}

// *** temporary hack until we replace cycle_count with proper time info:
unsigned long int cycle_count()
{
  time_t now;
  now = time(NULL);
  //return (unsigned long int)now * 31250;
  return (microseconds() / 32);
}

// This (pretty ugly) routine serves for both sunset/sunrise and moonset/moonrise
void renderSR2(time_t now, struct tm *local_bdt, struct tm *utc_bdt)
{
  static time_t date_for_calcs = 0;
  static time_t sunrise_time, sunset_time, moonrise_time, moonset_time;
  static double moon_fullness = 0.0;
  char fullness_str[255];
  char event_str[64];
  struct tm bdt;
  struct location my_location; // this will move to prefs and/or we'll get it from GPS

  static seg_or_flag sun[] = {
      {128, 0, SUN_SIZE, SUN_SIZE, cir, 0x0ff},
      {255, 255, 0, 0, cir, 0x00},
  };

  static seg_or_flag moon[] = {
      {128, 0, 127, 127, cir, 0xff},
      {144, 0, 38, 42, cir, 0xff},
      {106, 10, 12, 14, cir, 0xff},
      {114, 26, 14, 12, cir, 0xff},
      {140, 38, 24, 20, cir, 0xff},
      {255, 255, 0, 0, cir, 0x00},
  };

  static unsigned long int next_animation_time = 0; // allows us to keep tracj and calc rises and sets once/day
  static int sun_y = 0;
  //const int animation_period = 1024;
  int oneForSun = (display_mode == sunriseMode) ? 1 : 2;

  static int animation_step = 1;
  int animation_start = 0;
  int animation_stop = 80;

  // advance the animation if it's time:
  if (check_timer(&animation_step_timer))
  {
    offsetSegments(sun, 0, animation_step);
    offsetSegments(moon, 0, animation_step);
    reset_timer(&animation_step_timer);
    sun_y += animation_step;
    if (sun_y == animation_stop)
    {
      animation_step = -1;
    }
    else if (sun_y == 0)
    {
      animation_step = 1;
    }
  }
  {
  }

  clear_buffer(MAIN_BUFFER);
  //insetSegments(sun,16,16);
  if (oneForSun == 1)
  {
    float angle;
    float outset = 0.6 * SUN_SIZE;
    float outset2 = 0.9 * SUN_SIZE;
    compileSegments(sun, MAIN_BUFFER, APPEND);
    // draw rays

    for (angle = 0.0; angle < 2 * M_PI - 0.1; angle += 2 * M_PI / 12.0)
    {
      float origin_x = 128 + outset * cos(angle);
      float origin_y = sun_y + outset * sin(angle);
      float end_x = 128 + outset2 * cos(angle);
      float end_y = sun_y + outset2 * sin(angle);

      if (inBounds(origin_x, 0.0, 255.0) &&
          inBounds(origin_y, 0.0, 255.0) &&
          inBounds(end_x, 0.0, 255.0) &&
          inBounds(end_y, 0.0, 255.0))
      {
        line(origin_x, origin_y, end_x, end_y, MAIN_BUFFER);
      }
    }
  }
  else
  { // draw moon features here:
    compileSegments(moon, MAIN_BUFFER, APPEND);
  }
  //time_t today = midnightInTimeZone(now,-8);
  time_t today = midnightInTimeZone(now, -8);

  if (today != date_for_calcs)
  {
    date_for_calcs = today;
    init_location(&my_location); // test values for now.

    sunrise_time = calcSunOrMoonRiseForDate(today, 1, 1, my_location); // sunrise UTC
    sunrise_time += my_location.gmt_offset;                            // cheesy offset to local time
    sunset_time = calcSunOrMoonRiseForDate(today, 2, 1, my_location);  // sunset UTC
    sunset_time += my_location.gmt_offset;                             // offset to local time for display

    moonrise_time = calcSunOrMoonRiseForDate(today, 1, 2, my_location); // analogous to above for moon..
    moonrise_time += my_location.gmt_offset;
    moonset_time = calcSunOrMoonRiseForDate(today, 2, 2, my_location);
    moonset_time += my_location.gmt_offset;

    // calculate fullness of moon for good measure
    calcLunarAzimuth(NULL, NULL, &moon_fullness, NULL, NULL, now, my_location);
  }

  if (animation_step == 1)
  {
    bdt = oneForSun == 1 ? *gmtime(&sunrise_time) : *gmtime(&moonrise_time);
    strftime(event_str, sizeof(event_str), "%l:%M %p", &bdt);
  }
  else
  {
    bdt = oneForSun == 1 ? *gmtime(&sunset_time) : *gmtime(&moonset_time);
    strftime(event_str, sizeof(event_str), "%l:%M %p", &bdt);
  }
  compileString(event_str, 255, 160, MAIN_BUFFER, 2, APPEND);

  if (oneForSun == 2)
  {
    sprintf(fullness_str, "%.0f%% full", 100 * moon_fullness);
    compileString(fullness_str, 255, 230, MAIN_BUFFER, 1, APPEND);
  }
}

#if 0
void render_text_clock(time_t now,struct tm *local_bdt, struct tm *utc_bdt){
  char time_string[32];
  char day_of_week_string[12];
  char date_string[15];

  int seconds = local_bdt->tm_sec;
  int minutes = local_bdt->tm_min;
  int hours = local_bdt->tm_hour;
    
  int day_of_week = local_bdt->wday;
  int month = local_bdt->tm_mon;
  int day_of_month = local_bdt->tm_mday;
  int year = local_bdt>tm_year + 1900;
        
  sprintf(time_string,"%i:%02i:%02i",hours,minutes,seconds);
  compileString(time_string,255,46,MAIN_BUFFER,3,OVERWRITE);

  sprintf(date_string,"%s %i, %i",month_names[month-1],day_of_month,year);
 
  compileString(date_string,255,142,MAIN_BUFFER,1,APPEND);
     
  char dw[12];
  sprintf(dw,"%s",day_names[day_of_week-1]);
  compileString(dw,255,202,MAIN_BUFFER,2,APPEND);
}
#endif

void render_fine_circles()
{
  int x, y;
  int radius = 8;
  clear_buffer(MAIN_BUFFER);
  for (x = 8; x < 255 - radius; x += 2 * radius)
    for (y = 8; y < 255 - radius; y += 2 * radius)
      circle(x, y, radius, MAIN_BUFFER);
}

void render_single_circle()
{
  clear_buffer(MAIN_BUFFER);
  circle(128, 128, 128, MAIN_BUFFER);
}

#define CW 'a'
#define CCW 'b'
#define BTN 'c'

char poll_fifo()
{
  char buf[255];
  int bytes_read = 0;

  bytes_read = read(fifo_fd, buf, 254);
  if (bytes_read > 0)
  {
    buf[bytes_read] = 0;
    //printf("%s\n",buf);
  }
  else
  {
    buf[0] = '*';
    buf[1] = (char)0;
  }
  return buf[0];
}

vector_font test_pat3 = {
    {128, 254, 8, 8, cir, 0xff},
    {254, 128, 8, 8, cir, 0xff},
    {128, 0, 8, 8, cir, 0xff},
    {0, 128, 8, 8, cir, 0xff},
    {128, 128, 254, 254, cir, 0xff},
    {128, 128, 96, 96, cir, 0x55},
    {128, 128, 0, 128, pos, 0xff},
    {128, 128, 128, 0, pos, 0xff},

    {.flag = 0xff}};

vector_font hw_test_pat = {
    {128, 128, 128, 128, 0x0f, 0xff},
    // {128,128,254,254,cir,0xff},

    {.flag = 0xff}};

void render_medium_circle()
{
  clear_buffer(MAIN_BUFFER);
  circle(128, 128, 128, MAIN_BUFFER);
}

void render_hw_test_pattern()
{
  compileSegments(hw_test_pat, MAIN_BUFFER, OVERWRITE);
}

int main(int argc, char *argv[])
{

  int cmd, ret;
  int opt;
  char *rpmsg_dev = "/dev/rpmsg0";
  bool no_curling = false; // don't call web services if this is true

  curl_global_init(CURL_GLOBAL_DEFAULT);

  // settings stuff:
  //init_settings();

  while ((opt = getopt(argc, argv, "d:n")) != -1)
  {
    switch (opt)
    {
    case 'd':
      rpmsg_dev = optarg;
      break;

    case 'n':
      no_curling = true;
      break;

    default:
      printf("getopt return unsupported option: -%c\n", opt);
      break;
    }
  }

  printf("\r\n Open rpmsg dev \r\n");

  //fd = open(rpmsg_dev, O_RDWR | O_NONBLOCK);
  fd = open(rpmsg_dev, O_RDWR);

  if (fd < 0)
  {
    perror("Failed to open rpmsg file /dev/rpmsg0!");
    return -1;
  }

  i_payload = (struct _payload *)malloc(512);
  r_payload = (struct _payload *)malloc(512);

  if (i_payload == 0 || r_payload == 0)
  {
    printf("ERROR: Failed to allocate memory for payload.\n");
    return -1;
  }
  // open the fifo for receiving commands via IPC:
  fifo_fd = open(fifo_name, O_RDONLY | O_NONBLOCK);
  if (fifo_fd < 0)
  {
    vc_log("error %d opening fifo\n", fifo_fd);
    while (1)
      ;
  }
#undef TEST_KNOB
#ifdef TEST_KNOB
  printf("testing knob\n");
  while (1)
  {
    printf("starting test\n");
    printf("knob = %d\n", get_knob_position());
    printf("button = %d\n", get_button());
    sleep(1);
  }
#endif

  // initialize the system font:
  init_font();

  struct timespec clock_resolution;
  int time0, time1;
  int stat;

  stat = clock_getres(CLOCK_REALTIME, &clock_resolution);

  printf("Clock resolution is %ld seconds, %ld nanoseconds\n",
         clock_resolution.tv_sec, clock_resolution.tv_nsec);

  int which_clock_face = 0;
  init_flws();

  // TEMPORARY:
  init_location(&my_location);
  init_timer(&animation_step_timer, 24);

  // threads for weather and bitcoin updates:
  sem_init(&curl_mutex, 0, 1);
  pthread_t btc_thread_id = pthread_create(&btc_thread_id, NULL, &btc_thread, NULL);
  pthread_t wx_thread_id = pthread_create(&wx_thread_id, NULL, &weather_thread, (void *)&my_location);

  vc_log("entering main loop");
  vc_log("testing %d,%d,%d", 1, 2, 3);
  while (1)
  {
    // since many routines want local or GMT broken-down time, we calculate those here:
    time_t now;
    now = time(NULL);
    time_t local_now = now + (my_location.gmt_offset);
    struct tm local_bdt = *gmtime(&local_now); // my way of getting local time
    struct tm utc_bdt = *gmtime(&now);

    switch (poll_fifo())
    {

    case 'a': // clockwise increment of dial:
      if (manual_pong)
      {
        paddle_input -= 9;
      }
      else
      {
        which_clock_face = (which_clock_face + 1) % nmodes;
      }
      break;

    case 'b': // ccw icrement of dial:
      if (manual_pong)
      {
        paddle_input += 9;
      }
      else
      {
        which_clock_face = (which_clock_face + nmodes - 1) % nmodes;
      }
      break;

    case 'c': // button press on dial:
      // if in pong mode, switch to manual play
      //otherwise, switch to analog clock and set manual_pong to 0:
      if (which_clock_face == 8)
      {
        manual_pong = 1 - manual_pong;
      }
      else
      {
        which_clock_face = 3;
        manual_pong = 0;
      }
      break;

    default:
      //which_clock_face = (microseconds() / 5000000) % 9;
      //which_clock_face = 3;
      break;
    }
#define USE_KNOB
#ifdef USE_KNOB

    which_clock_face += knob_motion();
    if (which_clock_face < 0)
      which_clock_face += nmodes;
    switch (which_clock_face % nmodes)
#else
    switch (which_clock_face % nmodes)
#endif

    {

    case 0:
      renderAnalogClockBuffer(now, &local_bdt, &utc_bdt);
      //render_single_circle();
      break;

    case 1:
      render_lissajou_buffer(now, &local_bdt, &utc_bdt);
      break;

    case 2:
      //render_characters_buffer(now,&local_bdt,&utc_bdt);
      render_ip_address();
      break;

    case 3:
      render_pendulum_buffer(now, &local_bdt, &utc_bdt);
      break;

    case 4:
      compileSegments(test_pat3, MAIN_BUFFER, OVERWRITE);
      update_screen_saver(0, 0); // no screensaver offset for calibration screen
      break;

    case 5:
      //render_fine_circles();
      render_flw(now, &local_bdt, &utc_bdt);
      break;

    case 6:
      pong_update();
      render_pong_buffer(game_state, now, &local_bdt, &utc_bdt);
      break;

    case 7:
      render_word_clock(now, &local_bdt, &utc_bdt);
      break;

    case 8:
      //render_menagerie(now, &local_bdt, &utc_bdt);
      renderSunElev(now, &local_bdt, &utc_bdt);
      break;

    case 9:
      display_mode = moonriseMode;
      renderMoonElev(now, &local_bdt, &utc_bdt);
      break;

    case 10:
      display_mode = sunriseMode;
      renderSR2(now, &local_bdt, &utc_bdt);
      break;

    case 11:
      display_mode = moonriseMode;
      renderSR2(now, &local_bdt, &utc_bdt);
      break;

    case 12:
      render_BTC_price();
      break;

    case 13:
      render_text_clock(now, &local_bdt, &utc_bdt);
      break;

    case 14:
      render_current_weather(now, &local_bdt, &utc_bdt);
      break;

    case 15:
      render_menagerie(now, &local_bdt, &utc_bdt);
      break;
    }

    if (which_clock_face % nmodes != 4)
      update_screen_saver(local_bdt.tm_min % 5, (local_bdt.tm_min - 2) % 4);

#ifdef HW_TEST
    render_hw_test_pattern();
#endif

    if (sync_window())
      copy_seg_buffer(MAIN_BUFFER); // copy the display list to the remote processor, which will do the actual drawing

  foo:

    //if ((microseconds() > next_fps_check) )
    if (0)
    {
      printf("the frame rate =  %d \r\n", check_fps());
      printf("cycles/frame = %d \r\n", check_cycles_in_frame());

      printf("knob position = %d\n", get_knob_position());
      next_fps_check = microseconds() + 2000000;

      printf("button = %d\n", get_button());
      next_fps_check = microseconds() + 2000000;
    }
  }
  //send_done();

  // release the buffers:
  vc_log("releasing RPMsg buffers");
  free(i_payload);
  free(r_payload);
  vc_log("curl_global_cleanup")
  curl_global_cleanup();
  return 0;
}
