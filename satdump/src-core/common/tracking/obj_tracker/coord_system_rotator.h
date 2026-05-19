#ifndef COORD_SYSTEM_ROTATOR_H
#define COORD_SYSTEM_ROTATOR_H

#include <array>
#include "libs/predict/predict.h"

using Matrix3x3 = std::array<double, 9>;
using Vector3 = std::array<double, 3>;

Matrix3x3 multiply3x3Matrices(const Matrix3x3& A, const Matrix3x3& B);
Vector3 transformVector(const Matrix3x3& A, const Vector3& v);
double computeVectorMagnitude(const Vector3& v);
Vector3 normalizeVector(const Vector3& v);
Matrix3x3 transpose3x3Matrix(const Matrix3x3& A);
Matrix3x3 scaleMatrix(const Matrix3x3& A, double scaleFactor);
Vector3 scaleVector(const Vector3& v, double scaleFactor);
void rotate_coordinate_system(const Vector3 &tilt, Vector3 &pos, Vector3 &vel);
void predict_observe_orbit_new(const Vector3 &tilt, const predict_observer_t *observer, const struct predict_position *orbit, struct predict_observation *obs);

#endif //COORD_SYSTEM_ROTATOR_H
