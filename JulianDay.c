/*  

 Copyright (C) 2016-2018 Michael Boich

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

*/

#include "JulianDay.h"
#include "math.h"

char msg_buf[255];



#undef UNIT_TESTS

//int pref_gmt_offset =-7*3600;  // replace with actual prefs object in final implementation

double reduce360(double x){
	double q = x/360;
	int qInt = trunc(q);
	double result = x - (qInt * 360.0);
	if(result < 0){
		result = result + 360.0;
	}
	return result;
}

double degToRad(double deg){
  return (3.1415926536*deg/180);
}

double radToDeg(double rad){
  return (180.0 * rad/3.1415926536);
}

double degToHours(double deg){
  return deg * (24/360);
}

				 
int unix_month(int month){  // convert 1..12 to 0..11
  // should do range checking here, but not sure how to do it on the PSOC with no console

  return(month-1);
}

int unix_year(int y){
  return(y-1900);
}
       
	
time_t  midnightInTimeZone(time_t the_date, int gmt_offset){
  // We do our own time zone logic, since PSOC C is has no time zone support
  struct tm today;
  time_t result;
  the_date += gmt_offset;  //transform to local time
  today  = *gmtime(&the_date);
  the_date -= 3600 * today.tm_hour;
  the_date -= 60 * today.tm_min;
  the_date -= today.tm_sec;
  the_date -= gmt_offset; // transform back to utc
  
  return(the_date);
}

int secondsSinceGMTMidnight(time_t the_date){
  struct tm gmt_mn,in_time;
  in_time = *gmtime(&the_date);
  return 3600*in_time.tm_hour + 60*in_time.tm_min + in_time.tm_sec;
}

#define EASY_WAY

#ifdef EASY_WAY

time_t dateFromJulianDay(double jd){
  return (86400 * ( jd -   2440587.5));
}
#else



time_t dateFromJulianDay(double jd){
  struct tm result_date;
  time_t result_time;
  jd = jd + 0.5;
  int Z = trunc(jd);
  double F = jd-Z;	//fractional part
  double A;
  if (Z < 2299161) {
    A = Z;
  }
  else {
    int alpha = floor((Z-1867216.25)/36524.25);
    A = Z + 1 + alpha - floor(alpha/4);
  }
	
  int B = A + 1524;
  int C = floor((B-122.1)/365.25);
  int D = floor(365.25 * C);
  int E = floor((B - D) / 30.6001);
	
  double day = B - D - floor(30.6001 * E) + F;		// day of month, with possible fractional part
  int month;
  if (E > 13) {
    month = E - 13;
  }
  else {
    month = E - 1;
  }
	
  int year;
	
  if (month > 2) {
    year = C-4716;
  }
  else {
    year = C-4715;
  }

  //debugMsg("intermdiate vals mo: %d day: %d year %d F: %f\n",(int)month,(int)day,year,F);
  result_date.tm_mon = unix_month(floor(month));
  result_date.tm_mday =(int) day;
  result_date.tm_year = year-1900;
  result_date.tm_hour = result_date.tm_min = result_date.tm_sec=0;

  //debugMsg("mktime args: %d, %d, %d\n",result_date.tm_mon,result_date.tm_mday,result_date.tm_year);
  result_time = mktime(&result_date);
  //debugMsg("result of mktime = %ld\n",result_time);

 
	
  // add fraction part:
  int seconds = F*86400;
  result_time += seconds;
  return result_time;
}
#endif


double julianDayAt0000UT(time_t the_date){
	
  int month,day,year;
  struct tm gmt_date;
  gmt_date = *gmtime(&the_date);

  the_date -= gmt_date.tm_hour*3600;
  the_date -= gmt_date.tm_min*60;
  the_date -= gmt_date.tm_sec;


	
  return julianDay(the_date);
}




// calendarDateAt0000UT returns a unix time_t value corresponding to 0000 GMT for the given date

