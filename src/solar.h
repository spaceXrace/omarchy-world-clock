#ifndef CITIES_SOLAR_H
#define CITIES_SOLAR_H
#include <time.h>
// Earth-fixed Sun unit vector: X=90E, Y=north pole, Z=Greenwich.
void earth_sun_vector(time_t timestamp, float sun[3]);
#endif
