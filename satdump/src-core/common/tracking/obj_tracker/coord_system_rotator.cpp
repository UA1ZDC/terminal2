#include "coord_system_rotator.h"
#include <cmath>
#include "libs/predict/defs.h"
#include "libs/predict/sun.h"

/**
 * Geodetic position structure used by SGP4/SDP4 code.
 **/
typedef struct	{
	double lat, lon, alt, theta;
}  geodetic_t;

double ThetaG_JD(double jd)
{
	/* Reference:  The 1992 Astronomical Almanac, page B6. */

	double UT, TU, GMST;

	double dummy;
	UT=modf(jd+0.5, &dummy);
	jd = jd - UT;
	TU=(jd-2451545.0)/36525;
	GMST=24110.54841+TU*(8640184.812866+TU*(0.093104-TU*6.2E-6));
	GMST=fmod(GMST+SECONDS_PER_DAY*EARTH_ROTATIONS_PER_SIDERIAL_DAY*UT,SECONDS_PER_DAY);

	return (2*M_PI*GMST/SECONDS_PER_DAY);
}

double FMod2p(double x)
{
	/* Returns mod 2PI of argument */

	double ret_val = fmod(x, 2*M_PI);

	if (ret_val < 0.0)
		ret_val += (2*M_PI);

	return ret_val;
}

double Sqr(double arg)
{
	/* Returns square of a double */
	return (arg*arg);
}

void Calculate_User_PosVel(double time, geodetic_t *geodetic, double obs_pos[3], double obs_vel[3])
{
	/* Calculate_User_PosVel() passes the user's geodetic position
	   and the time of interest and returns the ECI position and
	   velocity of the observer.  The velocity calculation assumes
	   the geodetic position is stationary relative to the earth's
	   surface. */

	/* Reference:  The 1992 Astronomical Almanac, page K11. */

	double c, sq, achcp;

	geodetic->theta=FMod2p(ThetaG_JD(time)+geodetic->lon); /* LMST */
	c=1/sqrt(1+FLATTENING_FACTOR*(FLATTENING_FACTOR-2)*Sqr(sin(geodetic->lat)));
	sq=Sqr(1-FLATTENING_FACTOR)*c;
	achcp=(EARTH_RADIUS_KM_WGS84*c+geodetic->alt)*cos(geodetic->lat);
	obs_pos[0] = (achcp*cos(geodetic->theta)); /* kilometers */
	obs_pos[1] = (achcp*sin(geodetic->theta));
	obs_pos[2] = ((EARTH_RADIUS_KM_WGS84*sq+geodetic->alt)*sin(geodetic->lat));
	obs_vel[0] = (-EARTH_ANGULAR_VELOCITY*obs_pos[1]); /* kilometers/second */
	obs_vel[1] = (EARTH_ANGULAR_VELOCITY*obs_pos[0]);
	obs_vel[2] = (0);
}

Matrix3x3 multiply3x3Matrices(const Matrix3x3& A, const Matrix3x3& B) {
    Matrix3x3 resultMatrix;
    for (int i = 0; i < 3; i++) {
        const int row = i*3;
        resultMatrix[row] = A[row]*B[0] + A[row+1]*B[3] + A[row+2]*B[6];
        resultMatrix[row+1] = A[row]*B[1] + A[row+1]*B[4] + A[row+2]*B[7];
        resultMatrix[row+2] = A[row]*B[2] + A[row+1]*B[5] + A[row+2]*B[8];
    }
    return resultMatrix;
}

Vector3 transformVector(const Matrix3x3& A, const Vector3& v) {
    Vector3 resultVector;
    for (int i = 0; i < 3; i++) {
        const int row = i*3;
        resultVector[i] = A[row]*v[0] + A[row+1]*v[1] + A[row+2]*v[2];
    }
    return resultVector;
}