time_t calendarDateAt0000UT (time_t the_date, int gmt_offset){
  // we have to do the time zone logic ourselves, since ISO C doesn't (or at least the PSOC libraries don't..)

  struct tm utc_date;
  time_t result;
  
  utc_date = *gmtime(&the_date);
  utc_date.tm_hour=0;
  utc_date.tm_min=0;
  utc_date.tm_sec=0;
  result = mktime(&utc_date);  // mktime assumes local time, but we're on UTC tz
  
  return result;
	
}

double  julianDay(time_t the_date){
  return (the_date / 86400.0 ) + 2440587.5;
  }

double deltaTforDate(time_t the_date){
  return (75.0/86400);
  }

double dynamicalTimeFromDate(time_t the_date){
  // placeholder until I get around to the voluminous data entry:
  return julianDay(the_date) + deltaTforDate(the_date);  // approx correction for 2010.  Close enough for the next decade
  }

time_t dateFromDynamicalTime(double dt){
  return dateFromJulianDay(dt - (75.0/86400));
  }


double bigThetaZeroInDegrees(time_t the_date){		// Sidereal time at Meridian of Greenwich at 0h UT on the given date:
  double julianDay = julianDayAt0000UT(the_date);
  double T = (julianDay - 2451545.0)/36525;
	
  double thetaZero = -T/38710000;
  thetaZero = T*(thetaZero +.000387933);
  thetaZero = T*(thetaZero + 36000.770053608);
  thetaZero = thetaZero + 100.46061837;
  thetaZero = reduce360(thetaZero);
  return (thetaZero);
  }
	
	
double littleThetaZeroInDegrees(time_t the_date) {
  double jd = julianDay(the_date);		//only this line differs from above method, but I'm too lazy to refactor them right now.
  double T = (jd - 2451545.0)/36525;
	
  double thetaZero = -T/38710000;
  thetaZero = T*(thetaZero + .000387933);
  thetaZero = T*(thetaZero);
  thetaZero = thetaZero + 280.46061837 + 360.98564736629 * (jd - 2451545.0);
  thetaZero = reduce360(thetaZero);
  return thetaZero;
  }


#ifdef UNIT_TESTS
int main(){
  time_t now,mnitz;
  int utc_offset = -7;

  // reduce360:
  debugMsg("reduce360(1081.234) = %f\n",reduce360(1081.234));
  debugMsg("reduce360(-1081.234) = %f\n",reduce360(-1081.234));

  // midniteInTimeZone:
  now = time(NULL);
  debugMsg("Curent GMT time is %s\n", ctime(&now));
  mnitz = midnightInTimeZone(now,utc_offset);
  debugMsg("midnightInTimeZone(now) = %s\n",ctime(&mnitz));

  // secondsSinceGMTMidnight
  debugMsg("secondsSinceGMTMidnight = %d\n",secondsSinceGMTMidnight(now));

  //julianDay:
  debugMsg("Julian Day now = %f\n", julianDay(now));

  //julianDayAt0000UT:
  debugMsg("julianDayAt0000UTC = %f\n",julianDayAt0000UT(now));

  //dateFromJulianDay:
  time_t tmp;
  tmp = dateFromJulianDay(julianDay(now));
  debugMsg("dateFromJulianDay(%f) = %s\n",julianDay(now),ctime(&tmp));
  
  // BigTheta/littleTheta:
  debugMsg("Little Theta in deg(now) = %lf\n",littleThetaZeroInDegrees(time(NULL)));
  debugMsg("Big Theta in deg(now) = %lf\n",bigThetaZeroInDegrees(calendarDateAt0000UT(time(NULL),-7)));


    // examples from Meeus:
  tmp = dateFromJulianDay(julianDay(1842713));
  debugMsg("dateFromJulianDay(%f) = %s\n",1842713.0,ctime(&tmp));

  debugMsg("Big Theta in deg(2446895.5) = %lf\n",bigThetaZeroInDegrees(dateFromJulianDay(2446895.5)));
  
}

#endif
