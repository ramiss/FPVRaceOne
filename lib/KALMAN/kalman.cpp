#include "kalman.h"

#include <math.h>

KalmanFilter::KalmanFilter() {
    R = 1;  // noise power desirable
    Q = 1;  // noise power estimated
    A = 1;
    B = 0;
    C = 1;

    // Ensure the filter starts in a known "uninitialized" state.
    // If x/cov are left uninitialized, they may contain garbage and the
    // filter can output incorrect values (which can cause false triggers).
    x = NAN;
    cov = NAN;
}

float KalmanFilter::filter(uint16_t z, uint16_t u = 0) {
    if (isnan(x)) {
        x = (1 / C) * z;
        cov = (1 / C) * Q * (1 / C);
    } else {
        // compute prediction
        const float predX = (A * x) + (B * u);
        const float predCov = ((A * cov) * A) + R;

        // Kalman gain
        const float K = predCov * C * (1 / ((C * predCov * C) + Q));

        // correction
        x = predX + K * (z - (C * predX));
        cov = predCov - (K * C * predCov);
    }

    return x;
}

float KalmanFilter::lastMeasurement() {
    return x;
}

void KalmanFilter::setMeasurementNoise(float noise) {
    Q = noise;
}

void KalmanFilter::setProcessNoise(float noise) {
    R = noise;
}
