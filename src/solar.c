#define _POSIX_C_SOURCE 200809L
#include "solar.h"
#include <math.h>
#define PI 3.14159265358979323846
void earth_sun_vector(time_t timestamp, float sun[3]) {
    // NOAA fractional-year approximation, using UTC and equation of time.
    struct tm utc;
    gmtime_r(&timestamp, &utc);
    double hour = utc.tm_hour + utc.tm_min / 60. + utc.tm_sec / 3600.;
    int year = utc.tm_year + 1900;
    int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    double g = 2 * PI / (365 + leap) * (utc.tm_yday + (hour - 12) / 24);
    double eq = 229.18 * (.000075 + .001868 * cos(g) - .032077 * sin(g) - .014615 * cos(2 * g) -
                          .040849 * sin(2 * g));
    double dec = .006918 - .399912 * cos(g) + .070257 * sin(g) - .006758 * cos(2 * g) +
                 .000907 * sin(2 * g) - .002697 * cos(3 * g) + .00148 * sin(3 * g);
    double lon = (180 - hour * 15 - eq * .25) * PI / 180;
    sun[0] = cos(dec) * sin(lon);
    sun[1] = sin(dec);
    sun[2] = cos(dec) * cos(lon);
}