double computeVectorMagnitude(const Vector3& v) {
    return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

Vector3 normalizeVector(const Vector3& v) {
    const double magnitude = computeVectorMagnitude(v);
    return {v[0]/magnitude, v[1]/magnitude, v[2]/magnitude};
}

Matrix3x3 transpose3x3Matrix(const Matrix3x3& A) {
    return {A[0], A[3], A[6],
            A[1], A[4], A[7],
            A[2], A[5], A[8]};
}

Matrix3x3 scaleMatrix(const Matrix3x3& A, double scaleFactor) {
    Matrix3x3 resultMatrix;
    for (int i = 0; i < 9; i++) {
        resultMatrix[i] = A[i] * scaleFactor;
    }
    return resultMatrix;
}

Vector3 scaleVector(const Vector3& v, double scaleFactor) {
    return {v[0] * scaleFactor, v[1] * scaleFactor, v[2] * scaleFactor};
}

void rotate_coordinate_system(const Vector3 &tilt, Vector3 &pos, Vector3 &vel) {
	const double azimuthAngle = tilt[0] * (M_PI / 180.0);
	const double tiltAngle = tilt[1] * (M_PI / 180.0);
	const double tiltDirection = tilt[2] * (M_PI / 180.0);

	const double halfAzimuthAngle = azimuthAngle / 2;
	const double azimuthCos = cos(halfAzimuthAngle);
	const double azimuthSin = sin(halfAzimuthAngle);
	const Matrix3x3 azimuthRotation = {
		1 - 2*pow(azimuthSin,2), -2*azimuthCos*azimuthSin, 0,
		2*azimuthCos*azimuthSin,  1 - 2*pow(azimuthSin,2), 0,
		0,                        0,                        1
	};

	const double tiltX = cos(tiltAngle) * sin(-tiltDirection);
	const double tiltY = cos(tiltAngle) * cos(tiltDirection);
	const Vector3 tiltAxis = normalizeVector({-tiltX, -tiltY, 0});

	const double halfTiltAngle = tiltAngle / 2;
	const double tiltCos = cos(halfTiltAngle);
	const double tiltSin = sin(halfTiltAngle);
	const double rotX = tiltAxis[0] * tiltSin;
	const double rotY = tiltAxis[1] * tiltSin;

	const Matrix3x3 tiltRotation = {
		1 - 2*pow(rotY,2),    2*rotX*rotY,         2*tiltCos*rotY,
		2*rotX*rotY,          1 - 2*pow(rotX,2),   -2*tiltCos*rotX,
		-2*tiltCos*rotY,      2*tiltCos*rotX,      1 - 2*(pow(rotX,2) + pow(rotY,2))
	};

	const Matrix3x3 totalRotation = multiply3x3Matrices(tiltRotation, azimuthRotation);
	pos = transformVector(transpose3x3Matrix(totalRotation), pos);
	vel = transformVector(transpose3x3Matrix(totalRotation), vel);
}

void predict_observe_orbit_new(const Vector3 &tilt, const predict_observer_t *observer, const struct predict_position *orbit, struct predict_observation *obs)
{
	if (obs == NULL) return;

	double julTime = orbit->time + JULIAN_TIME_DIFF;
    double obs_pos[3];
	double obs_vel[3];
	double range[3];
	double rgvel[3];

	geodetic_t geodetic;
	geodetic.lat = observer->latitude;
	geodetic.lon = observer->longitude;
	geodetic.alt = observer->altitude / 1000.0;
	geodetic.theta = 0.0;
	Calculate_User_PosVel(julTime, &geodetic, obs_pos, obs_vel);

	range[0] = orbit->position[0] - obs_pos[0];
	range[1] = orbit->position[1] - obs_pos[1];
	range[2] = orbit->position[2] - obs_pos[2];
	rgvel[0] = orbit->velocity[0] - obs_vel[0];
	rgvel[1] = orbit->velocity[1] - obs_vel[1];
	rgvel[2] = orbit->velocity[2] - obs_vel[2];

	double range_length = sqrt(range[0]*range[0]+range[1]*range[1]+range[2]*range[2]);
	double range_rate_length = (range[0]*rgvel[0] + range[1]*rgvel[1] + range[2]*rgvel[2]) / range_length;

	double theta_dot = 2*M_PI*EARTH_ROTATIONS_PER_SIDERIAL_DAY/SECONDS_PER_DAY;
	double sin_lat = sin(geodetic.lat);
	double cos_lat = cos(geodetic.lat);
	double sin_theta = sin(geodetic.theta);
	double cos_theta = cos(geodetic.theta);

	double top_s = sin_lat*cos_theta*range[0] + sin_lat*sin_theta*range[1] - cos_lat*range[2];
	double top_e = -sin_theta*range[0] + cos_theta*range[1];
	double top_z = cos_lat*cos_theta*range[0] + cos_lat*sin_theta*range[1] + sin_lat*range[2];

	double top_s_dot = sin_lat*(cos_theta*rgvel[0] - sin_theta*range[0]*theta_dot) +
						sin_lat*(sin_theta*rgvel[1] + cos_theta*range[1]*theta_dot) -
						cos_lat*rgvel[2];
	double top_e_dot = - (sin_theta*rgvel[0] + cos_theta*range[0]*theta_dot) +
						(cos_theta*rgvel[1] - sin_theta*range[1]*theta_dot);

	double top_z_dot = cos_lat * ( cos_theta*(rgvel[0] + range[1]*theta_dot) +
								sin_theta*(rgvel[1] - range[0]*theta_dot) ) +
								sin_lat*rgvel[2];

    //COORD SYSTEM ROTATION!!!
	Vector3 top_pos = {top_s, top_e, top_z};
	Vector3 top_vel = {top_s_dot, top_e_dot, top_z_dot};
	rotate_coordinate_system(tilt, top_pos, top_vel);

	top_s = top_pos[0];
	top_e = top_pos[1];
	top_z = top_pos[2];
	top_s_dot = top_vel[0];
	top_e_dot = top_vel[1];
	top_z_dot = top_vel[2];
	//COORD SYSTEM ROTATION!!!

	// Azimut
	double y = -top_e / top_s;
	double az = atan(-top_e / top_s);

	if (top_s > 0.0) az = az + M_PI;
	if (az < 0.0) az = az + 2*M_PI;

	// Azimut rate
	double y_dot = - (top_e_dot*top_s - top_s_dot*top_e) / (top_s*top_s);
	double az_dot = y_dot / (1 + y*y);

	// Elevation
	double x = top_z / range_length;
	double el = asin(x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x));

	// Elevation rate
	double x_dot = (top_z_dot*range_length - range_rate_length*top_z) / (range_length * range_length);
	double el_dot = x_dot / sqrt( 1 - x*x );

	obs->azimuth = az;
	obs->azimuth_rate = az_dot;
	obs->elevation = el;
	obs->elevation_rate = el_dot;
	obs->range = range_length;
	obs->range_rate = range_rate_length;
	obs->range_x = range[0];
	obs->range_y = range[1];
	obs->range_z = range[2];

	// Calculate visibility status of the orbit: Orbit is visible if sun elevation is low enough and the orbit is above the horizon, but still in sunlight.
	obs->visible = false;
	struct predict_observation sun_obs;
	predict_observe_sun(observer, orbit->time, &sun_obs);
	if (!(orbit->eclipsed) && (sun_obs.elevation*180.0/M_PI < NAUTICAL_TWILIGHT_SUN_ELEVATION) && (obs->elevation*180.0/M_PI > 0)) {
		obs->visible = true;
	}
	obs->time = orbit->time;
}